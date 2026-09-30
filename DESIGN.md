# campy-cpp — design

A native Qt/C++ replacement for the Python campy acquisition tool.

Target rig: 6 × Basler a2A1920-160uc, 1920×1200 RGB8 @ 30 Hz, hardware-triggered
from an STM32H7 (`campy/trigger/trigger_h7_CAM_Sync`), NVENC H.264 to MP4.

## Why rewrite

Every failure we debugged in the Python tool was structural, not a coding slip:

| Symptom | Root cause | Fixed by this design |
|---|---|---|
| MP4s with no `moov` atom after Stop | ffmpeg ran as a **separate process**; killing campy killed it mid-write | Encoder lives in-process; `av_write_trailer()` runs from a destructor |
| 6 workers contending for COM4 | `OpenSystems()` at module scope, re-run by every `spawn` worker | One process, one `SerialController` |
| Failures invisible in the log | 7 processes interleaving stdout/stderr down one pipe | One process, one logger, one sink |
| Unbounded RAM growth under load | `writeQueue = deque()` with no cap | Fixed-capacity ring buffer + explicit drop accounting |
| Trigger "ready" when it was not | `print()` after a blind `write()` | Serial state machine requiring ACK |

## Toolchain

- **Qt 6.5 LTS, `msvc2019_64`** — matches VS2019 Professional (toolset 14.29 = v142).
- **pylon 12.3.0** C++ API. Import libs are `VC142`, so the MSVC kit is mandatory;
  MinGW cannot link them (name mangling / ABI).
- **FFmpeg dev libraries** (shared build with headers + `.lib`), for
  `libavcodec` / `libavformat` / `libavutil` / `libswscale`.
  BtbN `win64-gpl-shared` or gyan.dev `full_build-shared`.
- **CMake + Ninja**. pylon ships `pylon-config.cmake`; Qt6 is CMake-native.

Qt modules: Core, Gui, Widgets, Concurrent (all in the kit) plus **SerialPort**
(add-on, must be selected explicitly). Charts optional for diagnostics.
Qt Multimedia is deliberately **not** used.

## Architecture

    Qt main thread ─── MainWindow: config, sanity checks, preview @10Hz, Start/Stop
          │
          ├─ SerialController (QSerialPort)
          │     state machine: Idle → Handshaking → Running → Stopping
          │     every command requires an ACK; no blind writes
          │
          └─ CameraWorker × 6  (one QThread each)
                  CInstantCamera::RetrieveResult
                        ↓  (Frame: buffer + camera timestamp + index)
                  RingBuffer<Frame>  fixed capacity, SPSC, drops counted
                        ↓
                  VideoEncoder: libavcodec h264_nvenc → libavformat MP4
                        ↓
                  MetadataWriter: frametimes (.npy/.csv), parity with Python output

### Shutdown — the thing this rewrite exists for

    stopRequested (std::atomic<bool>)
        → grab loops exit at the next iteration boundary
        → each ring buffer drains fully into its encoder
        → avcodec_send_frame(nullptr); drain packets
        → av_write_trailer(); avio_closep()      ← in ~VideoEncoder(), RAII
        → threads join (GUI shows progress, with a timeout)
        → SerialController sends the stop command LAST
        → cameras closed, systems released

`av_write_trailer()` sits in the destructor, so the file is finalized on a
normal stop, on an exception, and on a failed camera alike. The Arduino stop is
sequenced strictly after all encoders have closed, matching the existing
requirement that the logger stops only once the videos are safely on disk.

### Colour handling

Keep what already works: feed **RGB/BGR0 straight to `h264_nvenc`** and let the
GPU do the conversion, exactly as the Python tool does via
`pixelFormatOutput: rgb0`. Do not hand-roll CPU `sws_scale` to NV12 — at
1920×1200×3 × 30 Hz × 6 that is ~1.24 GB/s of CPU work.

PCIe traffic at that rate is comfortable for the RTX 5070 Ti. If it ever is not,
the fallback is Bayer output from the cameras (1 byte/px instead of 3) with
debayering on the GPU — a 3× reduction in bus traffic.

**This is the one assumption to measure first.** See Phase 1.

### Backpressure

The Python `deque` is unbounded: a slow encoder silently eats RAM. Here each
ring buffer has a fixed capacity (default 120 frames ≈ 4 s). On overflow the
frame is dropped, a per-camera counter increments, and the GUI shows it in red.
A dropped frame must never be silent — for behavioural work a gap that nobody
noticed is worse than a crash.

### Timestamps

Use the **camera's** timestamp from the grab result, not host time — same as
`unicam.GetTimeStamp`. Host time drifts relative to the trigger and breaks
alignment with the ephys logger's USART1 frame counter.

## Layout

    cpp/
    ├── CMakeLists.txt
    ├── cmake/FindFFmpeg.cmake
    ├── DESIGN.md
    └── src/
        ├── main.cpp
        ├── core/      Config, Frame, RingBuffer, Logger
        ├── camera/    CameraManager (enumerate/map), CameraWorker (grab loop)
        ├── encode/    VideoEncoder (nvenc + mp4)
        ├── trigger/   SerialController (QSerialPort state machine)
        ├── meta/      MetadataWriter (frametimes parity)
        └── gui/       MainWindow, PreviewWidget, SanityChecks

Config stays in the **existing YAML format** (via yaml-cpp) so the current
`configs/*.yaml` and `.pfs` files carry over unchanged and both tools can run
against the same rig during the transition.

## Phases

1. **Spike — validate the risky assumptions.** One camera: pylon grab →
   `h264_nvenc` → MP4, Stop always finalizes. Measure sustained throughput and
   CPU with RGB-direct-to-NVENC, then with 6 streams' worth of synthetic load.
   Everything downstream depends on this being fast enough.
2. **Core pipeline.** CameraWorker + RingBuffer + VideoEncoder, 6 cameras,
   headless, driven from a config file. Drop accounting and metadata parity.
3. **SerialController.** State machine, ACK-based handshake, paired with the
   firmware changes below.
4. **GUI.** Port the existing sanity checks (they encode real rig knowledge:
   bandwidth budget, `.pfs` GenApi version, overwrite protection, trigger port),
   live preview, log pane.
5. **Parity + cutover.** Record the same session with both tools, compare frame
   counts, timestamps and file integrity before retiring the Python path.

## Firmware changes (worth doing regardless)

Against `campy/trigger/trigger_h7_CAM_Sync/trigger_h7_CAM_Sync.ino`:

1. **USB CDC starvation at rate 0.** Once `frame_rate_out == 0`, `loop()` skips
   the pulse block and spins with no `delay()`/`yield()`, so the `Frame period:`
   line never flushes. A `delay(1)` in the idle branch fixes it. campy currently
   works around this by confirming on the weaker `Frame rate set to` line.
2. **`SetDigPins()` can block forever** on `while (Serial.available() == 0) {}`.
   A truncated command wedges the board until a power cycle. Needs a timeout and
   a return to a known state.
3. **No ACK/NAK.** Add `OK ...` / `ERR <reason>` so the host can distinguish
   "parsed and applied" from "silence".
4. **No `STATUS` / `VERSION`.** The host cannot ask what the board is doing
   without reconfiguring it and paying the 4 s `ResetTimer()` penalty.
5. Header comment says pin 39 maps to **PH6**; the code uses `GPIO_PIN_15`.

---

# Reliability: no dropped frames, no death mid-recording

This is the primary requirement. The measurements below are from the actual rig
(2026-09-23), not assumptions.

## Measured hardware baseline — already sound

USB3 topology, camera serial -> host controller:

| Camera | Serial   | Controller             | Link    | Shared with |
|--------|----------|------------------------|---------|-------------|
| CAM1   | 40606908 | Fresco Logic FL1100    | 5 Gb/s  | dedicated   |
| CAM2   | 40606906 | AMD USB 3.2            | 20 Gb/s | **CAM6**    |
| CAM3   | 40606909 | AMD USB 3.1            | 10 Gb/s | dedicated   |
| CAM4   | 40552933 | Fresco Logic FL1100    | 5 Gb/s  | dedicated   |
| CAM5   | 40606902 | AMD USB 3.2            | 20 Gb/s | dedicated   |
| CAM6   | 40606912 | AMD USB 3.2            | 20 Gb/s | **CAM2**    |

Five host controllers for six cameras. USB3 bandwidth is shared **per
controller**, not per port, so the only pair worth checking is CAM2+CAM6:
2 x 207 = 414 MB/s on a 20 Gb/s controller, roughly 17% utilised. Not a
bottleneck. The two FL1100s carry ~207 of ~450 MB/s practical each.

Power management (the classic cause of "a camera died mid-recording") is
already correct on AC: USB selective suspend **disabled**, PCIe ASPM **off**,
and no USB device has per-device power saving enabled. Re-check after any
Windows feature update, which can silently restore defaults.

Budget: 1920x1200 RGB8 = 6.912 MB/frame; 207.4 MB/s per camera; **1.24 GB/s**
aggregate into RAM. Encoded output is only ~13 MB/s per camera (~80 MB/s total),
which the 4 TB SSD absorbs without noticing.

**Conclusion: the rig is not the limitation.** Every loss mechanism left is in
software, and the Python tool had no defence against any of them.

## 1. Ground truth on loss: BlockID gap detection

Every pylon grab result carries a monotonically increasing `BlockID` assigned by
the **camera**. Receiving 1, 2, 3, 5 means frame 4 was lost on the wire.

The Python tool has no concept of this. It counts frames it *received* and
reports that as the frame rate, so a camera silently delivering 28 of every 30
frames looks perfectly healthy in the log and produces a video that is quietly
wrong — the worst possible outcome for behavioural work.

Track per camera: last BlockID, cumulative gap count, largest gap. Any gap is
surfaced immediately in the GUI and written into the metadata sidecar, so a
recording is never *silently* incomplete. Enable chunk data for frame counter
and timestamp so this survives independently of host-side bookkeeping.

## 2. Stop swallowing grab failures

`unicam.py` today:

    except Exception as e:
        if cam_params["cameraDebug"]:      # off by default
            logging.error(...)
        time.sleep(0.001)

Any grab error is discarded unless a debug flag nobody sets is on. This is how
six cameras produced 0-byte files while the log looked calm.

Instead: check `GrabSucceeded()` on every result; on failure record
`GetErrorCode()` and `GetErrorDescription()`, increment a per-camera counter,
and surface it. Use `RetrieveResult(timeout, TimeoutHandling_Return)` and treat
a timeout as a first-class, counted event rather than an exception to swallow.

## 3. Deep buffers — the main shock absorber

With 125 GB RAM, buffer depth is essentially free:

| Depth | RAM per camera | Total (6) | Absorbs |
|-------|----------------|-----------|---------|
| 100 (current Python default) | 0.69 GB | 4.1 GB | 3.3 s |
| 300 (proposed)               | 2.07 GB | 12.4 GB | **10 s** |

Ten seconds of slack means a disk hiccup, a Windows scheduler stall or an NVENC
hitch cannot cost a single frame. Pair `MaxNumBuffer` with the USB transport
layer's `MaxTransferSize` and `MaxNumUSBBuffers`, which default low for this
data rate.

Ring buffers between grab and encode are **fixed capacity**, unlike the Python
`deque()`, which is unbounded and would rather exhaust RAM than admit it is
behind. On overflow: drop, count, and show it in red. A dropped frame must never
be silent.

## 4. One camera failing must not kill the recording

Today a camera that disconnects takes down its worker, and the multiprocessing
pool takes the rest with it — six ruined files instead of one.

Register a configuration event handler for device removal. On loss of a camera:
mark it failed, **finalize its MP4 cleanly**, keep the other five recording, and
raise a visible alarm. Five good files plus one honest error beats six corrupt
files. The session metadata records exactly which camera stopped and at which
frame.

## 5. Thread priorities and affinity

16 cores for 6 grab threads + 6 encode threads + GUI is comfortable, but
priority still matters: grab threads above normal (missing a USB transfer window
is unrecoverable), encode threads normal (they have 10 s of buffer to catch up),
GUI lowest. Pylon's internal grab engine thread priority is configurable and
should be raised with it.

## 6. Telemetry the operator can actually see

Per camera, live: frames grabbed, BlockID gaps, failed grabs, ring buffer
occupancy %, encoder queue depth, MB/s. Pylon's stream grabber statistics
(total/failed buffer counts, missed frame count, resynchronisation count) feed
straight into this.

The Python tool prints an average FPS at the end. By then the experiment is
over. The operator needs to know within a second that CAM3 is dropping frames,
while the animal is still in the arena.

## 7. Validation before cutover

- Record 6 cameras for a full session; assert BlockID gaps == 0 on every camera.
- Confirm frame count == `recTimeInSec * frameRate` exactly, per camera.
- Compare against the Python tool on the same rig.
- Deliberately unplug one camera mid-recording: the other five must finish with
  valid, playable MP4s.
- Deliberately stall the disk: buffers should absorb it with zero loss.

---

# Phase 1 results (2026-09-24) -- PASS

Measured with `tools/usbcheck` on the real rig, 1920x1200 RGB8, 300 buffers.

| Test | Throughput | Frames lost | Failed grabs | pylon missed/resync |
|---|---|---|---|---|
| Free-run 30 Hz, 21 s   | 1208 MB/s | **0** | 0 | 0 / 0 |
| Hardware trigger, 20 s |  945 MB/s | **0** | 0 | 0 / 0 |

All six cameras negotiated SuperSpeed; no USB 2.0 fallbacks. Under hardware
trigger every camera received an identical frame count (468, one at 469),
confirming the synchronised burst is handled cleanly -- including the shared
CAM2/CAM6 controller, the case of most concern. The lower average rate in the
triggered run is only the board's 4 s `ResetTimer()` dead time inside the
measurement window; steady state is a clean 30 fps on all six.

**The USB3 path sustains the full load with zero loss.** The buffer and
threading design above is validated; the remaining risk is the encoder stage,
which phase 2 must measure under the same scrutiny.

## Bug found: the trigger board parser desynchronises

`usbcheck --trigger` initially recorded zero frames while reporting success.
The board had echoed `Frame rate set to: 1.00 fps.` in response to `1,39,30`.

The firmware parses with `Serial.parseFloat()` and keeps no framing, so one
leftover token shifts every subsequent field. After a previous `1,39,0` the
trailing `0` remained, and `1,39,30` was then read as `num_pins=0, rate=1`.
With `n_sync == 0`, `SetPinsHigh()`/`SetPinsLow()` iterate zero times: **no pin
is driven at all**, yet the board still prints `Frame rate set to`, so the host
sees what looks like a successful handshake.

Host-side mitigation, now in both `usbcheck` and `campy/trigger/arduino.py`:

1. Send a bare newline and drain before each command, terminating any partial
   token left in the board parser.
2. Verify the board echoes back **our** values (`Digital pins: 39` and
   `Frame rate set to: 30.00 fps.`), not merely that a line arrived.
3. Retry up to three times, then fail loudly rather than record nothing.

This is the most plausible explanation for the Python tool occasionally
producing 0-byte files while reporting "Arduino ... is ready to trigger".
The firmware changes listed earlier (ACK/NAK, timeouts, framing) would remove
the failure mode at its source rather than working around it.

---

# Phase 2 results (2026-09-24) -- PASS

Full pipeline on the rig: hardware trigger -> 6 grab threads -> bounded rings ->
6 parallel NVENC sessions -> 6 finalized MP4s. Driver 617.14, h264_nvenc,
1920x1200 RGB8 @ 30 Hz, qp 21, 300 buffers, ring capacity 120.

| Run | Duration | Frames/camera | Lost | Dropped | Peak ring |
|---|---|---|---|---|---|
| cpp_test_1 | 60 s | 1801..1824 (spread) | 0 | 0 | 0% |
| cpp_test_2 | 45 s | 1366..1380 (spread) | 0 | 0 | 0% |
| cpp_test_3 | 30 s | **915 on all six**   | 0 | 0 | 0% |

cpp_test_3 verified at the file level: 915 packets and duration 30.466667 s on
every camera, BlockID 0..914 throughout. Frame N is the same trigger pulse in
all six files.

## Synchronisation: two ordering bugs, both fixed

The frame-count spread in the first two runs was not loss -- `lost` was 0
throughout and BlockID gaps were zero. It was start and stop skew:

1. **Unequal starts.** The trigger was started before the cameras were armed,
   so each camera joined an already-running pulse train as its grab thread came
   up: a 23-frame (0.77 s) spread. Fixed by arming all cameras first -- they
   block on Line2 at no cost -- and starting the trigger afterwards, so pulse #1
   reaches all six simultaneously.
2. **Unequal stops.** `Recorder::Stop()` called `CameraWorker::Stop()` in a
   loop, and that call blocks joining the grab thread, so later cameras kept
   capturing while earlier ones wound down: a 14-frame spread. Fixed with
   `RequestStop()` on every camera before blocking on any of them.

`firstBlockId` is now recorded per camera and the summary states plainly whether
the videos are aligned, so this is checked every run rather than assumed.

## Encoder frame accounting

An apparent 1-frame shortfall in 5 of 6 files was an artefact of how it was
measured, not real loss. `VideoEncoder` now counts frames **submitted** and
packets **muxed** separately and prints a MISMATCH line if they ever differ.
Against the files, `nb_read_packets` equals the encoded count on every camera;
`nb_read_frames` reads one lower on some because ffprobe's decode loop does not
flush its final frame. Packets are the ground truth.

One real bug was found along the way: `Drain()` took only a single packet per
submitted frame, so packets could accumulate whenever NVENC had several ready.
It now drains until EAGAIN.

## Still unproven

Peak ring occupancy was **0%** in every run: the encoders never fell behind, so
the backpressure and drop-accounting path has been exercised only by unit test,
never in anger. Worth forcing (slower disk, higher resolution, more cameras)
before trusting the drop counters in production.

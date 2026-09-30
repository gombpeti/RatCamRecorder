# RatCam Recorder

Frame-synchronised multi-camera video acquisition for Basler USB3 cameras, built
for long behavioural neuroscience recordings where **every camera must capture
the same instant** and **no frame may be silently lost**.

Written in C++ with Qt and the Basler pylon SDK. Encoding happens in-process via
libavcodec, on the GPU (NVENC) or CPU (x264).

---

## What it does

Six (or more) cameras share one hardware trigger line driven by a microcontroller.
Every camera exposes on the same electrical edge, so **video frame *N* is the same
moment in time in every file**. The software grabs, encodes and writes all streams
in parallel, and records enough per-frame metadata afterwards to *prove* the
recording is intact rather than merely assert it.

### Why not just use ffmpeg or a Python tool

The problems this exists to solve, all encountered in practice:

- **Files were left unfinalised.** Shelling out to `ffmpeg.exe` means that when
  the parent dies, the child is killed mid-write and the `.mp4` never receives its
  `moov` atom — the file is unplayable. Here the encoder is in-process, and the
  trailer is written on a normal stop, on an exception and on a camera disconnect
  alike.
- **Frame loss was undetectable.** A receive counter counts 1, 2, 3… whether or
  not frames went missing in between. This records the camera's own **BlockID**,
  so a gap is proof of loss and names exactly which frames are absent.
- **Metadata could disagree with the video.** If the host falls behind, frames are
  received but never encoded. Frame times are therefore derived from frames that
  are actually *in the file*, so row *k* is video frame *k*, always.

---

## Verified performance

Measured on the reference rig (6 × Basler a2A1920-160uc, 1920×1200, 30 Hz
hardware trigger, RTX 5070 Ti / NVENC):

| session | frames per camera | lost | dropped | alignment |
|---|---|---|---|---|
| 60 min | 108,012 | 0 | 0 | all cameras BlockID 0–108011 |
| 45 min | 81,012 | 0 | 0 | all cameras BlockID 0–81011 |

Trigger timing measured from the cameras' own hardware timestamps:

```
mean interval 33.3336 ms   (30.000 Hz)
jitter        ±8 µs
largest interval anywhere across all cameras and all frames: 33.3418 ms
```

A dropped trigger would appear as a doubled interval (~66.7 ms). None occurred.

---

## Features

- **Hardware-triggered synchronisation** across any number of cameras
- **Per-frame loss detection** using camera-assigned BlockIDs, not a host counter
- **Hardware timestamps** latched by the camera, not host arrival times
- **GPU encoding** via `h264_nvenc`, or CPU via `libx264`
- **Deep grab buffers** (default 1500/camera ≈ 50 s) so a host or disk stall costs
  nothing; the encoder ring is three-quarters of that
- **Encoder stall watchdog** that names which call is blocking — pixel conversion,
  GPU submit, or the write to disk — live and in the metadata
- **Live preview**, 2×3 grid, detachable to a second monitor, dark/light themes
- **Pre-flight checklist** gating the start of a session
- **Automatic Arduino COM-port detection**
- **campy-compatible metadata**, so existing analysis code keeps working

---

## Requirements

**Runtime (Windows x64)**

- Basler **pylon** runtime — provides the `plnu3v` USB3 driver. Install pylon
  before running; the installer checks for it.
- An NVIDIA GPU for `h264_nvenc` (optional — `libx264` works without one)

**Build**

| component | version used |
|---|---|
| Visual Studio | 2019 (MSVC v142 — must match pylon's ABI) |
| Qt | 6.5 LTS, `msvc2019_64`, with Qt Serial Port |
| Basler pylon SDK | 12.3.0 |
| FFmpeg | shared build, at `C:/Qt/ffmpeg-shared` (see `cmake/FindFFmpegShared.cmake`) |
| CMake | 3.20+ |

---

## Installing

Download `RatCamRecorder-x.y.z-Setup.exe` from the
[Releases](../../releases) page and run it. It installs the application, the Qt
and pylon runtime libraries it needs, and a desktop shortcut.

> The installer bundles the Basler pylon **runtime** DLLs because the system PATH
> commonly points at the 32-bit ones, which makes a 64-bit application fail to
> start with `0xc000007b`.

### Building from source

```bat
cmake -S . -B build -G "Visual Studio 16 2019" -A x64
cmake --build build --config Release
```

Binaries land in `build\bin\Release`. To build the installer:

```bat
"C:\Program Files (x86)\Inno Setup 6\ISCC.exe" packaging\RatCamRecorder.iss
```

---

## Using it

### GUI

Launch **RatCam Recorder**, then:

1. Choose the session folder and duration
2. Select the camera settings file (`.pfs`) — see [`configs/`](configs/)
3. Pick the Arduino COM port (auto-detected) and the trigger pin
4. Press **START** and confirm the three-item pre-flight checklist

Each camera appears in the live grid and in the status table with its received,
encoded, lost and dropped counts, plus buffer occupancy. Rows turn red the moment
anything is lost, dropped, or the encoder stalls.

### Command line

```bat
record --folder G:\session_01 --seconds 2700 --trigger COM4 ^
       --pfs configs\a2A1920-160ucBAS_40606902.pfs
```

| option | meaning |
|---|---|
| `--folder PATH` | output folder; one subfolder per camera |
| `--seconds N` | recording duration |
| `--trigger COM4` | drive the microcontroller (`1,<pin>,<fps>` to start, `1,<pin>,0` to stop) |
| `--pin N` | trigger pin (default 39) |
| `--fps F` | frame rate (default 30) |
| `--pfs PATH` | camera settings applied to every camera |
| `--topview PATH` | separate settings for a top-view camera |
| `--topview-div N` | that camera captures 1 frame per N triggers |
| `--codec NAME` | `h264_nvenc` (default) or `libx264` |
| `--qp N` | quality, lower is better (default 21) |
| `--buffers N` | grab buffers per camera (default 1500 ≈ 50 s at 30 Hz) |

Two diagnostic tools are included:

- **`usbcheck`** — enumerates cameras, reports link speed and per-camera
  bandwidth, and grabs for 30 s to confirm the USB topology can sustain the load
- **`camfeatures`** — read-only dump of what a camera actually supports:
  resolution limits, binning, pixel formats, `SensorReadoutTime`,
  `BslExposureStartDelay`, throughput limits

---

## Shutdown sequence

When the duration expires or **STOP** is pressed, in this order:

1. **The trigger is stopped first** (`1,<pin>,0`). Finalising tens of gigabytes of
   video takes minutes, and the cameras must not still be triggered while it
   happens.
2. **Frames already in flight are collected**, until no camera has received a new
   frame for one second (15 s ceiling). Nothing genuinely captured is discarded.
3. **The encoders drain and the files are finalised** — trailer written, `moov`
   atom in place.
4. **Metadata is written.**

---

## What a session produces

```
session_folder/
├── session_report.txt
├── CAM1/
│   ├── 1.mp4
│   ├── frametimes.npy      (2, N) float64 — frame number, seconds
│   ├── frametimes.mat      MATLAB: frameNumber, timeStamp, blockId
│   ├── blockids.npy        (1, N) float64 — camera frame number per video frame
│   ├── frameinfo.csv       every frame received, and where it ended up
│   ├── dropped.csv         frames received but not encoded
│   ├── gaps.csv            discontinuities in the BlockID sequence
│   ├── metadata.csv        recording parameters and totals
│   └── CAM1_note.txt       plain-English summary
└── CAM2/ …
```

**N counts frames that are in the video.** A frame the camera sent and the host
received does not exist in the file if the encoder was behind when it arrived, and
writing those into `frametimes` would shift every later row.

### Aligning cameras

Use `blockids.npy`, not the row index. Equal frame counts do not prove alignment —
two cameras can hold the same number of frames and not the same frames. Intersect
their BlockIDs and index each video by the row where its `blockId` matches.
`session_report.txt` states whether the session is frame-aligned, and requires
zero losses and zero drops before it says so.

### Key fields in `metadata.csv`

| field | meaning |
|---|---|
| `framesReceived` | frames the camera delivered to the host |
| `framesEncoded` / `totalFrames` | frames actually in the `.mp4` |
| `framesDropped` | received but not encoded (host could not keep up) |
| `framesMissing` | never arrived (lost on the wire) |
| `worstEncoderStallPhase` | which call blocked longest: conversion, GPU, or disk |
| `maxConvertMs` / `maxEncodeMs` / `maxMuxMs` | worst time in each stage |

---

## Camera configuration

Example `.pfs` files are in [`configs/`](configs/), with a guide to the
parameters that matter and how to change them in pylon Viewer.

## Trigger firmware

The microcontroller firmware is in [`arduino/`](arduino/) — a Portenta H7
generating the sync pulse train and a synchronous USART1 frame counter.

---

## Troubleshooting

**Frames dropped, `worstEncoderStallPhase` is "write to disk"**
Antivirus on-access scanning is the usual cause. Six continuously-growing
multi-gigabyte files are close to the worst case for it. Add a **process
exclusion** for `RatCamRecorder.exe`. On the reference rig this took the worst
stall from 33,027 ms to 2 ms.

**A camera runs at exactly half the trigger rate**
Its `BslExposureStartDelay` plus readout exceeds one trigger period. Check with
`camfeatures`. This is a fixed sensor property and cannot be configured away —
not by exposure, ROI, pixel format, bit depth or bandwidth. Either use
`--topview-div 2` to record it deliberately at half rate (still synchronised, on
every second pulse), or use a camera with a shorter latency. For reference,
a2A1920-160uc is **17 µs**; a2A3536-31ucPRO is **27,774 µs**.

**Application fails to start with `0xc000007b`**
Windows is loading 32-bit pylon DLLs into the 64-bit application. The installer
stages the x64 runtime beside the executable to prevent this.

**`ERROR: cannot open COMx`**
The Arduino IDE's Serial Monitor, or another instance, is holding the port.

---

## Licence

RatCam Recorder is licensed under the **GNU General Public License v3.0** — see
[`LICENSE`](LICENSE).

GPLv3 is required rather than merely chosen: the FFmpeg libraries this links
against are built with `--enable-gpl --enable-version3`, which places them under
GPLv3, and the combined work must therefore be GPLv3-compatible.

### Additional permission under GNU GPL version 3, section 7

> If you modify this Program, or any covered work, by linking or combining it
> with the Basler pylon Camera Software Suite (or a modified version of that
> library), containing parts covered by the terms of Basler's licence, the
> licensors of this Program grant you additional permission to convey the
> resulting work.

This exception exists because pylon is proprietary and cannot be relicensed
under the GPL, while the camera hardware cannot be used without it.

### Third-party components

| component | licence | reference |
|---|---|---|
| **FFmpeg** (libavcodec, libavformat, libswscale, libavutil, libswresample) | **GPLv3** as built here | [ffmpeg.org/legal.html](https://ffmpeg.org/legal.html) |
| **x264** | GPLv2+ | [videolan.org/developers/x264.html](https://www.videolan.org/developers/x264.html) |
| **x265** | GPLv2+ | [bitbucket.org/multicoreware/x265](https://bitbucket.org/multicoreware/x265) |
| **Qt 6** | LGPLv3 / commercial | [qt.io/licensing](https://www.qt.io/licensing/) |
| **Basler pylon SDK** | proprietary | Basler's licence terms, supplied with the SDK |

> FFmpeg is LGPLv2.1+ by default. It becomes GPL when built with
> `--enable-gpl`, and version 3 of that licence when built with
> `--enable-version3`. Both apply to the build used here.

### FFmpeg source availability

GPLv3 section 6 requires that the source of the GPL libraries distributed with a
binary be made available. The FFmpeg build bundled in the installer is:

```
ffmpeg version N-126782-gdc52424419-20260923
configured with: --enable-gpl --enable-version3 --enable-libx264 --enable-libx265
```

Its complete source is the corresponding upstream revision,
[`dc52424419`](https://git.ffmpeg.org/gitweb/ffmpeg.git/commit/dc52424419), from
<https://git.ffmpeg.org/ffmpeg.git>.

> Building FFmpeg with `--disable-gpl --disable-libx264 --disable-libx265
> --enable-nvenc` instead yields an LGPL build. GPU encoding (`h264_nvenc`, the
> default) still works; only the `libx264` CPU fallback is lost.

### Prior art

Metadata output is deliberately compatible with
[campy](https://github.com/ksseverson57/campy), so analysis written against it
keeps working.

---

## Author

**Peter Gombkoto**

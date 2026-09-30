# Camera settings files (`.pfs`)

A `.pfs` is a plain-text dump of a Basler camera's parameters, produced and read
by **pylon Viewer**. RatCam Recorder applies one to every camera at startup, so
all cameras are configured identically and reproducibly.

## Files here

| file | camera | what it is |
|---|---|---|
| `a2A1920-160ucBAS_40606902.pfs` | a2A1920-160uc | **The reference config.** 1920×1200, RGB8, 20 ms exposure, hardware trigger. Used for every verified recording. |
| `a2A1920-160ucBAS_BayerRG8.pfs` | a2A1920-160uc | Identical to the above except `PixelFormat BayerRG8` — one line different. Cuts USB bandwidth 3×; demosaicing moves to the host. |
| `a2A3536-31ucPRO_41975154.pfs` | a2A3536-31ucPRO | 12 MP top-view camera, cropped ROI. |
| `CAM0_3536x3536_30Hz.pfs` | a2A3536-31ucPRO | Full sensor, throughput limit at maximum. |
| `CAM0_2966x2974_30Hz.pfs` | a2A3536-31ucPRO | Cropped, throughput limit at maximum. |
| `CAM0_2966x2974_safe360.pfs` | a2A3536-31ucPRO | Cropped, default 360 MB/s limit. |

> **A `.pfs` belongs to a camera model.** Loading one written for a different
> model produces a wall of "node not found" errors. RatCam Recorder matches the
> model automatically where it can, but check the filename.

---

## Editing in pylon Viewer

1. Open **pylon Viewer** and select the camera (close RatCam Recorder first —
   a camera can only be opened by one program at a time)
2. Change parameters in the feature tree on the right
3. **Camera → Save Features…** to write a `.pfs`

To apply one: **Camera → Load Features…**

You can also edit the `.pfs` in a text editor — it's tab-separated
`Parameter<TAB>Value`. That is how `a2A1920-160ucBAS_BayerRG8.pfs` was made, by
changing exactly one line. Editing the text is safer than re-saving from the
Viewer, because re-saving also captures whatever else you happened to change.

---

## Parameters that matter

### Triggering — do not change these

These are what make the cameras synchronous. Getting them wrong silently breaks
the whole point of the system.

| parameter | value | why |
|---|---|---|
| `TriggerSelector` | `FrameStart` | the trigger starts a frame |
| `TriggerMode` | `On` | external triggering enabled |
| `TriggerSource` | **`Line2`** | the pin the trigger pulse arrives on |
| `TriggerActivation` | `RisingEdge` | all cameras must agree on the edge |
| `TriggerDelay` | `0` | any delay desynchronises this camera |
| `BslInputFilterTime` | `0.00` | debouncing would delay or reject pulses |
| `AcquisitionFrameRateEnable` | `0` | with an external trigger this only caps the camera |

In pylon Viewer these live under **Acquisition Control** (and **Digital I/O
Control** for the line settings).

> ⚠️ `TriggerSource` is the single most common mistake. Some `.pfs` files found
> online use `Line1` or `Line3`. On this rig it is **`Line2`** — a file with the
> wrong line will appear to load fine and then capture nothing.

### Exposure — the one you will actually tune

| parameter | reference value | notes |
|---|---|---|
| `ExposureTime` | `20000.0` µs | microseconds. **Must stay below one trigger period** — 33,333 µs at 30 Hz, and less in practice (see below) |
| `ExposureAuto` | `Off` | auto-exposure varies brightness between cameras and over time; keep it off for quantitative work |
| `ExposureMode` | `Timed` | exposure set by `ExposureTime`, not by pulse width |

**Exposure has a hard ceiling.** For a camera to capture every trigger:

```
BslExposureStartDelay + ExposureTime + SensorReadoutTime  <  1 / trigger rate
```

Read the first and last with `camfeatures`. On the a2A1920 the delay is 17 µs and
readout is 5.9 ms, so 20 ms exposure gives `0.017 + 20 + 5.9 = 26 ms` against a
33.3 ms budget — comfortable. If a camera runs at exactly half rate, this sum is
the first thing to check.

With a strobed light source, drop the exposure a long way — it buys margin and
freezes motion.

### Image quality

| parameter | reference value | notes |
|---|---|---|
| `Gain` | `0.000` dB | raise only if you cannot add light; gain adds noise |
| `GainAuto` | `Off` | same reasoning as auto-exposure |
| `BlackLevel` | `0.000` | leave alone unless calibrating |
| `Gamma` | `1.0000` | **keep at 1.0** — a non-linear response breaks intensity measurements |
| `BalanceWhiteAuto` | `Off` | auto white balance drifts between cameras |
| `BalanceRatio` | `0.25` (R, G, B) | equal on all three = no colour cast introduced |

In pylon Viewer: **Analog Control** and **Image Format Control**.

### Pixel format and bandwidth

| parameter | reference value | notes |
|---|---|---|
| `PixelFormat` | `RGB8` | 3 bytes/pixel. `BayerRG8` is 1 byte — same information, one third the USB traffic, demosaiced on the host |
| `DeviceLinkThroughputLimit` | `360000000` | bytes/second this camera may use. Raise if a camera cannot reach the trigger rate; lower if several cameras share a controller |
| `DeviceLinkThroughputLimitMode` | `On` | |
| `BslSensorBitDepthMode` | `Auto` | some models allow 8-bit, which can shorten readout |

Bandwidth per camera at 30 Hz, 1920×1200:

```
RGB8      6.6 MB/frame  →  ~207 MB/s
BayerRG8  2.3 MB/frame  →  ~69 MB/s
Mono8     2.3 MB/frame  →  ~69 MB/s
```

USB3 SuperSpeed is ~400 MB/s usable **per host controller**, shared by every
camera on it. Use `usbcheck` to see the real topology and measured throughput.

> **Mono8 does not make the sensor faster.** Readout time depends on the number
> of rows, not the pixel format — measured identically for Mono8 and RGB8 at the
> same ROI. It only reduces USB traffic.

### Region of interest

| parameter | reference value | notes |
|---|---|---|
| `Width` / `Height` | `1920` / `1200` | |
| `OffsetX` / `OffsetY` | `8` / `8` | must usually be multiples of 2 or 4 |
| `CenterX` / `CenterY` | | centres the ROI and overrides the offsets |

Under **Image Format Control**.

> On some sensors **cropping does not speed up acquisition** — the whole sensor is
> exposed and read out regardless of the ROI, and only the cropped region is
> transmitted. Cropping then saves bandwidth but not time. Verify with
> `camfeatures` rather than assuming.

---

## Making a config for a new camera

1. Open it in pylon Viewer
2. Load the nearest existing `.pfs` for that **model**
3. Adjust exposure for your lighting, keeping `Gain` at 0 if you can
4. Confirm `TriggerSource` is `Line2`, `TriggerMode` is `On`, activation is
   `RisingEdge`
5. Save as `<model>_<serial>.pfs`
6. Verify with `camfeatures --serial <serial> --pfs <file>` — check
   `SensorReadoutTime` and `BslExposureStartDelay` sum with your exposure to less
   than one trigger period
7. Record 20 s and confirm the frame count matches the other cameras exactly

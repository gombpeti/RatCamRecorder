# Trigger firmware

Generates the TTL sync pulse train that drives every camera, and emits a 16-bit
frame counter on USART1 in synchronous mode for an external logger.

**Board:** Arduino Portenta H7 (STM32H7).

| sketch | status |
|---|---|
| `trigger_h7_CAM_Sync_Alexei_v4/` | **current** — repeats the logger STOP packet |
| `trigger_h7_CAM_Sync_Alexei_v3/` | all camera recordings to date were made with this |
| `trigger_h7_CAM_Sync_Alexei_v2/` | kept for reference |

Each version is a copy, never an edit in place, so whatever is flashed on the
board stays recoverable.

> ⚠️ **The USART1 synchronous section is verified against external hardware that
> is not visible from the code** — `setupUSART1_Sync()`, the BRR divisor,
> `CR1`/`CR2`/`CR3` and `send16BitUSART1()`. Do not change it.

---

## How it drives the cameras

Each frame period the firmware holds the selected pins LOW for half the period,
then HIGH for the other half. **Cameras trigger on the rising edge** at the
midpoint. The frame counter for that pulse goes out on USART1 immediately after
the edge:

```c
SetPinsLow();
while (Timer() < frame_period / 2) {}
SetPinsHigh();              // ← rising edge: cameras expose
send16BitUSART1(counter);   // ← this pulse's number
while (Timer() < frame_period) {}
counter++;
```

---

## Serial protocol

USB CDC at 115200 baud. The Portenta has native USB, so **opening the port does
not reset the board** — it keeps running across host connections.

| command | meaning |
|---|---|
| `<n>,<pin…>,<rate>` | configure `n` pins and start at `rate` fps |
| `1,39,30` | **start**: one pin, pin 39, 30 fps |
| `1,39,0` | **stop** |
| `DELAY <start_ms> [stop_ms]` | set the settle windows |
| `VERSION` | report firmware version |

Rates are interpreted as:

- `≥ 2.5` → start pulsing at that rate
- `< 0.5` → stop
- `1` → LEDs on, acquisition untouched
- `2` → LEDs off, acquisition untouched

Rates between 0.5 and 2.5 are reserved for the LED commands and are rejected as
frame rates by the host software.

### Settle windows

| window | default | purpose |
|---|---|---|
| start | 9000 ms | between the START packet and the first pulse |
| stop | 4000 ms | after STOP |

During a settle window the sync pins stay LOW and USART1 is idle. The START
packet goes out on USART1 *before* the window, so downstream equipment is armed
and steady before the first camera trigger arrives.

---

## Stopping the electrophysiology logger

The same USART1 line carries four fixed 8-byte control packets:

| mode | packet | meaning |
|---|---|---|
| 0 | `1,1,17,17,33,33,46,46` | START |
| 1 | `1,1,17,17,33,33,49,49` | **STOP** — shuts the logger down |
| 2 | `1,1,17,17,33,33,206,206` | LED on |
| 3 | `1,1,17,17,33,33,209,209` | LED off |

**New in v4:** the STOP packet is sent **5 times, 2 s apart** (8 s from first to
last) rather than once. A single packet can be missed if the logger is busy or
mid-write, and a missed stop leaves it recording after the session has ended.

```c
const uint8_t  STOP_REPEATS       = 5;
const uint32_t STOP_REPEAT_GAP_MS = 2000;
```

Change those two constants to adjust the count or spacing.

The repeats are scheduled from `loop()` rather than with `delay()`, so the board
stays responsive to serial commands throughout, and they continue past the stop
settle window. **A new START cancels any still pending** — otherwise starting a
session within 8 s of stopping would shut the logger down mid-recording.

The host closes the serial port immediately after sending `1,39,0`, so most of
the repeats happen with no host attached. Their logging is therefore guarded
with `if (Serial)`: writing to a disconnected USB CDC risks stalling the board,
and the packet matters while the log line does not.

---

## The frame counter

`uint16_t counter`, sent on USART1 with each pulse.

- **Starts at 0.** Reset on both START (before the settle window) and STOP, so
  every acquisition begins at frame 0 with no carry-over.
- **The first pulse carries 0**, the second 1, and so on — the increment happens
  after the frame period completes.
- **It matches the camera BlockID exactly**, which also starts at 0. So
  `UART counter N` = `camera BlockID N` = video frame `N` (0-based) =
  `frametimes` row `N+1` (1-based, as MATLAB reads it). No offset to apply.

### It wraps

Being 16-bit, it returns to 0 after 65536 pulses:

```
65536 / 30 Hz = 2184.5 s = 36 min 24 s
```

**Any session longer than ~36.5 minutes wraps the counter.** The camera BlockID
does not. Unwrap in analysis — whenever the counter decreases, add 65536:

```python
import numpy as np
unwrapped = counter.astype(np.int64)
unwrapped += 65536 * np.cumsum(np.diff(counter, prepend=counter[0]) < 0)
```

This is deliberate. Widening the counter would change the USART1 wire format and
break the logger on the other end.

---

## Wiring

Pin **39** carries the sync pulse to every camera's trigger input. On the
cameras this arrives on **`Line2`**, configured as `TriggerSource` with
`RisingEdge` activation — see [`../configs/README.md`](../configs/README.md).

USART1 TX and CLK are on PA9/PA8 for the synchronous frame counter.

---

## Flashing

Open the sketch in the Arduino IDE, select **Arduino Portenta H7 (M7 core)**,
and upload.

> Close the Serial Monitor before recording. It holds the COM port, and RatCam
> Recorder will fail with `cannot open COMx`.

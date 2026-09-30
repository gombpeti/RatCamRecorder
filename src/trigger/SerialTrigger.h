#pragma once

// Drives trigger_h7_CAM_Sync_Alexei_v2 over USB serial.
//
// Protocol, as implemented by that firmware:
//   "1,39,30"   start: pin 39 at 30 fps
//   "1,39,0"    stop
//   "1,39,1"    LED ON      \  reserved band: round(rate) 1 or 2. These are
//   "1,39,2"    LED OFF     /  commands, NOT rates -- they never start or stop
//                              the pulse train. Real rates must be >= 2.5.
//
// Two behaviours of that firmware shape this class, both found the hard way:
//
//  * The parser has no framing. Serial.parseFloat() just takes the next number
//    it finds, so one stray token shifts every field: "1,39,30" gets read as
//    num_pins=0, rate=1, which lands in the reserved band and is executed as
//    LED ON. The board replies plausibly and nothing pulses. So the echoed
//    VALUES are verified, not merely the arrival of a reply.
//  * ResetTimer() does delay(START_SETTLE_MS=9000) or delay(STOP_SETTLE_MS=4000)
//    and THEN calls FlushSerialBuffer(). A command sent during that window is
//    not queued -- it is discarded. So a retry must wait the window out.
//
// Deliberately Win32 rather than QSerialPort, so tools can use it without Qt.

#include <string>

namespace campy {

class SerialTrigger {
public:
    ~SerialTrigger();

    bool Open(const std::string& port);
    void Close();
    bool IsOpen() const;

    /// Start pulsing `pin` at `rate` fps. Verifies the board echoed back these
    /// exact values. Rates in [0.5, 2.5) are refused: the firmware treats them
    /// as LED commands and would never start the pulse train.
    bool Start(int pin, double rate);

    /// Stop pulsing (rate 0), verified the same way.
    bool Stop(int pin);

    /// v3 firmware: set the board's settle windows in advance, so the arm
    /// delay for the IR device is a recording parameter rather than a
    /// compile-time constant that has to be reflashed. Returns false on v2,
    /// which does not understand DELAY -- that is not an error, it just means
    /// the board keeps its built-in 9000/4000 ms.
    bool SetDelay(int startMs, int stopMs = -1);

    /// True if the board answered VERSION, i.e. it is v3 or later.
    bool QueryVersion(std::string& versionOut);

    /// Milliseconds to wait after a successful Start() before the first pulse.
    /// Covers the firmware START_SETTLE_MS; raise or lower to match a reflash.
    void SetStartSettleMs(int ms) { startSettleMs_ = ms; }
    int StartSettleMs() const { return startSettleMs_; }

    const std::string& Echo() const { return echo_; }
    const std::string& LastError() const { return lastError_; }

private:
    void PrintEcho() const;
    bool Configure(int pin, double rate, int attempts);
    bool Send(const std::string& cmd, const char* needle, double timeoutSec);
    void Drain();

    void* handle_ = nullptr;      // HANDLE, kept opaque to avoid <windows.h>
    std::string echo_;
    std::string lastError_;
    int startSettleMs_ = 9500;    // firmware START_SETTLE_MS is 9000
    int stopSettleMs_ = 4500;     // firmware STOP_SETTLE_MS is 4000
};

} // namespace campy

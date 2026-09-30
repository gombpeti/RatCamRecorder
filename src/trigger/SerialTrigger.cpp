#include "trigger/SerialTrigger.h"

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

namespace campy {
namespace {

using Clock = std::chrono::steady_clock;
constexpr const char* kConfirm = "Frame rate set to";

HANDLE H(void* p) { return static_cast<HANDLE>(p); }

std::string FormatRate(double rate) {
    char buf[32];
    // "%g" so 30.0 becomes "30", matching what the firmware expects and what
    // the lab sends by hand.
    std::snprintf(buf, sizeof buf, "%g", rate);
    return buf;
}

} // namespace

SerialTrigger::~SerialTrigger() { Close(); }

bool SerialTrigger::IsOpen() const {
    return handle_ != nullptr && H(handle_) != INVALID_HANDLE_VALUE;
}

bool SerialTrigger::Open(const std::string& port) {
    Close();
    const std::string path = "\\\\.\\" + port;   // \\.\COMn works past COM9
    HANDLE h = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                           0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        lastError_ = "cannot open " + port +
                     " (in use by Arduino IDE, a Serial Monitor, or campy?)";
        return false;
    }

    DCB dcb{};
    dcb.DCBlength = sizeof dcb;
    if (!GetCommState(h, &dcb)) { CloseHandle(h); lastError_ = "GetCommState"; return false; }
    dcb.BaudRate    = 115200;
    dcb.ByteSize    = 8;
    dcb.Parity      = NOPARITY;
    dcb.StopBits    = ONESTOPBIT;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    if (!SetCommState(h, &dcb)) { CloseHandle(h); lastError_ = "SetCommState"; return false; }

    COMMTIMEOUTS to{};
    to.ReadIntervalTimeout      = 50;
    to.ReadTotalTimeoutConstant = 100;
    SetCommTimeouts(h, &to);
    PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);

    handle_ = h;
    return true;
}

void SerialTrigger::Close() {
    if (IsOpen()) CloseHandle(H(handle_));
    handle_ = nullptr;
}

void SerialTrigger::Drain() {
    // Host side only. Deliberately does NOT write a newline: a bare newline
    // makes the firmware's parseFloat() time out to 0, so num_pins becomes 0
    // and it then blocks in SetFrameRate() until the next command -- whose
    // leading "1" becomes the rate, i.e. the LED ON command. Recovery is
    // resending the whole command, not sending a separator.
    if (IsOpen()) PurgeComm(H(handle_), PURGE_RXCLEAR);
}

bool SerialTrigger::Send(const std::string& cmd, const char* needle,
                         double timeoutSec) {
    if (!IsOpen()) { lastError_ = "port not open"; return false; }
    Drain();

    const std::string line = cmd + "\n";
    DWORD written = 0;
    if (!WriteFile(H(handle_), line.data(), static_cast<DWORD>(line.size()),
                   &written, nullptr)) {
        lastError_ = "write failed";
        return false;
    }

    std::string acc;
    const auto deadline = Clock::now() + std::chrono::duration<double>(timeoutSec);
    char buf[256];
    while (Clock::now() < deadline) {
        DWORD got = 0;
        if (ReadFile(H(handle_), buf, sizeof buf, &got, nullptr) && got > 0) {
            acc.append(buf, got);
            if (acc.find(needle) != std::string::npos) { echo_ = acc; return true; }
        }
    }
    echo_ = acc;
    return false;
}

bool SerialTrigger::Configure(int pin, double rate, int attempts) {
    const std::string cmd = "1," + std::to_string(pin) + "," + FormatRate(rate);

    char wantPin[64], wantRate[64];
    std::snprintf(wantPin,  sizeof wantPin,  "Digital pins: %d", pin);
    std::snprintf(wantRate, sizeof wantRate, "Frame rate set to: %.2f fps.", rate);

    for (int attempt = 1; attempt <= attempts; ++attempt) {
        // A command sent while the board sits in ResetTimer()'s delay is not
        // queued: FlushSerialBuffer() runs straight after and discards it. So
        // a retry waits the settle window out instead of firing immediately.
        if (attempt > 1)
            std::this_thread::sleep_for(std::chrono::milliseconds(stopSettleMs_));

        // Print the literal bytes, not a description of them. What goes down
        // the wire is the thing worth being able to check.
        std::printf("[trigger] TX: \"%s\\n\"\n", cmd.c_str());
        std::fflush(stdout);

        Send(cmd, kConfirm, 8.0);

        for (const char* p = echo_.c_str(); *p; ) {
            const char* nl = std::strpbrk(p, "\r\n");
            const size_t n = nl ? static_cast<size_t>(nl - p) : std::strlen(p);
            if (n > 0) std::printf("[trigger] RX: %.*s\n", static_cast<int>(n), p);
            p = nl ? nl + 1 : p + n;
        }
        std::fflush(stdout);
        if (echo_.find(wantPin) != std::string::npos &&
            echo_.find(wantRate) != std::string::npos)
            return true;

        std::printf("[trigger] attempt %d did not take (wanted \"%s\" and \"%s\")\n",
                    attempt, wantPin, wantRate);
        std::fflush(stdout);
    }

    lastError_ = "board never confirmed " + cmd +
                 "; a wrong rate here means the parser is desynced and NO pin "
                 "is being driven";
    return false;
}

bool SerialTrigger::Start(int pin, double rate) {
    if (rate >= 0.5 && rate < 2.5) {
        char msg[192];
        std::snprintf(msg, sizeof msg,
                      "%g fps is in the firmware reserved band [0.5, 2.5): "
                      "round()==1 is LED ON, round()==2 is LED OFF. Neither "
                      "starts pulsing. Use a rate >= 2.5.", rate);
        lastError_ = msg;
        return false;
    }
    if (rate <= 0) { lastError_ = "use Stop() for rate 0"; return false; }
    return Configure(pin, rate, 4);
}

void SerialTrigger::PrintEcho() const {
    for (const char* p = echo_.c_str(); *p; ) {
        const char* nl = std::strpbrk(p, "\r\n");
        const size_t n = nl ? static_cast<size_t>(nl - p) : std::strlen(p);
        if (n > 0) std::printf("[trigger] RX: %.*s\n", static_cast<int>(n), p);
        p = nl ? nl + 1 : p + n;
    }
    std::fflush(stdout);
}

bool SerialTrigger::QueryVersion(std::string& versionOut) {
    if (!Send("VERSION", "OK", 3.0)) return false;
    // The version line comes before the OK.
    const size_t end = echo_.find_first_of("\r\n");
    versionOut = echo_.substr(0, end == std::string::npos ? echo_.size() : end);
    std::printf("[trigger] board: %s\n", versionOut.c_str());
    std::fflush(stdout);
    return true;
}

bool SerialTrigger::SetDelay(int startMs, int stopMs) {
    char cmd[64];
    if (stopMs >= 0) std::snprintf(cmd, sizeof cmd, "DELAY %d %d", startMs, stopMs);
    else             std::snprintf(cmd, sizeof cmd, "DELAY %d", startMs);

    std::printf("[trigger] TX: \"%s\\n\"\n", cmd);
    std::fflush(stdout);

    if (!Send(cmd, "OK", 4.0)) {
        lastError_ = "board did not accept DELAY (v2 firmware?)";
        return false;
    }
    PrintEcho();
    return true;
}

bool SerialTrigger::Stop(int pin) {
    // Worth retrying: a trigger left pulsing keeps the cameras exposing.
    return Configure(pin, 0.0, 3);
}

} // namespace campy

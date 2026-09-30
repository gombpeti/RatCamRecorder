// usbcheck -- phase-1 validation tool for campy-cpp.
//
// Answers the questions the Python tool cannot:
//   * What USB link speed did each camera actually negotiate?
//   * Are we losing frames on the wire? (BlockID gap detection -- ground truth)
//   * What sustained throughput do all cameras hold together?
//   * What do pylon's own stream statistics say afterwards?
//
// Needs only pylon. No Qt, no FFmpeg -- deliberately, so it can run before the
// rest of the toolchain is in place.
//
//   usbcheck --list
//   usbcheck --seconds 60 --pfs ../../configs/a2A1920-160ucBAS_40606902.pfs
//   usbcheck --seconds 30 --freerun 30      (no Arduino trigger needed)

#include <pylon/PylonIncludes.h>
#include <pylon/ParameterIncludes.h>

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace Pylon;
using Clock = std::chrono::steady_clock;

namespace {

struct CamStats {
    std::string model, serial, userId;
    uint64_t frames = 0;
    uint64_t bytes = 0;
    uint64_t lastBlockId = 0;
    bool haveLast = false;
    uint64_t gapEvents = 0;    // how many discontinuities
    uint64_t framesLost = 0;   // total frames missing on the wire
    uint64_t largestGap = 0;
    uint64_t failed = 0;       // GrabSucceeded() == false
    uint64_t prevFrames = 0;   // for the per-second rate line
    std::string speedMode;
    int64_t linkSpeed = 0;     // bytes/s
    int64_t throughputLimit = 0;
};

int64_t ReadInt(GenApi::INodeMap& nm, const char* name, int64_t def = -1) {
    try {
        CIntegerParameter p(nm, name);
        if (p.IsReadable()) return p.GetValue();
    } catch (const GenericException&) {}
    return def;
}

std::string ReadEnum(GenApi::INodeMap& nm, const char* name) {
    try {
        CEnumParameter p(nm, name);
        if (p.IsReadable()) return std::string(p.GetValue().c_str());
    } catch (const GenericException&) {}
    return std::string();
}

bool TrySetInt(GenApi::INodeMap& nm, const char* name, int64_t v) {
    try {
        CIntegerParameter p(nm, name);
        if (p.IsWritable()) { p.SetValue(v, IntegerValueCorrection_Nearest); return true; }
    } catch (const GenericException&) {}
    return false;
}

std::string HumanRate(double bytesPerSec) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.1f MB/s", bytesPerSec / 1e6);
    return buf;
}

// Minimal Win32 serial, enough to drive trigger_h7_CAM_Sync.ino. Deliberately
// not QSerialPort: this tool must build without Qt.
class SerialPort {
public:
    ~SerialPort() { Close(); }

    bool Open(const std::string& port) {
        const std::string path = "\\\\.\\" + port;   // \\.\COMn form works past COM9
        h_ = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                         0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h_ == INVALID_HANDLE_VALUE) return false;

        DCB dcb{};
        dcb.DCBlength = sizeof dcb;
        if (!GetCommState(h_, &dcb)) return false;
        dcb.BaudRate    = 115200;
        dcb.ByteSize    = 8;
        dcb.Parity      = NOPARITY;
        dcb.StopBits    = ONESTOPBIT;
        dcb.fDtrControl = DTR_CONTROL_ENABLE;
        if (!SetCommState(h_, &dcb)) return false;

        COMMTIMEOUTS to{};
        to.ReadIntervalTimeout        = 50;
        to.ReadTotalTimeoutConstant   = 100;
        SetCommTimeouts(h_, &to);
        PurgeComm(h_, PURGE_RXCLEAR | PURGE_TXCLEAR);
        return true;
    }

    // Send a command and wait for the board to echo `needle`.
    // The needle is "Frame rate set to" rather than the later "Frame period"
    // line: at rate 0 the firmware stops printing after the former, because
    // loop() then spins with no yield and starves USB CDC transmit.
    bool Command(const std::string& cmd, const char* needle, double timeoutSec) {
        if (h_ == INVALID_HANDLE_VALUE) return false;
        PurgeComm(h_, PURGE_RXCLEAR);
        const std::string line = cmd + "\n";
        DWORD written = 0;
        if (!WriteFile(h_, line.data(), static_cast<DWORD>(line.size()), &written, nullptr))
            return false;

        std::string acc;
        const auto deadline = Clock::now() + std::chrono::duration<double>(timeoutSec);
        char buf[256];
        while (Clock::now() < deadline) {
            DWORD got = 0;
            if (ReadFile(h_, buf, sizeof buf, &got, nullptr) && got > 0) {
                acc.append(buf, got);
                if (acc.find(needle) != std::string::npos) { echo_ = acc; return true; }
            }
        }
        echo_ = acc;
        return false;
    }

    // Drop anything the board has already said, host side only.
    //
    // Deliberately does NOT write a newline. trigger_h7_CAM_Sync_Alexei_v2
    // parses with Serial.parseFloat() and blocks in
    // `while (Serial.available() == 0) {}`; a bare newline makes parseFloat
    // time out to 0, so num_pins becomes 0 and the board then stalls in
    // SetFrameRate() until the next command arrives -- whose leading "1" is
    // then taken as the rate, which is the reserved LED-ON command. Recovery
    // is resending the whole command, not sending a separator.
    void Drain() {
        if (h_ != INVALID_HANDLE_VALUE) PurgeComm(h_, PURGE_RXCLEAR);
    }

    // Configure and VERIFY: the board must echo back the pin and rate we asked
    // for. "Frame rate set to" arriving is not proof the right values landed --
    // a desynced parser answers with a different number, and if that number
    // lands in the reserved band it is silently treated as an LED command that
    // never starts the pulse train.
    bool Configure(int pin, double rate, double timeoutSec = 8.0) {
        char cmd[64], wantPin[64], wantRate[64];
        std::snprintf(cmd,      sizeof cmd,      "1,%d,%g", pin, rate);
        std::snprintf(wantPin,  sizeof wantPin,  "Digital pins: %d", pin);
        std::snprintf(wantRate, sizeof wantRate, "Frame rate set to: %.2f fps.", rate);

        for (int attempt = 1; attempt <= 4; ++attempt) {
            Drain();
            // Resending the complete command is the recovery: each one supplies
            // a full set of tokens, and the board runs FlushSerialBuffer() at
            // the end of every command cycle, so alignment is restored.
            //
            // A command sent while the board is inside ResetTimer()'s delay is
            // not merely ignored -- FlushSerialBuffer() runs immediately after
            // and discards it. So a retry must wait out STOP_SETTLE_MS (4 s)
            // rather than firing straight away.
            if (attempt > 1)
                std::this_thread::sleep_for(std::chrono::milliseconds(4500));
            Command(cmd, "Frame rate set to", timeoutSec);
            const bool okPin  = echo_.find(wantPin)  != std::string::npos;
            const bool okRate = echo_.find(wantRate) != std::string::npos;
            if (okPin && okRate) { lastCmd_ = cmd; return true; }
            std::printf("    trigger: attempt %d did not take (want %s / %s)\n",
                        attempt, wantPin, wantRate);
        }
        lastCmd_ = cmd;
        return false;
    }

    const std::string& LastCommand() const { return lastCmd_; }

    const std::string& Echo() const { return echo_; }

    void Close() {
        if (h_ != INVALID_HANDLE_VALUE) { CloseHandle(h_); h_ = INVALID_HANDLE_VALUE; }
    }

private:
    HANDLE h_ = INVALID_HANDLE_VALUE;
    std::string echo_;
    std::string lastCmd_;
};

void PrintUsage() {
    std::printf(
        "usbcheck -- campy-cpp USB3 reliability probe\n\n"
        "  --list              enumerate cameras and exit\n"
        "  --seconds N         grab for N seconds (default 30)\n"
        "  --pfs PATH          load this .pfs into every camera first\n"
        "  --freerun FPS       disable the FrameStart trigger and free-run at FPS\n"
        "                      (lets you test bandwidth without the Arduino)\n"
        "  --trigger COM4      drive the Arduino: send 1,39,30 before grabbing and\n"
        "                      1,39,0 after. Tests the real synchronised burst,\n"
        "                      which is harder on the bus than free-run.\n"
        "  --pin N             trigger pin for --trigger (default 39)\n"
        "  --settle MS         wait this long after the start command before\n"
        "                      grabbing, covering the board ResetTimer() delay.\n"
        "                      Default 4500 for the current firmware; use ~700\n"
        "                      once that delay is reduced to 500 ms.\n"
        "  --buffers N         MaxNumBuffer per camera (default 300)\n\n");
}

} // namespace

int main(int argc, char* argv[]) {
    int seconds = 30;
    int buffers = 300;
    int pin = 39;
    int settleMs = 9500;   // START_SETTLE_MS (9000) in the firmware + margin
    double freerun = 0.0;
    std::string pfs, triggerPort;
    bool listOnly = false;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* what) -> std::string {
            if (i + 1 >= argc) { std::printf("ERROR: %s needs a value\n", what); std::exit(2); }
            return argv[++i];
        };
        if (a == "--list")          listOnly = true;
        else if (a == "--seconds")  seconds  = std::stoi(next("--seconds"));
        else if (a == "--buffers")  buffers  = std::stoi(next("--buffers"));
        else if (a == "--freerun")  freerun  = std::stod(next("--freerun"));
        else if (a == "--pfs")      pfs      = next("--pfs");
        else if (a == "--trigger")  triggerPort = next("--trigger");
        else if (a == "--pin")      pin      = std::stoi(next("--pin"));
        else if (a == "--settle")   settleMs = std::stoi(next("--settle"));
        else { PrintUsage(); return a == "--help" ? 0 : 2; }
    }

    PylonAutoInitTerm autoInit;
    int exitCode = 0;

    try {
        DeviceInfoList_t devices;
        CTlFactory& factory = CTlFactory::GetInstance();
        if (factory.EnumerateDevices(devices) == 0) {
            std::printf("No cameras found.\n");
            return 1;
        }

        std::printf("Found %u camera(s):\n", static_cast<unsigned>(devices.size()));
        for (size_t i = 0; i < devices.size(); ++i) {
            std::printf("  [%zu] %-24s serial %-12s %s\n", i,
                        devices[i].GetModelName().c_str(),
                        devices[i].GetSerialNumber().c_str(),
                        devices[i].GetUserDefinedName().c_str());
        }
        if (listOnly) return 0;

        CInstantCameraArray cameras(devices.size());
        std::vector<CamStats> stats(devices.size());

        std::printf("\n--- opening ---\n");
        for (size_t i = 0; i < cameras.GetSize(); ++i) {
            cameras[i].Attach(factory.CreateDevice(devices[i]));
            cameras[i].Open();

            GenApi::INodeMap& nm = cameras[i].GetNodeMap();
            CamStats& s = stats[i];
            s.model  = devices[i].GetModelName().c_str();
            s.serial = devices[i].GetSerialNumber().c_str();
            s.userId = devices[i].GetUserDefinedName().c_str();

            if (!pfs.empty()) {
                try {
                    // Validation off, matching campy (basler.py:53). The .pfs sets
                    // TriggerActivation=RisingEdge for the *Active selectors, which
                    // accept only LevelHigh/LevelLow; the camera coerces them and
                    // strict validation then rejects the entire load. The selector
                    // that matters, FrameStart, applies fine -- verified below
                    // rather than assumed.
                    CFeaturePersistence::Load(pfs.c_str(), &nm, false);
                } catch (const GenericException& e) {
                    std::printf("  [%zu] .pfs load FAILED: %s\n", i, e.GetDescription());
                    exitCode = 1;
                }
            }

            if (freerun > 0.0) {
                try {
                    CEnumParameter(nm, "TriggerSelector").TrySetValue("FrameStart");
                    CEnumParameter(nm, "TriggerMode").TrySetValue("Off");
                    CBooleanParameter(nm, "AcquisitionFrameRateEnable").TrySetValue(true);
                    CFloatParameter(nm, "AcquisitionFrameRate").TrySetValue(freerun);
                } catch (const GenericException& e) {
                    std::printf("  [%zu] free-run setup failed: %s\n", i, e.GetDescription());
                }
            }

            // Deep buffers are the main shock absorber; see DESIGN.md.
            try { cameras[i].MaxNumBuffer.SetValue(buffers); }
            catch (const GenericException& e) {
                std::printf("  [%zu] MaxNumBuffer failed: %s\n", i, e.GetDescription());
            }

            // USB transport layer defaults are low for this data rate.
            GenApi::INodeMap& sgm = cameras[i].GetStreamGrabberNodeMap();
            TrySetInt(sgm, "MaxTransferSize", 4 * 1024 * 1024);
            TrySetInt(sgm, "NumMaxQueuedUrbs", 64);

            s.speedMode       = ReadEnum(nm, "BslUSBSpeedMode");
            s.linkSpeed       = ReadInt(nm, "DeviceLinkSpeed");
            s.throughputLimit = ReadInt(nm, "DeviceLinkThroughputLimit");

            std::printf("  [%zu] %-12s %-22s link=%-12s",
                        i, s.serial.c_str(), s.model.c_str(),
                        s.speedMode.empty() ? "?" : s.speedMode.c_str());
            if (s.throughputLimit > 0)
                std::printf(" limit=%.0f MB/s", s.throughputLimit / 1e6);
            std::printf("\n");

            // Read back what the camera is ACTUALLY set to, rather than trusting
            // that the .pfs applied. This is the check campy never makes.
            const std::string pixFmt = ReadEnum(nm, "PixelFormat");
            const int64_t w = ReadInt(nm, "Width"), h = ReadInt(nm, "Height");
            std::string trigMode, trigSrc, trigAct;
            try {
                CEnumParameter(nm, "TriggerSelector").TrySetValue("FrameStart");
                trigMode = ReadEnum(nm, "TriggerMode");
                trigSrc  = ReadEnum(nm, "TriggerSource");
                trigAct  = ReadEnum(nm, "TriggerActivation");
            } catch (const GenericException&) {}
            std::printf("       %lldx%lld %s | FrameStart trigger: %s",
                        static_cast<long long>(w), static_cast<long long>(h),
                        pixFmt.c_str(), trigMode.empty() ? "?" : trigMode.c_str());
            if (trigMode == "On")
                std::printf(" src=%s act=%s", trigSrc.c_str(), trigAct.c_str());
            const double needed = static_cast<double>(w) * h *
                                  (pixFmt.rfind("RGB", 0) == 0 ? 3 : 1) * 30.0;
            std::printf(" | ~%.0f MB/s @30Hz\n", needed / 1e6);

            if (s.speedMode == "HighSpeed") {
                std::printf("       *** WARNING: negotiated USB 2.0 HighSpeed (~60 MB/s). "
                            "This camera CANNOT sustain full rate. Check cable/port. ***\n");
                exitCode = 1;
            }
        }

        std::printf("\n--- grabbing for %d s (buffers=%d%s) ---\n", seconds, buffers,
                    freerun > 0 ? ", free-run" : ", hardware trigger");

        SerialPort serial;
        const double rate = freerun > 0.0 ? freerun : 30.0;
        char startCmd[64], stopCmd[64];
        std::snprintf(startCmd, sizeof startCmd, "1,%d,%g", pin, rate);
        std::snprintf(stopCmd,  sizeof stopCmd,  "1,%d,0", pin);

        if (!triggerPort.empty()) {
            // The firmware reserves rates in [0.5, 2.5): round()==1 is LED ON,
            // round()==2 is LED OFF. Those are commands, not rates -- they never
            // start the pulse train, so asking for one here would silently
            // record nothing. Real rates must be >= 2.5.
            if (rate >= 0.5 && rate < 2.5) {
                std::printf("ERROR: %g fps is in the firmware reserved band "
                            "[0.5, 2.5).\n       round()==1 is LED ON, "
                            "round()==2 is LED OFF; neither starts pulsing.\n"
                            "       Use a rate >= 2.5.\n", rate);
                return 2;
            }
            if (!serial.Open(triggerPort)) {
                std::printf("ERROR: cannot open %s\n", triggerPort.c_str());
                return 1;
            }
            std::printf("    trigger: sending %s to %s\n", startCmd, triggerPort.c_str());
            const bool acked = serial.Configure(pin, rate);
            // Always show the reply; the echoed values are what matter, not the
            // mere arrival of a line.
            for (const char* p = serial.Echo().c_str(); *p; ) {
                const char* nl = std::strpbrk(p, "\r\n");
                const size_t n = nl ? static_cast<size_t>(nl - p) : std::strlen(p);
                if (n > 0) std::printf("      board: %.*s\n", static_cast<int>(n), p);
                p = nl ? nl + 1 : p + n;
            }
            if (!acked) {
                std::printf("ERROR: board did not confirm pin %d at %g fps after 3 tries.\n"
                            "Nothing will be triggered, so aborting.\n", pin, rate);
                return 1;
            }
            // trigger_h7_CAM_Sync ResetTimer() delays 4 s before the first pulse
            std::printf("    trigger: acknowledged, settling %d ms "
                        "(firmware START_SETTLE_MS is 9000)...\n", settleMs);
            std::this_thread::sleep_for(std::chrono::milliseconds(settleMs));
        } else if (freerun <= 0.0) {
            std::printf("    (hardware trigger: the Arduino must already be pulsing)\n");
        }

        cameras.StartGrabbing(GrabStrategy_OneByOne);

        const auto t0 = Clock::now();
        auto nextTick = t0 + std::chrono::seconds(1);
        CGrabResultPtr res;

        while (cameras.IsGrabbing()) {
            const auto now = Clock::now();
            if (std::chrono::duration_cast<std::chrono::seconds>(now - t0).count() >= seconds)
                break;

            if (cameras.RetrieveResult(1000, res, TimeoutHandling_Return) && res.IsValid()) {
                const intptr_t idx = res->GetCameraContext();
                if (idx >= 0 && static_cast<size_t>(idx) < stats.size()) {
                    CamStats& s = stats[idx];
                    if (res->GrabSucceeded()) {
                        s.frames++;
                        s.bytes += res->GetBufferSize();
                        const uint64_t bid = res->GetBlockID();
                        if (s.haveLast && bid > s.lastBlockId + 1) {
                            const uint64_t missing = bid - s.lastBlockId - 1;
                            s.gapEvents++;
                            s.framesLost += missing;
                            s.largestGap = (std::max)(s.largestGap, missing);
                        }
                        s.lastBlockId = bid;
                        s.haveLast = true;
                    } else {
                        s.failed++;
                    }
                }
            }

            if (Clock::now() >= nextTick) {
                nextTick += std::chrono::seconds(1);
                std::printf("  ");
                for (size_t i = 0; i < stats.size(); ++i) {
                    const uint64_t d = stats[i].frames - stats[i].prevFrames;
                    stats[i].prevFrames = stats[i].frames;
                    std::printf("%s:%llu%s ", stats[i].serial.c_str(),
                                static_cast<unsigned long long>(d),
                                stats[i].framesLost ? "!" : "");
                }
                std::printf("\n");
                std::fflush(stdout);
            }
        }

        cameras.StopGrabbing();
        const double elapsed = std::chrono::duration<double>(Clock::now() - t0).count();

        if (!triggerPort.empty()) {
            std::printf("\n    trigger: sending %s\n", stopCmd);
            if (!serial.Configure(pin, 0.0))
                std::printf("    WARNING: stop not confirmed; pin %d may still pulse.\n", pin);
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            serial.Close();
        }

        std::printf("\n=== results after %.1f s ===\n", elapsed);
        std::printf("%-12s %8s %7s %8s %8s %8s  %s\n",
                    "serial", "frames", "fps", "lost", "gaps", "failed", "rate");

        uint64_t totalLost = 0, totalFailed = 0, totalBytes = 0;
        for (size_t i = 0; i < stats.size(); ++i) {
            CamStats& s = stats[i];
            totalLost   += s.framesLost;
            totalFailed += s.failed;
            totalBytes  += s.bytes;
            std::printf("%-12s %8llu %7.2f %8llu %8llu %8llu  %s\n",
                        s.serial.c_str(),
                        static_cast<unsigned long long>(s.frames),
                        s.frames / elapsed,
                        static_cast<unsigned long long>(s.framesLost),
                        static_cast<unsigned long long>(s.gapEvents),
                        static_cast<unsigned long long>(s.failed),
                        HumanRate(s.bytes / elapsed).c_str());
        }
        std::printf("%-12s %8s %7s %8llu %8s %8llu  %s\n", "TOTAL", "", "",
                    static_cast<unsigned long long>(totalLost), "",
                    static_cast<unsigned long long>(totalFailed),
                    HumanRate(totalBytes / elapsed).c_str());

        std::printf("\n--- pylon stream statistics ---\n");
        for (size_t i = 0; i < cameras.GetSize(); ++i) {
            GenApi::INodeMap& sgm = cameras[i].GetStreamGrabberNodeMap();
            const int64_t total  = ReadInt(sgm, "Statistic_Total_Buffer_Count");
            const int64_t fail   = ReadInt(sgm, "Statistic_Failed_Buffer_Count");
            const int64_t missed = ReadInt(sgm, "Statistic_Missed_Frame_Count");
            const int64_t resync = ReadInt(sgm, "Statistic_Resynchronization_Count");
            std::printf("  %-12s total=%lld failed=%lld missed=%lld resync=%lld\n",
                        stats[i].serial.c_str(),
                        static_cast<long long>(total), static_cast<long long>(fail),
                        static_cast<long long>(missed), static_cast<long long>(resync));
            if (fail > 0 || missed > 0 || resync > 0) exitCode = 1;
        }

        for (size_t i = 0; i < cameras.GetSize(); ++i) cameras[i].Close();

        std::printf("\n=== verdict ===\n");
        uint64_t totalFrames = 0;
        for (const CamStats& s : stats) totalFrames += s.frames;

        if (totalFrames == 0) {
            // Never call this PASS: "nothing lost" is vacuous when nothing arrived.
            std::printf("FAIL -- no frames arrived at all.\n");
            if (!triggerPort.empty() || freerun <= 0.0)
                std::printf("Cameras are armed on FrameStart/Line2, so this means no\n"
                            "trigger pulse reached them. Check the board is really\n"
                            "pulsing (scope pin %d / PH15) and that Line2 is wired.\n", pin);
            exitCode = 1;
        } else if (totalLost == 0 && totalFailed == 0 && exitCode == 0) {
            std::printf("PASS -- no frames lost on the wire, no failed grabs.\n");
        } else {
            std::printf("FAIL -- %llu frame(s) lost, %llu failed grab(s).\n",
                        static_cast<unsigned long long>(totalLost),
                        static_cast<unsigned long long>(totalFailed));
            std::printf("Loss here means the USB path cannot hold this rate. Adding\n"
                        "buffers will not help; move a camera to its own controller,\n"
                        "lower DeviceLinkThroughputLimit, or reduce resolution/rate.\n");
            exitCode = 1;
        }
    } catch (const GenericException& e) {
        const char* desc = e.GetDescription();
        std::printf("\npylon exception: %s\n", desc ? desc : "(none)");
        if (desc && std::strstr(desc, "exclusively opened")) {
            std::printf("\nAnother process holds this camera. Close pylon Viewer and any\n"
                        "running campy/python process, then retry:\n"
                        "  Get-Process pylonviewer,python | Stop-Process\n");
        }
        return 1;
    }

    return exitCode;
}

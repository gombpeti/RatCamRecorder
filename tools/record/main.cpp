// record -- six-camera acquisition end to end, without a GUI.
//
// Drives the Arduino trigger, records every attached camera to its own MP4, and
// reports per-camera health afterwards. This is the piece that replaces
// campy-acquire; the Qt front end will drive the same Recorder.
//
//   record --folder "F:/Temp/session1" --seconds 60 --trigger COM4 \
//          --pfs ../../configs/a2A1920-160ucBAS_40606902.pfs

#include <pylon/PylonIncludes.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "core/Recorder.h"
#include "trigger/SerialTrigger.h"

using namespace campy;
using Clock = std::chrono::steady_clock;

namespace {

/// The optional top-view camera: same trigger, but one frame per N pulses,
/// and a different model at a different resolution and pixel format.
bool IsTopView(const std::string& name, const std::string& model,
               const std::string& serial) {
    return name == "CAM0" || serial == "41975154"
        || model.rfind("a2A3536", 0) == 0;
}

void PrintUsage() {
    std::printf(
        "record -- campy-cpp acquisition\n\n"
        "  --folder PATH    output folder; one subfolder per camera (required)\n"
        "  --seconds N      recording duration (default 60)\n"
        "  --pfs PATH       pylon feature file applied to every camera\n"
        "  --trigger COM4   drive the Arduino: 1,<pin>,<fps> then 1,<pin>,0\n"
        "  --pin N          trigger pin (default 39)\n"
        "  --settle MS      wait after start before grabbing (default 9500;\n"
        "                   firmware START_SETTLE_MS is 9000)\n"
        "  --fps F          frame rate (default 30)\n"
        "  --qp N           quality, lower is better (default 21)\n"
        "  --codec NAME     h264_nvenc (default) or libx264\n"
        "  --buffers N      MaxNumBuffer per camera (default 900 = 30 s at 30 Hz)\n"
        "  --ring N         encoder ring per camera (default 0 = 3/4 of buffers)\n\n");
}

} // namespace

int main(int argc, char* argv[]) {
    Recorder::Settings rs;
    std::string pfs, triggerPort, topPfs;
    int seconds = 60, pin = 39, settleMs = 9500;
    int topDivider = 2;
    bool useTopView = true;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* what) -> std::string {
            if (i + 1 >= argc) { std::printf("ERROR: %s needs a value\n", what); std::exit(2); }
            return argv[++i];
        };
        if      (a == "--folder")  rs.videoFolder = next("--folder");
        else if (a == "--seconds") seconds        = std::stoi(next("--seconds"));
        else if (a == "--pfs")     pfs            = next("--pfs");
        else if (a == "--trigger") triggerPort    = next("--trigger");
        else if (a == "--pin")     pin            = std::stoi(next("--pin"));
        else if (a == "--settle")  settleMs       = std::stoi(next("--settle"));
        else if (a == "--fps")     rs.frameRate   = std::stod(next("--fps"));
        else if (a == "--qp")      rs.qp          = std::stoi(next("--qp"));
        else if (a == "--codec")   rs.codec       = next("--codec");
        else if (a == "--buffers") rs.maxNumBuffer= std::stoi(next("--buffers"));
        else if (a == "--ring")    rs.ringCapacity= static_cast<size_t>(std::stoul(next("--ring")));
        else if (a == "--topview")     topPfs     = next("--topview");
        else if (a == "--topview-div") topDivider = std::stoi(next("--topview-div"));
        else if (a == "--no-topview")  useTopView = false;
        else { PrintUsage(); return a == "--help" ? 0 : 2; }
    }

    if (rs.videoFolder.empty()) { PrintUsage(); return 2; }

    // Never silently destroy a previous recording.
    if (std::filesystem::exists(rs.videoFolder) &&
        !std::filesystem::is_empty(rs.videoFolder)) {
        std::printf("ERROR: %s already exists and is not empty.\n"
                    "Recording would overwrite it. Choose another folder.\n",
                    rs.videoFolder.c_str());
        return 2;
    }

    Pylon::PylonAutoInitTerm autoInit;

    try {
        Pylon::DeviceInfoList_t devices;
        Pylon::CTlFactory::GetInstance().EnumerateDevices(devices);
        if (devices.empty()) { std::printf("No cameras found.\n"); return 1; }

        for (size_t i = 0; i < devices.size(); ++i) {
            const std::string model  = devices[i].GetModelName().c_str();
            const std::string serial = devices[i].GetSerialNumber().c_str();
            std::string name = devices[i].GetUserDefinedName().c_str();
            if (name.empty()) name = "CAM" + std::to_string(i + 1);

            Recorder::CameraSpec spec;
            spec.serial  = serial;
            spec.name    = name;
            spec.pfsPath = pfs;

            if (IsTopView(name, model, serial)) {
                if (!useTopView) {
                    std::printf("%s (%s): top view skipped\n",
                                name.c_str(), model.c_str());
                    continue;
                }
                if (topPfs.empty()) {
                    // The main .pfs is for a different model and would fail
                    // every node; refusing is better than a broken camera.
                    std::printf("%s (%s): top view needs its own .pfs "
                                "(--topview PATH) or --no-topview; skipping\n",
                                name.c_str(), model.c_str());
                    continue;
                }
                spec.pfsPath = topPfs;
                spec.triggerDivider = topDivider;
                std::printf("%s (%s): top view, 1 frame per %d triggers "
                            "-> %.4g fps\n", name.c_str(), model.c_str(),
                            topDivider, rs.frameRate / topDivider);
            }
            rs.cameras.push_back(spec);
        }
        std::printf("Recording %u camera(s) for %d s into %s\n\n",
                    static_cast<unsigned>(rs.cameras.size()), seconds,
                    rs.videoFolder.c_str());

        Recorder recorder(rs);
        if (!recorder.Open()) {
            std::printf("\nSetup failed; not recording.\n");
            return 1;
        }

        SerialTrigger trigger;
        trigger.SetStartSettleMs(settleMs);
        if (!triggerPort.empty() && !trigger.Open(triggerPort)) {
            std::printf("\nERROR: %s\n", trigger.LastError().c_str());
            return 1;
        }

        // Arm every camera BEFORE the trigger starts.
        //
        // Ordering matters for cross-camera alignment. Starting the trigger
        // first means each camera joins an already-running pulse train as its
        // grab thread comes up, so frame 0 is a different instant on each one
        // -- which showed up as a 23-frame spread over six cameras. Armed
        // first, all six sit waiting on Line2 and pulse #1 reaches them
        // simultaneously, so frame 0 is the same instant everywhere. Waiting
        // cameras cost nothing: they simply block on the trigger.
        std::printf("\nArming cameras (waiting for the trigger)...\n");
        recorder.Start();
        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        if (!triggerPort.empty()) {
            std::printf("[trigger] starting pin %d at %g fps on %s\n",
                        pin, rs.frameRate, triggerPort.c_str());
            if (!trigger.Start(pin, rs.frameRate)) {
                std::printf("ERROR: %s\n", trigger.LastError().c_str());
                recorder.Stop();
                return 1;
            }
            // The firmware sends its START packet, then waits START_SETTLE_MS
            // before the first pulse. Nothing is captured during that window,
            // so the recording clock starts after it.
            std::printf("[trigger] confirmed; board settles %d ms before the "
                        "first pulse\n", settleMs);
            std::this_thread::sleep_for(std::chrono::milliseconds(settleMs));
        } else {
            std::printf("[trigger] none given -- the board must already be pulsing\n");
        }

        const auto t0 = Clock::now();
        auto nextTick = t0 + std::chrono::seconds(1);
        std::vector<uint64_t> prev(rs.cameras.size(), 0);

        while (std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - t0)
                   .count() < seconds) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if (Clock::now() < nextTick) continue;
            nextTick += std::chrono::seconds(1);

            const auto status = recorder.Status();
            std::printf("  ");
            for (size_t i = 0; i < status.size(); ++i) {
                const uint64_t got = status[i].health.framesReceived;
                const uint64_t rate = got - prev[i];
                prev[i] = got;
                // Ring occupancy is the early warning: a rising percentage
                // means the encoder is losing ground before any frame is lost.
                const size_t pct = status[i].ring.capacity
                    ? status[i].ring.size * 100 / status[i].ring.capacity : 0;
                std::printf("%s:%llu", status[i].name.c_str(),
                            static_cast<unsigned long long>(rate));
                if (pct > 10) std::printf("(buf%zu%%)", pct);
                if (status[i].health.framesLost || status[i].health.framesDropped)
                    std::printf("!");
                std::printf(" ");
            }
            std::printf("\n");
            std::fflush(stdout);
        }

        // 1. Stop the trigger FIRST. Finalizing the videos takes minutes on a
        //    long session, and the Arduino must not keep pulsing cameras that
        //    nothing is reading any more.
        if (!triggerPort.empty()) {
            std::printf("\n[trigger] stopping pin %d\n", pin);
            if (!trigger.Stop(pin))
                std::printf("WARNING: %s\n", trigger.LastError().c_str());
            trigger.Close();
        }

        // 2. Collect the frames already in flight before shutting the cameras
        //    down, so none that were genuinely captured are discarded.
        {
            const uint64_t before = recorder.TotalFramesReceived();
            std::printf("Waiting for the last frames to arrive...\n");
            std::fflush(stdout);
            const bool quiet = recorder.WaitUntilQuiet(1000, 15000);
            const uint64_t after = recorder.TotalFramesReceived();
            if (!quiet)
                std::printf("WARNING: frames still arriving after 15 s -- is the "
                            "trigger really stopped? Finalizing anyway.\n");
            else
                std::printf("Cameras quiet (%llu frame(s) arrived after the "
                            "trigger stopped).\n",
                            static_cast<unsigned long long>(after - before));
            std::fflush(stdout);
        }

        // 3. Only now drain the encoders and finalize.
        std::printf("\nDraining buffers and finalizing files...\n");
        recorder.Stop();

        // After Stop() the per-frame records are final, so metadata can be
        // written from them.
        std::printf("\nWriting metadata...\n");
        recorder.WriteMetadata();

        const double elapsed = std::chrono::duration<double>(Clock::now() - t0).count();
        std::printf("\n=== results after %.1f s ===\n", elapsed);
        std::printf("%-8s %-12s %9s %9s %8s %8s %7s %10s %10s\n",
                    "camera", "serial", "received", "encoded", "lost",
                    "dropped", "peak%", "firstBlk", "lastBlk");

        uint64_t totalLost = 0, totalDropped = 0;
        const auto status = recorder.Status();
        for (const auto& s : status) {
            totalLost += s.health.framesLost;
            totalDropped += s.health.framesDropped;
            const size_t peakPct = s.ring.capacity
                ? s.ring.peak * 100 / s.ring.capacity : 0;
            std::printf("%-8s %-12s %9llu %9llu %8llu %8llu %6zu%% %10llu %10llu %s%s\n",
                        s.name.c_str(), s.serial.c_str(),
                        static_cast<unsigned long long>(s.health.framesReceived),
                        static_cast<unsigned long long>(s.framesEncoded),
                        static_cast<unsigned long long>(s.health.framesLost),
                        static_cast<unsigned long long>(s.health.framesDropped),
                        peakPct,
                        static_cast<unsigned long long>(s.health.firstBlockId),
                        static_cast<unsigned long long>(s.health.lastBlockId),
                        s.failed ? "FAILED " : "",
                        s.error.empty() ? "" : s.error.c_str());
        }

        // With a shared trigger line every camera numbers the same pulse
        // identically, so matching first/last BlockIDs prove the six videos are
        // frame-aligned. Differing ones say exactly how far apart they start.
        if (status.size() > 1) {
            uint64_t lo = status[0].health.firstBlockId, hi = lo;
            for (const auto& s : status) {
                lo = (std::min)(lo, s.health.firstBlockId);
                hi = (std::max)(hi, s.health.firstBlockId);
            }
            if (lo == hi)
                std::printf("\nAlignment: all cameras start at BlockID %llu -- "
                            "frame 0 is the same trigger pulse everywhere.\n",
                            static_cast<unsigned long long>(lo));
            else
                std::printf("\nAlignment: first BlockID spans %llu..%llu, so the "
                            "videos are offset by up to %llu frame(s).\n",
                            static_cast<unsigned long long>(lo),
                            static_cast<unsigned long long>(hi),
                            static_cast<unsigned long long>(hi - lo));
        }

        std::printf("\n=== verdict ===\n");
        if (recorder.AllHealthy() && totalLost == 0 && totalDropped == 0) {
            std::printf("PASS -- nothing lost on the wire, nothing dropped, "
                        "every file finalized.\n");
            return 0;
        }
        std::printf("FAIL -- %llu lost on the wire, %llu dropped at the encoder.\n",
                    static_cast<unsigned long long>(totalLost),
                    static_cast<unsigned long long>(totalDropped));
        if (totalLost)
            std::printf("  Wire loss is a USB problem: more buffers will not help.\n");
        if (totalDropped)
            std::printf("  Encoder drops mean compression could not keep up.\n");
        return 1;

    } catch (const Pylon::GenericException& e) {
        std::printf("\npylon exception: %s\n", e.GetDescription());
        return 1;
    }
}

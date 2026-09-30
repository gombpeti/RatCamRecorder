#include "core/Recorder.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <map>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <thread>
#include <vector>

using namespace Pylon;

namespace campy {

namespace {

/// Name the video after its camera instead of "0.mp4" everywhere.
/// CAM1 -> 1.mp4, CAM2 -> 2.mp4. Six identically named files is a real hazard
/// once they are copied out of their folders for analysis or sent to someone.
/// Falls back to the whole camera name when it carries no trailing number.
std::string VideoFileNameFor(const std::string& cameraName,
                             const std::string& templateName) {
    std::string ext = ".mp4";
    const size_t dot = templateName.rfind('.');
    if (dot != std::string::npos) ext = templateName.substr(dot);

    size_t i = cameraName.size();
    while (i > 0 && std::isdigit(static_cast<unsigned char>(cameraName[i - 1]))) --i;
    const std::string digits = cameraName.substr(i);

    return (digits.empty() ? cameraName : digits) + ext;
}

} // namespace

Recorder::Recorder(Settings settings) : settings_(std::move(settings)) {}

Recorder::~Recorder() { Stop(); }

bool Recorder::Open() {
    CTlFactory& factory = CTlFactory::GetInstance();
    DeviceInfoList_t devices;
    factory.EnumerateDevices(devices);

    // Give each CPU encoder a fair slice. libx264 left to its own devices picks
    // roughly one thread per core *each*, so six encoders would ask for ~42
    // threads on a 16-core machine and spend their time context switching.
    int encThreads = settings_.encoderThreads;
    if (encThreads <= 0) {
        const unsigned cores = std::max(1u, std::thread::hardware_concurrency());
        const size_t n = std::max<size_t>(1, settings_.cameras.size());
        // Leave headroom for the grab threads, which must never be starved:
        // a missed USB transfer window cannot be recovered, a slow encode can.
        encThreads = std::max(1, static_cast<int>((cores * 3 / 4) / n));
    }

    // Buffer depth, in frames, capped so the total allocation stays inside
    // the budget. Deep buffers are the only defence against a host stall: a
    // frame dropped here is gone for good, and the alternative -- stalling the
    // grab thread -- loses it on the wire instead, which is worse.
    {
        int maxBuffers = settings_.maxNumBuffer;
        const size_t nCams = std::max<size_t>(1, settings_.cameras.size());
        // Assume the worst case of 3 bytes/pixel: the real pixel format is not
        // known until the camera is open, and over-estimating only costs depth.
        const double frameMB = double(settings_.width) * settings_.height * 3
                             / (1024.0 * 1024.0);
        if (settings_.bufferBudgetMB > 0 &&
            frameMB * maxBuffers * double(nCams) > settings_.bufferBudgetMB) {
            const int capped = static_cast<int>(settings_.bufferBudgetMB
                                                / (frameMB * double(nCams)));
            maxBuffers = (std::max)(30, capped);
            std::printf("buffers capped to %d per camera to stay within %d MB "
                        "(asked for %d)\n",
                        maxBuffers, settings_.bufferBudgetMB,
                        settings_.maxNumBuffer);
        }
        maxNumBufferUsed_ = maxBuffers;
        ringCapacity_ = settings_.ringCapacity
                      ? settings_.ringCapacity
                      : static_cast<size_t>(maxBuffers) * 3 / 4;
        const double secs = settings_.frameRate > 0
                          ? ringCapacity_ / settings_.frameRate : 0.0;
        std::printf("buffers %d/camera, encoder ring %zu frames (%.1f s of "
                    "slack), about %.1f GB total\n",
                    maxBuffers, ringCapacity_, secs,
                    frameMB * maxBuffers * double(nCams) / 1024.0);
        std::fflush(stdout);
    }

    bool ok = true;
    for (const CameraSpec& spec : settings_.cameras) {
        auto channel = std::make_unique<Channel>();

        CameraWorker::Settings cs;
        cs.serial        = spec.serial;
        cs.name          = spec.name;
        cs.pfsPath       = spec.pfsPath;
        cs.maxNumBuffer  = maxNumBufferUsed_;
        cs.ringCapacity  = ringCapacity_;
        channel->camera  = std::make_unique<CameraWorker>(cs);
        if (settings_.previewFps > 0)
            channel->camera->EnablePreview(settings_.previewFps,
                                           settings_.previewWidth);

        IPylonDevice* device = nullptr;
        for (size_t i = 0; i < devices.size(); ++i) {
            if (spec.serial == devices[i].GetSerialNumber().c_str()) {
                device = factory.CreateDevice(devices[i]);
                break;
            }
        }
        if (!device) {
            std::printf("[%s] serial %s not found among %u attached camera(s)\n",
                        spec.name.c_str(), spec.serial.c_str(),
                        static_cast<unsigned>(devices.size()));
            ok = false;
            channels_.push_back(std::move(channel));
            continue;
        }

        if (!channel->camera->Open(device)) {
            std::printf("[%s] open failed: %s\n", spec.name.c_str(),
                        channel->camera->LastError().c_str());
            ok = false;
            channels_.push_back(std::move(channel));
            continue;
        }

        const auto cfg = channel->camera->ReadBackConfig();
        std::printf("[%s] %s  %lldx%lld %s  trigger %s", spec.name.c_str(),
                    spec.serial.c_str(),
                    static_cast<long long>(cfg.width),
                    static_cast<long long>(cfg.height),
                    cfg.pixelFormat.c_str(), cfg.triggerMode.c_str());
        if (cfg.triggerMode == "On")
            std::printf(" src=%s act=%s", cfg.triggerSource.c_str(),
                        cfg.triggerActivation.c_str());
        std::printf("  link=%s\n", cfg.linkSpeedMode.c_str());
        if (cfg.linkSpeedMode == "HighSpeed") {
            std::printf("[%s] WARNING: USB 2.0 link, cannot sustain full rate\n",
                        spec.name.c_str());
            ok = false;
        }

        const std::filesystem::path folder =
            std::filesystem::path(settings_.videoFolder) / spec.name;
        std::error_code ec;
        std::filesystem::create_directories(folder, ec);

        // Take resolution and pixel format from the camera rather than from
        // the session settings. The top-view camera is 2966x2974 BayerRG8
        // while the others are 1920x1200 RGB8, and a session-wide value would
        // be wrong for one of them whichever way it was set.
        const int divider = spec.triggerDivider > 0 ? spec.triggerDivider : 1;
        const double camFps = settings_.frameRate / divider;
        channel->frameRate = camFps;

        VideoEncoder::Settings es;
        es.path       = (folder / VideoFileNameFor(spec.name,
                                                    settings_.videoFilename)).string();
        es.width      = cfg.width  > 0 ? static_cast<int>(cfg.width)  : settings_.width;
        es.height     = cfg.height > 0 ? static_cast<int>(cfg.height) : settings_.height;
        es.frameRate  = camFps;
        es.cameraPixelFormat = cfg.pixelFormat.empty() ? "RGB8" : cfg.pixelFormat;
        es.qp         = settings_.qp;
        es.codec      = settings_.codec;
        es.gpuId      = settings_.gpuId;
        es.preset     = settings_.preset;
        es.threads    = encThreads;

        channel->encoder = std::make_unique<VideoEncoder>();
        if (!channel->encoder->Open(es)) {
            std::printf("[%s] encoder failed: %s\n", spec.name.c_str(),
                        channel->encoder->LastError().c_str());
            ok = false;
        }

        channels_.push_back(std::move(channel));
    }
    return ok;
}

bool Recorder::Start() {
    if (running_.exchange(true)) return false;
    for (size_t i = 0; i < channels_.size(); ++i) {
        Channel& ch = *channels_[i];
        if (!ch.camera || !ch.encoder || !ch.encoder->IsOpen()) continue;
        // Encode thread first, so it is already waiting when frames arrive.
        ch.thread = std::thread(&Recorder::EncodeLoop, this, i);
        ch.camera->Start();
    }
    watchdog_ = std::thread(&Recorder::WatchdogLoop, this);
    return true;
}

void Recorder::WatchdogLoop() {
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    // A frame arrives every 33 ms, so anything past a second is already a
    // hundred frames of backlog. Report once per episode, not once per tick.
    const uint64_t warnMs = 1000;
    std::vector<bool> reported(channels_.size(), false);

    while (running_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        for (size_t i = 0; i < channels_.size(); ++i) {
            Channel& ch = *channels_[i];
            if (!ch.encoder) continue;
            const uint64_t ms = ch.encoder->PhaseElapsedMs();
            if (ms < warnMs) {
                if (reported[i]) {
                    std::printf("[%s] encoder recovered after %llu ms\n",
                                ch.camera ? ch.camera->Name().c_str() : "?",
                                static_cast<unsigned long long>(
                                    ch.worstStallMs.load()));
                    std::fflush(stdout);
                    reported[i] = false;
                }
                continue;
            }
            const auto phase = ch.encoder->CurrentPhase();
            if (ms > ch.worstStallMs.load()) {
                ch.worstStallMs.store(ms);
                ch.worstStallPhase.store(static_cast<int>(phase));
                ch.worstStallAtSec.store(
                    std::chrono::duration<double>(clock::now() - t0).count()
                    - double(ms) / 1000.0);
            }
            if (!reported[i]) {
                reported[i] = true;
                std::printf("[%s] ENCODER STALLED %llu ms in %s -- frames are "
                            "piling up in the ring and will be dropped when it "
                            "fills\n",
                            ch.camera ? ch.camera->Name().c_str() : "?",
                            static_cast<unsigned long long>(ms),
                            VideoEncoder::PhaseName(phase));
                std::fflush(stdout);
            }
        }
    }
}

void Recorder::EncodeLoop(size_t index) {
    Channel& ch = *channels_[index];
    RingBuffer<Frame>& ring = ch.camera->Ring();

    for (;;) {
        auto frame = ring.Pop(std::chrono::milliseconds(200));
        if (!frame) {
            // Only finish once the producer has closed AND the ring is empty;
            // this is what makes Stop drain rather than truncate.
            if (ring.IsDrained()) break;
            continue;
        }
        if (!frame->Valid()) continue;

        if (!ch.encoder->WriteFrame(frame->Pixels(), frame->Bytes())) {
            if (!ch.encodeFailed.exchange(true)) {
                ch.encodeError = ch.encoder->LastError();
                std::printf("[%s] encode failed: %s\n",
                            ch.camera->Name().c_str(), ch.encodeError.c_str());
                std::fflush(stdout);
            }
            continue;
        }
        ch.framesEncoded.fetch_add(1, std::memory_order_relaxed);
    }

    // Finalize here rather than on the main thread: six files close in
    // parallel, and a camera that failed early still gets a valid trailer.
    ch.encoder->Finish();
}

uint64_t Recorder::TotalFramesReceived() const {
    uint64_t total = 0;
    for (const auto& ch : channels_)
        if (ch->camera) total += ch->camera->Health().framesReceived;
    return total;
}

bool Recorder::WaitUntilQuiet(int quietMs, int timeoutMs,
                              const std::function<void(uint64_t, int)>& onTick) {
    using clock = std::chrono::steady_clock;
    const auto start = clock::now();
    auto lastChange = start;
    uint64_t last = TotalFramesReceived();

    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const uint64_t now = TotalFramesReceived();
        const auto t = clock::now();
        if (now != last) { last = now; lastChange = t; }

        const int quietFor = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(t - lastChange).count());
        const int elapsed = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(t - start).count());

        if (onTick) onTick(now, quietFor);
        if (quietFor >= quietMs) return true;
        if (elapsed >= timeoutMs) return false;
    }
}

void Recorder::Stop() {
    if (!running_.exchange(false)) return;

    // Signal every camera before joining any of them. CameraWorker::Stop()
    // blocks on its grab thread, so stopping them one at a time lets each
    // later camera keep capturing while the earlier ones wind down -- which
    // showed up as a 14-frame spread in where the six videos ended. Raising
    // the flag on all six first bounds that to the jitter of one loop.
    for (auto& ch : channels_)
        if (ch->camera) ch->camera->RequestStop();

    for (auto& ch : channels_)
        if (ch->camera) ch->camera->Stop();

    if (watchdog_.joinable()) watchdog_.join();

    for (auto& ch : channels_)
        if (ch->thread.joinable()) ch->thread.join();
}

std::vector<Recorder::CameraStatus> Recorder::Status() const {
    std::vector<CameraStatus> out;
    out.reserve(channels_.size());
    for (const auto& ch : channels_) {
        CameraStatus s;
        if (ch->camera) {
            s.name   = ch->camera->Name();
            s.serial = ch->camera->Serial();
            s.health = ch->camera->Health();
            s.ring   = ch->camera->Ring().Stats();
            s.failed = ch->camera->Failed();
            s.error  = ch->camera->LastError();
        }
        s.framesEncoded = ch->framesEncoded.load();
        if (ch->encoder) {
            s.encoderStallMs = ch->encoder->PhaseElapsedMs();
            s.encoderPhase = VideoEncoder::PhaseName(ch->encoder->CurrentPhase());
        }
        s.worstStallMs = ch->worstStallMs.load();
        s.worstStallPhase = VideoEncoder::PhaseName(
            static_cast<VideoEncoder::Phase>(ch->worstStallPhase.load()));
        if (ch->encodeFailed.load()) {
            s.failed = true;
            if (s.error.empty()) s.error = ch->encodeError;
        }
        out.push_back(std::move(s));
    }
    return out;
}

bool Recorder::TakePreview(size_t index, std::vector<uint8_t>& rgb,
                           int& w, int& h, uint64_t& lastSerial) {
    if (index >= channels_.size() || !channels_[index]->camera) return false;
    return channels_[index]->camera->TakePreview(rgb, w, h, lastSerial);
}

std::string Recorder::CameraName(size_t index) const {
    if (index >= channels_.size() || !channels_[index]->camera) return std::string();
    return channels_[index]->camera->Name();
}

std::vector<CameraReport> Recorder::WriteMetadata() {
    std::vector<CameraReport> reports;
    for (const auto& ch : channels_) {
        if (!ch->camera) continue;

        const auto& recs = ch->camera->Records();
        std::vector<FrameTime> frames;
        frames.reserve(recs.size());
        for (const auto& r : recs) frames.push_back(FrameTime{r.blockId, r.timestampNs, r.inVideo});

        const std::filesystem::path folder =
            std::filesystem::path(settings_.videoFolder) / ch->camera->Name();

        // Mirror campy's metadata.csv keys so existing readers keep working.
        MetadataWriter::Meta meta = {
            {"videoFolder", settings_.videoFolder},
            {"videoFilename", VideoFileNameFor(ch->camera->Name(),
                                              settings_.videoFilename)},
            {"frameRate", std::to_string(ch->frameRate)},
            {"sessionFrameRate", std::to_string(settings_.frameRate)},
            {"numCams", std::to_string(settings_.cameras.size())},
            {"cameraNames", ch->camera->Name()},
            {"cameraSerialNo", ch->camera->Serial()},
            {"cameraMake", "basler"},
            {"frameWidth", std::to_string(settings_.width)},
            {"frameHeight", std::to_string(settings_.height)},
            {"codec", ch->encoder ? ch->encoder->CodecName() : settings_.codec},
            {"quality", std::to_string(settings_.qp)},
            {"gpuID", std::to_string(settings_.gpuId)},
            {"bufferSize", std::to_string(maxNumBufferUsed_)},
            {"ringCapacity", std::to_string(ringCapacity_)},
            {"framesDropped", std::to_string(ch->camera->Health().framesDropped)},
            {"framesEncoded", std::to_string(ch->framesEncoded.load())},
            {"worstEncoderStallMs", std::to_string(ch->worstStallMs.load())},
            {"worstEncoderStallPhase", VideoEncoder::PhaseName(
                 static_cast<VideoEncoder::Phase>(ch->worstStallPhase.load()))},
            {"maxConvertMs", ch->encoder
                 ? std::to_string(ch->encoder->GetTimings().maxConvertMs) : "0"},
            {"maxEncodeMs", ch->encoder
                 ? std::to_string(ch->encoder->GetTimings().maxEncodeMs) : "0"},
            {"maxMuxMs", ch->encoder
                 ? std::to_string(ch->encoder->GetTimings().maxMuxMs) : "0"},
            {"software", "RatCam Recorder"},
        };

        std::string err;
        CameraReport rep = MetadataWriter::WriteCamera(
            folder.string(), ch->camera->Name(), ch->camera->Serial(),
            frames, ch->frameRate, meta, &err);
        if (!err.empty()) {
            std::printf("[%s] metadata: %s\n", ch->camera->Name().c_str(), err.c_str());
            std::fflush(stdout);
        } else {
            // Report the number that matches the file on disk. Printing the
            // received count here read as confirmation that every frame was
            // saved, which is exactly the thing that was not true.
            std::printf("[%s] wrote frametimes.npy/.mat, blockids.npy, "
                        "metadata.csv, frameinfo.csv, dropped.csv, gaps.csv "
                        "(%llu frames in video, %llu dropped, %llu missing)\n",
                        ch->camera->Name().c_str(),
                        static_cast<unsigned long long>(rep.framesInVideo),
                        static_cast<unsigned long long>(rep.framesDropped),
                        static_cast<unsigned long long>(rep.framesMissing));
            std::fflush(stdout);
        }
        reports.push_back(std::move(rep));
    }

    // Second pass for the notes: a camera that stopped early can only be
    // recognised by comparing it against how far the others got.
    // Only compare BlockIDs between cameras running at the same rate. The
    // top-view camera captures one frame per two triggers, so its last
    // BlockID is legitimately about half -- comparing it against the 30 Hz
    // cameras would report every session as "stopped early".
    uint64_t sessionLastBlockId = 0;
    {
        // The rate shared by the most cameras is the session's main rate.
        std::map<int, int> rateCounts;
        for (const auto& ch2 : channels_)
            if (ch2->camera) rateCounts[int(ch2->frameRate * 100 + 0.5)]++;
        int mainRateKey = 0, best = 0;
        for (const auto& kv : rateCounts)
            if (kv.second > best) { best = kv.second; mainRateKey = kv.first; }

        size_t j = 0;
        for (const auto& ch2 : channels_) {
            if (!ch2->camera) continue;
            if (j < reports.size() &&
                int(ch2->frameRate * 100 + 0.5) == mainRateKey)
                sessionLastBlockId = (std::max)(sessionLastBlockId,
                                                reports[j].lastBlockId);
            ++j;
        }
    }

    size_t idx = 0;
    for (const auto& ch : channels_) {
        if (!ch->camera) continue;
        if (idx >= reports.size()) break;

        const auto& recs = ch->camera->Records();
        std::vector<FrameTime> frames;
        frames.reserve(recs.size());
        for (const auto& r : recs) frames.push_back(FrameTime{r.blockId, r.timestampNs, r.inVideo});

        const std::filesystem::path folder =
            std::filesystem::path(settings_.videoFolder) / ch->camera->Name();

        const bool sameRate =
            reports.size() > idx && reports[idx].nominalFps > 0 &&
            std::fabs(ch->frameRate - reports[idx].nominalFps) < 0.01 &&
            std::fabs(ch->frameRate - settings_.frameRate) < 0.01;

        std::string noteErr;
        if (!MetadataWriter::WriteCameraNote(folder.string(), reports[idx],
                                             ch->frameRate, frames,
                                             sameRate ? sessionLastBlockId : 0,
                                             &noteErr))
            std::printf("[%s] note: %s\n", ch->camera->Name().c_str(),
                        noteErr.c_str());

        if (sameRate && sessionLastBlockId > 0 &&
            reports[idx].lastBlockId < sessionLastBlockId)
            std::printf("[%s] STOPPED EARLY: last BlockID %llu vs %llu elsewhere "
                        "-- %llu frame(s) missing from the end\n",
                        ch->camera->Name().c_str(),
                        (unsigned long long)reports[idx].lastBlockId,
                        (unsigned long long)sessionLastBlockId,
                        (unsigned long long)(sessionLastBlockId -
                                             reports[idx].lastBlockId));
        ++idx;
    }

    if (!reports.empty()) {
        std::string err;
        if (MetadataWriter::WriteSessionReport(settings_.videoFolder, reports,
                                               settings_.frameRate, &err))
            std::printf("wrote session_report.txt\n");
        else
            std::printf("session_report.txt: %s\n", err.c_str());
        std::fflush(stdout);
    }
    return reports;
}

uint64_t Recorder::MinFramesReceived() const {
    // Count only cameras that are still alive. A camera that has been
    // unplugged stops counting, and including it would freeze the minimum
    // forever -- the session would never reach its target and would wait for
    // someone to press Stop. The survivors should finish the run normally.
    //
    // When nothing has failed, which is every ordinary recording, this is
    // exactly the minimum over all cameras as before.
    uint64_t lowest = UINT64_MAX;
    bool any = false;
    for (const auto& ch : channels_) {
        if (!ch->camera || ch->camera->Failed()) continue;
        any = true;
        lowest = (std::min)(lowest, ch->camera->Health().framesReceived);
    }
    if (any) return lowest;

    // Every camera has failed. Report the best anyone managed, so the caller
    // sees a number rather than a misleading zero.
    uint64_t best = 0;
    for (const auto& ch : channels_)
        if (ch->camera) best = (std::max)(best, ch->camera->Health().framesReceived);
    return best;
}

double Recorder::MinRecordedSeconds() const {
    double lowest = 1e18;
    bool any = false;
    for (const auto& ch : channels_) {
        if (!ch->camera || ch->camera->Failed()) continue;
        const double rate = ch->frameRate > 0 ? ch->frameRate : 1.0;
        lowest = (std::min)(lowest,
                            double(ch->camera->Health().framesReceived) / rate);
        any = true;
    }
    return any ? lowest : 0.0;
}

bool Recorder::AnyCameraFailed() const {
    for (const auto& ch : channels_)
        if (ch->camera && ch->camera->Failed()) return true;
    return false;
}

bool Recorder::AllHealthy() const {
    for (const auto& s : Status())
        if (s.failed || !s.health.Healthy()) return false;
    return true;
}

} // namespace campy

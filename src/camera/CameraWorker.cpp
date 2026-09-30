#include "camera/CameraWorker.h"

#include <pylon/ParameterIncludes.h>

#include <cstdio>

using namespace Pylon;

namespace campy {
namespace {

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

void TrySetInt(GenApi::INodeMap& nm, const char* name, int64_t v) {
    try {
        CIntegerParameter p(nm, name);
        if (p.IsWritable()) p.SetValue(v, IntegerValueCorrection_Nearest);
    } catch (const GenericException&) {}
}

} // namespace

CameraWorker::CameraWorker(Settings settings)
    : settings_(std::move(settings)), ring_(settings_.ringCapacity) {}

CameraWorker::~CameraWorker() {
    Stop();
    try {
        if (camera_.IsOpen()) camera_.Close();
    } catch (const GenericException&) {}
}

bool CameraWorker::Open(IPylonDevice* device) {
    try {
        camera_.Attach(device);
        camera_.Open();

        GenApi::INodeMap& nm = camera_.GetNodeMap();

        if (!settings_.pfsPath.empty()) {
            // Validation off, as campy does (basler.py:53). The .pfs sets
            // TriggerActivation=RisingEdge for the *Active selectors, which
            // accept only LevelHigh/LevelLow; the camera coerces them and
            // strict validation then rejects the whole load. FrameStart, the
            // selector that matters, applies correctly -- ReadBackConfig()
            // verifies that rather than assuming it.
            CFeaturePersistence::Load(settings_.pfsPath.c_str(), &nm, false);
        }

        camera_.MaxNumBuffer.SetValue(settings_.maxNumBuffer);

        // USB transport layer defaults are low for 207 MB/s per camera.
        GenApi::INodeMap& sgm = camera_.GetStreamGrabberNodeMap();
        TrySetInt(sgm, "MaxTransferSize", 4 * 1024 * 1024);
        TrySetInt(sgm, "NumMaxQueuedUrbs", 64);

        // The preview samples raw pixels, so it must know their layout. Read
        // it after the .pfs has been applied, not before.
        const std::string pf = ReadEnum(nm, "PixelFormat");
        srcIsBayer_ = pf.rfind("Bayer", 0) == 0;
        srcBytesPerPixel_ = (pf == "RGB8" || pf == "BGR8") ? 3 : 1;

        return true;
    } catch (const GenericException& e) {
        lastError_ = e.GetDescription() ? e.GetDescription() : "unknown pylon error";

        // A wall of "node not found" almost always means the .pfs belongs to a
        // different camera model, not that anything is broken. Say so, because
        // the node names alone do not point anywhere useful.
        if (!settings_.pfsPath.empty() &&
            lastError_.find("not found") != std::string::npos) {
            std::string model;
            try { model = camera_.GetDeviceInfo().GetModelName().c_str(); }
            catch (const GenericException&) {}
            std::printf("[%s] the settings file does not match this camera.\n"
                        "      camera: %s\n"
                        "      .pfs  : %s\n"
                        "      Features below exist on other Basler models but "
                        "not this one, so the file is\n"
                        "      almost certainly for a different model. Pick the "
                        ".pfs for %s.\n",
                        settings_.name.c_str(),
                        model.empty() ? "(unknown)" : model.c_str(),
                        settings_.pfsPath.c_str(),
                        model.empty() ? "this camera" : model.c_str());
            std::fflush(stdout);
        }

        failed_ = true;
        return false;
    }
}

CameraWorker::Configured CameraWorker::ReadBackConfig() {
    Configured c;
    try {
        GenApi::INodeMap& nm = camera_.GetNodeMap();
        c.width           = ReadInt(nm, "Width");
        c.height          = ReadInt(nm, "Height");
        c.pixelFormat     = ReadEnum(nm, "PixelFormat");
        c.linkSpeedMode   = ReadEnum(nm, "BslUSBSpeedMode");
        c.throughputLimit = ReadInt(nm, "DeviceLinkThroughputLimit");

        CEnumParameter(nm, "TriggerSelector").TrySetValue("FrameStart");
        c.triggerMode       = ReadEnum(nm, "TriggerMode");
        c.triggerSource     = ReadEnum(nm, "TriggerSource");
        c.triggerActivation = ReadEnum(nm, "TriggerActivation");
    } catch (const GenericException&) {}
    return c;
}

void CameraWorker::Start() {
    if (thread_.joinable()) return;
    stop_ = false;
    thread_ = std::thread(&CameraWorker::GrabLoop, this);
}

void CameraWorker::Stop() {
    if (!thread_.joinable()) return;
    stop_ = true;
    thread_.join();
    // Closing after the grab thread has exited means every frame it queued is
    // still readable: the encoder drains rather than losing the tail. This is
    // what makes a Stop finish the file instead of truncating it.
    ring_.Close();
}

void CameraWorker::GrabLoop() {
    try {
        camera_.StartGrabbing(GrabStrategy_OneByOne);
    } catch (const GenericException& e) {
        lastError_ = e.GetDescription() ? e.GetDescription() : "StartGrabbing failed";
        failed_ = true;
        ring_.Close();
        return;
    }

    // One allocation up front rather than growing during acquisition.
    records_.reserve(4096);

    CGrabResultPtr res;
    while (!stop_.load()) {
        try {
            if (!camera_.RetrieveResult(settings_.grabTimeoutMs, res,
                                        TimeoutHandling_Return)) {
                ++health_.timeouts;
                // A timeout is normal while waiting on a hardware trigger. A
                // timeout with the device gone is not.
                if (camera_.IsCameraDeviceRemoved()) {
                    lastError_ = "camera was disconnected";
                    failed_ = true;
                    break;
                }
                continue;
            }

            if (!res.IsValid()) continue;

            if (!res->GrabSucceeded()) {
                ++health_.failedGrabs;
                std::printf("[%s] grab failed: 0x%08x %s\n",
                            settings_.name.c_str(),
                            static_cast<unsigned>(res->GetErrorCode()),
                            res->GetErrorDescription().c_str());
                std::fflush(stdout);
                continue;
            }

            Frame f;
            f.result      = res;
            f.blockId     = res->GetBlockID();
            f.timestampNs = res->GetTimeStamp();
            f.index       = nextIndex_++;
            f.width       = static_cast<uint32_t>(res->GetWidth());
            f.height      = static_cast<uint32_t>(res->GetHeight());

            // Ground truth for loss: a gap in the camera's own block ids means
            // frames never reached the host at all.
            if (!haveLastBlockId_) health_.firstBlockId = f.blockId;
            health_.lastBlockId = f.blockId;

            if (haveLastBlockId_ && f.blockId > lastBlockId_ + 1) {
                const uint64_t missing = f.blockId - lastBlockId_ - 1;
                ++health_.gapEvents;
                health_.framesLost += missing;
                if (missing > health_.largestGap) health_.largestGap = missing;
                std::printf("[%s] LOST %llu frame(s) on the wire "
                            "(blockId %llu -> %llu)\n",
                            settings_.name.c_str(),
                            static_cast<unsigned long long>(missing),
                            static_cast<unsigned long long>(lastBlockId_),
                            static_cast<unsigned long long>(f.blockId));
                std::fflush(stdout);
            }
            lastBlockId_ = f.blockId;
            haveLastBlockId_ = true;
            ++health_.framesReceived;

            // ~24 bytes per frame: a 45 min session at 30 Hz is about 2 MB.
            records_.push_back(FrameRecord{f.blockId, f.timestampNs, true});

            // Preview is fed from the frame we are about to hand to the
            // encoder, before the move, and only every Nth frame.
            if (previewEvery_ > 0 && (f.index % previewEvery_) == 0) {
                UpdatePreview(static_cast<const uint8_t*>(res->GetBuffer()),
                              static_cast<int>(res->GetWidth()),
                              static_cast<int>(res->GetHeight()));
            }

            if (!ring_.Push(std::move(f))) {
                // Encoder is behind. Dropping here is deliberate: stalling the
                // grab thread would cost USB transfer windows, which cannot be
                // recovered, whereas a dropped frame is at least countable.
                ++health_.framesDropped;
                records_.back().inVideo = false;   // and identifiable afterwards
            }
        } catch (const GenericException& e) {
            ++health_.failedGrabs;
            lastError_ = e.GetDescription() ? e.GetDescription() : "grab exception";
            if (camera_.IsCameraDeviceRemoved()) { failed_ = true; break; }
        }
    }

    try {
        if (camera_.IsGrabbing()) camera_.StopGrabbing();
    } catch (const GenericException&) {}

    // If we exited on failure rather than Stop(), the encoder still needs to
    // be told to finish so its file gets a valid trailer.
    if (failed_.load()) ring_.Close();
}

void CameraWorker::EnablePreview(int targetFps, int maxWidth) {
    // Frame rate is not known here, so express the throttle as "every Nth
    // frame" the way campy does (frameNumber % frameRatio). At 30 Hz capture
    // and 10 Hz preview that is every 3rd frame.
    previewEvery_ = targetFps > 0 ? (30 / targetFps) : 0;
    if (previewEvery_ < 1) previewEvery_ = 1;
    previewMaxWidth_ = maxWidth > 0 ? maxWidth : 480;
}

bool CameraWorker::TakePreview(std::vector<uint8_t>& rgb, int& w, int& h,
                               uint64_t& lastSerial) {
    std::lock_guard<std::mutex> lock(previewMutex_);
    if (previewSerial_ == lastSerial || previewW_ == 0) return false;
    rgb = previewRgb_;
    w = previewW_;
    h = previewH_;
    lastSerial = previewSerial_;
    return true;
}

/// Point-sample the frame down to at most previewMaxWidth_ and store it.
/// Nearest-neighbour on purpose: 480x300 out of 1920x1200 touches 144k pixels
/// instead of 2.3M, so at 10 Hz across six cameras this is a rounding error
/// against the 1.24 GB/s the recording path is already moving.
void CameraWorker::UpdatePreview(const uint8_t* pixels, int srcW, int srcH) {
    if (!pixels || srcW <= 0 || srcH <= 0) return;

    int step = 1;
    while (srcW / step > previewMaxWidth_) ++step;
    // A Bayer source carries colour in 2x2 blocks, so sampling has to move in
    // even steps or the preview picks one colour plane and comes out tinted.
    if (srcIsBayer_ && (step % 2)) ++step;
    const int outW = srcW / step, outH = srcH / step;
    if (outW <= 0 || outH <= 0) return;

    // try_lock: the grab thread must never wait on the GUI. A skipped preview
    // frame is free; a stalled grab thread costs a USB transfer window.
    std::unique_lock<std::mutex> lock(previewMutex_, std::try_to_lock);
    if (!lock.owns_lock()) return;

    previewRgb_.resize(static_cast<size_t>(outW) * outH * 3);
    const size_t rowBytes = static_cast<size_t>(srcW) * srcBytesPerPixel_;

    for (int y = 0; y < outH; ++y) {
        const size_t sy = static_cast<size_t>(y) * step;
        if (sy + 1 >= static_cast<size_t>(srcH)) break;   // never read past the end
        const uint8_t* src = pixels + sy * rowBytes;
        const uint8_t* src1 = src + rowBytes;             // next line, for Bayer
        uint8_t* dst = previewRgb_.data() + static_cast<size_t>(y) * outW * 3;

        for (int x = 0; x < outW; ++x) {
            const size_t sx = static_cast<size_t>(x) * step;
            if (srcBytesPerPixel_ == 3) {
                const uint8_t* sp = src + sx * 3;
                dst[3 * x + 0] = sp[0];
                dst[3 * x + 1] = sp[1];
                dst[3 * x + 2] = sp[2];
            } else if (srcIsBayer_) {
                // RGGB block: R at (x,y), G at (x+1,y), B at (x+1,y+1).
                // Crude but free, and right enough to judge framing by.
                dst[3 * x + 0] = src[sx];
                dst[3 * x + 1] = src[sx + 1];
                dst[3 * x + 2] = src1[sx + 1];
            } else {
                const uint8_t g = src[sx];               // Mono8
                dst[3 * x + 0] = g;
                dst[3 * x + 1] = g;
                dst[3 * x + 2] = g;
            }
        }
    }
    previewW_ = outW;
    previewH_ = outH;
    ++previewSerial_;
}

CameraHealth CameraWorker::Health() const { return health_; }

CameraWorker::StreamStats CameraWorker::ReadStreamStats() {
    StreamStats s;
    try {
        GenApi::INodeMap& sgm = camera_.GetStreamGrabberNodeMap();
        s.total  = ReadInt(sgm, "Statistic_Total_Buffer_Count");
        s.failed = ReadInt(sgm, "Statistic_Failed_Buffer_Count");
        s.missed = ReadInt(sgm, "Statistic_Missed_Frame_Count");
        s.resync = ReadInt(sgm, "Statistic_Resynchronization_Count");
    } catch (const GenericException&) {}
    return s;
}

} // namespace campy

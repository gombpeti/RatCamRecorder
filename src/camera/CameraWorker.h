#pragma once

// One camera: owns its pylon device, its grab thread, and the ring buffer that
// feeds its encoder.
//
// Three things here exist because the Python tool lacks them:
//
//  * BlockID gap detection. Every grab result carries a camera-assigned block
//    id; a gap means frames were lost on the wire. campy counts frames it
//    *received* and reports that as the rate, so a camera delivering 28 of
//    every 30 frames looks healthy and produces a quietly wrong video.
//  * Grab failures are recorded, not swallowed. unicam.py discards the
//    exception unless a debug flag nobody sets is enabled.
//  * Device removal is isolated. One camera disconnecting must not take the
//    other five down with it; its file is finalized and the rest keep running.

#include <atomic>
#include <mutex>
#include <vector>
#include <memory>
#include <string>
#include <thread>

#include <pylon/PylonIncludes.h>

#include "core/Frame.h"
#include "core/RingBuffer.h"

namespace campy {

class CameraWorker {
public:
    struct Settings {
        std::string serial;          // device serial, selects the camera
        std::string name;            // CAM1 etc, used for filenames and logs
        std::string pfsPath;         // optional pylon feature file

        // Deep buffers are the shock absorber: with 125 GB RAM, 300 frames is
        // ~2 GB per camera and ~10 s of slack at 30 Hz.
        int maxNumBuffer = 300;

        // Must stay below maxNumBuffer. Frames are zero-copy, so a queued frame
        // holds one of the camera's buffers; if the ring could hold them all,
        // a slow encoder would starve the grab engine instead of showing up in
        // our own drop counter, which is the signal we actually want.
        size_t ringCapacity = 120;

        // RetrieveResult timeout. A timeout is a counted event, not an error.
        unsigned grabTimeoutMs = 1000;
    };

    explicit CameraWorker(Settings settings);
    ~CameraWorker();

    CameraWorker(const CameraWorker&) = delete;
    CameraWorker& operator=(const CameraWorker&) = delete;

    /// Attach and open the device, apply the .pfs, set buffer parameters.
    /// Returns false and fills LastError() on failure.
    bool Open(Pylon::IPylonDevice* device);

    /// Read back what the camera is really set to, rather than trusting the
    /// .pfs applied. Valid only after Open().
    struct Configured {
        int64_t width = 0, height = 0;
        std::string pixelFormat;
        std::string triggerMode, triggerSource, triggerActivation;
        std::string linkSpeedMode;
        int64_t throughputLimit = 0;
    };
    Configured ReadBackConfig();

    void Start();   ///< begin grabbing on a dedicated thread

    /// Raise the stop flag without waiting. Lets a caller signal every camera
    /// before blocking on any of them, so six cameras stop within the jitter
    /// of one loop rather than one after another.
    void RequestStop() { stop_ = true; }

    void Stop();    ///< signal, join, then Close() the ring so the encoder drains

    /// One row per frame actually received, kept for the metadata files.
    /// blockId is what makes gap analysis possible: it is assigned by the
    /// camera, so a jump proves frames were lost on the wire. campy records
    /// only its own received-count, which cannot distinguish "frame 400" from
    /// "the 400th frame I happened to get".
    struct FrameRecord {
        uint64_t blockId = 0;
        uint64_t timestampNs = 0;   // camera clock, not host time
        /// False when the ring was full and this frame never reached the
        /// encoder. Without this the frame times and the video file silently
        /// disagree: frametimes would list every frame received while the
        /// video contains only those encoded, so row N stops being frame N.
        bool inVideo = true;
    };
    const std::vector<FrameRecord>& Records() const { return records_; }

    RingBuffer<Frame>& Ring() { return ring_; }

    /// Live preview, deliberately cheap and deliberately lossy.
    ///
    /// The grab thread point-samples a small RGB image every Nth frame into a
    /// single slot. It never keeps a CGrabResultPtr (that would consume one of
    /// the camera's buffers) and never blocks: if the GUI holds the lock the
    /// frame is simply skipped. Recording always wins -- a dropped preview
    /// frame costs nothing, a dropped recorded frame is unrecoverable.
    void EnablePreview(int targetFps, int maxWidth = 480);

    /// Copy the newest preview image out. Returns false if nothing new since
    /// `lastSerial`, which is then updated.
    bool TakePreview(std::vector<uint8_t>& rgb, int& w, int& h,
                     uint64_t& lastSerial);
    CameraHealth Health() const;

    /// True once the device has gone away. The recording continues without it.
    bool Failed() const { return failed_.load(); }

    const std::string& Name() const { return settings_.name; }
    const std::string& Serial() const { return settings_.serial; }
    const std::string& LastError() const { return lastError_; }

    /// pylon's own view, read after Stop() as a cross-check on our counters.
    struct StreamStats { int64_t total = -1, failed = -1, missed = -1, resync = -1; };
    StreamStats ReadStreamStats();

private:
    void GrabLoop();
    void UpdatePreview(const uint8_t* pixels, int srcW, int srcH);

    // Filled in Open(): the preview has to know the layout of the pixels it
    // is sampling, and this rig mixes RGB8 with BayerRG8.
    int srcBytesPerPixel_ = 3;
    bool srcIsBayer_ = false;

    Settings settings_;
    Pylon::CInstantCamera camera_;
    RingBuffer<Frame> ring_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> failed_{false};
    std::string lastError_;

    // Touched only by the grab thread, then read after join().
    CameraHealth health_;
    uint64_t lastBlockId_ = 0;
    bool haveLastBlockId_ = false;
    uint64_t nextIndex_ = 0;
    std::vector<FrameRecord> records_;

    // Preview state. previewMutex_ guards the three fields after it.
    int previewEvery_ = 0;          // 0 disables; else take every Nth frame
    int previewMaxWidth_ = 480;
    mutable std::mutex previewMutex_;
    std::vector<uint8_t> previewRgb_;
    int previewW_ = 0, previewH_ = 0;
    uint64_t previewSerial_ = 0;
};

} // namespace campy

#pragma once

// Runs N cameras concurrently: one grab thread and one encode thread each,
// decoupled by that camera's ring buffer.
//
// Threading model on the 16-core rig, 6 cameras:
//
//   main            orchestration, trigger, progress reporting
//   grab   x6       pylon RetrieveResult -> BlockID check -> ring  (CameraWorker)
//   encode x6       ring -> swscale (SIMD) -> encoder -> mp4 mux
//   pylon internal  its own grab-engine threads, one or two per camera
//
// ~13 application threads plus pylon's, on 16 cores / 32 hardware threads.
// Cameras share nothing: no global lock, no shared queue, no cross-camera
// synchronisation on the hot path. A slow encoder on CAM3 shows up only as
// CAM3's ring filling, never as back-pressure on the others.
//
// Compression itself is on the GPU whenever NVENC opens: each encode thread
// submits to NVENC and blocks only on that submission, so six cameras encode
// truly in parallel on the GPU's encode engines. When NVENC is unavailable the
// encoders fall back to libx264 with a capped thread count -- six encoders each
// choosing their own default would oversubscribe the CPU several times over.

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "camera/CameraWorker.h"
#include "encode/VideoEncoder.h"
#include "meta/MetadataWriter.h"

namespace campy {

class Recorder {
public:
    struct CameraSpec {
        std::string serial;
        std::string name;        // CAM1 ... used for the output folder
        std::string pfsPath;

        /// Triggers per captured frame. The top-view camera captures one frame
        /// for every two trigger pulses, so its effective rate is half. Used
        /// for the encoder timebase and so its lower frame count is judged
        /// against the right expectation rather than reported as loss.
        int triggerDivider = 1;
    };

    struct Settings {
        std::vector<CameraSpec> cameras;
        std::string videoFolder;
        std::string videoFilename = "0.mp4";
        int width = 1920;
        int height = 1200;
        double frameRate = 30.0;
        int qp = 21;
        std::string codec = "h264_nvenc";
        int gpuId = 0;
        std::string preset = "fast";
        /// Grab buffers per camera. This is the whole tolerance for a host
        /// that briefly stops keeping up: at 30 Hz, 1500 buffers is 50 s, and the
        /// ring three quarters of that. Sized from a measured failure: a 33 s
        /// block inside a single write to disk, seen twice in 30 minutes.
        int maxNumBuffer = 1500;
        /// 0 = derive from maxNumBuffer, keeping a quarter of the buffers free
        /// for the driver to fill while the ring is full.
        size_t ringCapacity = 0;
        /// Ceiling on buffers x frame size x cameras, in MB. Depth is counted
        /// in frames but paid for in bytes, and the frame size depends on the
        /// resolution and pixel format, so a depth that is harmless for six
        /// 1920x1200 cameras could ask for far more with larger ones.
        int bufferBudgetMB = 73728;
        /// 0 = split the machine evenly across the CPU encoders.
        int encoderThreads = 0;
        /// Live preview rate, 0 disables. Throttled in the grab thread so it
        /// cannot affect recording.
        int previewFps = 10;
        int previewWidth = 480;
    };

    explicit Recorder(Settings settings);
    ~Recorder();

    /// Enumerate, open and configure every camera. False if any failed to open.
    bool Open();

    /// Launch grab and encode threads. Returns immediately.
    bool Start();

    /// Signal every camera to stop, let the rings drain into the encoders, then
    /// finalize each file. Blocks until all threads have joined.
    void Stop();

    bool Running() const { return running_.load(); }

    struct CameraStatus {
        std::string name, serial;
        CameraHealth health;
        RingStats ring;
        uint64_t framesEncoded = 0;
        bool failed = false;
        std::string error;
        /// How long this camera's encoder has been inside one call, ms, and
        /// which call. Non-zero for more than a frame interval means the
        /// encoder is not consuming and the ring is filling behind it.
        uint64_t encoderStallMs = 0;
        std::string encoderPhase;
        /// Worst stall seen so far this session, and what was blocking.
        uint64_t worstStallMs = 0;
        std::string worstStallPhase;
    };
    std::vector<CameraStatus> Status() const;

    /// Total frames received across every camera. Cheap enough to poll: it is
    /// the signal used to tell when the cameras have gone quiet.
    uint64_t TotalFramesReceived() const;

    /// Block until no camera has received a new frame for `quietMs`, or until
    /// `timeoutMs` has elapsed. Returns true if the cameras went quiet.
    ///
    /// Called after the trigger is stopped and before the encoders are shut
    /// down, so that frames already in flight are captured rather than cut off
    /// mid-session. `onTick` runs once per poll; the GUI uses it to keep the
    /// window responsive while waiting.
    bool WaitUntilQuiet(int quietMs, int timeoutMs,
                        const std::function<void(uint64_t, int)>& onTick = {});

    /// True when every camera lost nothing, dropped nothing and failed nothing.
    bool AllHealthy() const;

    /// Fewest frames any *still-working* camera has received. Failed cameras
    /// are excluded so one unplugged camera cannot stall the whole session.
    uint64_t MinFramesReceived() const;

    /// Recorded length in seconds, taken as the minimum across working
    /// cameras of framesReceived / that camera's own rate. Cameras running at
    /// different rates are therefore compared on equal terms -- a 15 Hz camera
    /// with half the frames is exactly as far through the session.
    double MinRecordedSeconds() const;

    /// True once any camera has dropped out, e.g. been unplugged.
    bool AnyCameraFailed() const;

    /// Newest preview image for camera `index`, or false if nothing new.
    bool TakePreview(size_t index, std::vector<uint8_t>& rgb, int& w, int& h,
                     uint64_t& lastSerial);
    size_t CameraCount() const { return channels_.size(); }
    std::string CameraName(size_t index) const;

    /// Write frametimes/metadata/gap files for every camera plus the session
    /// report. Called after Stop(), when the frame records are final.
    std::vector<CameraReport> WriteMetadata();

private:
    void EncodeLoop(size_t index);

    struct Channel {
        std::unique_ptr<CameraWorker> camera;
        std::unique_ptr<VideoEncoder> encoder;
        std::thread thread;
        std::atomic<uint64_t> framesEncoded{0};
        double frameRate = 30.0;   // this camera's own rate
        std::atomic<bool> encodeFailed{false};
        std::string encodeError;
        /// Filled in by the watchdog thread, read by Status() and the report.
        std::atomic<uint64_t> worstStallMs{0};
        std::atomic<int> worstStallPhase{0};
        std::atomic<double> worstStallAtSec{0.0};
    };

    /// Watches the encoders for calls that stop returning. Runs on its own
    /// thread because the thing it is watching is, by definition, not running.
    void WatchdogLoop();
    std::thread watchdog_;

    Settings settings_;
    /// The depths actually used, after the budget cap. Reported in metadata.csv
    /// so a recording says what it really ran with, not what was asked for.
    int maxNumBufferUsed_ = 0;
    size_t ringCapacity_ = 0;
    Pylon::CTlFactory* factory_ = nullptr;
    std::vector<std::unique_ptr<Channel>> channels_;
    std::atomic<bool> running_{false};
};

} // namespace campy

#pragma once

// H.264 encode to MP4 via libavcodec/libavformat, in-process.
//
// This class is the reason for the rewrite. campy shells out to ffmpeg.exe
// through imageio-ffmpeg, so when the parent is killed the encoder dies
// mid-write and the MP4 never receives its moov atom -- that is the corruption
// we spent a week repairing. Here the encoder is ours: Finish() flushes,
// drains and calls av_write_trailer(), and the destructor calls Finish() if
// nobody else did. A file is finalized on a normal stop, on an exception, and
// on a camera disconnect alike.
//
// Pixel format: RGB straight into h264_nvenc, letting the GPU do the colour
// conversion -- the same thing campy gets today from pixelFormatOutput: rgb0.
// Converting to NV12 on the CPU with sws_scale would be ~1.24 GB/s of work
// across six cameras for no benefit.

#include <atomic>
#include <cstdint>
#include <string>

struct AVCodecContext;
struct AVFormatContext;
struct AVFrame;
struct AVPacket;
struct AVStream;
struct SwsContext;

namespace campy {

class VideoEncoder {
public:
    struct Settings {
        std::string path;                 // output .mp4
        int width = 0;
        int height = 0;
        double frameRate = 30.0;
        int qp = 21;                      // campy "quality"
        std::string codec = "h264_nvenc"; // falls back to libx264 if absent
        int gpuId = 0;
        std::string preset = "fast";
        /// Worker threads for the CPU encoder. 0 lets the library decide,
        /// which is wrong when six encoders share one machine: libx264 picks
        /// ~7 each, so six of them oversubscribe a 16-core CPU badly.
        int threads = 0;

        /// Pixel layout coming out of the camera, as pylon names it:
        /// "RGB8", "BayerRG8", "Mono8"... Mapped to an FFmpeg format and fed
        /// to swscale. Bayer is passed through as Bayer rather than being
        /// debayered first: at 2966x2974 the top-view camera produces 8.8 MB
        /// per frame as Bayer and 26.5 MB as RGB, and the RGB rate would
        /// exceed its own 360 MB/s link limit.
        std::string cameraPixelFormat = "RGB8";
    };

    VideoEncoder() = default;
    ~VideoEncoder();

    VideoEncoder(const VideoEncoder&) = delete;
    VideoEncoder& operator=(const VideoEncoder&) = delete;

    /// Open the file and encoder. False on failure; see LastError().
    bool Open(const Settings& settings);

    /// Encode one frame. `pixels` must hold width*height*3 bytes of RGB24.
    bool WriteFrame(const uint8_t* pixels, size_t bytes);

    /// Flush the encoder, drain remaining packets and write the trailer.
    /// Idempotent, and called by the destructor if it was not called directly,
    /// so no path can leave the file without a moov atom.
    bool Finish();

    bool IsOpen() const { return fmt_ != nullptr; }
    /// Frames accepted by the encoder (submitted via avcodec_send_frame).
    uint64_t FramesWritten() const { return framesWritten_; }

    /// Packets actually muxed into the file. Counted separately because these
    /// are NOT the same number: the encoder has an internal delay pipeline, so
    /// a submitted frame may still be in flight. After Finish() they must
    /// agree -- a difference means a frame was genuinely lost, and that has to
    /// be visible rather than inferred.
    uint64_t PacketsMuxed() const { return packetsMuxed_; }

    /// Where a WriteFrame call currently is. The encode thread can block in
    /// any of three places -- the CPU colour conversion, the NVENC driver, or
    /// the write to disk -- and from outside they look identical: frames stop
    /// being consumed. A 45 min session died exactly this way, silently, with
    /// nothing in the Windows event log. Naming the phase turns "the encoder
    /// stopped" into "the encoder blocked in <this call> for <this long>".
    enum class Phase : int { Idle = 0, Convert = 1, Encode = 2, Mux = 3 };

    Phase CurrentPhase() const {
        return static_cast<Phase>(phase_.load(std::memory_order_acquire));
    }
    static const char* PhaseName(Phase p);

    /// How long the current phase has been running, ms. 0 when idle. Safe to
    /// call from another thread: this is the watchdog's only input.
    uint64_t PhaseElapsedMs() const;

    /// Worst single occurrence of each phase, ms. Written only by the encode
    /// thread, read by anyone.
    struct Timings {
        uint64_t maxConvertMs = 0, maxEncodeMs = 0, maxMuxMs = 0;
        uint64_t totalConvertMs = 0, totalEncodeMs = 0, totalMuxMs = 0;
    };
    Timings GetTimings() const;
    const std::string& LastError() const { return lastError_; }
    const std::string& Path() const { return settings_.path; }

    /// The codec that actually opened -- not what was requested. Matters when
    /// NVENC is refused and a camera silently lands on CPU instead.
    const std::string& CodecName() const { return settings_.codec; }

private:
    bool Drain(bool flushing);
    void Cleanup();
    bool Fail(const std::string& what, int averr);

    Settings settings_;
    AVFormatContext* fmt_ = nullptr;
    AVCodecContext* enc_ = nullptr;
    AVStream* stream_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVPacket* packet_ = nullptr;
    SwsContext* sws_ = nullptr;   // only for the CPU path: RGB24 -> YUV420P
    int bytesPerPixel_ = 3;
    int64_t pts_ = 0;
    uint64_t framesWritten_ = 0;
    uint64_t packetsMuxed_ = 0;

    /// Phase tracking. phaseStartNs_ is a steady-clock reading, not a wall
    /// time, so it is unaffected by the clock being adjusted mid-session.
    std::atomic<int> phase_{0};
    std::atomic<uint64_t> phaseStartNs_{0};
    std::atomic<uint64_t> maxConvertUs_{0}, maxEncodeUs_{0}, maxMuxUs_{0};
    std::atomic<uint64_t> totalConvertUs_{0}, totalEncodeUs_{0}, totalMuxUs_{0};

    void EnterPhase(Phase p);
    void LeavePhase(Phase p);
    bool finished_ = false;
    std::string lastError_;
};

} // namespace campy

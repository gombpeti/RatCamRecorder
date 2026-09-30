#include "encode/VideoEncoder.h"

#include <chrono>
#include <cstdio>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

namespace campy {
namespace {

/// pylon pixel format name -> FFmpeg input format, and its bytes per pixel.
/// Returns false for anything unrecognised rather than guessing, because a
/// wrong guess produces plausible-looking but wrong colour.
bool MapPixelFormat(const std::string& pylonName, AVPixelFormat& fmt, int& bpp) {
    struct Entry { const char* name; AVPixelFormat fmt; int bpp; };
    static const Entry table[] = {
        {"RGB8",      AV_PIX_FMT_RGB24,        3},
        {"BGR8",      AV_PIX_FMT_BGR24,        3},
        {"Mono8",     AV_PIX_FMT_GRAY8,        1},
        {"BayerRG8",  AV_PIX_FMT_BAYER_RGGB8,  1},
        {"BayerGR8",  AV_PIX_FMT_BAYER_GRBG8,  1},
        {"BayerGB8",  AV_PIX_FMT_BAYER_GBRG8,  1},
        {"BayerBG8",  AV_PIX_FMT_BAYER_BGGR8,  1},
    };
    for (const Entry& e : table)
        if (pylonName == e.name) { fmt = e.fmt; bpp = e.bpp; return true; }
    return false;
}

std::string AvError(int err) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(err, buf, sizeof buf);
    return buf;
}


} // namespace

VideoEncoder::~VideoEncoder() {
    // Last line of defence: an exception, an early return or a forgotten call
    // must still leave a playable file rather than one missing its moov atom.
    if (!finished_ && fmt_) Finish();
    Cleanup();
}

bool VideoEncoder::Fail(const std::string& what, int averr) {
    lastError_ = what;
    if (averr != 0) lastError_ += ": " + AvError(averr);
    return false;
}

bool VideoEncoder::Open(const Settings& settings) {
    settings_ = settings;
    finished_ = false;
    pts_ = 0;
    framesWritten_ = 0;
    packetsMuxed_ = 0;

    const AVCodec* codec = avcodec_find_encoder_by_name(settings_.codec.c_str());
    if (!codec) {
        // A missing NVENC (driver, or all sessions busy) should degrade to CPU
        // rather than abort a recording that is already under way.
        std::printf("[encoder] %s unavailable, falling back to libx264\n",
                    settings_.codec.c_str());
        codec = avcodec_find_encoder_by_name("libx264");
        settings_.codec = "libx264";
    }
    if (!codec) return Fail("no usable H.264 encoder", 0);

    int rc = avformat_alloc_output_context2(&fmt_, nullptr, "mp4",
                                            settings_.path.c_str());
    if (rc < 0 || !fmt_) return Fail("avformat_alloc_output_context2", rc);

    stream_ = avformat_new_stream(fmt_, nullptr);
    if (!stream_) return Fail("avformat_new_stream", 0);

    enc_ = avcodec_alloc_context3(codec);
    if (!enc_) return Fail("avcodec_alloc_context3", 0);

    const AVRational tb = av_d2q(1.0 / settings_.frameRate, 1000000);
    enc_->width     = settings_.width;
    enc_->height    = settings_.height;
    enc_->time_base = tb;
    enc_->framerate = av_d2q(settings_.frameRate, 1000000);
    enc_->gop_size  = static_cast<int>(settings_.frameRate);   // 1 s keyframes
    enc_->max_b_frames = 0;    // no reordering: pts stays monotonic and a
                               // truncated file is still frame-aligned
    // Hand RGB to the encoder and let it convert on the GPU, as campy does via
    // pixelFormatOutput: rgb0. NVENC accepts bgr0/rgb0 directly.
    const bool nvenc = settings_.codec.find("nvenc") != std::string::npos;
    enc_->pix_fmt = nvenc ? AV_PIX_FMT_BGR0 : AV_PIX_FMT_YUV420P;

    if (fmt_->oformat->flags & AVFMT_GLOBALHEADER)
        enc_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    if (nvenc) {
        av_opt_set(enc_->priv_data, "preset", settings_.preset.c_str(), 0);
        av_opt_set(enc_->priv_data, "rc", "constqp", 0);
        av_opt_set_int(enc_->priv_data, "qp", settings_.qp, 0);
        av_opt_set_int(enc_->priv_data, "gpu", settings_.gpuId, 0);
    } else {
        av_opt_set(enc_->priv_data, "preset", settings_.preset.c_str(), 0);
        av_opt_set(enc_->priv_data, "tune", "fastdecode", 0);
        av_opt_set_int(enc_->priv_data, "crf", settings_.qp, 0);
        if (settings_.threads > 0) enc_->thread_count = settings_.threads;
    }

    rc = avcodec_open2(enc_, codec, nullptr);
    if (rc < 0 && nvenc) {
        // NVENC can exist in the build yet refuse to open: driver too old for
        // the API the build targets, or every encode session already in use.
        // A recording must not die for that, so fall back to CPU and say so
        // loudly -- silently producing nothing would be far worse.
        std::printf("[encoder] %s would not open (%s); falling back to libx264\n",
                    settings_.codec.c_str(), AvError(rc).c_str());
        std::fflush(stdout);

        avcodec_free_context(&enc_);
        codec = avcodec_find_encoder_by_name("libx264");
        if (!codec) return Fail("h264_nvenc unusable and libx264 absent", rc);
        settings_.codec = "libx264";

        enc_ = avcodec_alloc_context3(codec);
        if (!enc_) return Fail("avcodec_alloc_context3", 0);
        enc_->width     = settings_.width;
        enc_->height    = settings_.height;
        enc_->time_base = tb;
        enc_->framerate = av_d2q(settings_.frameRate, 1000000);
        enc_->gop_size  = static_cast<int>(settings_.frameRate);
        enc_->max_b_frames = 0;
        enc_->pix_fmt   = AV_PIX_FMT_YUV420P;
        if (fmt_->oformat->flags & AVFMT_GLOBALHEADER)
            enc_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        av_opt_set(enc_->priv_data, "preset", settings_.preset.c_str(), 0);
        av_opt_set(enc_->priv_data, "tune", "fastdecode", 0);
        av_opt_set_int(enc_->priv_data, "crf", settings_.qp, 0);
        if (settings_.threads > 0) enc_->thread_count = settings_.threads;

        rc = avcodec_open2(enc_, codec, nullptr);
    }
    if (rc < 0) return Fail("avcodec_open2", rc);

    rc = avcodec_parameters_from_context(stream_->codecpar, enc_);
    if (rc < 0) return Fail("avcodec_parameters_from_context", rc);
    stream_->time_base = tb;

    if (!(fmt_->oformat->flags & AVFMT_NOFILE)) {
        rc = avio_open(&fmt_->pb, settings_.path.c_str(), AVIO_FLAG_WRITE);
        if (rc < 0) return Fail("avio_open " + settings_.path, rc);
    }

    // faststart moves the moov to the front on close; the trailer is written
    // either way, so an interrupted file is still repairable.
    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "movflags", "+faststart", 0);
    rc = avformat_write_header(fmt_, &opts);
    av_dict_free(&opts);
    if (rc < 0) return Fail("avformat_write_header", rc);

    frame_ = av_frame_alloc();
    packet_ = av_packet_alloc();
    if (!frame_ || !packet_) return Fail("av_frame_alloc / av_packet_alloc", 0);

    frame_->format = enc_->pix_fmt;
    frame_->width  = enc_->width;
    frame_->height = enc_->height;
    rc = av_frame_get_buffer(frame_, 0);
    if (rc < 0) return Fail("av_frame_get_buffer", rc);

    // One scaler for both paths, targeting whatever format the encoder actually
    // opened with: RGB24 -> BGR0 for NVENC (a byte shuffle), RGB24 -> YUV420P
    // for libx264 (a real colour conversion).
    //
    // swscale rather than a hand-written loop: it has SIMD kernels for these
    // conversions. A scalar per-pixel loop over 2.3 Mpixel x 30 Hz x 6 cameras
    // is ~415 Mpixel/s of avoidable single-threaded work.
    AVPixelFormat inFmt = AV_PIX_FMT_RGB24;
    if (!MapPixelFormat(settings_.cameraPixelFormat, inFmt, bytesPerPixel_))
        return Fail("unsupported camera pixel format: " +
                    settings_.cameraPixelFormat, 0);

    // Bilinear for Bayer: SWS_POINT on a Bayer source samples one colour plane
    // per output pixel and produces obvious colour fringing.
    const bool bayer = settings_.cameraPixelFormat.rfind("Bayer", 0) == 0;
    sws_ = sws_getContext(enc_->width, enc_->height, inFmt,
                          enc_->width, enc_->height, enc_->pix_fmt,
                          bayer ? SWS_BILINEAR : SWS_POINT,
                          nullptr, nullptr, nullptr);
    if (!sws_) return Fail("sws_getContext (RGB24 -> encoder format)", 0);

    std::printf("[encoder] %s -> %s (%dx%d @ %.3g fps, qp %d)\n",
                settings_.codec.c_str(), settings_.path.c_str(),
                settings_.width, settings_.height, settings_.frameRate,
                settings_.qp);
    return true;
}

namespace {
/// Steady clock in nanoseconds. Deliberately not the wall clock: a session can
/// outlive an NTP correction, and a stall measured against a clock that jumped
/// is worse than no measurement.
uint64_t SteadyNs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}
} // namespace

const char* VideoEncoder::PhaseName(Phase p) {
    switch (p) {
        case Phase::Convert: return "pixel conversion (CPU)";
        case Phase::Encode:  return "encoder submit (GPU/NVENC)";
        case Phase::Mux:     return "write to disk";
        default:             return "idle";
    }
}

void VideoEncoder::EnterPhase(Phase p) {
    // Order matters: the start time must be visible before the phase, or the
    // watchdog can read a new phase against a stale start and report a stall
    // of hours. Release on the phase store pairs with the watchdog's acquire.
    phaseStartNs_.store(SteadyNs(), std::memory_order_relaxed);
    phase_.store(static_cast<int>(p), std::memory_order_release);
}

void VideoEncoder::LeavePhase(Phase p) {
    const uint64_t us =
        (SteadyNs() - phaseStartNs_.load(std::memory_order_relaxed)) / 1000;
    phase_.store(static_cast<int>(Phase::Idle), std::memory_order_release);

    std::atomic<uint64_t>* mx = nullptr;
    std::atomic<uint64_t>* tot = nullptr;
    switch (p) {
        case Phase::Convert: mx = &maxConvertUs_; tot = &totalConvertUs_; break;
        case Phase::Encode:  mx = &maxEncodeUs_;  tot = &totalEncodeUs_;  break;
        case Phase::Mux:     mx = &maxMuxUs_;     tot = &totalMuxUs_;     break;
        default: return;
    }
    tot->fetch_add(us, std::memory_order_relaxed);
    uint64_t prev = mx->load(std::memory_order_relaxed);
    while (us > prev && !mx->compare_exchange_weak(prev, us,
                                                   std::memory_order_relaxed)) {}
}

uint64_t VideoEncoder::PhaseElapsedMs() const {
    if (phase_.load(std::memory_order_acquire) == static_cast<int>(Phase::Idle))
        return 0;
    const uint64_t start = phaseStartNs_.load(std::memory_order_relaxed);
    const uint64_t now = SteadyNs();
    return now > start ? (now - start) / 1000000 : 0;
}

VideoEncoder::Timings VideoEncoder::GetTimings() const {
    Timings t;
    t.maxConvertMs   = maxConvertUs_.load(std::memory_order_relaxed) / 1000;
    t.maxEncodeMs    = maxEncodeUs_.load(std::memory_order_relaxed) / 1000;
    t.maxMuxMs       = maxMuxUs_.load(std::memory_order_relaxed) / 1000;
    t.totalConvertMs = totalConvertUs_.load(std::memory_order_relaxed) / 1000;
    t.totalEncodeMs  = totalEncodeUs_.load(std::memory_order_relaxed) / 1000;
    t.totalMuxMs     = totalMuxUs_.load(std::memory_order_relaxed) / 1000;
    return t;
}

bool VideoEncoder::WriteFrame(const uint8_t* pixels, size_t bytes) {
    if (!fmt_ || !enc_ || finished_) return Fail("encoder is not open", 0);
    if (!pixels) return Fail("null pixel buffer", 0);

    const size_t expected =
        static_cast<size_t>(settings_.width) * settings_.height * bytesPerPixel_;
    if (bytes < expected) {
        char msg[160];
        std::snprintf(msg, sizeof msg,
                      "short frame: got %zu bytes, need %zu for %dx%d %s",
                      bytes, expected, settings_.width, settings_.height,
                      settings_.cameraPixelFormat.c_str());
        return Fail(msg, 0);
    }

    int rc = av_frame_make_writable(frame_);
    if (rc < 0) return Fail("av_frame_make_writable", rc);

    // SIMD conversion into whatever the encoder wants. For NVENC that is a
    // BGR0 shuffle and the actual colour conversion happens on the GPU; for
    // libx264 it is the full RGB24 -> YUV420P conversion on the CPU.
    if (!sws_) return Fail("no scaler", 0);
    const uint8_t* srcData[4] = {pixels, nullptr, nullptr, nullptr};
    const int srcStride[4] = {enc_->width * bytesPerPixel_, 0, 0, 0};
    EnterPhase(Phase::Convert);
    sws_scale(sws_, srcData, srcStride, 0, enc_->height,
              frame_->data, frame_->linesize);
    LeavePhase(Phase::Convert);

    frame_->pts = pts_++;
    EnterPhase(Phase::Encode);
    rc = avcodec_send_frame(enc_, frame_);
    LeavePhase(Phase::Encode);
    if (rc < 0) return Fail("avcodec_send_frame", rc);

    if (!Drain(false)) return false;
    ++framesWritten_;
    return true;
}

bool VideoEncoder::Drain(bool flushing) {
    for (;;) {
        EnterPhase(Phase::Encode);
        const int rc = avcodec_receive_packet(enc_, packet_);
        LeavePhase(Phase::Encode);
        if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) return true;
        if (rc < 0) return Fail("avcodec_receive_packet", rc);

        av_packet_rescale_ts(packet_, enc_->time_base, stream_->time_base);
        packet_->stream_index = stream_->index;

        EnterPhase(Phase::Mux);
        const int wrc = av_interleaved_write_frame(fmt_, packet_);
        LeavePhase(Phase::Mux);
        av_packet_unref(packet_);
        if (wrc < 0) return Fail("av_interleaved_write_frame", wrc);
        ++packetsMuxed_;
    }
}

bool VideoEncoder::Finish() {
    if (finished_ || !fmt_) return true;
    finished_ = true;

    bool ok = true;
    if (enc_) {
        const int rc = avcodec_send_frame(enc_, nullptr);   // enter flush mode
        if (rc < 0 && rc != AVERROR_EOF) ok = Fail("flush avcodec_send_frame", rc);
        if (!Drain(true)) ok = false;
    }

    // Write the trailer even if flushing failed: a file with a moov atom and a
    // few missing tail frames is recoverable, one without is not.
    const int rc = av_write_trailer(fmt_);
    if (rc < 0) ok = Fail("av_write_trailer", rc);

    if (framesWritten_ != packetsMuxed_)
        std::printf("[encoder] MISMATCH on %s: %llu frame(s) submitted but "
                    "%llu packet(s) muxed -- %llu frame(s) lost inside the "
                    "encoder\n",
                    settings_.path.c_str(),
                    static_cast<unsigned long long>(framesWritten_),
                    static_cast<unsigned long long>(packetsMuxed_),
                    static_cast<unsigned long long>(framesWritten_ - packetsMuxed_));

    std::printf("[encoder] closed %s (%llu submitted, %llu muxed)%s\n",
                settings_.path.c_str(),
                static_cast<unsigned long long>(framesWritten_),
                static_cast<unsigned long long>(packetsMuxed_),
                ok ? "" : " WITH ERRORS");
    std::fflush(stdout);

    Cleanup();
    return ok;
}

void VideoEncoder::Cleanup() {
    if (sws_)    { sws_freeContext(sws_); sws_ = nullptr; }
    if (frame_)  av_frame_free(&frame_);
    if (packet_) av_packet_free(&packet_);
    if (enc_)    avcodec_free_context(&enc_);
    if (fmt_) {
        if (fmt_->pb && !(fmt_->oformat->flags & AVFMT_NOFILE)) avio_closep(&fmt_->pb);
        avformat_free_context(fmt_);
        fmt_ = nullptr;
    }
    stream_ = nullptr;
}

} // namespace campy

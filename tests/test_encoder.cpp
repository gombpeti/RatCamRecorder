// Proves the guarantee this rewrite exists for: an MP4 written by VideoEncoder
// always ends up with a moov atom.
//
// campy shells out to ffmpeg.exe, so killing the parent kills the encoder
// mid-write and the moov is never written -- files that then need byte-level
// repair before they can be opened. Here the encoder is in-process and
// av_write_trailer() runs from Finish(), which the destructor calls if nobody
// else did. These tests check the destructor path specifically, because that is
// the one an exception or an early return takes.

#include "encode/VideoEncoder.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace campy;

namespace {

int failures = 0;

void Check(bool cond, const char* what) {
    if (!cond) { std::printf("  FAIL: %s\n", what); ++failures; }
}

constexpr int kWidth = 320;
constexpr int kHeight = 240;
constexpr int kFrames = 30;

/// A moving gradient, so the encoder has real work and successive frames differ.
std::vector<uint8_t> MakeFrame(int n) {
    std::vector<uint8_t> px(static_cast<size_t>(kWidth) * kHeight * 3);
    for (int y = 0; y < kHeight; ++y)
        for (int x = 0; x < kWidth; ++x) {
            const size_t i = (static_cast<size_t>(y) * kWidth + x) * 3;
            px[i + 0] = static_cast<uint8_t>((x + n * 4) & 0xFF);
            px[i + 1] = static_cast<uint8_t>((y + n * 2) & 0xFF);
            px[i + 2] = static_cast<uint8_t>((x + y + n) & 0xFF);
        }
    return px;
}

/// Scan for the 'moov' box. Without it a player sees no tracks at all -- this
/// is exactly what was missing from the corrupted recordings.
bool HasMoovAtom(const std::string& path, size_t* sizeOut = nullptr) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (sizeOut) *sizeOut = static_cast<size_t>(size < 0 ? 0 : size);
    if (size <= 0) { std::fclose(f); return false; }

    std::vector<char> buf(static_cast<size_t>(size));
    const size_t got = std::fread(buf.data(), 1, buf.size(), f);
    std::fclose(f);

    for (size_t i = 0; i + 4 <= got; ++i)
        if (std::memcmp(buf.data() + i, "moov", 4) == 0) return true;
    return false;
}

VideoEncoder::Settings SettingsFor(const std::string& path) {
    VideoEncoder::Settings s;
    s.path = path;
    s.width = kWidth;
    s.height = kHeight;
    s.frameRate = 30.0;
    s.qp = 28;
    return s;
}

std::string TempPath(const char* name) {
    const char* tmp = std::getenv("TEMP");
    return std::string(tmp ? tmp : ".") + "\\campy_test_" + name + ".mp4";
}

void TestExplicitFinish() {
    const std::string path = TempPath("explicit");
    std::remove(path.c_str());
    {
        VideoEncoder enc;
        Check(enc.Open(SettingsFor(path)), "Open succeeds");
        for (int i = 0; i < kFrames; ++i) {
            const auto px = MakeFrame(i);
            Check(enc.WriteFrame(px.data(), px.size()), "WriteFrame succeeds");
        }
        Check(enc.Finish(), "Finish succeeds");
        Check(enc.FramesWritten() == kFrames, "all frames accounted for");
    }
    size_t bytes = 0;
    Check(HasMoovAtom(path, &bytes), "explicit Finish -> file has moov");
    std::printf("  explicit Finish: %zu bytes\n", bytes);
    std::remove(path.c_str());
}

void TestDestructorFinalises() {
    // The important case. Finish() is never called; only the destructor runs,
    // which is the path taken by an exception or an early return.
    const std::string path = TempPath("dtor");
    std::remove(path.c_str());
    {
        VideoEncoder enc;
        Check(enc.Open(SettingsFor(path)), "Open succeeds");
        for (int i = 0; i < kFrames; ++i) {
            const auto px = MakeFrame(i);
            enc.WriteFrame(px.data(), px.size());
        }
        // deliberately no Finish()
    }
    size_t bytes = 0;
    Check(HasMoovAtom(path, &bytes), "destructor alone -> file still has moov");
    std::printf("  destructor only: %zu bytes\n", bytes);
    std::remove(path.c_str());
}

/// Count how many H.264 access units the file actually contains, by parsing
/// the mdat for Annex-B-style NAL starts is unreliable in MP4, so instead check
/// the encoder's own two counters agree. Every frame submitted must come out as
/// a muxed packet; a difference is a frame lost inside the encoder.
void TestEveryFrameReachesTheFile() {
    const std::string path = TempPath("counts");
    std::remove(path.c_str());
    uint64_t submitted = 0, muxed = 0;
    {
        VideoEncoder enc;
        Check(enc.Open(SettingsFor(path)), "Open succeeds");
        for (int i = 0; i < kFrames; ++i) {
            const auto px = MakeFrame(i);
            enc.WriteFrame(px.data(), px.size());
        }
        Check(enc.Finish(), "Finish succeeds");
        submitted = enc.FramesWritten();
        muxed = enc.PacketsMuxed();
    }
    std::printf("  submitted %llu, muxed %llu\n",
                static_cast<unsigned long long>(submitted),
                static_cast<unsigned long long>(muxed));
    Check(submitted == static_cast<uint64_t>(kFrames), "all frames submitted");
    Check(muxed == submitted,
          "every submitted frame is muxed -- none stuck in the encoder");
    std::remove(path.c_str());
}

void TestShortFrameRejected() {
    const std::string path = TempPath("short");
    std::remove(path.c_str());
    {
        VideoEncoder enc;
        Check(enc.Open(SettingsFor(path)), "Open succeeds");
        std::vector<uint8_t> tooSmall(100, 0);
        Check(!enc.WriteFrame(tooSmall.data(), tooSmall.size()),
              "undersized frame is refused, not read past the end");
        Check(!enc.LastError().empty(), "failure reports why");
    }
    std::remove(path.c_str());
}

} // namespace

int main() {
    std::printf("VideoEncoder tests\n");
    TestExplicitFinish();
    TestDestructorFinalises();
    TestEveryFrameReachesTheFile();
    TestShortFrameRejected();

    if (failures == 0) { std::printf("PASS\n"); return 0; }
    std::printf("FAILED: %d check(s)\n", failures);
    return 1;
}

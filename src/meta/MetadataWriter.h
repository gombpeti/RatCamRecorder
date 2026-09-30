#pragma once

// Per-camera metadata, campy-compatible plus the gap analysis campy cannot do.
//
// Files written into each camera's folder:
//
//   frametimes.npy   (2, N) float64 -- row 0 frame number (1-based), row 1
//                    camera timestamp in seconds, zeroed to the first frame.
//                    Byte-compatible with what campy's SaveMetadata() writes,
//                    so existing analysis code keeps working.
//   frametimes.mat   MATLAB variables `frameNumber`, `timeStamp` and `blockId`.
//   blockids.npy     NEW: (1, N) float64, the camera's own frame number for
//                    each video frame. Align cameras on this, not on the row
//                    index: two cameras can hold the same number of frames and
//                    still not be the same frames.
//   dropped.csv      NEW: the frames that arrived but did not reach the video.
//
// N counts only frames that are in the video. A frame the camera sent and the
// host received still does not exist in the file if the encoder was behind
// when it arrived, and writing those into frametimes would shift every row
// after the first drop -- the frame times would describe a video that was
// never written.
//   metadata.csv     quoted key,value pairs, as campy writes them.
//   frameinfo.csv    NEW: one row per frame with the camera's BlockID and the
//                    inter-frame interval, for after-the-fact checking.
//   gaps.csv         NEW: one row per discontinuity -- which frames are
//                    missing, how many, and how much time they represent.
//
// Why the extra two files: campy records only its own received-count, which
// counts 1,2,3... whether or not frames were lost in between. Nothing in its
// output can distinguish "1800 frames arrived" from "1800 of 1830 arrived".
// BlockID is assigned by the camera, so a jump in it is proof of loss and says
// exactly which frames went missing.

#include <cstdint>
#include <string>
#include <vector>

namespace campy {

struct FrameTime {
    uint64_t blockId = 0;
    uint64_t timestampNs = 0;
    /// False when the encoder could not keep up and this frame never reached
    /// the video file. Only frames with this set true are written to
    /// frametimes, so row N of frametimes stays frame N of the video.
    bool inVideo = true;
};

/// One discontinuity in a camera's BlockID sequence.
struct FrameGap {
    uint64_t afterFrame = 0;        // 1-based index of the last good frame
    uint64_t firstMissingBlock = 0;
    uint64_t lastMissingBlock = 0;
    uint64_t missingCount = 0;
    double   timeBefore = 0.0;      // seconds, zeroed to the first frame
    double   timeAfter = 0.0;
    double   gapSeconds = 0.0;      // measured interval across the gap
    double   expectedSeconds = 0.0; // what it should have been
    double   lostSeconds = 0.0;     // gapSeconds - expectedSeconds
};

struct CameraReport {
    std::string name;
    std::string serial;
    uint64_t framesReceived = 0;
    uint64_t framesExpected = 0;    // from the BlockID span
    uint64_t framesMissing = 0;     // lost on the wire, never arrived
    /// How many of the received frames are actually in the video. Lower than
    /// framesReceived when the encoder fell behind. This is the number that
    /// matches ffprobe, and the number frametimes describes.
    uint64_t framesInVideo = 0;
    uint64_t framesDropped = 0;     // received but not encoded
    /// BlockID range of what is in the video, which is what cross-camera
    /// alignment has to compare. The received range can agree between two
    /// cameras while the videos still hold different frames.
    uint64_t firstVideoBlockId = 0;
    uint64_t lastVideoBlockId = 0;
    uint64_t firstBlockId = 0;
    uint64_t lastBlockId = 0;
    double   durationSeconds = 0.0;
    double   measuredFps = 0.0;
    double   lostSeconds = 0.0;
    /// This camera's own rate. Not necessarily the session rate: the top-view
    /// camera captures one frame per two triggers, so it runs at half. Kept
    /// here so cross-camera checks compare like with like instead of reading
    /// its lower frame count as loss.
    double   nominalFps = 0.0;
    std::vector<FrameGap> gaps;
    bool complete() const { return framesMissing == 0; }
};

class MetadataWriter {
public:
    /// Key/value lines for metadata.csv, in the order given.
    using Meta = std::vector<std::pair<std::string, std::string>>;

    /// Write every file for one camera and return its analysis.
    /// `folder` is that camera's own directory.
    static CameraReport WriteCamera(const std::string& folder,
                                    const std::string& cameraName,
                                    const std::string& serial,
                                    const std::vector<FrameTime>& frames,
                                    double nominalFps,
                                    const Meta& meta,
                                    std::string* errorOut = nullptr);

    /// Human-readable per-camera note, e.g. CAM1_note.txt, written beside the
    /// video. Says in plain words what the numbers mean, so the recording can
    /// be judged without loading anything into MATLAB first.
    /// `sessionLastBlockId` is the highest last-BlockID across all cameras.
    /// A camera whose own last BlockID is lower stopped early -- unplugged,
    /// or failed -- and the frames after that point are absent from its video
    /// even though they are not a "gap" in what it did receive. Pass 0 when
    /// there is nothing to compare against.
    static bool WriteCameraNote(const std::string& folder,
                                const CameraReport& report,
                                double nominalFps,
                                const std::vector<FrameTime>& frames,
                                uint64_t sessionLastBlockId = 0,
                                std::string* errorOut = nullptr);

    /// Session-level summary across cameras, written to the session root.
    /// Checks that every camera covers the same BlockID range, which is what
    /// proves the videos are frame-aligned rather than merely the same length.
    static bool WriteSessionReport(const std::string& sessionFolder,
                                   const std::vector<CameraReport>& reports,
                                   double nominalFps,
                                   std::string* errorOut = nullptr);
};

} // namespace campy

#include "meta/MetadataWriter.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>

namespace campy {
namespace {

/// NPY v1.0. The header is padded so the data starts on a 64-byte boundary,
/// which numpy requires, and the dict text must match what numpy emits closely
/// enough to parse -- it is read with ast.literal_eval, not a strict parser.
bool WriteNpyRows(const std::string& path,
                  const std::vector<const std::vector<double>*>& rows,
                  std::string* err) {
    if (rows.empty()) { if (err) *err = "npy needs at least one row"; return false; }
    const size_t n = rows[0]->size();
    for (const auto* r : rows) {
        if (r->size() != n) { if (err) *err = "npy rows differ in length"; return false; }
    }
    std::ofstream f(path, std::ios::binary);
    if (!f) { if (err) *err = "cannot open " + path; return false; }

    char dict[128];
    std::snprintf(dict, sizeof dict,
                  "{'descr': '<f8', 'fortran_order': False, 'shape': (%zu, %zu), }",
                  rows.size(), n);
    size_t headerLen = std::strlen(dict);
    // 10 = magic(6) + version(2) + headerLen field(2)
    size_t total = 10 + headerLen + 1;               // +1 for the trailing \n
    const size_t padded = ((total + 63) / 64) * 64;
    const size_t padding = padded - total;

    f.write("\x93NUMPY", 6);
    const unsigned char ver[2] = {1, 0};
    f.write(reinterpret_cast<const char*>(ver), 2);
    const uint16_t hlen = static_cast<uint16_t>(headerLen + padding + 1);
    f.write(reinterpret_cast<const char*>(&hlen), 2);
    f.write(dict, static_cast<std::streamsize>(headerLen));
    for (size_t i = 0; i < padding; ++i) f.put(' ');
    f.put('\n');

    // C-order: each row laid down whole, in turn.
    for (const auto* r : rows)
        f.write(reinterpret_cast<const char*>(r->data()),
                static_cast<std::streamsize>(r->size() * sizeof(double)));
    return f.good();
}

/// MATLAB Level 4. Chosen over Level 5 deliberately: v4 needs no zlib and no
/// tag nesting, and both MATLAB and scipy.io.loadmat read it without fuss.
/// Each variable is a 1xN double row vector, so column-major ordering is moot.
bool WriteMat4(const std::string& path,
               const std::vector<std::pair<std::string, const std::vector<double>*>>& vars,
               std::string* err) {
    std::ofstream f(path, std::ios::binary);
    if (!f) { if (err) *err = "cannot open " + path; return false; }

    for (const auto& v : vars) {
        const std::vector<double>& data = *v.second;
        const int32_t type = 0;          // little-endian, double, full matrix
        const int32_t rows = 1;
        const int32_t cols = static_cast<int32_t>(data.size());
        const int32_t imag = 0;
        const int32_t namelen = static_cast<int32_t>(v.first.size()) + 1;

        for (int32_t field : {type, rows, cols, imag, namelen})
            f.write(reinterpret_cast<const char*>(&field), 4);
        f.write(v.first.c_str(), namelen);       // includes the NUL
        f.write(reinterpret_cast<const char*>(data.data()),
                static_cast<std::streamsize>(data.size() * sizeof(double)));
    }
    return f.good();
}

std::string Quote(const std::string& s) {
    std::string out = "\"";
    for (char c : s) { if (c == '"') out += '"'; out += c; }
    out += '"';
    return out;
}

} // namespace

CameraReport MetadataWriter::WriteCamera(const std::string& folder,
                                         const std::string& cameraName,
                                         const std::string& serial,
                                         const std::vector<FrameTime>& frames,
                                         double nominalFps,
                                         const Meta& meta,
                                         std::string* errorOut) {
    CameraReport rep;
    rep.name = cameraName;
    rep.serial = serial;
    rep.nominalFps = nominalFps;
    rep.framesReceived = frames.size();
    if (frames.empty()) {
        if (errorOut) *errorOut = cameraName + ": no frames, nothing to write";
        return rep;
    }

    // Zero the clock to the first frame that is in the video, so timeStamp(1)
    // is 0 as campy's readers expect, and convert nanoseconds to seconds. The
    // same origin is used for every file here, so frameinfo.csv and frametimes
    // can be read against each other directly.
    uint64_t t0 = frames.front().timestampNs;
    for (const FrameTime& ft : frames)
        if (ft.inVideo) { t0 = ft.timestampNs; break; }

    // Timeline over every frame received, used for the gap analysis and for
    // frameinfo.csv. Frames dropped at the encoder still arrived, so they are
    // evidence about the camera even though they are not in the file.
    std::vector<double> recvTime(frames.size());
    for (size_t i = 0; i < frames.size(); ++i)
        recvTime[i] = static_cast<double>(frames[i].timestampNs - t0) * 1e-9;

    // What frametimes describes: only the frames that reached the video. The
    // index into these arrays is the frame index in the .mp4, which is the
    // whole point -- indexing a video by a row that counts frames it does not
    // contain is silently wrong rather than obviously wrong.
    std::vector<double> frameNumber, timeStamp, blockIds;
    std::vector<size_t> videoRowOfFrame(frames.size(), 0);   // 0 = not in video
    frameNumber.reserve(frames.size());
    timeStamp.reserve(frames.size());
    blockIds.reserve(frames.size());
    for (size_t i = 0; i < frames.size(); ++i) {
        if (!frames[i].inVideo) continue;
        frameNumber.push_back(static_cast<double>(frameNumber.size() + 1)); // 1-based
        timeStamp.push_back(recvTime[i]);
        blockIds.push_back(static_cast<double>(frames[i].blockId));
        videoRowOfFrame[i] = frameNumber.size();
    }
    rep.framesInVideo = frameNumber.size();
    rep.framesDropped = rep.framesReceived - rep.framesInVideo;
    if (!blockIds.empty()) {
        rep.firstVideoBlockId = static_cast<uint64_t>(blockIds.front());
        rep.lastVideoBlockId  = static_cast<uint64_t>(blockIds.back());
    }

    rep.firstBlockId = frames.front().blockId;
    rep.lastBlockId  = frames.back().blockId;
    rep.framesExpected = rep.lastBlockId - rep.firstBlockId + 1;
    rep.framesMissing = rep.framesExpected > rep.framesReceived
                      ? rep.framesExpected - rep.framesReceived : 0;
    rep.durationSeconds = recvTime.back();
    rep.measuredFps = rep.durationSeconds > 0
                    ? (rep.framesReceived - 1) / rep.durationSeconds : 0.0;

    const double expectedStep = nominalFps > 0 ? 1.0 / nominalFps : 0.0;

    // Walk the BlockID sequence: any jump greater than one is lost frames, and
    // the timestamps either side say how much time went with them.
    for (size_t i = 1; i < frames.size(); ++i) {
        const uint64_t prev = frames[i - 1].blockId;
        const uint64_t cur = frames[i].blockId;
        if (cur <= prev + 1) continue;

        FrameGap g;
        g.afterFrame = i;                       // 1-based index of last good
        g.firstMissingBlock = prev + 1;
        g.lastMissingBlock = cur - 1;
        g.missingCount = cur - prev - 1;
        g.timeBefore = recvTime[i - 1];
        g.timeAfter = recvTime[i];
        g.gapSeconds = g.timeAfter - g.timeBefore;
        g.expectedSeconds = expectedStep;
        g.lostSeconds = g.gapSeconds - expectedStep;
        rep.lostSeconds += g.lostSeconds > 0 ? g.lostSeconds : 0.0;
        rep.gaps.push_back(g);
    }

    const std::string base = folder + "/";
    std::string err;

    if (!WriteNpyRows(base + "frametimes.npy", {&frameNumber, &timeStamp}, &err)) {
        if (errorOut) *errorOut = err;
        return rep;
    }
    // The camera's own frame numbers, separately so frametimes.npy keeps the
    // (2, N) shape campy's readers expect. Cameras must be aligned on these:
    // after a drop, equal frame counts no longer mean the same frames.
    if (!WriteNpyRows(base + "blockids.npy", {&blockIds}, &err)) {
        if (errorOut) *errorOut = err;
        return rep;
    }
    const std::vector<std::pair<std::string, const std::vector<double>*>> vars = {
        {"frameNumber", &frameNumber}, {"timeStamp", &timeStamp},
        {"blockId", &blockIds}};
    if (!WriteMat4(base + "frametimes.mat", vars, &err)) {
        if (errorOut) *errorOut = err;
        return rep;
    }

    // metadata.csv -- quoted key,value pairs, as campy writes them, with the
    // recording totals appended the same way.
    {
        std::ofstream f(base + "metadata.csv");
        if (!f) { if (errorOut) *errorOut = "cannot open metadata.csv"; return rep; }
        for (const auto& kv : meta)
            f << Quote(kv.first) << "," << Quote(kv.second) << "\n";
        f << Quote("totalFrames") << "," << Quote(std::to_string(rep.framesInVideo)) << "\n";
        f << Quote("framesReceived") << "," << Quote(std::to_string(rep.framesReceived)) << "\n";
        f << Quote("framesDropped") << "," << Quote(std::to_string(rep.framesDropped)) << "\n";
        f << Quote("totalTime") << "," << Quote(std::to_string(rep.durationSeconds)) << "\n";
        f << Quote("firstBlockId") << "," << Quote(std::to_string(rep.firstBlockId)) << "\n";
        f << Quote("lastBlockId") << "," << Quote(std::to_string(rep.lastBlockId)) << "\n";
        f << Quote("framesMissing") << "," << Quote(std::to_string(rep.framesMissing)) << "\n";
    }

    // frameinfo.csv -- per-frame detail for after-the-fact checking.
    {
        std::ofstream f(base + "frameinfo.csv");
        if (!f) { if (errorOut) *errorOut = "cannot open frameinfo.csv"; return rep; }
        // One row per frame RECEIVED, so the camera's behaviour is fully
        // recorded. videoFrame is the 1-based index in the .mp4, or 0 for a
        // frame that was received but never encoded -- that column is what
        // makes the two accounts reconcilable after the fact.
        f << "receivedIndex,blockId,timeStamp_s,interval_s,inVideo,videoFrame\n";
        f.setf(std::ios::fixed);
        for (size_t i = 0; i < frames.size(); ++i) {
            const double dt = i ? recvTime[i] - recvTime[i - 1] : 0.0;
            f.precision(0); f << (i + 1) << "," << frames[i].blockId << ",";
            f.precision(9); f << recvTime[i] << "," << dt << ",";
            f.precision(0); f << (frames[i].inVideo ? 1 : 0) << ","
                              << videoRowOfFrame[i] << "\n";
        }
    }

    // dropped.csv -- frames the camera delivered that the encoder could not
    // take. These are not wire losses: the data reached the host and was
    // thrown away here. Listing them is what lets a recording with drops
    // still be used, by saying exactly which camera frames are absent.
    {
        std::ofstream f(base + "dropped.csv");
        if (!f) { if (errorOut) *errorOut = "cannot open dropped.csv"; return rep; }
        f << "camera,receivedIndex,blockId,timeStamp_s,precedingVideoFrame\n";
        f.setf(std::ios::fixed);
        size_t lastInVideo = 0;
        for (size_t i = 0; i < frames.size(); ++i) {
            if (frames[i].inVideo) { lastInVideo = videoRowOfFrame[i]; continue; }
            f.precision(0);
            f << cameraName << "," << (i + 1) << "," << frames[i].blockId << ",";
            f.precision(9); f << recvTime[i] << ",";
            f.precision(0); f << lastInVideo << "\n";
        }
    }

    // gaps.csv -- always written, with a header even when empty, so its
    // absence never has to be interpreted.
    {
        std::ofstream f(base + "gaps.csv");
        if (!f) { if (errorOut) *errorOut = "cannot open gaps.csv"; return rep; }
        f << "camera,afterFrame,firstMissingBlockId,lastMissingBlockId,"
             "missingFrames,timeBefore_s,timeAfter_s,gap_s,expected_s,lost_s\n";
        f.setf(std::ios::fixed);
        for (const FrameGap& g : rep.gaps) {
            f << cameraName << "," << g.afterFrame << "," << g.firstMissingBlock
              << "," << g.lastMissingBlock << "," << g.missingCount << ",";
            f.precision(9);
            f << g.timeBefore << "," << g.timeAfter << "," << g.gapSeconds << ","
              << g.expectedSeconds << "," << g.lostSeconds << "\n";
        }
    }

    return rep;
}


bool MetadataWriter::WriteCameraNote(const std::string& folder,
                                     const CameraReport& r,
                                     double nominalFps,
                                     const std::vector<FrameTime>& frames,
                                     uint64_t sessionLastBlockId,
                                     std::string* errorOut) {
    std::ofstream f(folder + "/" + r.name + "_note.txt");
    if (!f) { if (errorOut) *errorOut = "cannot open " + r.name + "_note.txt"; return false; }
    f.setf(std::ios::fixed);

    f << r.name << "  (serial " << r.serial << ")" << "\n";
    f << "======================================" << "\n" << "\n";

    f << "Frames received : " << r.framesReceived << "\n";
    f << "Frames IN VIDEO : " << r.framesInVideo
      << "   <- this is what the .mp4 contains" << "\n";
    if (r.framesDropped) {
        f << "Frames DROPPED  : " << r.framesDropped
          << "   received but not encoded; see dropped.csv" << "\n";
    }
    f << "Frames expected : " << r.framesExpected
      << "   (BlockID " << r.firstBlockId << ".." << r.lastBlockId << ")" << "\n";
    f << "Frames MISSING  : " << r.framesMissing << "\n";
    f.precision(3);
    f << "Duration        : " << r.durationSeconds << " s" << "\n";
    f.precision(4);
    f << "Measured rate   : " << r.measuredFps << " fps  (nominal "
      << nominalFps << ")" << "\n";
    f.precision(4);
    f << "Time lost       : " << r.lostSeconds << " s" << "\n" << "\n";

    // Interval statistics: how steady the hardware trigger actually was.
    if (frames.size() > 2) {
        const uint64_t t0 = frames.front().timestampNs;
        double mn = 1e9, mx = -1e9, sum = 0.0;
        size_t n = 0;
        for (size_t i = 1; i < frames.size(); ++i) {
            const double dt =
                double(frames[i].timestampNs - frames[i - 1].timestampNs) * 1e-9;
            mn = (std::min)(mn, dt); mx = (std::max)(mx, dt); sum += dt; ++n;
        }
        const double mean = sum / double(n);
        double ss = 0.0;
        for (size_t i = 1; i < frames.size(); ++i) {
            const double dt =
                double(frames[i].timestampNs - frames[i - 1].timestampNs) * 1e-9;
            ss += (dt - mean) * (dt - mean);
        }
        (void)t0;
        f << "Frame interval (camera hardware clock, latched at the trigger edge)" << "\n";
        f.precision(9);
        f << "   mean " << mean << " s   min " << mn << " s   max " << mx << " s" << "\n";
        f.precision(1);
        f << "   jitter " << (mx - mn) * 1e6 << " us peak-to-peak, sd "
          << std::sqrt(ss / double(n)) * 1e6 << " us" << "\n";
        f.precision(1);
        if (nominalFps > 0) {
            const double ppm = (mean - 1.0 / nominalFps) / (1.0 / nominalFps) * 1e6;
            f << "   " << ppm << " ppm vs the requested rate -- this is the"
              << " difference between the" << "\n"
              << "   trigger board crystal and the camera clock, not dropped frames." << "\n";
        }
        f << "\n";
    }

    // A camera that stopped early has no "gap" -- its BlockID run is simply
    // shorter. Reporting "no missing frames" there would be true of what it
    // received and badly misleading about the session, so compare against the
    // furthest any camera got.
    const bool stoppedEarly =
        sessionLastBlockId > 0 && r.lastBlockId < sessionLastBlockId;
    const uint64_t missedAtEnd =
        stoppedEarly ? sessionLastBlockId - r.lastBlockId : 0;

    if (stoppedEarly) {
        f << "*** THIS CAMERA STOPPED EARLY ***" << "\n";
        f << "--------------------------------------" << "\n";
        f << "Its last frame is BlockID " << r.lastBlockId
          << ", but other cameras reached " << sessionLastBlockId << "." << "\n";
        f << missedAtEnd << " trigger pulse(s) after that are NOT in this video."
          << "\n";
        if (nominalFps > 0) {
            f.precision(3);
            f << "That is about " << (double(missedAtEnd) / nominalFps)
              << " s missing from the end of this recording." << "\n";
        }
        f << "Most likely the camera was disconnected or failed mid-session."
          << "\n";
        f << "Align with the other cameras on BlockID, not on frame index."
          << "\n" << "\n";
    }

    if (r.gaps.empty()) {
        if (stoppedEarly) {
            f << "No gaps WITHIN the frames it did receive: BlockID "
              << r.firstBlockId << " to " << r.lastBlockId
              << " is complete." << "\n";
            f << "All of the loss is at the end, described above." << "\n";
        } else {
            f << "NO MISSING FRAMES." << "\n";
            f << "Every trigger pulse between the first and last frame was captured." << "\n";
        }
    } else {
        f << "MISSING FRAMES -- " << r.framesMissing << " frame(s) in "
          << r.gaps.size() << " gap(s)" << "\n";
        f << "--------------------------------------" << "\n";
        for (const FrameGap& g : r.gaps) {
            char line[320];
            std::snprintf(line, sizeof line,
                "after frame %llu (t=%.6f s): BlockID %llu..%llu missing, "
                "%llu frame(s)%s"
                "   next frame at t=%.6f s, so the gap is %.6f s instead of %.6f s "
                "-- %.6f s lost%s",
                (unsigned long long)g.afterFrame, g.timeBefore,
                (unsigned long long)g.firstMissingBlock,
                (unsigned long long)g.lastMissingBlock,
                (unsigned long long)g.missingCount, "\n",
                g.timeAfter, g.gapSeconds, g.expectedSeconds, g.lostSeconds, "\n");
            f << line << "\n";
        }
        f << "\n";
        f << "A missing frame never reached the host: the camera numbered it but" << "\n";
        f << "it was lost on the wire. It is absent from the video file, so any" << "\n";
        f << "timing downstream must use frametimes, not frame index." << "\n";
    }

    f << "\n" << "Files here" << "\n" << "----------" << "\n";
    f << "frametimes.npy  (2,N) float64: row 0 frame number, row 1 seconds." << "\n";
    f << "                N counts frames IN THE VIDEO, so row k is video" << "\n";
    f << "                frame k. Frames dropped at the encoder are not" << "\n";
    f << "                here -- including them would shift every later row." << "\n";
    f << "frametimes.mat  MATLAB frameNumber, timeStamp and blockId" << "\n";
    f << "blockids.npy    (1,N) float64: the camera frame number of each" << "\n";
    f << "                video frame. Match cameras on THIS." << "\n";
    f << "frameinfo.csv   every frame received, with where it ended up" << "\n";
    f << "dropped.csv     frames received but not encoded" << "\n";
    f << "gaps.csv        one row per discontinuity" << "\n";
    f << "metadata.csv    recording parameters" << "\n";
    return f.good();
}

bool MetadataWriter::WriteSessionReport(const std::string& sessionFolder,
                                        const std::vector<CameraReport>& reports,
                                        double nominalFps,
                                        std::string* errorOut) {
    std::ofstream f(sessionFolder + "/session_report.txt");
    if (!f) { if (errorOut) *errorOut = "cannot open session_report.txt"; return false; }

    f << "RatCam Recorder -- session report\n";
    f << "=================================\n\n";
    f.setf(std::ios::fixed);

    f << "camera    serial         recvd  inVideo  dropped  missing   dur(s)    fps\n";
    uint64_t totalMissing = 0, totalDropped = 0;
    for (const CameraReport& r : reports) {
        totalMissing += r.framesMissing;
        totalDropped += r.framesDropped;
        char line[256];
        std::snprintf(line, sizeof line,
                      "%-9s %-12s %7llu %8llu %8llu %8llu %8.3f %6.2f\n",
                      r.name.c_str(), r.serial.c_str(),
                      static_cast<unsigned long long>(r.framesReceived),
                      static_cast<unsigned long long>(r.framesInVideo),
                      static_cast<unsigned long long>(r.framesDropped),
                      static_cast<unsigned long long>(r.framesMissing),
                      r.durationSeconds, r.measuredFps);
        f << line;
    }
    f << "\nrecvd   = frames the camera delivered to the host\n";
    f << "inVideo = frames actually written to the .mp4  <- the file length\n";
    f << "dropped = received but not encoded (the host could not keep up)\n";
    f << "missing = never arrived (lost on the wire)\n";


    // Synchronisation. Every camera sees the same trigger pulses, so with a
    // shared trigger line they must cover the same BlockID range. Matching
    // ranges are what prove the videos are frame-aligned; equal frame counts
    // alone do not, because two cameras could each miss a different frame.
    f << "\nSynchronisation\n---------------\n";
    if (reports.size() > 1) {
        uint64_t firstLo = reports[0].firstBlockId, firstHi = firstLo;
        uint64_t lastLo = reports[0].lastBlockId, lastHi = lastLo;
        for (const CameraReport& r : reports) {
            firstLo = (std::min)(firstLo, r.firstBlockId);
            firstHi = (std::max)(firstHi, r.firstBlockId);
            lastLo  = (std::min)(lastLo,  r.lastBlockId);
            lastHi  = (std::max)(lastHi,  r.lastBlockId);
        }
        // Matching BlockID ranges are necessary but not sufficient. Two
        // cameras can span the same range and still hold different frames:
        // one lost frame on the wire, or one frame dropped at the encoder,
        // and every later index in that file refers to a different pulse.
        // The earlier version of this check looked only at the range and
        // so reported ALIGNED for recordings whose videos were not.
        const bool sameSpan = (firstLo == firstHi && lastLo == lastHi);
        if (sameSpan && totalMissing == 0 && totalDropped == 0) {
            f << "ALIGNED: every camera covers BlockID " << firstLo << ".."
              << lastHi << " with no frame missing or dropped," << "\n"
              << "so video frame N is the same trigger pulse in all of them.\n";
        } else if (!sameSpan) {
            f << "NOT ALIGNED: first BlockID spans " << firstLo << ".." << firstHi
              << ", last spans " << lastLo << ".." << lastHi << ".\n"
              << "The cameras did not cover the same pulses. Align on blockids.npy,\n"
              << "not on frame index, when comparing cameras.\n";
        } else {
            f << "NOT FRAME-ALIGNED: the cameras covered the same BlockID range ("
              << firstLo << ".." << lastHi << ")," << "\n"
              << "but " << (totalMissing + totalDropped)
              << " frame(s) are absent from the videos, and not the same ones\n"
              << "in each. The files therefore have different lengths and video\n"
              << "frame N is NOT the same instant in every file.\n"
              << "\nTo compare cameras, intersect their blockids.npy and index each\n"
              << "video by the row where its blockId matches. Every frame that IS\n"
              << "present is still hardware-synchronised -- nothing is mistimed,\n"
              << "frames are simply absent, and dropped.csv says which.\n";
        }
    }

    f << "\nMissing frames\n--------------\n";
    if (totalMissing == 0) {
        f << "None. Every trigger pulse between the first and last frame was\n"
             "captured by every camera.\n";
    } else {
        for (const CameraReport& r : reports) {
            if (r.gaps.empty()) continue;
            f << "\n" << r.name << " (" << r.serial << "): " << r.framesMissing
              << " frame(s) missing in " << r.gaps.size() << " gap(s), "
              << r.lostSeconds << " s lost\n";
            for (const FrameGap& g : r.gaps) {
                char line[256];
                std::snprintf(line, sizeof line,
                              "   after frame %llu: BlockID %llu..%llu missing "
                              "(%llu frame(s)), t=%.6f..%.6f s, gap %.6f s "
                              "instead of %.6f s\n",
                              static_cast<unsigned long long>(g.afterFrame),
                              static_cast<unsigned long long>(g.firstMissingBlock),
                              static_cast<unsigned long long>(g.lastMissingBlock),
                              static_cast<unsigned long long>(g.missingCount),
                              g.timeBefore, g.timeAfter, g.gapSeconds,
                              g.expectedSeconds);
                f << line;
            }
        }
        f << "\nA gap means the frame never reached the host: the camera numbered\n"
             "it but it was lost on the wire. It is absent from the video file,\n"
             "so downstream timing must use frametimes, not frame index.\n";
    }

    f << "\nnominal frame rate: " << nominalFps << " fps\n";
    f << "per-camera detail: frameinfo.csv and gaps.csv in each camera folder\n";
    return f.good();
}

} // namespace campy

#pragma once

// A grabbed frame as it travels from the camera thread to the encoder.
//
// Zero-copy on purpose: the payload stays in pylon's own buffer and we carry
// the refcounted CGrabResultPtr. At 1.24 GB/s across six cameras a memcpy per
// frame is real work for no benefit. The cost is that a queued frame holds one
// of the camera's MaxNumBuffer slots, so the ring capacity must stay below
// MaxNumBuffer -- otherwise a slow encoder starves the grab engine instead of
// being caught by our own drop accounting, which is the failure we want to see.

#include <cstdint>

#include <pylon/PylonIncludes.h>

namespace campy {

struct Frame {
    Pylon::CGrabResultPtr result;   // owns the pixel data

    uint64_t blockId = 0;      // camera-assigned; gaps here are lost frames
    uint64_t timestampNs = 0;  // camera clock, NOT host time -- host time
                               // drifts against the trigger and breaks
                               // alignment with the logger frame counter
    uint64_t index = 0;        // sequential as received, host-assigned
    uint32_t width = 0;
    uint32_t height = 0;

    const uint8_t* Pixels() const {
        return result.IsValid()
            ? static_cast<const uint8_t*>(result->GetBuffer())
            : nullptr;
    }

    size_t Bytes() const {
        return result.IsValid() ? result->GetBufferSize() : 0;
    }

    bool Valid() const { return result.IsValid(); }
};

/// Running per-camera tally of everything that can go wrong, so no failure is
/// silent. campy swallows grab errors unless a debug flag nobody sets is on.
struct CameraHealth {
    uint64_t framesReceived = 0;
    /// BlockID of the first frame this camera saw. With a shared hardware
    /// trigger every camera numbers the same pulse identically, so equal first
    /// BlockIDs across cameras prove frame 0 is the same instant everywhere.
    uint64_t firstBlockId = 0;
    uint64_t lastBlockId = 0;
    uint64_t framesLost = 0;     // from BlockID gaps: lost on the wire
    uint64_t gapEvents = 0;      // number of discontinuities
    uint64_t largestGap = 0;
    uint64_t failedGrabs = 0;    // GrabSucceeded() == false
    uint64_t timeouts = 0;       // RetrieveResult returned nothing
    uint64_t framesDropped = 0;  // ring overflow: encoder fell behind
    uint64_t framesEncoded = 0;

    bool Healthy() const {
        return framesLost == 0 && failedGrabs == 0 && framesDropped == 0;
    }
};

} // namespace campy

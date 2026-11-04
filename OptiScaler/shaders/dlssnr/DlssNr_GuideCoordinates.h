#pragma once
#include <cstdint>
namespace DlssNr::GuideCoordinates
{
struct Mapping
{
    bool depthRowsFlip = false, motionRowsFlip = false, vectorYNegated = false;
    uint32_t key = 0;
};
// Settings describe native guide encoding. Final-picture producer guides already share colour coordinates.
constexpr Mapping Resolve(bool depthRequested, bool motionRequested, bool signRequested,
                          bool hasDepth, bool motionIsPictureFlow, bool depthIsPictureAligned)
{
    Mapping m;
    // History follows the coordinate contract, not whether an optional guide arrived this frame.
    // A no-depth continuation must keep the same contract as the next depth-bearing capture.
    const bool depthRowsContract = !depthIsPictureAligned && depthRequested;
    m.depthRowsFlip = hasDepth && depthRowsContract;
    m.motionRowsFlip = !motionIsPictureFlow && motionRequested;
    m.vectorYNegated = !motionIsPictureFlow && signRequested;
    m.key = (depthRowsContract ? 1u : 0u) | (m.motionRowsFlip ? 2u : 0u) | (m.vectorYNegated ? 4u : 0u) |
            (motionIsPictureFlow ? 8u : 0u) | (depthIsPictureAligned ? 16u : 0u);
    return m;
}
}

#pragma once
#include <cstdint>
namespace DlssNr::GuideCoordinates
{
struct Mapping
{
    bool depthRowsFlip = false, motionRowsFlip = false, vectorYNegated = false;
    uint32_t key = 0;
};
constexpr uint32_t MakeKey(bool depthRowsContract, bool motionRowsFlip, bool vectorYNegated,
                           bool motionIsPictureFlow, bool depthIsPictureAligned, bool motionIsPictureAligned)
{
    return (depthRowsContract ? 1u : 0u) | (motionRowsFlip ? 2u : 0u) | (vectorYNegated ? 4u : 0u) |
           (motionIsPictureFlow ? 8u : 0u) | (depthIsPictureAligned ? 16u : 0u) |
           (motionIsPictureAligned ? 32u : 0u);
}
// Settings describe native guide encoding. Final-picture producer guides already share colour coordinates.
constexpr Mapping Resolve(bool depthRequested, bool motionRequested, bool signRequested,
                          bool hasDepth, bool motionIsPictureFlow, bool depthIsPictureAligned, bool motionIsPictureAligned = false)
{
    Mapping m;
    // History follows the coordinate contract, not whether an optional guide arrived this frame.
    // A no-depth continuation must keep the same contract as the next depth-bearing capture.
    const bool depthRowsContract = !depthIsPictureAligned && depthRequested;
    m.depthRowsFlip = hasDepth && depthRowsContract;
    const bool alignedMotion = motionIsPictureFlow || motionIsPictureAligned;
    m.motionRowsFlip = !alignedMotion && motionRequested;
    m.vectorYNegated = !alignedMotion && signRequested;
    m.key = MakeKey(depthRowsContract, m.motionRowsFlip, m.vectorYNegated, motionIsPictureFlow,
                    depthIsPictureAligned, motionIsPictureAligned);
    return m;
}
}

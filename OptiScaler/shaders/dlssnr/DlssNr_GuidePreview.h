#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

// Independent of DebugView (whose 0..3 meanings remain unchanged) and of model/history settings.
// One selected resource, snapshotted before Evaluate, displayed after all normal history/capture work.
struct alignas(256) DlssNrGuidePreviewConstants
{
    uint32_t width = 0, height = 0, mode = 0, available = 0;
    uint32_t baseX = 0, baseY = 0, regionWidth = 0, regionHeight = 0;
    float motionScaleX = 1, motionScaleY = 1, motionGain = 16, colourWhite = 1;
    uint32_t flipY = 0, rawResource = 0, colourHdr = 0, depthInverted = 0;
    uint32_t padding[48] = {};
};
static_assert(sizeof(DlssNrGuidePreviewConstants) == 256);
static_assert(offsetof(DlssNrGuidePreviewConstants, baseX) == 16);
static_assert(offsetof(DlssNrGuidePreviewConstants, motionScaleX) == 32);
static_assert(offsetof(DlssNrGuidePreviewConstants, flipY) == 48);

namespace DlssNr
{
// Published separately from g_nrMutex so opening the menu never waits for NGX Evaluate.
std::string GuidePreviewStatus();
}

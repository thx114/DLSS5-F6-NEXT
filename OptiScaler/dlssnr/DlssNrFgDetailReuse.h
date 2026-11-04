#pragma once

// Read-only evidence from the existing NGX FG evaluation seam. This is NOT a
// generated-picture consumer. A supplied pointer/phase index is not proof of
// resource state, GPU completion, presentation, or generated-to-base guide phase.
// Never retain or dereference the borrowed resources, mutate parameters, or feed
// a generated picture into NR from this observer.
#include <d3d12.h>
#include <nvsdk_ngx.h>
#include <cstdint>
#include <mutex>

namespace DlssNr
{
struct FgDetailReuseInfo
{
    bool supported = false; // no complete, verified generated-output consumer exists
    const char* why = "FG output detail reuse is unavailable: no generated-output contract has been observed";
    uint64_t observedNgxEvaluations = 0;
    uint64_t evaluationsWithGeneratedOutput = 0;
    uint64_t evaluationsWithMultiFramePhase = 0;
    uint64_t consumedOutputs = 0; // advances only after a future real reuse consumer submits work
    bool generatedOutputOffered = false;
    bool realOutputOffered = false;
    bool suppressionOutputOffered = false;
    bool multiFramePhaseOffered = false; // valid index/count, NOT verified spatial guide phase
    bool baseFrameIdOffered = false; // supplied identifier, NOT matched NR history identity
    unsigned multiFrameIndex = 0, multiFrameCount = 0;
    unsigned long long baseFrameId = 0;
};

namespace FgDetailReuseEvidence
{
inline std::mutex mutex;
inline FgDetailReuseInfo latest;
}

inline FgDetailReuseInfo FgDetailReuseStatus()
{
    std::lock_guard lock(FgDetailReuseEvidence::mutex);
    return FgDetailReuseEvidence::latest;
}

// Call only for an already identified NGX FrameGeneration feature, while its
// parameter object is valid. These keys and typed Get APIs already occur in the
// NGX providers. Failed Get leaves that fact unknown, regardless of pointer bits.
inline void ObserveFgDetailReuseNgx(const NVSDK_NGX_Parameter* parameters)
{
    if (!parameters) return;
    ID3D12Resource* generated = nullptr;
    ID3D12Resource* real = nullptr;
    ID3D12Resource* suppression = nullptr;
    FgDetailReuseInfo next;
    next.generatedOutputOffered =
        parameters->Get("DLSSG.OutputInterpolated", &generated) == NVSDK_NGX_Result_Success && generated != nullptr;
    next.realOutputOffered =
        parameters->Get("DLSSG.OutputReal", &real) == NVSDK_NGX_Result_Success && real != nullptr;
    next.suppressionOutputOffered =
        parameters->Get("DLSSG.OutputDisableInterpolation", &suppression) == NVSDK_NGX_Result_Success && suppression != nullptr;
    unsigned index = 0, count = 0;
    const bool indexOffered = parameters->Get("DLSSG.MultiFrameIndex", &index) == NVSDK_NGX_Result_Success;
    const bool countOffered = parameters->Get("DLSSG.MultiFrameCount", &count) == NVSDK_NGX_Result_Success;
    next.multiFramePhaseOffered = indexOffered && countOffered && count > 0 && index > 0 && index <= count;
    if (next.multiFramePhaseOffered)
    {
        next.multiFrameIndex = index;
        next.multiFrameCount = count;
    }
    unsigned long long baseId = 0;
    next.baseFrameIdOffered =
        parameters->Get("DLSSG.BackbufferFrameID", &baseId) == NVSDK_NGX_Result_Success;
    if (next.baseFrameIdOffered) next.baseFrameId = baseId;

    if (!next.generatedOutputOffered)
        next.why = "FG output detail reuse is unavailable: NGX generated output is not offered";
    else if (!next.multiFramePhaseOffered || !next.baseFrameIdOffered)
        next.why = "FG output detail reuse is unavailable: NGX output phase or base identity is unknown";
    else
        next.why = "FG output detail reuse is unavailable: generated-frame guides, resource states and queue lifetime are not verified";

    // Never promote support from metadata alone. There is no GPU consumer here,
    // so supported and consumedOutputs intentionally retain their zero defaults.
    std::lock_guard lock(FgDetailReuseEvidence::mutex);
    const auto& previous = FgDetailReuseEvidence::latest;
    next.observedNgxEvaluations = previous.observedNgxEvaluations + 1;
    next.evaluationsWithGeneratedOutput = previous.evaluationsWithGeneratedOutput + next.generatedOutputOffered;
    next.evaluationsWithMultiFramePhase = previous.evaluationsWithMultiFramePhase + next.multiFramePhaseOffered;
    FgDetailReuseEvidence::latest = next;
}
}

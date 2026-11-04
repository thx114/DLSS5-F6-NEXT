#pragma once

#include <cstddef>
#include <nvsdk_ngx_defs.h>
#include <utility>

namespace NgxInput
{
// The routed DLSS backend and CPU regressions execute this exact contract.
// A handle value alone never establishes successful native acquisition.
class NativeFeatureRelease
{
    enum class Acquisition { Empty, Acquired, FailedOutput, Released };
    Acquisition acquisition = Acquisition::Empty;
    NVSDK_NGX_Handle* handle = nullptr;
    NVSDK_NGX_Result createFailure = NVSDK_NGX_Result_Fail;

  public:
    bool HoldsOutput() const
    { return acquisition == Acquisition::Acquired || acquisition == Acquisition::FailedOutput; }
    bool Acquired() const { return acquisition == Acquisition::Acquired; }

    NVSDK_NGX_Result ObserveCreate(NVSDK_NGX_Result result, NVSDK_NGX_Handle* output)
    {
        // A retry cannot overwrite an unreleased or uncertain native owner.
        if (HoldsOutput()) return NVSDK_NGX_Result_FAIL_InvalidParameter;
        handle = output;
        if (result == NVSDK_NGX_Result_Success && output)
            acquisition = Acquisition::Acquired;
        else if (output)
        {
            acquisition = Acquisition::FailedOutput;
            createFailure = result;
        }
        else
            acquisition = Acquisition::Empty;
        return result == NVSDK_NGX_Result_Success && !output ? NVSDK_NGX_Result_Fail : result;
    }

    template <class Release> NVSDK_NGX_Result TryRelease(NVSDK_NGX_Handle*& published, Release release)
    {
        if (acquisition == Acquisition::Empty || acquisition == Acquisition::Released)
            return NVSDK_NGX_Result_Success;
        // An unsuccessful Create with output does not prove that Release may
        // retire it. Keep the complete feature/params alive until restart.
        if (acquisition == Acquisition::FailedOutput) return createFailure;
        if (published != handle) return NVSDK_NGX_Result_FAIL_InvalidParameter;
        if (!release) return NVSDK_NGX_Result_FAIL_FeatureNotSupported;
        const auto result = release(handle);
        if (result == NVSDK_NGX_Result_Success)
        {
            acquisition = Acquisition::Released;
            published = nullptr;
            handle = nullptr;
        }
        return result;
    }
};

// All destructive input side effects belong in commit. A native failure must
// return before exposure/FG cleanup, currentFeature/params changes, or erase.
template <class Feature, class Commit>
NVSDK_NGX_Result CommitAfterFeatureRelease(bool routed, Feature* feature, Commit&& commit)
{
    if (routed && feature)
    {
        const auto result = feature->TryRelease();
        if (result != NVSDK_NGX_Result_Success) return result;
    }
    return std::forward<Commit>(commit)();
}

// The provider increments phase 0 to 1 before moving the old feature into
// DelayedDestroy. Only this boundary may release the current native backend.
template <class Feature, class Advance>
NVSDK_NGX_Result AdvanceAfterFeatureRelease(bool routed, int phase, Feature* feature, Advance&& advance)
{
    return CommitAfterFeatureRelease(routed && phase == 0, feature, std::forward<Advance>(advance));
}
}

#pragma once

#include <cstdint>

// Observation only: no resource ownership, polling, fallback or admission rights.
// In particular, an FG native-guide receipt is not mapped FinishedPicture NR depth.
namespace native::NrInputObservation
{
enum class SourceDepth : uint8_t
{
    Unobserved,
    Supplied,
    SourceHasNoDepth,
    MissingPresentReceipt,
    NativeGuideWithoutMappedLease,
    MissingMappedLease,
    MappedDepthNotAcquiredOrBound
};

enum class GuideDepth : uint8_t
{
    NotRun,
    FlowUnavailable,
    NoSourceDepth,
    UnknownDepthView,
    PairedPipelineUnavailable,
    TrustDispatchFailed,
    GuidesNotRequested,
    GuideBuildFailed,
    GuideDepthMissing,
    Supplied,
    UnmappedProducerCoordinates
};

inline const char* Text(SourceDepth value) noexcept
{
    switch (value)
    {
    case SourceDepth::Supplied: return "supplied-to-producer";
    case SourceDepth::SourceHasNoDepth: return "source-has-no-depth-upstream-reason-unknown";
    case SourceDepth::MissingPresentReceipt: return "no-current-present-depth-receipt";
    case SourceDepth::NativeGuideWithoutMappedLease: return "native-guide-receipt-without-mapped-nr-lease";
    case SourceDepth::MissingMappedLease: return "present-receipt-without-mapped-nr-lease";
    case SourceDepth::MappedDepthNotAcquiredOrBound: return "mapped-depth-not-acquired-or-bind-refused";
    default: return "unobserved";
    }
}

inline const char* Text(GuideDepth value) noexcept
{
    switch (value)
    {
    case GuideDepth::FlowUnavailable: return "flow-unavailable";
    case GuideDepth::NoSourceDepth: return "no-source-depth";
    case GuideDepth::UnknownDepthView: return "unknown-depth-view";
    case GuideDepth::PairedPipelineUnavailable: return "paired-depth-pipeline-unavailable";
    case GuideDepth::TrustDispatchFailed: return "trust-dispatch-failed";
    case GuideDepth::GuidesNotRequested: return "guides-not-requested-or-scene-reset";
    case GuideDepth::GuideBuildFailed: return "guide-build-failed";
    case GuideDepth::GuideDepthMissing: return "guide-build-returned-no-depth";
    case GuideDepth::Supplied: return "guide-built-with-depth";
    case GuideDepth::UnmappedProducerCoordinates: return "producer-depth-without-picture-uv-mapping";
    default: return "not-run";
    }
}

struct Depth
{
    SourceDepth source = SourceDepth::Unobserved;
    GuideDepth guide = GuideDepth::NotRun;
    int acquiredCount = 0;
    bool paired = false;
};

// Synchronous Driver -> ApplyNativeInput association. Raw addresses are compared
// only, never dereferenced; this cannot authorize a consumer or retain a GPU read.
struct SourceReceipt
{
    uintptr_t selected=0,original=0,prepared=0,mapped=0,imported=0;
    uint64_t lifetime=0,originalLifetime=0,loss=0,version=0,clear=0,mapping=0,total=0,covered=0,unresolved=0;
};
struct Dispatch
{
    SourceReceipt source;
    Depth depth;
    const void* queue = nullptr;
    const void* list = nullptr;
    const void* color = nullptr;
    const void* guideDepth = nullptr;
    const void* motion = nullptr;
    const void* distrust = nullptr;
    uint64_t epoch = 0, generation = 0, sourceFrame = 0, pictureSerial = 0;
};

inline thread_local const Dispatch* current = nullptr;

class Scope
{
  public:
    explicit Scope(const Dispatch& value) noexcept : previous_(current) { current = &value; }
    ~Scope() { current = previous_; }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
  private:
    const Dispatch* previous_;
};

inline const Dispatch* Current(const void* queue, const void* list, const void* color,
                               const void* depth, const void* motion, const void* distrust,
                               uint64_t epoch) noexcept
{
    const auto* value = current;
    return value && value->queue == queue && value->list == list && value->color == color &&
        value->guideDepth == depth && value->motion == motion && value->distrust == distrust &&
        value->epoch == epoch ? value : nullptr;
}

// Bound transition output too: toggling a setting every frame must not become
// per-frame disk logging. After 24 initial/transition samples, emit once per 600 calls.
class Reporter
{
  public:
    bool Sample(const Depth& value, bool hasDepth, bool ran) noexcept
    {
        const uint32_t key = static_cast<uint32_t>(value.source) |
            (static_cast<uint32_t>(value.guide) << 8) | (hasDepth ? 1u << 16 : 0u) |
            (ran ? 1u << 17 : 0u);
        const bool changed = !seen_ || key != key_;
        seen_ = true; key_ = key;
        if (calls_ < 600) ++calls_;
        const bool periodic = calls_ == 600;
        if (periodic) calls_ = 0;
        if (burst_ < 24 && (burst_ < 3 || changed)) { ++burst_; return true; }
        return periodic;
    }
  private:
    uint32_t key_ = 0, calls_ = 0, burst_ = 0;
    bool seen_ = false;
};
} // namespace native::NrInputObservation

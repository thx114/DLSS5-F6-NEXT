#pragma once

#include <array>
#include <cstdint>
#include <limits>

namespace HoYoShade
{
enum class Stage : unsigned { BeforeSr = 1, BeforeNr = 2, FinalOutput = 3 };
enum class Reason : unsigned
{
    Disabled, InvalidStage, OtherStage, CorePending, CoreMissing, MultipleCores,
    IdentityRejected, ExportsMissing, RegistrationRejected, PinFailed,
    NoRuntime, UnsupportedApi, UnknownFrame, DeviceMismatch, QueueUnknown,
    QueueMismatch, ListUnobserved, AmbiguousRuntime, DuplicateFrame, StaleFrame,
    Capacity, ExecutionUnverified, FinalEndpointUnavailable, UnsupportedFormat, StateUnknown,
    FenceUnavailable, InternalFailure, ProducerUnsubmitted, ColorSpaceUnknown,
    ExtentMismatch, NaturalUnavailable, NaturalFrameConsumed, GeneratedUnavailable, SelectionUnavailable, SelectionPending, Count
};
inline const char* Text(Reason reason) noexcept
{
    constexpr const char* names[] = {
        "disabled", "invalid-stage", "other-stage", "core-pending", "core-not-loaded", "multiple-loaded-cores",
        "core-disk-identity-rejected", "exports-missing", "api20-registration-rejected", "module-pin-failed",
        "runtime-not-observed", "unsupported-api", "source-frame-unknown", "device-mismatch-or-unproven-alias",
        "queue-unknown", "queue-mismatch-or-unproven-alias", "command-list-not-observed-or-unproven-alias",
        "ambiguous-runtime", "duplicate-source-frame", "stale-source-frame", "observation-capacity",
        "stage-execution-contract-unverified", "final-output-endpoint-not-integrated", "unsupported-sdr-format",
        "color-state-unknown", "same-queue-fence-unavailable", "observer-internal-failure",
        "producer-not-submitted", "color-transfer-unknown-or-hdr", "active-extent-mismatch",
        "natural-runtime-unavailable-or-ambiguous", "natural-frame-already-consumed",
        "generated-output-endpoint-unavailable", "natural-selection-source-unavailable",
        "selected-effects-not-ready" };
    static_assert(sizeof(names) / sizeof(*names) == static_cast<unsigned>(Reason::Count));
    const auto index = static_cast<unsigned>(reason);
    return index < static_cast<unsigned>(Reason::Count) ? names[index] : "unknown";
}
inline const char* Text(Stage stage) noexcept
{
    switch (stage) {
    case Stage::BeforeSr: return "before-sr";
    case Stage::BeforeNr: return "before-nr-chain";
    case Stage::FinalOutput: return "final-output";
    default: return "invalid";
    }
}
struct Identity
{
    uintptr_t runtime = 0, sdkDevice = 0, sdkQueue = 0;
    uint64_t nativeDevice = 0, nativeQueue = 0;
    unsigned api = 0;
    bool operator==(const Identity&) const = default;
};
struct Runtime
{
    Identity id;
    uintptr_t sdkRuntime = 0, sdkDevice = 0, sdkQueue = 0;
    uint64_t generation = 0, presents = 0, begins = 0, finishes = 0, techniques = 0, reloads = 0;
    // No public loading-state query is assumed. Even a reload event is not stage validation.
    bool reloadObserved = false;
};
struct List
{
    uintptr_t sdk = 0, sdkDevice = 0;
    uint64_t native = 0, nativeDevice = 0;
    unsigned api = 0;
};
struct Request
{
    Stage stage = Stage::BeforeNr;
    uint64_t nativeDevice = 0, nativeQueue = 0, nativeList = 0;
    uint64_t stream = 0, sourceGeneration = 0, sourceFrame = 0;
    unsigned api = 0;
};
struct Decision
{
    Reason reason = Reason::NoRuntime;
    Runtime runtime;
    List list;
    bool matched = false;
    bool owned = false;
    bool executionAllowed = false;
};
class Registry
{
    std::array<Runtime, 32> runtimes_{};
    std::array<List, 256> lists_{};
    struct Seen { uint64_t stream = 0, sourceGeneration = 0, runtimeGeneration = 0, frame = 0; };
    std::array<Seen, 128> seen_{};
    uint64_t nextGeneration_ = 0;
public:
    Runtime* Find(uintptr_t runtime) noexcept
    {
        for (auto& entry : runtimes_) if (runtime && entry.id.runtime == runtime) return &entry;
        return nullptr;
    }
    Runtime* Enroll(const Identity& id) noexcept
    {
        if (!id.runtime || !id.sdkDevice || !id.sdkQueue || !id.nativeDevice || !id.nativeQueue) return nullptr;
        auto* slot = Find(id.runtime);
        if (slot && slot->id == id) return slot;
        if (slot) Destroy(id.runtime); // Retire the old generation's dedup entries on pointer reuse.
        if (!slot) for (auto& entry : runtimes_) if (!entry.id.runtime) { slot = &entry; break; }
        if (!slot || nextGeneration_ == (std::numeric_limits<uint64_t>::max)()) return nullptr;
        *slot = {}; slot->id = id; slot->generation = ++nextGeneration_;
        return slot;
    }
    void Destroy(uintptr_t runtime) noexcept
    {
        if (auto* slot = Find(runtime)) {
            for (auto& seen : seen_) if (seen.runtimeGeneration == slot->generation) seen = {};
            *slot = {};
        }
    }
    // Any swapchain reset invalidates observations for its SDK device. Conservative
    // invalidation is preferable to assuming an undocumented runtime->swapchain alias.
    void InvalidateDevice(uintptr_t sdkDevice) noexcept
    {
        for (auto& entry : runtimes_) if (sdkDevice && entry.id.sdkDevice == sdkDevice) Destroy(entry.id.runtime);
    }
    void DestroyDevice(uintptr_t sdkDevice) noexcept
    {
        InvalidateDevice(sdkDevice);
        for (auto& entry : lists_) if (entry.sdkDevice == sdkDevice) entry = {};
    }
    void DestroyQueue(uintptr_t sdkQueue) noexcept
    {
        for (auto& entry : runtimes_) if (sdkQueue && entry.id.sdkQueue == sdkQueue) Destroy(entry.id.runtime);
    }
    bool EnrollList(const List& list) noexcept
    {
        if (!list.sdk || !list.sdkDevice || !list.native || !list.nativeDevice) return false;
        for (auto& entry : lists_) if (entry.sdk == list.sdk) { entry = list; return true; }
        for (auto& entry : lists_) if (!entry.sdk) { entry = list; return true; }
        return false;
    }
    void DestroyList(uintptr_t sdk) noexcept
    {
        for (auto& entry : lists_) if (entry.sdk == sdk) entry = {};
    }
    Decision Inspect(const Request& request, unsigned selected) noexcept
    {
        Decision result;
        if (selected < 1 || selected > 3) { result.reason = Reason::InvalidStage; return result; }
        if (static_cast<unsigned>(request.stage) != selected) { result.reason = Reason::OtherStage; return result; }
        if (request.stage == Stage::FinalOutput) { result.reason = Reason::FinalEndpointUnavailable; return result; }
        if (request.api != 0xc000) { result.reason = Reason::UnsupportedApi; return result; }
        if (!request.stream || !request.sourceFrame) { result.reason = Reason::UnknownFrame; return result; }
        bool any = false, device = false, queue = false;
        unsigned matches = 0;
        for (const auto& entry : runtimes_) {
            if (!entry.id.runtime) continue;
            any = true;
            if (entry.id.api != request.api || entry.id.nativeDevice != request.nativeDevice) continue;
            device = true;
            if (!request.nativeQueue || entry.id.nativeQueue != request.nativeQueue) continue;
            queue = true;
            result.runtime = entry; ++matches;
        }
        if (!any) result.reason = Reason::NoRuntime;
        else if (!device) result.reason = Reason::DeviceMismatch;
        else if (!request.nativeQueue) result.reason = Reason::QueueUnknown;
        else if (!queue) result.reason = Reason::QueueMismatch;
        else if (matches != 1) result.reason = Reason::AmbiguousRuntime;
        else {
            bool list = false;
            for (const auto& entry : lists_) if (entry.native && entry.native == request.nativeList &&
                entry.nativeDevice == request.nativeDevice && entry.api == request.api &&
                entry.sdkDevice == result.runtime.id.sdkDevice) { list = true; result.list = entry; break; }
            if (!list) { result.reason = Reason::ListUnobserved; return result; }
            result.matched = true;
            Seen* slot = nullptr;
            for (auto& seen : seen_) if (seen.stream == request.stream &&
                seen.runtimeGeneration == result.runtime.generation) { slot = &seen; break; }
            // One bounded slot per producer/runtime, not per frame or source generation.
            // A new source generation may restart its frame ordinal; old generations cannot return.
            if (slot && (request.sourceGeneration < slot->sourceGeneration ||
                (request.sourceGeneration == slot->sourceGeneration && request.sourceFrame <= slot->frame))) {
                result.reason = request.sourceGeneration == slot->sourceGeneration && slot->frame == request.sourceFrame
                    ? Reason::DuplicateFrame : Reason::StaleFrame; return result;
            }
            if (!slot) for (auto& seen : seen_) if (!seen.stream) { slot = &seen; break; }
            if (!slot) { result.reason = Reason::Capacity; return result; }
            *slot = {request.stream, request.sourceGeneration, result.runtime.generation, request.sourceFrame};
            result.reason = Reason::ExecutionUnverified;
        }
        return result;
    }
    // The registry is observation-only for execution. An owned runtime may be
    // created before the core emits any runtime/list callback, so it needs a
    // separate bounded source-frame claim that never stores a raw SDK pointer.
    Decision InspectOwned(const Request& request, unsigned selected) noexcept
    {
        Decision result;
        if (selected < 1 || selected > 3) { result.reason = Reason::InvalidStage; return result; }
        if (static_cast<unsigned>(request.stage) != selected) { result.reason = Reason::OtherStage; return result; }
        if (request.api != 0xc000) { result.reason = Reason::UnsupportedApi; return result; }
        if (!request.stream || !request.sourceFrame || !request.nativeDevice || !request.nativeQueue || !request.nativeList)
        { result.reason = Reason::UnknownFrame; return result; }

        Seen* slot = nullptr;
        for (auto& seen : seen_)
            if (seen.stream == request.stream) { slot = &seen; break; }
        if (slot && (request.sourceGeneration < slot->sourceGeneration ||
            (request.sourceGeneration == slot->sourceGeneration && request.sourceFrame <= slot->frame)))
        {
            result.reason = request.sourceGeneration == slot->sourceGeneration && slot->frame == request.sourceFrame
                ? Reason::DuplicateFrame : Reason::StaleFrame;
            return result;
        }
        if (!slot)
            for (auto& seen : seen_) if (!seen.stream) { slot = &seen; break; }
        if (!slot) { result.reason = Reason::Capacity; return result; }
        *slot = { request.stream, request.sourceGeneration, 0, request.sourceFrame };
        result.reason = Reason::ExecutionUnverified;
        result.executionAllowed = true;
        return result;
    }
    void ReleaseClaim(const Request& request, uint64_t runtimeGeneration) noexcept
    {
        for (auto& seen : seen_)
            if (seen.stream == request.stream && seen.sourceGeneration == request.sourceGeneration &&
                (!runtimeGeneration || seen.runtimeGeneration == runtimeGeneration) && seen.frame == request.sourceFrame)
                seen = {};
    }
};
}

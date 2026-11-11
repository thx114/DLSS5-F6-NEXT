#pragma once
#include <ui_mask/DrawUiMask.h>
#include "HoYoShade.h"
#include "HoYoShadeColorContract.h"
#include "HoYoShadeSelection.h"
#include "HoYoShadeDepthSemantics.h"
#include "HoYoShadeStageBinding.h"
#include <algorithm>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <vector>

// R148/R156 candidate, continued in R161/R166 in place.
// All state entry points require the existing executionMutex.
// Natural SDK calls occur ONLY in OwnerPresent with its live callback argument.
// Render receives authenticated producer color, never a natural backbuffer.
namespace HoYoShade::OwnedStage
{
namespace api = reshade::api;
using Microsoft::WRL::ComPtr;
enum class Phase { Natural, Warming, Ready, Owned, Restore, Failed };
inline const char* Text(Phase phase) noexcept
{
    switch (phase) {
    case Phase::Natural: return "natural";
    case Phase::Warming: return "warming";
    case Phase::Ready: return "ready-for-owner-ack";
    case Phase::Owned: return "owned";
    case Phase::Restore: return "restore-on-owner";
    case Phase::Failed: return "failed";
    }
    return "UNKNOWN";
}

// Value-copy the selection graph; no natural or owned SDK handles survive.
// The private file owner is shared, so GPU retirement also retains its files.
inline Selection::Source Configuration(const Selection::Source& from)
{
    Selection::Source out;
    out.valid = from.valid; out.enabled = from.enabled; out.refresh = false;
    out.config = from.config; out.preset = from.preset; out.files = from.files;
    out.effects = from.effects; out.techniques = from.techniques;
    for (auto& t : out.techniques) { t.natural = {}; t.owned = {}; }
    for (const auto& u : from.uniforms) {
        Selection::Uniform v;
        v.effect = u.effect; v.name = u.name; v.annotationSource = u.annotationSource;
        v.type = u.type; v.rows = u.rows; v.columns = u.columns; v.length = u.length; v.count = u.count;
        v.floats = u.floats; v.ints = u.ints; v.uints = u.uints;
        if (u.bools) {
            v.bools = std::make_unique<bool[]>(u.count);
            std::copy_n(u.bools.get(), u.count, v.bools.get());
        }
        out.uniforms.push_back(std::move(v));
    }
    out.graphKnown = from.graphKnown; out.graphValid = from.graphValid;
    out.graphFacts = from.graphFacts; out.graphBuilds = from.graphBuilds; out.snapshots = from.snapshots;
    return out;
}

// Compare immutable build dependencies separately from copied parameter values.
// The shared Files identity pins the existing private config/defines/search paths;
// a new snapshot or preset is never equivalent merely because names match.
// Device/queue/format/extent remain in Render's original R488 binding predicate.
inline bool SameWarmupBuild(const Selection::Source& frozen, const Selection::Source& live) noexcept
{
    if (!live.valid || !live.graphValid || !live.techniquesKnown ||
        frozen.files != live.files || frozen.config != live.config || frozen.preset != live.preset ||
        frozen.enabled != live.enabled || frozen.techniques.size() != live.techniques.size() ||
        frozen.uniforms.size() != live.uniforms.size()) return false;
    for (size_t i = 0; i < frozen.techniques.size(); ++i) {
        const auto& a = frozen.techniques[i]; const auto& b = live.techniques[i];
        if (a.effect != b.effect || a.name != b.name || a.enabled != b.enabled) return false;
    }
    for (size_t i = 0; i < frozen.uniforms.size(); ++i) {
        const auto& a = frozen.uniforms[i]; const auto& b = live.uniforms[i];
        if (a.effect != b.effect || a.name != b.name || a.annotationSource != b.annotationSource ||
            a.type != b.type || a.rows != b.rows || a.columns != b.columns ||
            a.length != b.length || a.count != b.count) return false;
    }
    return true;
}
inline bool SameWarmupValues(const Selection::Source& frozen, const Selection::Source& live) noexcept
{
    // Call only after SameWarmupBuild: indices/shapes/source annotations agree.
    for (size_t i = 0; i < frozen.uniforms.size(); ++i) {
        const auto& a = frozen.uniforms[i]; const auto& b = live.uniforms[i];
        // Existing Selection::Capture/Apply intentionally do not copy dynamic
        // source values. Their private core owns timer/add-on frame progression.
        if (!a.annotationSource.empty()) continue;
        if (a.floats.size() != b.floats.size() || a.ints != b.ints || a.uints != b.uints ||
            static_cast<bool>(a.bools) != static_cast<bool>(b.bools)) return false;
        if (!a.floats.empty() && std::memcmp(a.floats.data(), b.floats.data(), a.floats.size() * sizeof(float)))
            return false; // bitwise equality also handles an unchanged NaN value
        if (a.bools) for (size_t j = 0; j < a.count; ++j) if (a.bools[j] != b.bools[j]) return false;
    }
    return true;
}

struct Targets final : OwnedRuntime::Resource
{
    api::device* device = nullptr;
    api::resource color{}, depth{};
    api::resource_view linear{}, srgb{}, depthSrv{};
    DXGI_FORMAT depthFormat = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT viewFormat = DXGI_FORMAT_UNKNOWN;
    uint32_t depthWidth = 0, depthHeight = 0;
    uint64_t boundReload = UINT64_MAX;
    std::vector<api::effect_texture_variable> depthVariables; // private handles, refreshed only at idle reload
    bool released = false, ready = false, depthReflected = false;
    void DestroyGpuObjects(api::device* d) noexcept override
    {
        if (device != d) return;
        try {
            if (depthSrv.handle) { d->destroy_resource_view(depthSrv); depthSrv = {}; }
            if (linear.handle) { d->destroy_resource_view(linear); linear = {}; }
            if (srgb.handle) { d->destroy_resource_view(srgb); srgb = {}; }
            if (depth.handle) { d->destroy_resource(depth); depth = {}; }
            if (color.handle) { d->destroy_resource(color); color = {}; }
            released = true;
        } catch (...) {} // Pool retains this bundle/runtime on partial destruction.
    }
    bool GpuObjectsReleased() const noexcept override { return released; }
};
struct InputHold
{
    std::shared_ptr<DrawUiMask::Frame> uiMask;
    static void Observe(void* payload,OwnedRuntime::PayloadEvent event,uintptr_t owner,uint64_t generation,uint64_t submission) noexcept {
        const auto state=event==OwnedRuntime::PayloadEvent::Submitted?DrawUiMask::Lifecycle::Submitted:
            event==OwnedRuntime::PayloadEvent::Completed?DrawUiMask::Lifecycle::Completed:DrawUiMask::Lifecycle::Quarantined;
        DrawUiMask::Observe(static_cast<InputHold*>(payload)->uiMask,DrawUiMask::Consumer::Filter,state,owner,generation,submission);
    }
    // Own contents, not the endpoint's borrowed TLS scope or naturalSource.
    std::shared_ptr<GenshinTransport::ProducerFrame> producer;
    std::shared_ptr<GenshinTransport::Lease> transport;
    std::shared_ptr<const GenshinPresentNr::NativeColorCapability> picture;
    native::NativeSubmission submission;
    std::shared_ptr<Depth::Reader> depthReader;
    ComPtr<ID3D12Resource> color, depth;
    ComPtr<ID3D12Fence> ready;
};
// R207: scheduling permission is deliberately separate from runtime/effect/GPU
// authority. No frame, rotating resource, producer fence value or SDK handle is
// part of this wake tuple. The exact native pool key remains unchanged.
struct WarmupKey
{
    uintptr_t natural = 0;
    uint64_t lifetime = 0, settings = 0, revision = 0, reload = 0, bindingGeneration = 0;
    bool operator==(const WarmupKey&) const = default;
};
struct WarmupSchedule
{
    static constexpr uint64_t MinimumMs = 32, MaximumMs = 500;
    WarmupKey observed{};
    bool haveKey = false, haveFinished = false;
    uint64_t finished = 0, next = 0, interval = MinimumMs, skipped = 0;
    static uint64_t After(uint64_t now, uint64_t ms) noexcept
    { return now > UINT64_MAX - ms ? UINT64_MAX : now + ms; }
    bool Admit(const WarmupKey& key, uint64_t now) noexcept
    {
        if (!haveKey || !(observed == key)) {
            observed = key; haveKey = true; interval = MinimumMs;
            // Actual owner/settings/revision/reload/binding changes wake early,
            // while coalescing event storms to the same minimum wall interval.
            next = haveFinished ? After(finished, MinimumMs) : 0;
        }
        if (now >= next) return true;
        if (skipped != UINT64_MAX) ++skipped;
        return false;
    }
    void Pending(uint64_t now) noexcept
    {
        haveFinished = true; finished = now; next = After(now, interval);
        interval = (std::min)(interval * 2, MaximumMs);
    }
    void PredecessorPending(uint64_t now) noexcept
    {
        // Acquire performed the real completion/lease checks. Busy/GpuPending
        // is not selection failure, progress, or permission to discard owners.
        haveFinished = true; finished = now; next = After(now, MinimumMs);
    }
};
struct State
{
    Phase phase = Phase::Natural;
    uintptr_t natural = 0; // scalar callback identity, NEVER an execution pointer
    uint64_t lifetime = 0, settings = 0, revision = 0, generation = 0;
    bool originalEnabled = false, owesRestore = false, changingState = false, restart = false, uncertain = false;
    bool active = false; // callbacks revoke scalars; never replace a graph borrowed by Render
    bool writingValues = false; // only our synchronous Apply/dynamic uniform setters
    std::shared_ptr<const Selection::Source> snapshot;
    // Retain the live-owner capture cache through Warming for bounded soft
    // request revalidation. Handles are used ONLY by a live OwnerPresent call.
    // A local shared reference protects its containers against nested retirement.
    std::shared_ptr<Selection::Source> capturing;
    bool sourceRefreshPending = false;
    WarmupSchedule sourceRefresh; // separate from the private shader progress cadence
    Selection::Source selection; // owned-only mutable handle map, not the published snapshot
    OwnedRuntime::Binding binding;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<IDXGIFactory2> factory;
    std::shared_ptr<OwnedRuntime::Resource> targets;
    api::effect_runtime* owned = nullptr; // compare-only outside exclusive owned lease
    uint64_t reload = 0, techniques = 0;
    uint64_t applyReportGeneration = 0;
    unsigned applyReports = 0;
    Selection::CaptureReason applyReportReason = Selection::CaptureReason::Pending;
    bool applyReadyReported = false;
    uint64_t dynamicUniformReportGeneration = 0; // first typed dynamic request during an actual render only
    uint64_t submitInvalidationReportGeneration = 0; // first sealed-tail invalidation per private generation
    uint64_t lastStream = 0, lastGeneration = 0, lastFrame = 0;
    StageBinding::Key bindingSource; // stable runtime lifetime, never per-frame resource authority
    OwnedRuntime::Completion latest;
    OwnedRuntime::Detail detail; // exact pool failure/pending cause, independent of effects evidence
    Depth::Semantics::State semantics;
    const Depth::Access* renderingDepth = nullptr; // synchronous owned render only
    api::command_list* renderingList = nullptr; // genuine owned list, synchronous callback comparison only
    uint64_t renderingReload = 0;
    bool rendering = false, depthVerified = false, depthLost = false;
    WarmupSchedule warmup;
};
inline State& Get() { static auto* s = new State; return *s; }
inline uint64_t Owner() noexcept { return reinterpret_cast<uintptr_t>(&Get()); }
inline void Retire() noexcept
{
    auto& s = Get();
    if (s.binding.owner) OwnedRuntime::Retire(s.binding.owner, s.binding.generation);
    s.targets.reset(); s.binding = {}; s.bindingSource = {}; s.owned = nullptr;
    s.device.Reset(); s.queue.Reset(); s.factory.Reset(); s.semantics = {};
    s.capturing.reset();
    s.sourceRefreshPending = false; s.sourceRefresh = {};
    s.warmup = {}; // restored/retired binding; never a timer-based resource release
    Selection::InvalidateOwned(s.selection);
}
// Revoke publication BEFORE any drain, SDK restore or private-runtime teardown.
// Completion-unknown batches remain quarantined; elapsed frames cannot heal them.
inline void Fail(bool restart = false) noexcept
{
    auto& s = Get();
    s.restart = s.restart || restart;
    s.phase = s.owesRestore ? Phase::Restore : Phase::Failed;
}
inline bool Bump(uint64_t& value) noexcept
{
    if (value == UINT64_MAX) { Fail(); return false; }
    ++value;
    return true;
}
inline bool RestorationPending() noexcept { return Get().owesRestore; }
inline bool Handles(const ColorEndpoint& e) noexcept
{
    return e.output == OutputKind::Original &&
        ((e.stage == Stage::BeforeSr && e.colorMode == ColorMode::GenshinNativeTypedBeforeSr) ||
         (e.stage == Stage::BeforeNr && e.colorMode == ColorMode::GenshinNativeTypedBeforeNr));
}
inline void SettingsChanged(uint64_t settings) noexcept
{
    auto& s = Get();
    if (s.snapshot && s.settings != settings && s.phase != Phase::Natural) Fail(true);
}
// An authenticated stage may fail before Render obtains its owned lease. That
// refusal must revoke a prior persistent takeover too, without hiding natural
// effects indefinitely or turning this optional filter into a new SR/NR gate.
struct Admission final
{
    bool applicable = false, delegated = false;
    Admission(const ColorEndpoint& e, StageResult& result) noexcept : applicable(Handles(e))
    {
        if (applicable) {
            result.attempt.naturalSuppressed = Get().owesRestore;
            Get().detail = {}; // a pre-admission refusal did not attempt this frame's pool operation
        }
    }
    ~Admission()
    {
        if (applicable && !delegated && (Get().phase == Phase::Owned || Get().phase == Phase::Ready)) Fail(true);
    }
};
struct ValueWrites final
{
    bool previous = Get().writingValues;
    ValueWrites() noexcept { Get().writingValues = true; }
    ~ValueWrites() { Get().writingValues = previous; }
};
inline bool OwnsNatural(api::effect_runtime* r) noexcept
{ return Get().natural == reinterpret_cast<uintptr_t>(r) && Get().phase != Phase::Natural; }
inline uint64_t ReloadSerial(api::effect_runtime* r) noexcept { return Get().owned == r ? Get().reload : 0; }
inline bool ReflectDepth(api::effect_runtime* r, Targets& target)
{
    // Collect only on binding/reload. Steady-state technique witnesses use this
    // small list instead of enumerating every texture for every technique.
    std::vector<api::effect_texture_variable> variables;
    const auto reload = Get().reload, revision = Get().revision;
    r->enumerate_texture_variables(nullptr, [&](auto* runtime, auto variable) {
        if (Get().reload != reload || Get().revision != revision) return;
        api::resource_view l{}, g{}; runtime->get_texture_binding(variable, &l, &g);
        if (Get().reload != reload || Get().revision != revision) return;
        if (l == target.depthSrv && g == target.depthSrv) variables.push_back(variable);
    });
    target.depthVariables = std::move(variables);
    return !target.depthVariables.empty();
}
inline bool DepthCurrent(api::effect_runtime* r, const Targets& target)
{
    const auto revision = Get().revision;
    if (target.boundReload != Get().reload || (Get().phase != Phase::Warming && Get().phase != Phase::Owned)) return false;
    for (const auto variable : target.depthVariables) {
        if (target.boundReload != Get().reload || Get().revision != revision) return false;
        api::resource_view l{}, g{};
        r->get_texture_binding(variable, &l, &g);
        if (l != target.depthSrv || g != target.depthSrv) return false;
    }
    return target.boundReload == Get().reload && Get().revision == revision;
}
inline void Technique(api::effect_runtime* r, api::command_list* list) noexcept
{
    auto& s = Get();
    if (reinterpret_cast<uintptr_t>(r) == s.natural && s.phase == Phase::Owned) {
        Fail(true); // an actual natural technique broke the persistent shutdown
        return;
    }
    if (!s.rendering || s.owned != r) return;
    if (list != s.renderingList || s.renderingReload != s.reload || !Bump(s.techniques)) {
        s.depthLost = true; Fail(); return;
    }
    if (!s.renderingDepth || !s.targets) return;
    auto& t = *static_cast<Targets*>(s.targets.get());
    try {
        // Witness after all Begin handlers, using ONLY this private runtime's
        // own handles/list. Never rebind a semantic inside an effects callback.
        const bool current = DepthCurrent(r, t);
        s.depthVerified = current && t.depthReflected;
        s.depthLost = s.depthLost || !current;
    } catch (...) { s.depthLost = true; }
}
inline void Changed(api::effect_runtime* r, bool destroyed = false, bool reloaded = false) noexcept
{
    auto& s = Get();
    if (r == s.owned) {
        if (!destroyed && !reloaded && s.writingValues) return;
        if (reloaded) Bump(s.reload);
        Bump(s.revision);
        if (reloaded || destroyed || !s.writingValues) {
            Selection::InvalidateOwned(s.selection);
            // Warming can advance its private reload; an already publishing
            // runtime must restore/re-warm before it may publish new effects.
            if (s.phase == Phase::Owned || s.rendering || destroyed) Fail(!destroyed);
        }
        return;
    }
    if (reinterpret_cast<uintptr_t>(r) != s.natural || (s.changingState && !destroyed && !reloaded)) return;
    Bump(s.revision);
    Fail(true); // scalar revocation only; Capture/Apply may still borrow vectors
    if (destroyed) {
        // Original mode belongs to the destroyed lifetime, never a reused pointer.
        // Actual pending GPU owners stay in the pool, even after pointer revocation.
        s.owesRestore = false; s.natural = 0; s.lifetime = 0;
        s.phase = Phase::Failed;
    }
}
inline void TopologyChanged() noexcept
{
    auto& s = Get();
    if (s.natural) { Bump(s.revision); Fail(true); }
}
// Public setters are BEFORE requests, not acknowledgements of changed values.
// Only unpublished Warming may retain its runtime pending a real owner readback.
// Revoke current admission immediately; hard lifecycle/selection events and all
// Ready/Owned requests retain Changed's original restore/retire behavior.
inline void SelectionRequested(api::effect_runtime* r) noexcept
{
    auto& s = Get();
    if (reinterpret_cast<uintptr_t>(r) == s.natural && s.phase == Phase::Warming &&
        s.snapshot && s.capturing) {
        if (Bump(s.revision)) s.sourceRefreshPending = true;
        return;
    }
    Changed(r);
}
// Capture/Apply exclude source-annotated uniforms from static selection values.
// Their BEFORE value notifications must follow the same rule: a timer tick is
// not a preset edit and must not revoke an in-progress private render. Use only
// a currently witnessed typed graph; stale/unknown handles keep the old path.
inline void UniformRequested(api::effect_runtime* r, api::effect_uniform_variable variable) noexcept
{
    auto& s = Get();
    if (r == s.owned && s.writingValues) return; // existing synchronous Apply/Begin exemption
    const Selection::Source* source = nullptr;
    const bool owned = r && r == s.owned;
    if (variable.handle && owned && s.binding.owner && s.binding.generation &&
        !s.selection.mappingDirty && s.selection.boundRuntime == r &&
        s.selection.boundGeneration == s.binding.generation) {
        source = &s.selection;
    } else if (variable.handle && r && reinterpret_cast<uintptr_t>(r) == s.natural &&
        s.lifetime && s.capturing && s.capturing->naturalRuntime == r &&
        s.capturing->naturalGeneration == s.lifetime) {
        source = s.capturing.get();
    }
    if (source && source->valid && source->graphKnown && source->graphValid) {
        for (const auto& uniform : source->uniforms) {
            if (uniform.annotationSource.empty() || (owned ? uniform.owned : uniform.natural) != variable) continue;
            const auto events = Selection::Watch(r).Read();
            const auto revision = owned ? source->boundRevision : source->naturalRevision;
            if (events.reloadObserved && events.revision && events.revision == revision) {
                if (owned && s.rendering && s.dynamicUniformReportGeneration != s.generation) {
                    s.dynamicUniformReportGeneration = s.generation;
                    try { LOG_INFO("HoYoShade R573 private uniform request runtime=0x{:x} private_generation={} reload={} phase={} classification=typed-dynamic action=retain-selection mapping_current=true per_generation_limit=1 effects_submission=not-certified pixels=UNKNOWN",
                        reinterpret_cast<uintptr_t>(r), s.generation, s.reload, Text(s.phase)); } catch (...) {}
                }
                return;
            }
            break; // a lifecycle change never inherits an old dynamic classification
        }
    }
    // The original path immediately leaves Warming/Owned, so this failure
    // witness is naturally one-shot until a real owner recovery/rebuild.
    if (owned && s.rendering && (s.phase == Phase::Warming || s.phase == Phase::Owned)) {
        try { LOG_INFO("HoYoShade R573 private uniform request runtime=0x{:x} private_generation={} reload={} phase={} classification=static-or-unwitnessed action=original-invalidation effects_submission=not-certified pixels=UNKNOWN",
            reinterpret_cast<uintptr_t>(r), s.generation, s.reload, Text(s.phase)); } catch (...) {}
    }
    SelectionRequested(r); // static, missing, invalidated or unreflected remains a real invalidation
}
// A public BEFORE mutation is an invalidation, not proof that another add-on
// accepted it. Restore only our remembered mode bit, never a technique/preset.
inline bool EffectsRequested(api::effect_runtime* r, bool enabled) noexcept
{
    auto& s = Get();
    if (r == s.owned) { Changed(r); return false; }
    if (reinterpret_cast<uintptr_t>(r) != s.natural || s.changingState) return false;
    SelectionRequested(r);
    // While we own the mode, defer this global request to the live owner callback.
    // Returning true is API20's documented mutation veto (not render veto).
    if (s.owesRestore) { s.originalEnabled = enabled; return true; }
    return false;
}
// Optional diagnostics consume the already captured local source. They cannot
// authorize a snapshot, call SDK getters or change persistent ownership.
using SelectionObserver = void (*)(api::effect_runtime*, uint64_t, uint64_t,
                                   const Selection::Source&) noexcept;
inline bool CapturePending(const Selection::Source& source) noexcept
{
    switch (source.capture.reason) {
    case Selection::CaptureReason::TechniquesEmpty:
    case Selection::CaptureReason::GlobalDisabled:
    case Selection::CaptureReason::NoEnabledTechniques:
    case Selection::CaptureReason::ValuesUnavailable:
    case Selection::CaptureReason::OwnerChanged:
    case Selection::CaptureReason::OwnerExportPending:
    case Selection::CaptureReason::Reentrant: return true;
    case Selection::CaptureReason::BoundaryChanged:
        // A private-file attempt is one-shot until a real owner/selection
        // refresh, even when its final failure was a boundary revocation.
        return source.snapshotReason != Selection::CaptureReason::BoundaryChanged || !source.snapshotIoAttempted;
    case Selection::CaptureReason::PresetPathUnavailable:
        return source.capture.presetRead == Selection::PresetPathRead::Empty;
    default: return false; // actual IO/reflection/identity failures need a real event
    }
}
inline void OwnerPresent(api::effect_runtime* r, uint64_t lifetime, const CoreApi& core,
                         uint64_t settings, bool requested, bool unique,
                         SelectionObserver observer = nullptr) noexcept
{
    auto& s = Get();
    if (!r || !lifetime || s.active) return;
    try {
        const auto id = reinterpret_cast<uintptr_t>(r);
        if (s.natural && (s.natural != id || s.lifetime != lifetime)) return;
        const bool wanted = requested && unique && settings != 0;
        const bool settingsChanged = s.settings != settings;
        if (!wanted || settingsChanged) {
            Fail(settingsChanged && wanted);
            // A desired-settings change also releases an earlier sticky failure.
            // No private graph/config destruction while a Render call borrows it.
            if (!s.owesRestore && !s.uncertain && (!s.latest.fence || s.latest.IsComplete())) {
                Retire(); s.snapshot.reset(); s.phase = Phase::Natural;
                s.natural = 0; s.lifetime = 0; s.restart = false;
            }
        }
        if (s.phase == Phase::Restore) {
            // No CPU/GPU wait. Known predecessor completion permits the owner
            // callback to restore; unknown submission keeps debt and owners live.
            if (s.uncertain || (s.latest.fence && !s.latest.IsComplete())) return;
            struct Guard { bool& flag; Guard(bool& b):flag(b){flag=true;} ~Guard(){flag=false;} } guard(s.changingState);
            const auto revision = s.revision;
            r->set_effects_state(s.originalEnabled);
            if (s.natural != id || s.lifetime != lifetime || s.revision != revision || s.phase != Phase::Restore) return;
            if (r->get_effects_state() != s.originalEnabled) return; // veto/exception is not an acknowledgement
            if (s.natural != id || s.lifetime != lifetime || s.revision != revision || s.phase != Phase::Restore) return;
            s.owesRestore = false;
            s.phase = (s.restart || !wanted || settingsChanged) ? Phase::Natural : Phase::Failed;
            s.restart = false;
            Retire(); s.snapshot.reset();
            if (s.phase == Phase::Natural) { s.natural = 0; s.lifetime = 0; }
            return; // restoration and re-arm must be separate owner callbacks
        }
        if (!wanted || s.uncertain) return;
        if (s.phase == Phase::Failed && s.restart) {
            if (s.latest.fence && !s.latest.IsComplete()) return;
            Retire(); s.snapshot.reset(); s.phase = Phase::Natural; s.restart = false;
        }
        if (s.phase == Phase::Warming && s.sourceRefreshPending) {
            const WarmupKey key{s.natural, s.lifetime, s.settings, s.revision, s.reload, s.binding.generation};
            if (!s.sourceRefresh.Admit(key, GetTickCount64())) return;
            s.sourceRefresh.Pending(GetTickCount64()); // even reentrant revocation consumes this bounded attempt
            const auto revision = s.revision;
            const auto captured = s.capturing; // event-driven retirement cannot destroy an active SDK argument
            if (!captured || !s.snapshot) { Fail(true); return; }
            struct RefreshReport {
                SelectionObserver observer; api::effect_runtime* runtime;
                uint64_t lifetime, settings; const Selection::Source& source;
                ~RefreshReport() { if (observer) observer(runtime, lifetime, settings, source); }
            } report{observer, r, lifetime, settings, *captured};
            Selection::Capture(r, core, *captured, false, lifetime, Selection::CaptureAccess::OwnerCallback);
            if (s.natural != id || s.lifetime != lifetime || s.settings != settings ||
                s.phase != Phase::Warming || s.revision != revision) return;
            if (!Selection::HasEnabledTechniques(*captured)) {
                if (captured->capture.reason == Selection::CaptureReason::GlobalDisabled ||
                    captured->capture.reason == Selection::CaptureReason::NoEnabledTechniques) Fail(true);
                else if (!CapturePending(*captured)) Fail();
                return; // pending readback never grants any stage/output authority
            }
            if (!SameWarmupBuild(*s.snapshot, *captured) || !SameWarmupValues(*s.snapshot, *captured)) {
                Fail(true); return; // real static parameter changes still use the original re-warm path
            }
            s.sourceRefreshPending = false; // actual equivalent owner state, not the requested setter payload
            s.sourceRefresh = {}; s.sourceRefresh.Pending(GetTickCount64()); // completed readback returns to 32ms
            return; // no natural ownership acknowledgement or GPU work in this callback
        }
        if (s.phase == Phase::Natural) {
            s.natural = id; s.lifetime = lifetime; s.settings = settings;
            s.latest = {};
            const auto revision = s.revision;
            // Reuse R207's one scheduler before there is a private binding.
            // Pending capture cannot suppress natural effects or create a core
            // runtime; real owner/settings/selection events wake the same key.
            const WarmupKey key{s.natural, s.lifetime, s.settings, s.revision, s.reload, s.binding.generation};
            if (!s.warmup.Admit(key, GetTickCount64())) return;
            if (!s.capturing) s.capturing = std::make_shared<Selection::Source>();
            const auto captured = s.capturing;
            struct CaptureReport {
                SelectionObserver observer; api::effect_runtime* runtime;
                uint64_t lifetime, settings; const Selection::Source& source;
                ~CaptureReport() { if (observer) observer(runtime, lifetime, settings, source); }
            } report{observer, r, lifetime, settings, *captured};
            Selection::Capture(r, core, *captured, false, lifetime, Selection::CaptureAccess::OwnerCallback);
            if (s.natural != id || s.lifetime != lifetime || s.settings != settings ||
                s.phase != Phase::Natural || s.revision != revision) {
                return; // preserve the event's revocation/restart; no stale publication
            }
            if (!Selection::HasEnabledTechniques(*captured)) {
                if (CapturePending(*captured)) s.warmup.Pending(GetTickCount64());
                else Fail();
                return; // natural stays enabled; private IO failures are not retried
            }
            s.originalEnabled = captured->enabled;
            s.snapshot = std::make_shared<const Selection::Source>(Configuration(*captured));
            s.selection = Configuration(*s.snapshot);
            // Keep the bounded natural cache for soft-request readback. Render
            // still receives only the handle-free immutable Configuration copy.
            s.sourceRefreshPending = false; s.sourceRefresh = {}; s.warmup = {};
            s.phase = Phase::Warming;
        } else if (s.phase == Phase::Ready) {
            if (!s.latest.IsComplete()) return;
            const auto revision = s.revision;
            if (r->get_effects_state() != s.originalEnabled || s.revision != revision || s.phase != Phase::Ready) {
                Fail(true); return;
            }
            // Debt and revoked phase precede the setter. Only the live owner's
            // persistent false readback grants subsequent owned copyback rights.
            s.owesRestore = true;
            s.phase = Phase::Restore;
            struct Guard { bool& flag; Guard(bool& b):flag(b){flag=true;} ~Guard(){flag=false;} } guard(s.changingState);
            r->set_effects_state(false);
            if (s.natural != id || s.lifetime != lifetime || s.revision != revision || s.phase != Phase::Restore) return;
            if (!r->get_effects_state() && s.revision == revision && s.phase == Phase::Restore)
                s.phase = Phase::Owned;
        } else if (s.phase == Phase::Owned && r->get_effects_state()) {
            Fail(true); // unexpected mode change: revoke now, restore next owner callback
        }
    } catch (...) { Fail(); }
}

inline void DepthUniforms(api::effect_runtime* runtime, const Selection::Source& selection, const Depth::Access* a)
{
    // The graph was reflected once by Apply. Do not enumerate every uniform
    // and reread source strings on every frame.
    const auto revision = Get().revision, reload = Get().reload;
    for (const auto& uniform : selection.uniforms) {
        if (Get().revision != revision || Get().reload != reload || Get().owned != runtime) return;
        const auto& source = uniform.annotationSource;
        if (!uniform.owned.handle || source.empty()) continue;
        bool value;
        if (source == "bufready_depth") value = a != nullptr;
        else if (source == "hoyoshade_depth_reversed") value = a && a->reversed;
        else if (source == "hoyoshade_depth_y_flipped") value = a && a->yFlipped;
        else if (source == "hoyoshade_depth_validity_paired") value = a && a->validityPaired;
        else continue;
        runtime->set_uniform_value_bool(uniform.owned, &value, 1);
    }
}
inline void Begin(api::effect_runtime* runtime) noexcept
{
    auto& s = Get();
    if (!s.rendering || runtime != s.owned || s.renderingReload != s.reload) return;
    try {
        ValueWrites writes;
        const auto* t = static_cast<const Targets*>(s.targets.get());
        DepthUniforms(runtime, s.selection, t && t->depthReflected ? s.renderingDepth : nullptr);
    }
    catch (...) { s.depthLost = true; }
}
inline bool MakeTargets(OwnedRuntime::Lease& lease, const ColorEndpoint& e,
                        const ColorContract::Formats& formats, const D3D12_RESOURCE_DESC* depthDesc)
{
    auto& s = Get();
    if (s.targets) return static_cast<Targets*>(s.targets.get())->ready;
    auto bundle = std::make_unique<Targets>(); bundle->device = lease.Device();
    std::unique_ptr<OwnedRuntime::Resource> base = std::move(bundle);
    if (!lease.AdoptResource(base, s.targets)) return false;
    // Adopt BEFORE creating anything, including partially returned SDK objects.
    auto& t = *static_cast<Targets*>(s.targets.get());
    auto* d = lease.Device();
    const api::resource_desc color(e.width, e.height, 1, 1, formats.working, 1, api::memory_heap::default_,
        api::resource_usage::render_target | api::resource_usage::shader_resource |
        api::resource_usage::copy_source | api::resource_usage::copy_dest);
    if (!d->create_resource(color, nullptr, api::resource_usage::copy_dest, &t.color) || !t.color.handle ||
        !d->create_resource_view(t.color, api::resource_usage::render_target, api::resource_view_desc(formats.linear, 0, 1, 0, 1), &t.linear) ||
        !t.linear.handle ||
        !d->create_resource_view(t.color, api::resource_usage::render_target, api::resource_view_desc(formats.srgb, 0, 1, 0, 1), &t.srgb) ||
        !t.srgb.handle) return false;
    if (!depthDesc) { t.ready = true; return true; } // fresh runtime's empty DEPTH semantic, no stale slot
    t.depthFormat = depthDesc->Format; t.viewFormat = e.depth.typedFormat;
    t.depthWidth = e.depth.width; t.depthHeight = e.depth.height;
    const api::resource_desc depth(t.depthWidth, t.depthHeight, 1, 1, static_cast<api::format>(depthDesc->Format), 1,
        api::memory_heap::default_, api::resource_usage::shader_resource | api::resource_usage::copy_dest);
    t.ready = d->create_resource(depth, nullptr, api::resource_usage::shader_resource, &t.depth) && t.depth.handle &&
        d->create_resource_view(t.depth, api::resource_usage::shader_resource,
            api::resource_view_desc(static_cast<api::format>(e.depth.typedFormat), 0, 1, 0, 1), &t.depthSrv) && t.depthSrv.handle;
    return t.ready;
}

// Called only after HoYoShade.cpp's original AdmitColor/extent/queue/device/
// ready-fence checks. No natural SDK, COM alias inference or natural picture gate.
// currentSettings includes the live request/route; changes revoke copyback.
template<class ColorCurrent>
inline StageResult Render(const ColorEndpoint e, ID3D12Device* device, HMODULE core,
                          const CoreApi& apiBridge, uint64_t (*currentSettings)() noexcept,
                          ColorCurrent&& colorCurrent) noexcept
{
    StageResult result; result.reason = Reason::SelectionPending;
    auto& s = Get();
    result.attempt.naturalSuppressed = s.owesRestore;
    if ((s.phase != Phase::Warming && s.phase != Phase::Owned) || !s.snapshot || !currentSettings ||
        currentSettings() != s.settings) { if (s.phase == Phase::Owned) Fail(); return result; }
    if (s.sourceRefreshPending) return result; // owner readback must acknowledge equivalence first
    if (s.active || s.uncertain || s.revision == UINT64_MAX || s.reload == UINT64_MAX) return result;
    struct Active {
        State& state;
        explicit Active(State& value) : state(value) { state.active = true; }
        ~Active() { state.active = false; }
    } active(s);
    const bool commit = s.phase == Phase::Owned;
    const auto revision = s.revision, settings = s.settings;
    const auto current = [&] { return s.revision == revision && currentSettings() == settings &&
        s.phase == (commit ? Phase::Owned : Phase::Warming); };
    const auto sourceCurrent = [&] {
        // The value endpoint freezes this call's tuple, not live authority.
        // Recheck its genuine TLS producer/transport or native picture receipt
        // at every boundary; immutable allocation facts remain call-local.
        return colorCurrent(e) && SUCCEEDED(device->GetDeviceRemovedReason()) &&
            e.ready.fence->GetCompletedValue() != UINT64_MAX;
    };
    std::shared_ptr<Depth::Detail::ReadLifetime> reader;
    const auto reject = [&](Reason reason) {
        result.reason = reason;
        if (result.Uncertain() && reader && reader->Began()) reader->Quarantine();
        Fail();
        return result;
    };
    const auto pending = [&] { result.reason = Reason::SelectionPending; return result; };
    try {
        // DEPTH is optional. An absent input uses a NEW color-only runtime;
        // disappearance/appearance retires the preceding binding generation.
        // A nonempty but mismatched depth capability never becomes real DEPTH.
        const auto& a = e.depth;
        const bool hasDepth = a.resource && a.reader;
        if (!device || !e.color || !e.queue || !e.stream || !e.generation || !e.frame ||
            !e.ready.queue || !e.ready.fence || !e.ready.value || e.ready.value == UINT64_MAX)
            return reject(Reason::DeviceMismatch);
        if ((a.resource || a.reader) && (!hasDepth || a.owner != e.stream || a.generation != e.generation || a.frame != e.frame ||
            !a.epoch || a.queue != e.queue || a.readyFence != e.ready.fence || a.readyValue != e.ready.value ||
            !a.reader->Validate(a) || !Depth::Compatible(a.resource->GetDesc(), a))) return reject(Reason::SelectionPending);
        const auto depthDesc = hasDepth ? a.resource->GetDesc() : D3D12_RESOURCE_DESC{};
        ComPtr<ID3D12Device> depthDevice;
        if ((hasDepth && (FAILED(a.resource->GetDevice(IID_PPV_ARGS(&depthDevice))) || depthDevice.Get() != device)) ||
            FAILED(device->GetDeviceRemovedReason()) || e.ready.fence->GetCompletedValue() == UINT64_MAX)
            return reject(Reason::DeviceMismatch);
        const auto colorFormats = ColorContract::Resolve(e.color->GetDesc().Format);
        if (!colorFormats) return reject(Reason::UnsupportedFormat);
        if (s.lastStream == e.stream && s.lastGeneration == e.generation && e.frame <= s.lastFrame) {
            result.reason = Reason::DuplicateFrame; return result; // duplicate is not a mode transition
        }
        // Only runtime lifecycle/shape changes retire the private binding.
        // A Genshin Render-token generation advances every invocation; using
        // it here retired still-warming runtimes on consecutive invocations.
        // Read immutable producer metadata only. The original sourceCurrent /
        // depth Validate calls below still authenticate EACH recorded batch.
        StageBinding::Key bindingSource;
        if (e.stage == Stage::BeforeSr && e.colorMode == ColorMode::GenshinNativeTypedBeforeSr) {
#ifndef HOYOSHADE_STAGE_TEST
            if (e.nativeColor.producer)
                bindingSource = StageBinding::BeforeSr(e.stream, e.nativeColor.producer->Owner());
#endif
        } else if (e.stage == Stage::BeforeNr && e.colorMode == ColorMode::GenshinNativeTypedBeforeNr) {
            bindingSource = StageBinding::BeforeNr(e.stream, e.generation);
        }
        if (!bindingSource.Valid()) return reject(Reason::SelectionPending);
        // Per-frame color/depth allocations may rotate; private views stay
        // bound only while the original queue/device/extents/formats agree.
        auto* old = static_cast<Targets*>(s.targets.get());
        const bool changed = s.binding.owner && (s.device.Get() != device || s.queue.Get() != e.queue ||
            s.binding.width != e.width || s.binding.height != e.height || s.binding.format != colorFormats.runtime ||
            !(s.bindingSource == bindingSource) || (old &&
            (old->depthFormat != depthDesc.Format || old->viewFormat != (hasDepth ? a.typedFormat : DXGI_FORMAT_UNKNOWN) ||
             old->depthWidth != (hasDepth ? a.width : 0) || old->depthHeight != (hasDepth ? a.height : 0))));
        if (changed) {
            if (commit) { Fail(true); return pending(); } // revoke, drain, owner-restore BEFORE replacement
            Retire(); // warming has no copyback rights; pool still owns pending inputs/resources
        }
        if (!s.binding.owner) {
            if (s.generation >= UINT64_MAX - 1) return reject(Reason::Capacity);
            if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&s.factory)))) return reject(Reason::NoRuntime);
            s.device = device; s.queue = e.queue;
            s.bindingSource = bindingSource;
            s.binding = {Owner(), ++s.generation, device, e.queue, s.factory.Get(), e.width, e.height,
                colorFormats.runtime, s.selection.config.c_str(), s.selection.files};
        }
        std::optional<OwnedRuntime::WarmupTrace> trace;
        std::optional<OwnedRuntime::WallScope> attemptWall;
        if (!commit) {
            const WarmupKey key{s.natural, s.lifetime, s.settings, s.revision, s.reload, s.binding.generation};
            // Crucially BEFORE Acquire/GetList: a skipped attempt owns no new
            // immediate list and owes no flush/update/Signal or quarantine.
            if (!s.warmup.Admit(key, GetTickCount64())) return pending();
            trace.emplace();
            auto& t = *trace;
            t.attempt = OwnedRuntime::NextWarmupAttempt();
            t.natural = s.natural; t.lifetime = s.lifetime; t.settings = s.settings;
            t.revision = s.revision; t.reload = s.reload;
            t.owner = s.binding.owner; t.generation = s.binding.generation;
            t.device = reinterpret_cast<uintptr_t>(s.binding.device); t.queue = reinterpret_cast<uintptr_t>(s.binding.queue);
            t.factory = reinterpret_cast<uintptr_t>(s.binding.factory);
            t.configLifetime = reinterpret_cast<uintptr_t>(s.binding.configLifetime.get());
            t.width = s.binding.width; t.height = s.binding.height; t.format = static_cast<uint32_t>(s.binding.format);
            t.stream = e.stream; t.sourceGeneration = e.generation; t.sourceFrame = e.frame;
            t.skipped = s.warmup.skipped; s.warmup.skipped = 0;
            t.predecessor = reinterpret_cast<uintptr_t>(s.latest.fence.Get()); t.predecessorValue = s.latest.value;
            attemptWall.emplace(OwnedRuntime::WallOperation::Attempt, &t);
        }
        auto* measured = trace ? &*trace : nullptr;
        OwnedRuntime::Lease lease;
        const auto acquired = OwnedRuntime::AcquireForRender(s.binding, core, apiBridge, lease, &s.detail, measured);
        if (attemptWall) attemptWall->Context(*trace);
        if (acquired == OwnedRuntime::Status::GpuPending || acquired == OwnedRuntime::Status::Busy) {
            if (!commit) s.warmup.PredecessorPending(GetTickCount64());
            return pending(); // normal nonblocking predecessor wait; keep the persistent mode
        }
        if (acquired != OwnedRuntime::Status::Ready) {
            if (acquired == OwnedRuntime::Status::Quarantined || acquired == OwnedRuntime::Status::CompletionUnknown ||
                acquired == OwnedRuntime::Status::SignalFailed || acquired == OwnedRuntime::Status::IdentityMismatch ||
                acquired == OwnedRuntime::Status::CreateFailed) s.uncertain = true;
            return reject(Reason::NoRuntime);
        }
        // An issued list has no retirement receipt until Submit succeeds.
        // Any exception/early refusal leaves this flag set AND the pool lease
        // quarantined, including failures before the first producer read.
        s.uncertain = true;
        auto* r = lease.Runtime(); auto* d = lease.Device(); auto* q = lease.Queue(); auto* list = lease.List();
        if (!r || !d || !q || !list || !lease.NativeList() || q->get_native() != reinterpret_cast<uint64_t>(e.queue))
            return reject(Reason::QueueMismatch);
        s.owned = r;
        const auto idle = [&](Reason reason = Reason::SelectionPending) {
            // Every issued lease is either submitted or deliberately quarantined.
            // Private warmup does not touch producer color or claim filter output.
            OwnedRuntime::Completion done;
            s.uncertain = true;
            if (lease.Submit(done, &s.detail, measured) != OwnedRuntime::Status::Ready || !done.fence || !done.value ||
                done.value == UINT64_MAX || done.fence->GetCompletedValue() == UINT64_MAX)
                return reject(Reason::FenceUnavailable);
            s.latest = done;
            s.uncertain = false;
            if (!commit) s.warmup.Pending(GetTickCount64());
            // Attempt describes producer-color access, so scratch-only warmup
            // stays Untouched even when its private submission is quarantined.
            result.completion = done; result.reason = reason;
            return result;
        };
        const auto warmup = [&] {
            // Pending semantics needs the same private-core progress opportunity
            // as an incomplete selection. No producer reads or ready publication.
            OwnedRuntime::MeasureWarmup(measured, OwnedRuntime::WallOperation::EmptyRender, [&] {
                r->render_effects(list, {0}, {0}); // OWNED list only; never natural suppression
            });
            return idle();
        };
        const auto semantics = OwnedRuntime::MeasureWarmup(measured, OwnedRuntime::WallOperation::DepthSemantics, [&] {
            return Depth::Semantics::Project(s.semantics, r, lease.Generation(), s.selection, hasDepth ? &a : nullptr, ReloadSerial);
        });
        if (semantics.invalidateSelection) Selection::InvalidateOwned(s.selection);
        if (!current()) return idle();
        if (semantics.outcome == Depth::Semantics::Outcome::Pending) {
            if (commit) Fail(true); // direction/reload change warms only after natural restoration
            return warmup();
        }
        if (hasDepth && semantics.outcome != Depth::Semantics::Outcome::Ready) {
            Fail(); return idle();
        }
        const bool applied = [&] {
            ValueWrites writes;
            const bool ready = [&] {
                if (!measured) return Selection::Apply(s.selection, r, lease.Generation());
                OwnedRuntime::WallScope wall(OwnedRuntime::WallOperation::ApplySelection, measured);
                const bool applied = Selection::Apply(s.selection, r, lease.Generation());
                wall.Outcome(applied ? "applied" : Selection::Name(s.selection.applied.reason));
                return applied;
            }();
            // Record actual Apply outcome transitions per private generation,
            // independently of R207's early-attempt quota. Repeated empty
            // enumerations do not consume the first later-ready observation.
            if (s.applyReportGeneration != s.generation) {
                s.applyReportGeneration = s.generation; s.applyReports = 0;
                s.applyReportReason = Selection::CaptureReason::Pending; s.applyReadyReported = false;
            }
            const auto& facts = s.selection.applied;
            if ((!s.applyReadyReported && ready) || (s.applyReports < 8 &&
                (!s.applyReports || s.applyReportReason != facts.reason))) {
                ++s.applyReports; s.applyReportReason = facts.reason; s.applyReadyReported |= ready;
                try { LOG_INFO("HoYoShade R548 private selection runtime=0x{:x} private_generation={} reload={} observation={} transition_limit=8 first_ready_reserved=true reason={} enum_complete={} techniques={} mapping_complete={} cached={} apply_returned={} effects_submission=not-certified pixels=UNKNOWN",
                    reinterpret_cast<uintptr_t>(r), s.generation, s.reload, s.applyReports, Selection::Name(facts.reason),
                    facts.techniqueEnumComplete, facts.techniques, facts.mappingComplete, facts.cached, ready); } catch (...) {}
            }
            return ready;
        }();
        if (!current()) return idle();
        if (!applied) {
            // Prepare the external RTV permutation without publishing any color.
            if (commit) Fail(true);
            return warmup();
        }
        if (!MakeTargets(lease, e, colorFormats, hasDepth ? &depthDesc : nullptr)) return reject(Reason::NoRuntime);
        auto& t = *static_cast<Targets*>(s.targets.get());
        if (!lease.KeepAlive(s.targets)) return reject(Reason::Capacity);
        if (hasDepth && t.boundReload != s.reload) {
            // One semantic update per private runtime/reload/shape generation,
            // BEFORE producer reads. No per-frame Clear/Begin/End rebinding.
            const auto reload = s.reload;
            r->update_texture_bindings("DEPTH", t.depthSrv, t.depthSrv);
            if (!current() || s.reload != reload) return idle();
            const bool reflected = ReflectDepth(r, t);
            if (!current() || s.reload != reload) return idle();
            t.depthReflected = reflected; // no declaration means color-only effects, not a failed render
            t.boundReload = reload;
        }
        auto input = std::make_shared<InputHold>();
        input->uiMask = e.uiMask;
        input->producer = e.nativeColor.producer; input->transport = e.nativeColor.transport;
        input->picture = e.beforeNrNativeColor.picture; input->submission = e.ready.submission;
        input->depthReader = a.reader;
        input->color = e.color; input->depth = a.resource; input->ready = e.ready.fence;
        if (hasDepth) reader = std::make_shared<Depth::Detail::ReadLifetime>(a.reader);
        if (!lease.KeepAlive(input,InputHold::Observe) || (reader && !lease.KeepAlive(reader))) return reject(Reason::Capacity);
        if (!current() || !sourceCurrent() || (hasDepth && !a.reader->Validate(a))) {
            Fail(true); return idle();
        }
        if (e.ready.queue != e.queue && FAILED(e.queue->Wait(e.ready.fence, e.ready.value)))
            return reject(Reason::FenceUnavailable);
        // No retry once commands may be recorded. Unknown completion keeps the
        // actual owned pool entry, content leases and private SDK resources.
        s.lastStream = e.stream; s.lastGeneration = e.generation; s.lastFrame = e.frame;
        result.attempt.state = SubmissionState::RecordedOrUncertain;
        s.uncertain = true;
        if (reader && !reader->Begin()) return reject(Reason::InternalFailure);
        const api::resource color{reinterpret_cast<uint64_t>(e.color)}, depth{reinterpret_cast<uint64_t>(a.resource)};
        const auto depthUsage = a.entryState == D3D12_RESOURCE_STATE_COMMON ? api::resource_usage::general :
            static_cast<api::resource_usage>(a.entryState);
        const auto colorUsage = e.state == D3D12_RESOURCE_STATE_COMMON ? api::resource_usage::general :
            static_cast<api::resource_usage>(e.state);
        if (reader) {
            list->barrier(depth, depthUsage, api::resource_usage::copy_source);
            list->barrier(t.depth, api::resource_usage::shader_resource, api::resource_usage::copy_dest);
            list->copy_resource(depth, t.depth);
            list->barrier(t.depth, api::resource_usage::copy_dest, api::resource_usage::shader_resource);
            list->barrier(depth, api::resource_usage::copy_source, depthUsage);
            reader->Restored();
        }
        list->barrier(color, colorUsage, api::resource_usage::copy_source);
        list->copy_resource(color, t.color);
        list->barrier(t.color, api::resource_usage::copy_dest, api::resource_usage::render_target);
        // No late HUD from this frame exists at BeforeSR.
        auto* nativeColor=reinterpret_cast<ID3D12Resource*>(t.color.handle);
        const bool protectUi=e.stage!=Stage::BeforeSr &&
            DrawUiMask::Begin(e.uiMask,DrawUiMask::Consumer::Filter,lease.NativeList(),nativeColor,D3D12_RESOURCE_STATE_RENDER_TARGET);
        const auto before = s.techniques;
        uint64_t count = 0;
        {
            struct Rendering {
                State& s;
                Rendering(State& state, api::command_list* list, const Depth::Access* a):s(state) {
                    s.rendering = true; s.renderingList = list; s.renderingReload = s.reload;
                    s.renderingDepth = a; s.depthVerified = s.depthLost = false;
                }
                ~Rendering() { s.rendering = false; s.renderingList = nullptr; s.renderingDepth = nullptr; }
            } rendering(s, list, hasDepth ? &a : nullptr);
            if (e.stage == Stage::BeforeSr) {
                Selection::RenderScope render(s.selection, r, lease.Generation(), list, t.linear, t.srgb);
                if (render.Ready()) r->render_effects(list, t.linear, t.srgb);
                count = render.RecordedTechniques();
            } else r->render_effects(list, t.linear, t.srgb);
        }
        if(protectUi)DrawUiMask::End(e.uiMask,DrawUiMask::Consumer::Filter,lease.NativeList(),nativeColor,D3D12_RESOURCE_STATE_RENDER_TARGET);
        if (e.stage != Stage::BeforeSr) count = s.techniques >= before ? s.techniques - before : 0;
        if (!sourceCurrent() || (hasDepth && !a.reader->Validate(a))) return reject(Reason::SelectionPending);
        if (hasDepth && t.depthReflected && (!current() || !DepthCurrent(r, t))) s.depthLost = true;
        const bool accepted = current() && !s.depthLost && count != 0;
        const bool copiedBack = commit && accepted;
        if (!accepted && commit) Fail(true); // revoke before even the scratch drain
        if (copiedBack) {
            list->barrier(t.color, api::resource_usage::render_target, api::resource_usage::copy_source);
            list->barrier(color, api::resource_usage::copy_source, api::resource_usage::copy_dest);
            list->copy_resource(t.color, color);
            list->barrier(color, api::resource_usage::copy_dest, colorUsage);
            list->barrier(t.color, api::resource_usage::copy_source, api::resource_usage::copy_dest);
        } else {
            list->barrier(color, api::resource_usage::copy_source, colorUsage);
            list->barrier(t.color, api::resource_usage::render_target, api::resource_usage::copy_dest);
        }
        // A callback may revoke during barriers/copy. Already recorded copyback
        // cannot be unrecorded or published under a newer configuration.
        if (copiedBack && (!current() || !sourceCurrent() || (hasDepth && !a.reader->Validate(a))))
            return reject(Reason::SelectionPending);
        OwnedRuntime::Completion done;
        if (lease.Submit(done, &s.detail, measured) != OwnedRuntime::Status::Ready || !done.fence || !done.value ||
            done.value == UINT64_MAX || done.fence->GetCompletedValue() == UINT64_MAX) {
            if (reader) reader->Quarantine();
            return reject(Reason::FenceUnavailable);
        }
        // Store the genuine fence even if the separate depth reader rejects its
        // completion binding. Natural restoration may drain this actual batch;
        // the rejected depth owner itself remains quarantined.
        s.latest = done;
        s.uncertain = false;
        if (!commit && !accepted) s.warmup.Pending(GetTickCount64());
        result.completion = done;
        if (reader && !reader->Complete(done.fence.Get(), done.value)) return reject(Reason::FenceUnavailable);
        // A real tail and accepted depth completion do not repair a revoked
        // producer. Such failure still follows the uncertain/quarantine path.
        if (!sourceCurrent()) return reject(Reason::SelectionPending);
        result.attempt.state = SubmissionState::Submitted;
        // Submit's update may revoke configuration AFTER recording/flush but
        // before its successful Signal. That revokes effects publication, not
        // the sealed producer transaction. Changed deliberately keeps private
        // reload/preset progress in Warming: preserve that decision here too.
        if (!current()) {
            const bool keepWarming = !commit && s.phase == Phase::Warming &&
                s.settings == settings && currentSettings() == settings;
            if (keepWarming) {
                // !accepted already scheduled a bounded retry above. Neither
                // path publishes a revoked selection or promotes it to Ready.
                if (accepted) s.warmup.Pending(GetTickCount64());
            } else {
                // A publishing runtime or hard lifecycle/settings revocation
                // still restores natural effects through its owner callback.
                Fail(true);
            }
            if (s.submitInvalidationReportGeneration != s.generation) {
                s.submitInvalidationReportGeneration = s.generation;
                try { LOG_INFO("HoYoShade R592 sealed submission selection revoked runtime=0x{:x} private_generation={} reload={} recorded_revision={} live_revision={} phase={} commit={} source_refresh_pending={} action={} per_generation_limit=1 effects_submission=not-certified pixels=UNKNOWN",
                    reinterpret_cast<uintptr_t>(r), s.generation, s.reload, revision, s.revision,
                    Text(s.phase), commit, s.sourceRefreshPending, keepWarming ? "retain-warming" : "restore-rewarm"); } catch (...) {}
            }
            result.attempt.naturalSuppressed = s.owesRestore;
            result.reason = Reason::SelectionPending;
            return result; // real completion retained; techniques/depth stay unpublished
        }
        result.depthAvailable = hasDepth && s.depthVerified && !s.depthLost;
        result.techniques = copiedBack ? count : 0;
        if (result.techniques) result.beforeSr.Recorded(e);
        result.attempt.naturalSuppressed = s.owesRestore;
        result.reason = accepted ? Reason::ExecutionUnverified : Reason::SelectionPending;
        if (accepted && !commit && current()) { s.warmup = {}; s.phase = Phase::Ready; }
        return result;
    } catch (...) { return reject(Reason::InternalFailure); }
}
}

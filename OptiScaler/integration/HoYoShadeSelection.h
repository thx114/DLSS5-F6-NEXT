#pragma once
#include "HoYoShadeApi.h"
#include "HoYoShadeOwnedRuntime.h"
#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <memory>
#include <cstring>
#include <exception>

namespace HoYoShade::Selection
{
namespace api = reshade::api;
constexpr size_t MaxItems = 4096, MaxScalars = 65536;
enum class CacheEvent { Init, Reload, Preset, Order, Destroy };
struct EventState {
    api::effect_runtime* runtime = nullptr;
    uint64_t revision = 0, topology = 0;
    bool reloadObserved = false;
};
// Protected by HoYoShade's existing executionMutex, including reentrant owned
// callbacks. No SDK calls, pointer dereferences, heap growth or eviction here.
// Missing events/table capacity never authorize cached handle reuse.
struct EventTable {
    std::array<EventState, 64> entries{};
    uint64_t serial = 0, mutation = 0;
    bool exhausted = false, mutationExhausted = false;
};
inline EventTable& CacheEvents() { static EventTable table; return table; }
// Independent of the bounded runtime table and its optional revision. In
// particular, revision==0 cannot hide destroy/reload during fallback calls.
inline void Mutated() noexcept
{
    auto& table = CacheEvents();
    if (table.mutation == UINT64_MAX) table.mutationExhausted = true;
    else ++table.mutation;
}
struct CallBoundary {
    uint64_t mutation = CacheEvents().mutation;
    bool Current() const noexcept { return !CacheEvents().mutationExhausted && mutation == CacheEvents().mutation; }
    template<class Function> bool Invoke(Function&& function) const
    {
        if (!Current()) return false;
        function();
        // Check BEFORE publishing returned handles or invoking another SDK
        // getter/setter. Recursive executionMutex does not prevent reentry.
        return Current();
    }
};
inline thread_local bool selectionBusy = false;
struct Operation {
    bool entered = !selectionBusy;
    Operation() noexcept { if (entered) selectionBusy = true; else Mutated(); }
    ~Operation() { if (entered) selectionBusy = false; }
    Operation(const Operation&) = delete;
    Operation& operator=(const Operation&) = delete;
};
struct EventWatch {
    api::effect_runtime* runtime = nullptr;
    const EventState* state = nullptr;
    EventState Read() const noexcept { return state && state->runtime == runtime ? *state : EventState{}; }
};
inline EventWatch Watch(api::effect_runtime* runtime) noexcept
{
    if (runtime) for (const auto& entry : CacheEvents().entries)
        if (entry.runtime == runtime) return {runtime, &entry};
    return {};
}
inline void Observe(api::effect_runtime* runtime, CacheEvent event) noexcept
{
    if (!runtime) return;
    Mutated(); // Before any capacity/exhaustion/destroy early return.
    auto& table = CacheEvents();
    EventState* state = nullptr;
    for (auto& entry : table.entries) if (entry.runtime == runtime) { state = &entry; break; }
    if (event == CacheEvent::Destroy) { if (state) *state = {}; return; }
    if (table.exhausted) return;
    if (!state) for (auto& entry : table.entries) if (!entry.runtime) { state = &entry; break; }
    if (!state) return;
    if (table.serial == UINT64_MAX) { table.entries = {}; table.exhausted = true; return; }
    if (event == CacheEvent::Init) *state = {};
    state->runtime = runtime;
    state->revision = ++table.serial;
    if (event != CacheEvent::Order) state->topology = state->revision;
    if (event == CacheEvent::Reload) state->reloadObserved = true;
}
struct Files {
    std::filesystem::path directory;
    // Immutable compile scope. A frozen runtime keeps its own exported preset;
    // a newly selected effect gets a fresh snapshot, never an in-place edit.
    std::vector<std::string> effects;
    ~Files() { if (directory.empty()) return; std::error_code ec; std::filesystem::remove_all(directory, ec); }
};
// First failed public API boundary/bounded graph operation; diagnostics grant no admission.
enum class CaptureReason {
    Pending, Ready, GlobalDisabled, NoEnabledTechniques, Reentrant, RuntimeUnavailable,
    BoundaryChanged, OwnerChanged, PresetPathUnavailable, BasePathUnavailable,
    ConfigEffects, ConfigTextures, ConfigDefines, ConfigLineBreak, TempPath,
    SnapshotDirectory, PresetExport, PrivateConfig, SnapshotUnavailable, CounterExhausted,
    TechniqueCapacity, TechniqueHandle, TechniqueIdentity, TechniqueDuplicate,
    TechniquesEmpty, UniformCapacity, UniformHandle, UniformIdentity, UniformDuplicate,
    UniformAnnotation, UniformShape, UniformScalarCapacity, UniformType,
    GraphUnavailable, ValuesUnavailable, OwnedTechniqueMissing, OwnedUniformMismatch,
    OwnedOrderPending, OwnedStateMismatch, Exception,
    SnapshotNotAttemptedCounterExhausted, OwnerExportPending
};
inline const char* Name(CaptureReason reason) noexcept
{
    switch (reason) {
    case CaptureReason::Pending: return "pending";
    case CaptureReason::Ready: return "ready";
    case CaptureReason::GlobalDisabled: return "global-disabled";
    case CaptureReason::NoEnabledTechniques: return "zero-live-enabled-techniques";
    case CaptureReason::Reentrant: return "selection-reentrant";
    case CaptureReason::RuntimeUnavailable: return "runtime-or-api-unavailable";
    case CaptureReason::BoundaryChanged: return "sdk-boundary-mutation";
    case CaptureReason::OwnerChanged: return "runtime-lifetime-or-revision-changed";
    case CaptureReason::PresetPathUnavailable: return "preset-path-unavailable";
    case CaptureReason::BasePathUnavailable: return "snapshot-base-path-unavailable";
    case CaptureReason::ConfigEffects: return "snapshot-effect-paths-unavailable";
    case CaptureReason::ConfigTextures: return "snapshot-texture-paths-unavailable";
    case CaptureReason::ConfigDefines: return "snapshot-defines-unavailable";
    case CaptureReason::ConfigLineBreak: return "snapshot-config-line-break";
    case CaptureReason::TempPath: return "snapshot-temp-path-unavailable";
    case CaptureReason::SnapshotDirectory: return "snapshot-directory-failed";
    case CaptureReason::PresetExport: return "snapshot-live-preset-export-failed";
    case CaptureReason::OwnerExportPending: return "snapshot-awaiting-live-owner-export";
    case CaptureReason::PrivateConfig: return "snapshot-private-config-failed";
    case CaptureReason::SnapshotUnavailable: return "private-snapshot-unavailable";
    case CaptureReason::CounterExhausted: return "selection-counter-exhausted";
    case CaptureReason::SnapshotNotAttemptedCounterExhausted: return "private-snapshot-not-attempted-counter-exhausted";
    case CaptureReason::TechniqueCapacity: return "technique-capacity";
    case CaptureReason::TechniqueHandle: return "technique-handle-or-runtime-invalid";
    case CaptureReason::TechniqueIdentity: return "technique-effect-or-name-invalid";
    case CaptureReason::TechniqueDuplicate: return "technique-identity-ambiguous";
    case CaptureReason::TechniquesEmpty: return "techniques-empty-timing-unknown";
    case CaptureReason::UniformCapacity: return "selected-uniform-capacity";
    case CaptureReason::UniformHandle: return "uniform-handle-or-runtime-invalid";
    case CaptureReason::UniformIdentity: return "selected-uniform-effect-or-name-invalid";
    case CaptureReason::UniformDuplicate: return "selected-uniform-identity-ambiguous";
    case CaptureReason::UniformAnnotation: return "uniform-source-annotation-invalid";
    case CaptureReason::UniformShape: return "uniform-shape-or-item-scalars-invalid";
    case CaptureReason::UniformScalarCapacity: return "selected-uniform-scalar-capacity";
    case CaptureReason::UniformType: return "selected-uniform-type-unsupported";
    case CaptureReason::GraphUnavailable: return "required-graph-incomplete";
    case CaptureReason::ValuesUnavailable: return "live-uniform-values-unavailable";
    case CaptureReason::OwnedTechniqueMissing: return "owned-enabled-technique-missing";
    case CaptureReason::OwnedUniformMismatch: return "owned-required-uniform-graph-mismatch";
    case CaptureReason::OwnedOrderPending: return "owned-order-unconfirmed";
    case CaptureReason::OwnedStateMismatch: return "owned-technique-state-unconfirmed";
    case CaptureReason::Exception: return "selection-exception";
    }
    return "unknown";
}
inline bool Reject(CaptureReason& first, CaptureReason reason) noexcept
{
    if (first == CaptureReason::Pending) first = reason;
    return false;
}
struct GraphFacts {
    CaptureReason reason = CaptureReason::Pending;
    size_t uniforms = 0, dynamicUniforms = 0, scalars = 0;
    bool enumComplete = false, complete = false;
    bool operator==(const GraphFacts&) const = default;
};
// Diagnostic result of the EXISTING API20 getter, never preset-file inference.
enum class PresetPathRead { NotRead, Empty, Oversize, Unterminated, Observed, Revoked };
inline const char* Name(PresetPathRead read) noexcept
{
    switch (read) {
    case PresetPathRead::NotRead: return "not-read";
    case PresetPathRead::Empty: return "getter-returned-empty";
    case PresetPathRead::Oversize: return "getter-size-out-of-range";
    case PresetPathRead::Unterminated: return "getter-path-unterminated";
    case PresetPathRead::Observed: return "getter-path-observed";
    case PresetPathRead::Revoked: return "read-revoked-by-capture-boundary";
    }
    return "UNKNOWN";
}
struct CaptureFacts {
    CaptureReason reason = CaptureReason::Pending;
    CaptureReason snapshotReason = CaptureReason::Pending;
    uint64_t snapshotRuntime = 0, snapshotLifetime = 0;
    PresetPathRead presetRead = PresetPathRead::NotRead;
    GraphFacts graph;
    size_t techniques = 0, enabled = 0, effects = 0;
    bool presetKnown = false, snapshotReady = false, globalKnown = false, globalEnabled = false;
    bool techniqueEnumComplete = false, statesComplete = false;
    bool topologyCached = false, uniformsCached = false;
    bool operator==(const CaptureFacts&) const = default;
};
struct ApplyFacts {
    CaptureReason reason = CaptureReason::Pending;
    GraphFacts graph;
    size_t techniques = 0;
    bool techniqueEnumComplete = false, mappingComplete = false, cached = false;
    bool operator==(const ApplyFacts&) const = default;
};
struct Effect {
    // Exact API20 file-name key; no basename extraction or case folding.
    std::string key;
    bool enabled = false, included = false;
};
struct Technique {
    std::string effect, name;
    api::effect_technique natural{}, owned{};
    bool enabled = false, identityAmbiguous = false;
    size_t effectIndex = 0;
};
struct Uniform {
    std::string effect, name;
    api::effect_uniform_variable natural{}, owned{};
    std::string annotationSource; // nonempty: core-owned dynamic value, never copied
    api::format type{};
    uint32_t rows = 0, columns = 0, length = 0;
    size_t count = 0;
    std::vector<float> floats;
    std::vector<int32_t> ints;
    std::vector<uint32_t> uints;
    std::unique_ptr<bool[]> bools;
};
struct Source {
    bool valid = false, enabled = false, refresh = true;
    std::string config, preset;
    std::shared_ptr<Files> files;
    std::vector<Technique> techniques;
    std::vector<Uniform> uniforms;
    std::vector<Effect> effects;
    bool techniquesKnown = false;
    CaptureReason techniqueFailure = CaptureReason::Pending;
    CaptureReason snapshotReason = CaptureReason::Pending; // retain last snapshot attempt within this refresh
    bool snapshotIoAttempted = false; // private-directory IO admission; never reset by a timer retry
    uint64_t snapshotRuntime = 0, snapshotLifetime = 0; // last private-snapshot attempt, not live authority
    GraphFacts graphFacts, ownedGraphFacts;
    CaptureFacts capture;
    ApplyFacts applied;
    std::vector<api::effect_technique> techniqueHandles, ownedOrder, previousOrder, ownedHandles;
    std::vector<std::pair<api::effect_technique, size_t>> ownedStates;
    bool mappingDirty = true, graphKnown = false, graphValid = false;
    std::vector<api::effect_uniform_variable> uniformHandles, ownedUniforms;
    api::effect_runtime* naturalRuntime = nullptr;
    api::effect_runtime* boundRuntime = nullptr;
    uint64_t naturalGeneration = 0, naturalRevision = 0, boundRevision = 0;
    uint64_t boundGeneration = 0, snapshots = 0, graphBuilds = 0, configReads = 0;
    // Counts describe this helper, not allocations/IO internal to the core API.
};
inline void InvalidateOwned(Source& source) noexcept
{
    source.ownedGraphFacts = {};
    source.mappingDirty = true; source.boundRuntime = nullptr; source.boundGeneration = 0; source.boundRevision = 0;
    // Notification only: never clear/resize containers borrowed by an active
    // SDK call. The next guarded rebuild replaces their revoked contents.
    for (auto& h : source.ownedHandles) h = {};
    for (auto& h : source.ownedUniforms) h = {};
    for (auto& h : source.ownedOrder) h = {};
    for (auto& h : source.previousOrder) h = {};
    for (auto& state : source.ownedStates) state.first = {};
    for (auto& item : source.techniques) item.owned = {};
    for (auto& item : source.uniforms) item.owned = {};
}
inline void InvalidateNatural(Source& source, bool refresh) noexcept
{
    source.valid = source.graphKnown = source.graphValid = source.techniquesKnown = false;
    source.techniqueFailure = CaptureReason::Pending; source.graphFacts = {};
    source.naturalRevision = 0;
    source.refresh = source.refresh || refresh;
    InvalidateOwned(source);
    for (auto& h : source.techniqueHandles) h = {};
    for (auto& h : source.uniformHandles) h = {};
    // Retain value/name storage, but revoke every borrowed handle immediately.
    // Reentrant notifications must not destroy a Capture/Apply loop's elements.
    for (auto& item : source.techniques) item.natural = {};
    for (auto& item : source.uniforms) item.natural = {};
}
inline void Invalidate(Source& source, api::effect_runtime* runtime, CacheEvent event) noexcept
{
    if (!runtime) return;
    if (source.naturalRuntime == runtime) {
        InvalidateNatural(source, event != CacheEvent::Order);
        if (event == CacheEvent::Destroy) { source.naturalRuntime = nullptr; source.naturalGeneration = 0; }
    }
    if (source.boundRuntime == runtime) {
        if (event == CacheEvent::Order) {
            // BEFORE notification, potentially vetoed or rewritten later.
            // Invalidate only; it never proves a resulting rendering order.
            source.mappingDirty = true; source.boundRevision = 0;
        } else InvalidateOwned(source);
    }
}
inline std::string Utf8(const std::filesystem::path& path)
{
    const auto value = path.u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}
enum class ConfigRead { Missing, Empty, Value, Failed, Oversize, Truncated, Changed };
inline bool OptionalConfig(ConfigRead read) noexcept
{
    return read == ConfigRead::Missing || read == ConfigRead::Empty || read == ConfigRead::Value;
}
inline ConfigRead Config(const CoreApi& core, api::effect_runtime* runtime,
                         const char* key, std::string& value, const CallBoundary& call = CallBoundary{})
{
    value.clear();
    if (!core.getConfigValue) return ConfigRead::Failed;
    constexpr size_t MaxConfigBytes = 65536;
    size_t required = SIZE_MAX;
    bool found = false;
    if (!call.Invoke([&] { found = core.getConfigValue(nullptr, runtime, "GENERAL", key, nullptr, &required); })) return ConfigRead::Changed;
    // API20 checked with the pinned HoYo core in filters/abi-host-01: false AND
    // size=0 is Missing (also parsed empty INI fields); an explicit empty element
    // is true/size=2. False with untouched/nonzero length is a failure.
    if (!found) return required == 0 ? ConfigRead::Missing : ConfigRead::Failed;
    if (required > MaxConfigBytes) return ConfigRead::Oversize;
    if (required < 2) return ConfigRead::Failed;
    // Query includes list terminator + final sentinel. Data read returns copied
    // bytes excluding the final sentinel, but including the list terminator.
    std::string buffer(required, '\x7f');
    size_t copied = buffer.size();
    bool copiedValue = false;
    if (!call.Invoke([&] { copiedValue = core.getConfigValue(nullptr, runtime, "GENERAL", key, buffer.data(), &copied); })) return ConfigRead::Changed;
    if (!copiedValue) return ConfigRead::Failed;
    if (!copied || copied >= buffer.size() || copied != required - 1 ||
        buffer[copied] != '\0' || buffer[copied - 1] != '\0') return ConfigRead::Truncated;
    // The API can report success after copying only capacity-1 bytes. A grown
    // list cut exactly on an element boundary also has two trailing NULs, so
    // confirm its required size before accepting this snapshot (no retry/IO loop).
    size_t confirmed = SIZE_MAX;
    bool confirmedValue = false;
    if (!call.Invoke([&] { confirmedValue = core.getConfigValue(nullptr, runtime, "GENERAL", key, nullptr, &confirmed); })) return ConfigRead::Changed;
    if (!confirmedValue) return ConfigRead::Failed;
    if (confirmed > MaxConfigBytes) return ConfigRead::Oversize;
    if (confirmed != required) return ConfigRead::Changed;
    buffer.resize(copied);
    while (!buffer.empty() && !buffer.back()) buffer.pop_back();
    value = std::move(buffer);
    return value.empty() ? ConfigRead::Empty : ConfigRead::Value;
}
inline std::string List(const std::string& values, const std::filesystem::path* base = nullptr)
{
    std::string result;
    for (size_t start = 0; start < values.size();) {
        auto end = values.find('\0', start);
        if (end == std::string::npos) end = values.size();
        auto value = values.substr(start, end - start);
        if (base && !value.empty()) {
            auto path = std::filesystem::u8path(value);
            if (path.is_relative()) path = *base / path;
            value = Utf8(path.lexically_normal());
        }
        if (!result.empty()) result += ',';
        // ReShade INI list escaping: literal commas are doubled.
        for (char c : value) { result += c; if (c == ',') result += c; }
        start = end + 1;
    }
    return result;
}
inline bool SnapshotEffectsCurrent(const Source& source) noexcept
{
    if (!source.files) return false;
    size_t count = 0;
    for (const auto& effect : source.effects) {
        if (!effect.enabled) continue;
        ++count;
        if (std::find(source.files->effects.begin(), source.files->effects.end(), effect.key) == source.files->effects.end())
            return false;
    }
    return count == source.files->effects.size();
}
// This is call-site authority, never inferred from executionMutex, cached TID
// or a saved runtime pointer. ExistingSnapshot may read current selection under
// its existing contract, but only a live core owner callback may export files.
enum class CaptureAccess { ExistingSnapshot, OwnerCallback };
inline bool Snapshot(api::effect_runtime* runtime, const CoreApi& core, Source& out, const char* selected,
                     CaptureAccess access, CaptureReason& reason, const CallBoundary& call = CallBoundary{})
{
    if (access != CaptureAccess::OwnerCallback) return Reject(reason, CaptureReason::OwnerExportPending);
    char baseBuffer[32768]{}; size_t baseSize = sizeof(baseBuffer);
    if (!call.Invoke([&] { core.getBasePath(baseBuffer, &baseSize); })) return Reject(reason, CaptureReason::BoundaryChanged);
    if (!baseBuffer[0] || baseSize >= sizeof(baseBuffer)) return Reject(reason, CaptureReason::BasePathUnavailable);
    const auto base = std::filesystem::u8path(baseBuffer);
    std::string effects, textures, defines;
    ++out.configReads;
    if (Config(core, runtime, "EffectSearchPaths", effects, call) != ConfigRead::Value)
        return Reject(reason, call.Current() ? CaptureReason::ConfigEffects : CaptureReason::BoundaryChanged);
    if (!OptionalConfig(Config(core, runtime, "TextureSearchPaths", textures, call)))
        return Reject(reason, call.Current() ? CaptureReason::ConfigTextures : CaptureReason::BoundaryChanged);
    if (!OptionalConfig(Config(core, runtime, "PreprocessorDefinitions", defines, call)))
        return Reject(reason, call.Current() ? CaptureReason::ConfigDefines : CaptureReason::BoundaryChanged);
    effects = List(effects, &base); textures = List(textures, &base); defines = List(defines);
    if ((effects + textures + defines).find_first_of("\r\n") != std::string::npos) return Reject(reason, CaptureReason::ConfigLineBreak);
    wchar_t temp[32768]{};
    const auto count = GetTempPathW(static_cast<DWORD>(std::size(temp)), temp);
    if (!count || count >= std::size(temp)) return Reject(reason, CaptureReason::TempPath);
    static std::atomic<uint64_t> serial{0};
    auto files = std::make_shared<Files>();
    files->directory = std::filesystem::path(temp) / (L"OptiScaler-HoYoShade-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(++serial));
    out.snapshotIoAttempted = true; // first filesystem attempt, including a failed create
    // Do not remove a directory we failed to create (including a name collision).
    if (!CreateDirectoryW(files->directory.c_str(), nullptr)) { files->directory.clear(); return Reject(reason, CaptureReason::SnapshotDirectory); }
    const auto privatePreset = files->directory / L"preset.ini", privateConfig = files->directory / L"ReShade.ini";
    // API20 exports CURRENT loaded state, including selection, static values
    // and preset definitions. Copying the user's on-disk preset can select a
    // different effect set when its live changes have not yet been saved.
    // This new destination does not exist: the natural preset is never saved,
    // switched or passed to a setter. Export runs only in the live owner call.
    const auto destination = Utf8(privatePreset);
    if (!call.Invoke([&] { runtime->export_current_preset(destination.c_str()); }))
        return Reject(reason, CaptureReason::BoundaryChanged);
    if (!std::filesystem::is_regular_file(privatePreset)) return Reject(reason, CaptureReason::PresetExport);
    for (const auto& effect : out.effects) if (effect.enabled) files->effects.push_back(effect.key);
    std::ofstream config(privateConfig, std::ios::binary);
    config << "[ADDON]\nAddonPath=" << Utf8(files->directory / L"no-addons") <<
        "\n\n[GENERAL]\nEffectSearchPaths=" << effects << "\nTextureSearchPaths=" << textures <<
        "\nPreprocessorDefinitions=" << defines << "\nPresetPath=" << Utf8(privatePreset) <<
        "\nIntermediateCachePath=" << Utf8(files->directory / L"cache") <<
        "\nNoEffectCache=1\nPerformanceMode=0\nSkipLoadingDisabledEffects=1\nPresetTransitionDuration=0\n"
        "\n[OVERLAY]\nShowFPS=0\nShowFrameTime=0\n";
    config.close();
    if (!config) return Reject(reason, CaptureReason::PrivateConfig);
    // Only private files ever reach CreateEffectRuntime. Neither the natural
    // config nor its preset is passed to an owned setter/save/create operation.
    if (!call.Current()) return Reject(reason, CaptureReason::BoundaryChanged);
    out.config = Utf8(privateConfig); out.preset = selected; out.files = std::move(files);
    InvalidateOwned(out);
    ++out.snapshots;
    return true;
}
// Only techniques are global. Uniform enumeration ALWAYS receives one exact
// enabled-effect key, deduplicated from the complete live technique topology.
inline bool EnumerateTechniques(api::effect_runtime* runtime, std::vector<api::effect_technique>& handles,
                                size_t& visited, CaptureReason& reason, const CallBoundary& call)
{
    bool complete = true;
    if (!call.Invoke([&] {
        runtime->enumerate_techniques(nullptr, [&](api::effect_runtime* owner, api::effect_technique h) {
            if (!call.Current()) { complete = false; Reject(reason, CaptureReason::BoundaryChanged); return; }
            if (visited <= MaxItems) ++visited; // saturated lower bound after capacity
            if (!complete) return;
            if (owner != runtime || !h.handle) { complete = Reject(reason, CaptureReason::TechniqueHandle); return; }
            if (handles.size() == MaxItems) { complete = Reject(reason, CaptureReason::TechniqueCapacity); return; }
            if (std::find(handles.begin(), handles.end(), h) != handles.end()) {
                complete = Reject(reason, CaptureReason::TechniqueDuplicate); return;
            }
            handles.push_back(h);
        });
    })) return Reject(reason, CaptureReason::BoundaryChanged);
    return complete;
}
template<class Read>
inline bool Identity(Read&& read, std::string& value, CaptureReason& reason,
                     CaptureReason failure, const CallBoundary& call)
{
    char text[1024]{}; size_t size = sizeof(text);
    if (!call.Invoke([&] { read(text, &size); })) return Reject(reason, CaptureReason::BoundaryChanged);
    if (!text[0] || !size || size >= sizeof(text) || !std::memchr(text, 0, sizeof(text)))
        return Reject(reason, failure);
    value = text;
    return true;
}
inline bool TechniqueGraph(api::effect_runtime* runtime, const std::vector<api::effect_technique>& handles,
                           std::vector<Technique>& techniques, std::vector<Effect>& effects,
                           CaptureReason& reason, const CallBoundary& call)
{
    for (auto h : handles) {
        Technique item; item.natural = h;
        if (!Identity([&](char* p, size_t* n) { runtime->get_technique_effect_name(h, p, n); },
            item.effect, reason, CaptureReason::TechniqueIdentity, call) ||
            !Identity([&](char* p, size_t* n) { runtime->get_technique_name(h, p, n); },
            item.name, reason, CaptureReason::TechniqueIdentity, call)) return false;
        for (auto& other : techniques) {
            if (other.effect == item.effect && other.name == item.name)
                other.identityAmbiguous = item.identityAmbiguous = true;
        }
        // Cache immutable identity facts; only selected pairing depends on
        // reversibility. Unselected handles still participate in state/order.
        // The API20 key is a filename, not a unique filesystem path.
        api::effect_technique found{};
        if (!call.Invoke([&] { found = runtime->find_technique(item.effect.c_str(), item.name.c_str()); }))
            return Reject(reason, CaptureReason::BoundaryChanged);
        if (found != h) item.identityAmbiguous = true;
        auto effect = std::find_if(effects.begin(), effects.end(), [&](const auto& e) { return e.key == item.effect; });
        item.effectIndex = static_cast<size_t>(effect - effects.begin());
        if (effect == effects.end()) effects.push_back({item.effect});
        techniques.push_back(std::move(item));
    }
    return call.Current() || Reject(reason, CaptureReason::BoundaryChanged);
}
inline bool UniformGraph(api::effect_runtime* runtime, const std::vector<Effect>& effects,
                         std::vector<api::effect_uniform_variable>& handles, std::vector<Uniform>& uniforms,
                         GraphFacts& facts, const CallBoundary& call)
{
    // Callers lend only local vectors/strings. Reentrant invalidation cannot
    // destroy an active SDK argument even when it replaces the whole Source.
    for (const auto& effect : effects) {
        if (!effect.enabled) continue;
        const auto begin = handles.size();
        bool complete = true;
        if (!call.Invoke([&] {
            runtime->enumerate_uniform_variables(effect.key.c_str(),
                [&](api::effect_runtime* owner, api::effect_uniform_variable h) {
                    if (!call.Current()) { complete = false; Reject(facts.reason, CaptureReason::BoundaryChanged); return; }
                    if (facts.uniforms <= MaxItems) ++facts.uniforms;
                    if (!complete) return;
                    if (owner != runtime || !h.handle) { complete = Reject(facts.reason, CaptureReason::UniformHandle); return; }
                    if (handles.size() == MaxItems) { complete = Reject(facts.reason, CaptureReason::UniformCapacity); return; }
                    if (std::find(handles.begin(), handles.end(), h) != handles.end()) {
                        complete = Reject(facts.reason, CaptureReason::UniformDuplicate); return;
                    }
                    handles.push_back(h);
                });
        })) return Reject(facts.reason, CaptureReason::BoundaryChanged);
        if (!complete) return false;
        // Include ALL uniforms of this effect, even for one enabled technique.
        // An effect with no uniforms is complete after its empty enumeration.
        for (size_t i = begin; i < handles.size(); ++i) {
            const auto h = handles[i]; Uniform item; item.natural = h;
            if (!Identity([&](char* p, size_t* n) { runtime->get_uniform_variable_effect_name(h, p, n); },
                item.effect, facts.reason, CaptureReason::UniformIdentity, call) ||
                !Identity([&](char* p, size_t* n) { runtime->get_uniform_variable_name(h, p, n); },
                item.name, facts.reason, CaptureReason::UniformIdentity, call)) return false;
            if (item.effect != effect.key) return Reject(facts.reason, CaptureReason::UniformIdentity);
            if (std::any_of(uniforms.begin(), uniforms.end(), [&](const auto& other) {
                return other.effect == item.effect && other.name == item.name;
            })) return Reject(facts.reason, CaptureReason::UniformDuplicate);
            api::effect_uniform_variable found{};
            if (!call.Invoke([&] { found = runtime->find_uniform_variable(item.effect.c_str(), item.name.c_str()); }))
                return Reject(facts.reason, CaptureReason::BoundaryChanged);
            if (found != h) return Reject(facts.reason, CaptureReason::UniformDuplicate);
            char annotation[1024]{}; size_t size = sizeof(annotation); bool present = false;
            if (!call.Invoke([&] { present = runtime->get_annotation_string_from_uniform_variable(h, "source", annotation, &size); }))
                return Reject(facts.reason, CaptureReason::BoundaryChanged);
            if (present) {
                if (size >= sizeof(annotation) || !std::memchr(annotation, 0, sizeof(annotation)))
                    return Reject(facts.reason, CaptureReason::UniformAnnotation);
                item.annotationSource = annotation;
            }
            if (!call.Invoke([&] { runtime->get_uniform_variable_type(h, &item.type, &item.rows, &item.columns, &item.length); }))
                return Reject(facts.reason, CaptureReason::BoundaryChanged);
            if (!item.annotationSource.empty()) ++facts.dynamicUniforms;
            else {
                // Check factors before multiplication (three uint32_t factors
                // can overflow even uint64_t).
                const auto length = (std::max)(1u, item.length);
                if (!item.rows || !item.columns || item.rows > 4096 || item.columns > 4096 / item.rows ||
                    length > 4096 / (item.rows * item.columns)) return Reject(facts.reason, CaptureReason::UniformShape);
                item.count = static_cast<size_t>(item.rows) * item.columns * length;
                if (item.count > MaxScalars - facts.scalars) return Reject(facts.reason, CaptureReason::UniformScalarCapacity);
                facts.scalars += item.count;
                switch (item.type) {
                case api::format::r16_float: case api::format::r32_float: item.floats.resize(item.count); break;
                case api::format::r16_sint: case api::format::r32_sint: item.ints.resize(item.count); break;
                case api::format::r16_uint: case api::format::r32_uint: item.uints.resize(item.count); break;
                case api::format::r32_typeless: item.bools = std::make_unique<bool[]>(item.count); break;
                default: return Reject(facts.reason, CaptureReason::UniformType);
                }
            }
            uniforms.push_back(std::move(item));
        }
    }
    if (!call.Current()) return Reject(facts.reason, CaptureReason::BoundaryChanged);
    facts.enumComplete = facts.complete = true;
    facts.reason = CaptureReason::Ready;
    return true;
}
// Each overload makes exactly one public SDK call; callers put it inside an
// Invoke boundary. SDK arguments always point to local, stable scalar storage.
inline void GetValues(api::effect_runtime* r, api::effect_uniform_variable h, float* p, size_t n) { r->get_uniform_value_float(h, p, n); }
inline void GetValues(api::effect_runtime* r, api::effect_uniform_variable h, int32_t* p, size_t n) { r->get_uniform_value_int(h, p, n); }
inline void GetValues(api::effect_runtime* r, api::effect_uniform_variable h, uint32_t* p, size_t n) { r->get_uniform_value_uint(h, p, n); }
inline void GetValues(api::effect_runtime* r, api::effect_uniform_variable h, bool* p, size_t n) { r->get_uniform_value_bool(h, p, n); }
inline void SetValues(api::effect_runtime* r, api::effect_uniform_variable h, const float* p, size_t n) { r->set_uniform_value_float(h, p, n); }
inline void SetValues(api::effect_runtime* r, api::effect_uniform_variable h, const int32_t* p, size_t n) { r->set_uniform_value_int(h, p, n); }
inline void SetValues(api::effect_runtime* r, api::effect_uniform_variable h, const uint32_t* p, size_t n) { r->set_uniform_value_uint(h, p, n); }
inline void SetValues(api::effect_runtime* r, api::effect_uniform_variable h, const bool* p, size_t n) { r->set_uniform_value_bool(h, p, n); }
template<class Scalar>
inline bool CaptureValues(api::effect_runtime* runtime, api::effect_uniform_variable h, Scalar* destination,
                          size_t count, const CallBoundary& call)
{
    if (!count || count > 4096 || !h.handle) return false;
    Scalar values[4096];
    if (!call.Invoke([&] { GetValues(runtime, h, values, count); })) return false;
    // The destination may have been revoked/freed by a lifecycle callback;
    // it is accessed only after Invoke proves no such callback ran.
    std::copy_n(values, count, destination);
    return true;
}
template<class Scalar>
inline bool SyncValues(api::effect_runtime* runtime, api::effect_uniform_variable h, const Scalar* desired,
                       size_t count, const CallBoundary& call)
{
    if (!count || count > 4096 || !h.handle) return false;
    Scalar values[4096];
    if (!call.Invoke([&] { GetValues(runtime, h, values, count); })) return false;
    if (!std::equal(desired, desired + count, values)) {
        std::copy_n(desired, count, values);
        if (!call.Invoke([&] { SetValues(runtime, h, values, count); })) return false;
    }
    return true;
}
// Under executionMutex. Cache only reflected topology/metadata
// after the existing observed reload contract. Poll live state/values every
// Capture; changing the required effect set rebuilds only the uniform graph.
// No new retry loop, steady-state container, file copy, hash/stat or compile.
inline bool CacheGraphFailure(CaptureReason reason) noexcept
{
    switch (reason) {
    case CaptureReason::UniformIdentity: case CaptureReason::UniformDuplicate:
    case CaptureReason::UniformAnnotation: case CaptureReason::UniformShape:
    case CaptureReason::UniformScalarCapacity: case CaptureReason::UniformType: return true;
    default: return false; // enum/capacity/mutation failures retain callback retry cadence
    }
}
inline void Capture(api::effect_runtime* runtime, const CoreApi& core, Source& out, bool reload = false,
                     uint64_t generation = 0, CaptureAccess access = CaptureAccess::ExistingSnapshot) noexcept
{
    Operation operation;
    CaptureFacts facts;
    struct Publish {
        Source& out; CaptureFacts& facts;
        ~Publish() {
            facts.snapshotReason = out.snapshotReason;
            facts.snapshotRuntime = out.snapshotRuntime; facts.snapshotLifetime = out.snapshotLifetime;
            if (facts.reason == CaptureReason::BoundaryChanged || facts.reason == CaptureReason::OwnerChanged ||
                facts.reason == CaptureReason::Exception || facts.reason == CaptureReason::Reentrant) {
                // Earlier scalar reads are not a current-live diagnostic after
                // reentry/lifetime revocation. Graph counters remain attempt facts.
                facts.presetKnown = facts.globalKnown = facts.statesComplete = false;
                if (facts.presetRead == PresetPathRead::Observed) facts.presetRead = PresetPathRead::Revoked;
            }
            out.capture = facts;
        }
    } publish{out, facts};
    out.valid = false; // never publish partial selection/donor authority
    if (!operation.entered) { Reject(facts.reason, CaptureReason::Reentrant); return; }
    const CallBoundary call;
    try {
        if (!runtime || !core.getConfigValue || !core.getBasePath) { Reject(facts.reason, CaptureReason::RuntimeUnavailable); return; }
        if (!call.Current()) { Reject(facts.reason, CaptureReason::BoundaryChanged); return; }
        if (out.naturalRuntime != runtime || out.naturalGeneration != generation) {
            InvalidateNatural(out, true);
            out.naturalRuntime = runtime; out.naturalGeneration = generation;
        }
        if (reload) InvalidateNatural(out, true);
        const auto events = Watch(runtime).Read();
        char selected[32768]{}; size_t size = sizeof(selected);
        facts.presetRead = PresetPathRead::Revoked; // an interrupted getter is not an empty/not-read result
        if (!call.Invoke([&] { runtime->get_current_preset_path(selected, &size); })) { Reject(facts.reason, CaptureReason::BoundaryChanged); return; }
        const bool presetKnown = *selected && size < sizeof(selected) && std::memchr(selected, 0, sizeof(selected));
        if (!presetKnown) {
            facts.presetRead = !*selected ? PresetPathRead::Empty :
                size >= sizeof(selected) ? PresetPathRead::Oversize : PresetPathRead::Unterminated;
            selected[0] = 0; // unavailable path never becomes a private-config input
        } else {
            facts.presetRead = PresetPathRead::Observed;
            facts.presetKnown = true;
        }
        bool enabled = false;
        if (!call.Invoke([&] { enabled = runtime->get_effects_state(); })) { Reject(facts.reason, CaptureReason::BoundaryChanged); return; }
        facts.globalKnown = true; facts.globalEnabled = enabled; out.enabled = enabled;
        const bool refresh = out.refresh || out.preset != selected;
        if (refresh) {
            // A failed same-path snapshot is not retried from ordinary Present.
            InvalidateNatural(out, false); out.refresh = false;
            out.preset = selected; out.config.clear(); out.files.reset();
            out.snapshotReason = CaptureReason::Pending;
            out.snapshotIoAttempted = false;
        }
        // Path/file readiness is independent of live selection. Reflect the
        // natural handles and states first, even when no preset path is exposed.
        facts.snapshotReady = out.files && !out.config.empty();
        const bool cacheable = generation && events.revision && events.reloadObserved;
        if (cacheable && out.graphKnown && !out.techniquesKnown && out.naturalRevision == events.revision &&
            out.techniqueFailure != CaptureReason::Pending) {
            // Preserve the previous unsupported-topology cache policy; no new
            // rapid retries of immutable reflection failures.
            facts.topologyCached = true; facts.techniqueEnumComplete = true;
            facts.techniques = out.techniqueHandles.size();
            Reject(facts.reason, out.techniqueFailure); return;
        }
        if (!cacheable || !out.techniquesKnown || out.naturalRevision != events.revision) {
            InvalidateNatural(out, false);
            std::vector<api::effect_technique> handles;
            if (!EnumerateTechniques(runtime, handles, facts.techniques, facts.reason, call)) return;
            facts.techniqueEnumComplete = true;
            if (handles.empty()) { Reject(facts.reason, CaptureReason::TechniquesEmpty); return; }
            std::vector<Technique> techniques; std::vector<Effect> effects;
            if (!TechniqueGraph(runtime, handles, techniques, effects, facts.reason, call)) {
                if (call.Current()) {
                    out.techniqueHandles = std::move(handles);
                    out.graphKnown = true; out.graphValid = false;
                    out.naturalRevision = events.revision; out.techniqueFailure = facts.reason;
                }
                return;
            }
            out.techniqueHandles = std::move(handles); out.techniques = std::move(techniques);
            out.effects = std::move(effects); out.techniquesKnown = true;
            out.naturalRevision = events.revision;
        } else {
            facts.topologyCached = true; facts.techniqueEnumComplete = true;
            facts.techniques = out.techniques.size();
        }
        for (auto& effect : out.effects) effect.enabled = false;
        // Read ALL live states before selecting any uniform scope. No enabled
        // predicate is inferred from preset text or cached as an immutable fact.
        for (size_t i = 0; i < out.techniques.size(); ++i) {
            const auto h = out.techniques[i].natural;
            if (!h.handle) { Reject(facts.reason, CaptureReason::TechniqueHandle); return; }
            if (!call.Invoke([&] { enabled = runtime->get_technique_state(h); })) { Reject(facts.reason, CaptureReason::BoundaryChanged); return; }
            auto& item = out.techniques[i]; item.enabled = enabled;
            // A previously absent DISABLED target technique can become needed
            // without changing this shared effect's uniform set. Rebuild its
            // mapping at the next existing Apply instead of caching absence.
            if (enabled && !item.owned.handle && !out.mappingDirty) InvalidateOwned(out);
            if (item.effectIndex >= out.effects.size()) { Reject(facts.reason, CaptureReason::TechniqueIdentity); return; }
            if (enabled) { ++facts.enabled; out.effects[item.effectIndex].enabled = true; }
        }
        facts.statesComplete = true;
        // Disabled identities are not dependencies. Recheck the live selection
        // against cached ambiguity facts even when only a technique toggles.
        for (const auto& item : out.techniques) {
            if (item.enabled && item.identityAmbiguous) {
                Reject(facts.reason, CaptureReason::TechniqueDuplicate); return;
            }
        }
        if (!out.enabled || !facts.enabled) {
            // Keep inactive selections out of private snapshot IO, but retain
            // the required-scope change so a later enable rebuilds its graph.
            Reject(facts.reason, !out.enabled ? CaptureReason::GlobalDisabled : CaptureReason::NoEnabledTechniques);
            return; // no private files or runtime for an actually inactive selection
        }
        bool scopeChanged = false;
        for (const auto& effect : out.effects) {
            facts.effects += effect.enabled;
            scopeChanged |= effect.enabled != effect.included;
        }
        if (!out.graphKnown || scopeChanged) {
            out.graphKnown = out.graphValid = false;
            InvalidateOwned(out);
            for (auto& h : out.uniformHandles) h = {};
            for (auto& item : out.uniforms) item.natural = {};
            // Only a topology/selection change or the existing uncached/failure
            // path allocates. Never lend Source's strings across SDK callbacks.
            const auto effects = out.effects;
            std::vector<api::effect_uniform_variable> handles; std::vector<Uniform> uniforms;
            GraphFacts graph;
            const bool complete = UniformGraph(runtime, effects, handles, uniforms, graph, call);
            facts.graph = graph;
            if (!call.Current()) { Reject(facts.reason, CaptureReason::BoundaryChanged); return; }
            out.graphFacts = graph;
            if (!complete) {
                out.graphKnown = CacheGraphFailure(graph.reason);
                if (out.graphKnown) for (auto& effect : out.effects) effect.included = effect.enabled;
                Reject(facts.reason, graph.reason); return;
            }
            if (out.graphBuilds == UINT64_MAX) { Reject(facts.reason, CaptureReason::CounterExhausted); return; }
            out.uniformHandles = std::move(handles); out.uniforms = std::move(uniforms);
            for (auto& effect : out.effects) effect.included = effect.enabled;
            out.graphKnown = out.graphValid = true;
            ++out.graphBuilds;
        } else { facts.uniformsCached = true; facts.graph = out.graphFacts; }
        if (!out.graphValid) { Reject(facts.reason, out.graphFacts.reason); return; }
        for (size_t i = 0; i < out.uniforms.size(); ++i) {
            auto& u = out.uniforms[i];
            if (!out.enabled || !u.annotationSource.empty()) continue;
            const auto h = u.natural; const auto count = u.count;
            if ((!u.floats.empty() && !CaptureValues(runtime, h, u.floats.data(), count, call)) ||
                (!u.ints.empty() && !CaptureValues(runtime, h, u.ints.data(), count, call)) ||
                (!u.uints.empty() && !CaptureValues(runtime, h, u.uints.data(), count, call)) ||
                (u.bools && !CaptureValues(runtime, h, u.bools.get(), count, call))) {
                Reject(facts.reason, call.Current() ? CaptureReason::ValuesUnavailable : CaptureReason::BoundaryChanged); return;
            }
        }
        if (!call.Current()) { Reject(facts.reason, CaptureReason::BoundaryChanged); return; }
        const auto current = Watch(runtime).Read();
        if (out.naturalRuntime != runtime || out.naturalGeneration != generation || out.refresh ||
            current.revision != events.revision || current.topology != events.topology) {
            Reject(facts.reason, CaptureReason::OwnerChanged); return;
        }
        if (!out.graphKnown || !out.graphValid) { Reject(facts.reason, CaptureReason::GraphUnavailable); return; }
        if (!presetKnown) { Reject(facts.reason, CaptureReason::PresetPathUnavailable); return; }
        if (out.files && !SnapshotEffectsCurrent(out)) {
            // Real enabled-effect scope change, not a timer or equivalent setter.
            // Old immutable files stay alive with the preceding frozen runtime.
            out.files.reset(); out.config.clear(); facts.snapshotReady = false;
            out.snapshotReason = CaptureReason::Pending; out.snapshotIoAttempted = false;
        }
        if (!out.files || out.config.empty()) {
            // Only a boundary revoked BEFORE private IO may retry under the
            // existing sparse Capture schedule. Retain actual IO failures and
            // post-IO revocations until a real refresh/path change.
            if (out.snapshotReason != CaptureReason::Pending &&
                (out.snapshotReason != CaptureReason::BoundaryChanged || out.snapshotIoAttempted)) {
                Reject(facts.reason, out.snapshotReason); return;
            }
            // A non-callback refresh can revoke stale output and update live
            // selection, but cannot export through the cached runtime pointer.
            // Do not consume the one-shot IO attempt or poison snapshotReason:
            // the next normal owner callback can complete the pending snapshot.
            if (access != CaptureAccess::OwnerCallback) {
                Reject(facts.reason, CaptureReason::OwnerExportPending); return;
            }
            out.snapshotRuntime = reinterpret_cast<uintptr_t>(runtime); out.snapshotLifetime = generation;
            if (out.snapshots == UINT64_MAX || out.configReads == UINT64_MAX) {
                out.snapshotReason = CaptureReason::SnapshotNotAttemptedCounterExhausted;
                Reject(facts.reason, CaptureReason::CounterExhausted); return;
            }
            try {
                if (!Snapshot(runtime, core, out, selected, access, facts.reason, call)) {
                    out.snapshotReason = facts.reason; return;
                }
            } catch (...) {
                // Keep R201's exact last-attempt provenance through reentry.
                out.snapshotReason = CaptureReason::Exception;
                out.snapshotRuntime = reinterpret_cast<uintptr_t>(runtime); out.snapshotLifetime = generation;
                throw;
            }
            out.snapshotReason = CaptureReason::Ready;
        }
        if (!call.Current()) { Reject(facts.reason, CaptureReason::BoundaryChanged); return; }
        const auto afterSnapshot = Watch(runtime).Read();
        if (out.naturalRuntime != runtime || out.naturalGeneration != generation || out.refresh ||
            afterSnapshot.revision != events.revision || afterSnapshot.topology != events.topology) {
            Reject(facts.reason, CaptureReason::OwnerChanged); return;
        }
        facts.snapshotReady = true;
        out.valid = true; // includes complete required graph + current manual values
        facts.reason = CaptureReason::Ready;
    } catch (...) { Reject(facts.reason, CaptureReason::Exception); InvalidateNatural(out, false); }
}
// No natural SDK call here. Cache only an observed mapping/order; mutable values
// still compare against the owned runtime on every invocation.
// Cached live selection only; no preset IO, enumeration or cache mutation.
inline bool HasEnabledTechniques(const Source& source) noexcept
{
    return source.valid && source.enabled && std::any_of(source.techniques.begin(), source.techniques.end(),
        [](const auto& technique) { return technique.enabled; });
}
inline bool Apply(Source& source, api::effect_runtime* runtime, uint64_t generation)
{
    Operation operation;
    ApplyFacts facts;
    struct Publish {
        Source& source; ApplyFacts& facts;
        int exceptions = std::uncaught_exceptions();
        ~Publish() {
            if (std::uncaught_exceptions() > exceptions) Reject(facts.reason, CaptureReason::Exception);
            source.applied = facts;
        }
    } publish{source, facts};
    if (!operation.entered) return Reject(facts.reason, CaptureReason::Reentrant);
    if (!runtime) return Reject(facts.reason, CaptureReason::RuntimeUnavailable);
    if (!HasEnabledTechniques(source) || !source.graphKnown || !source.graphValid)
        return Reject(facts.reason, CaptureReason::GraphUnavailable);
    const CallBoundary call;
    if (!call.Current()) return Reject(facts.reason, CaptureReason::BoundaryChanged);
    const auto events = Watch(runtime).Read();
    if (source.boundRuntime != runtime || source.boundGeneration != generation || source.boundRevision != events.revision) {
        InvalidateOwned(source);
        source.boundRuntime = runtime; source.boundGeneration = generation;
    }
    bool enabled = false;
    if (!call.Invoke([&] { enabled = runtime->get_effects_state(); })) return Reject(facts.reason, CaptureReason::BoundaryChanged);
    if (enabled != source.enabled) {
        const bool desired = source.enabled;
        if (!call.Invoke([&] { runtime->set_effects_state(desired); })) return Reject(facts.reason, CaptureReason::BoundaryChanged);
        if (!call.Invoke([&] { enabled = runtime->get_effects_state(); })) return Reject(facts.reason, CaptureReason::BoundaryChanged);
        if (enabled != desired) return Reject(facts.reason, CaptureReason::OwnedStateMismatch);
    }
    const bool cacheable = generation && events.revision && events.reloadObserved;
    if (source.mappingDirty || !cacheable) {
        std::vector<api::effect_technique> handles;
        if (!EnumerateTechniques(runtime, handles, facts.techniques, facts.reason, call)) return false;
        facts.techniqueEnumComplete = true;
        if (handles.empty()) return Reject(facts.reason, CaptureReason::TechniquesEmpty);
        std::vector<Technique> targetTechniques; std::vector<Effect> targetEffects;
        if (!TechniqueGraph(runtime, handles, targetTechniques, targetEffects, facts.reason, call)) return false;
        std::vector<api::effect_technique> techniques(source.techniques.size()), desiredOrder, actualOrder;
        std::vector<std::pair<api::effect_technique, size_t>> states;
        for (size_t i = 0; i < techniques.size(); ++i) {
            const auto& item = source.techniques[i];
            if (item.identityAmbiguous) {
                if (item.enabled) return Reject(facts.reason, CaptureReason::TechniqueDuplicate);
                continue;
            }
            const auto found = std::find_if(targetTechniques.begin(), targetTechniques.end(), [&](const auto& t) {
                return t.effect == item.effect && t.name == item.name;
            });
            if (found == targetTechniques.end()) {
                if (item.enabled) return Reject(facts.reason, CaptureReason::OwnedTechniqueMissing);
                continue;
            }
            if (found->identityAmbiguous) {
                if (item.enabled) return Reject(facts.reason, CaptureReason::TechniqueDuplicate);
                continue; // Every unmatched target handle below is still disabled.
            }
            techniques[i] = found->natural; desiredOrder.push_back(found->natural);
        }
        for (auto h : handles) {
            size_t index = 0;
            while (index < techniques.size() && techniques[index] != h) ++index;
            states.emplace_back(h, index);
            if (index < techniques.size()) actualOrder.push_back(h);
        }
        if (actualOrder != desiredOrder) {
            // BEFORE notification is not success. Keep the existing idle-owned
            // drain/retry path and do not publish a partial mapping.
            auto request = desiredOrder;
            for (const auto& [h, index] : states) if (index == techniques.size()) request.push_back(h);
            if (!call.Current()) return Reject(facts.reason, CaptureReason::BoundaryChanged);
            try { runtime->reorder_techniques(request.size(), request.data()); }
            catch (...) { Mutated(); InvalidateOwned(source); return Reject(facts.reason, CaptureReason::Exception); }
            Mutated(); InvalidateOwned(source);
            return Reject(facts.reason, CaptureReason::OwnedOrderPending);
        }
        // EXACTLY the source's current required effects, never the target's
        // preset or its possibly stale enabled set. Extra target techniques
        // remain in states, so all required disables/enables are still applied.
        const auto effects = source.effects;
        std::vector<api::effect_uniform_variable> uniformHandles;
        std::vector<Uniform> targetUniforms;
        if (!UniformGraph(runtime, effects, uniformHandles, targetUniforms, facts.graph, call)) {
            Reject(facts.reason, facts.graph.reason); return false;
        }
        if (targetUniforms.size() != source.uniforms.size()) return Reject(facts.reason, CaptureReason::OwnedUniformMismatch);
        std::vector<api::effect_uniform_variable> uniforms(source.uniforms.size());
        for (size_t i = 0; i < uniforms.size(); ++i) {
            const auto& item = source.uniforms[i];
            const auto found = std::find_if(targetUniforms.begin(), targetUniforms.end(), [&](const auto& u) {
                return u.effect == item.effect && u.name == item.name;
            });
            if (found == targetUniforms.end() || found->annotationSource != item.annotationSource ||
                found->type != item.type || found->rows != item.rows || found->columns != item.columns || found->length != item.length)
                return Reject(facts.reason, CaptureReason::OwnedUniformMismatch);
            uniforms[i] = found->natural;
        }
        if (!call.Current()) return Reject(facts.reason, CaptureReason::BoundaryChanged);
        for (size_t i = 0; i < techniques.size(); ++i) source.techniques[i].owned = techniques[i];
        for (size_t i = 0; i < uniforms.size(); ++i) source.uniforms[i].owned = uniforms[i];
        source.ownedHandles = std::move(handles); source.ownedUniforms = std::move(uniformHandles);
        source.ownedOrder = std::move(desiredOrder); source.previousOrder = std::move(actualOrder);
        source.ownedStates = std::move(states); source.ownedGraphFacts = facts.graph;
        source.mappingDirty = false; source.boundRevision = events.revision;
    } else {
        facts.cached = true; facts.techniqueEnumComplete = true;
        facts.techniques = source.ownedHandles.size(); facts.graph = source.ownedGraphFacts;
    }
    facts.mappingComplete = true;
    for (size_t i = 0; i < source.ownedStates.size(); ++i) {
        const auto [h, index] = source.ownedStates[i];
        const bool desired = index < source.techniques.size() && source.techniques[index].enabled;
        if (!h.handle) return Reject(facts.reason, CaptureReason::TechniqueHandle);
        if (!call.Invoke([&] { enabled = runtime->get_technique_state(h); })) return Reject(facts.reason, CaptureReason::BoundaryChanged);
        if (enabled != desired) {
            if (!call.Invoke([&] { runtime->set_technique_state(h, desired); })) return Reject(facts.reason, CaptureReason::BoundaryChanged);
            // Another add-on can veto a setter. Its event is not completion.
            if (!call.Invoke([&] { enabled = runtime->get_technique_state(h); })) return Reject(facts.reason, CaptureReason::BoundaryChanged);
            if (enabled != desired) return Reject(facts.reason, CaptureReason::OwnedStateMismatch);
        }
    }
    for (const auto& item : source.techniques)
        if (item.enabled && !item.owned.handle) return Reject(facts.reason, CaptureReason::OwnedTechniqueMissing);
    for (size_t i = 0; i < source.uniforms.size(); ++i) {
        const auto& item = source.uniforms[i];
        if (!item.annotationSource.empty()) continue; // target core owns dynamic sources
        const auto h = item.owned; const auto count = item.count;
        if (!h.handle) return Reject(facts.reason, CaptureReason::OwnedUniformMismatch);
        if ((!item.floats.empty() && !SyncValues(runtime, h, item.floats.data(), count, call)) ||
            (!item.ints.empty() && !SyncValues(runtime, h, item.ints.data(), count, call)) ||
            (!item.uints.empty() && !SyncValues(runtime, h, item.uints.data(), count, call)) ||
            (item.bools && !SyncValues(runtime, h, item.bools.get(), count, call)))
            return Reject(facts.reason, call.Current() ? CaptureReason::ValuesUnavailable : CaptureReason::BoundaryChanged);
    }
    if (!call.Current()) return Reject(facts.reason, CaptureReason::BoundaryChanged);
    if (!source.valid || !source.graphKnown || !source.graphValid || source.mappingDirty ||
        source.boundRuntime != runtime || source.boundGeneration != generation)
        return Reject(facts.reason, CaptureReason::OwnerChanged);
    facts.reason = CaptureReason::Ready;
    return true;
}

// A ready mapping can still contain effects whose external-RTV permutation is
// compiling. Publish only a complete render of the selected handles in order.
// This synchronous callback witness proves recording, never GPU completion or
// pixels; the caller still owns copyback, submission and reader retirement.
class RenderScope final
{
public:
    enum class Event { Begin, Technique, Finish };
private:
    inline static thread_local RenderScope* current_ = nullptr;
    RenderScope* previous_ = nullptr;
    api::effect_runtime* runtime_ = nullptr;
    api::effect_runtime* sourceRuntime_ = nullptr;
    api::command_list* list_ = nullptr;
    api::resource_view linear_{}, srgb_{};
    CallBoundary boundary_;
    std::vector<api::effect_technique> expected_;
    std::vector<api::effect_uniform_variable> staticUniforms_, sourceStaticUniforms_;
    size_t next_ = 0;
    bool valid_ = false, began_ = false, finished_ = false;
public:
    RenderScope(const Source& source, api::effect_runtime* runtime, uint64_t generation,
                api::command_list* list, api::resource_view linear, api::resource_view srgb)
        : runtime_(runtime), sourceRuntime_(source.naturalRuntime), list_(list), linear_(linear), srgb_(srgb)
    {
        if (runtime && list && linear.handle && srgb.handle && generation &&
            HasEnabledTechniques(source) && source.graphKnown && source.graphValid && !source.mappingDirty &&
            source.boundRuntime == runtime && source.boundGeneration == generation &&
            source.techniques.size() <= MaxItems && boundary_.Current()) {
            // Freeze handles before callbacks can revoke the source's map.
            // Disabled library entries do not become render requirements.
            for (const auto& item : source.techniques) if (item.enabled) {
                if (!item.owned.handle || item.identityAmbiguous) { expected_.clear(); break; }
                expected_.push_back(item.owned);
            }
            valid_ = !expected_.empty();
            // Dynamic source uniforms belong to this same core's frame update.
            // A selected static value changed during render revokes the image,
            // even when all technique-after callbacks still arrive in order.
            for (const auto& item : source.uniforms) if (item.annotationSource.empty()) {
                staticUniforms_.push_back(item.owned);
                if (item.natural.handle) sourceStaticUniforms_.push_back(item.natural);
            }
        }
        previous_ = current_;
        if (previous_) { previous_->valid_ = false; valid_ = false; }
        current_ = this;
    }
    ~RenderScope() { current_ = previous_; }
    RenderScope(const RenderScope&) = delete;
    RenderScope& operator=(const RenderScope&) = delete;
    bool Ready() const noexcept { return valid_ && !began_ && boundary_.Current(); }
    uint64_t RecordedTechniques() const noexcept
    {
        return valid_ && began_ && finished_ && next_ == expected_.size() && boundary_.Current()
            ? static_cast<uint64_t>(expected_.size()) : 0;
    }
    static void SelectionRequested(api::effect_runtime* runtime) noexcept
    {
        if (current_ && (current_->runtime_ == runtime || current_->sourceRuntime_ == runtime)) current_->valid_ = false;
    }
    static void UniformRequested(api::effect_runtime* runtime, api::effect_uniform_variable variable) noexcept
    {
        auto* scope = current_;
        if (!scope) return;
        const auto* uniforms = scope->runtime_ == runtime ? &scope->staticUniforms_ :
            scope->sourceRuntime_ == runtime ? &scope->sourceStaticUniforms_ : nullptr;
        if (uniforms && std::find(uniforms->begin(), uniforms->end(), variable) != uniforms->end())
            scope->valid_ = false;
    }
    static void Observe(Event event, api::effect_runtime* runtime, api::command_list* list,
                        api::resource_view linear, api::resource_view srgb, api::effect_technique technique = {}) noexcept
    {
        auto* scope = current_;
        if (!scope) return;
        auto& s = *scope;
        if (!s.valid_ || !s.boundary_.Current() || runtime != s.runtime_ || list != s.list_ ||
            linear != s.linear_ || srgb != s.srgb_) { s.valid_ = false; return; }
        switch (event) {
        case Event::Begin:
            if (s.began_ || s.finished_) s.valid_ = false;
            else s.began_ = true;
            break;
        case Event::Technique:
            if (!s.began_ || s.finished_ || s.next_ >= s.expected_.size() ||
                technique != s.expected_[s.next_]) s.valid_ = false;
            else ++s.next_;
            break;
        case Event::Finish:
            if (!s.began_ || s.finished_ || s.next_ != s.expected_.size()) s.valid_ = false;
            s.finished_ = true;
            break;
        }
    }
};
}

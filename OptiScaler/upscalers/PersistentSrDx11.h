#pragma once
#include "PersistentSrPool.h"
#include <upscalers/dlss/DLSSFeature_Dx11On12.h>
#include <with_dx12/with_dx12.h>
#include <wrl/client.h>
#include <algorithm>

namespace PersistentSrDx11
{
using Microsoft::WRL::ComPtr;
using Raw = std::vector<std::pair<std::string, Parameter>>;
struct Owner
{
    // Independent owning references: the bridge's matching members are borrowed.
    ComPtr<ID3D11Device> device11;
    ComPtr<ID3D11DeviceContext> context11;
    ComPtr<ID3D12Device> device12;
    ComPtr<ID3D12CommandQueue> queue12;
    ComPtr<IUnknown> identity11, identityContext, identity12, identityQueue;
    Raw requested;
    std::vector<PersistentSr::Atom> effectiveConfiguration;
    bool missingExposurePending = false;
    // A logical Create starts new temporal history, including a parked owner's
    // next lease. Only a successful first Evaluate consumes this boundary.
    bool resetPending = true;
    bool resetTraceRecorded = false;
    Upscaler exposureBackendRequest = Upscaler::Reset;
    std::unique_ptr<NVNGX_Parameters> privateParams;
    ContextData<IFeature_Dx11> context;
};
using Pool = PersistentSr::Pool<Owner, NVSDK_NGX_Handle, 2, 65536>;
using Status = PersistentSr::Status;
using Key = PersistentSr::Key;
using Recipe = PersistentSr::Recipe;
using Atom = PersistentSr::Atom;
inline Pool& Instance() { static auto* pool = new Pool; return *pool; }
struct Counters
{
    uint64_t hits = 0, initialCreates = 0, replacements = 0, parked = 0;
    uint64_t fallbacks = 0, stale = 0, failures = 0, metadataFull = 0;
};
inline Counters& Stats() { static Counters counters; return counters; }
inline void LogCounts(const char* event)
{
    const auto& c = Stats();
    LOG_INFO("PersistentSR {} hit={} init={} replace={} park={} fallback={} stale={} fail={} metadataFull={} owners={} handles={}",
             event, c.hits, c.initialCreates, c.replacements, c.parked, c.fallbacks,
             c.stale, c.failures, c.metadataFull, Instance().OwnerCount(), Instance().HandleCount());
}
inline bool IsStarRail()
{
    static const bool match = [] {
        wchar_t path[32768] {};
        const auto length = GetModuleFileNameW(nullptr, path, _countof(path));
        return length && length < _countof(path) &&
               _wcsicmp(std::filesystem::path(path).filename().c_str(), L"StarRail.exe") == 0;
    }();
    return match;
}
inline bool UseOwnedParameters()
{
    return IsStarRail() && Config::Instance()->DLSSEnabled.value_or_default() &&
           Config::Instance()->Dx11Upscaler.value_or_default() == Upscaler::DLSS_on12;
}
class Entry
{
  public:
    explicit Entry(Pool& pool) { if (IsStarRail()) guard.emplace(pool); }
    explicit operator bool() const { return !guard || bool(*guard); }
  private:
    std::optional<Pool::Entry> guard;
};
template<class T> void Capture(std::vector<Atom>& out, const std::string& name, const T& value)
{
    out.push_back(PersistentSr::Scalar(name, 100 + sizeof(T), value));
}
template<class C> void Capture(std::vector<Atom>& out, const std::string& name, const std::basic_string<C>& value)
{
    Atom atom { name, 200 + sizeof(C), std::vector<uint8_t>(value.size() * sizeof(C)) };
    if (!atom.value.empty()) std::memcpy(atom.value.data(), value.data(), atom.value.size());
    out.push_back(std::move(atom));
}
template<class T> void Capture(std::vector<Atom>& out, const std::string& name, const std::optional<T>& value)
{
    Capture(out, name + ".present", value.has_value());
    if (value) Capture(out, name, *value);
}
template<class T, HasDefaultValue D>
void Capture(std::vector<Atom>& out, const std::string& name, const CustomOptional<T, D>& value)
{
    Capture(out, name + ".present", value.has_value());
    if constexpr (D == NoDefault) { if (value) Capture(out, name, value.value()); }
    else Capture(out, name, value.value_or_default());
}
inline std::vector<Atom> Configuration()
{
    std::vector<Atom> out;
    auto& cfg = *Config::Instance(); auto& state = State::Instance();
#define PS_CFG(name) Capture(out, #name, cfg.name)
    PS_CFG(HDR); PS_CFG(OverrideSharpness); PS_CFG(Sharpness); PS_CFG(RcasEnabled);
    PS_CFG(DepthInverted); PS_CFG(JitterCancellation); PS_CFG(DisplayResolution); PS_CFG(AutoExposure);
    PS_CFG(ExtendedLimits); PS_CFG(OutputScalingEnabled); PS_CFG(OutputScalingMultiplier);
    PS_CFG(OutputScalingDownscaler); PS_CFG(RenderPresetOverride); PS_CFG(RenderPresetForAll);
    PS_CFG(RenderPresetDLAA); PS_CFG(RenderPresetUltraQuality); PS_CFG(RenderPresetQuality);
    PS_CFG(RenderPresetBalanced); PS_CFG(RenderPresetPerformance); PS_CFG(RenderPresetUltraPerformance);
    PS_CFG(Dx11Upscaler); PS_CFG(DLSSEnabled); PS_CFG(UseGenericAppIdWithDlss);
    PS_CFG(NvngxPath); PS_CFG(NVNGX_DLSS_Library); PS_CFG(DLSSFeaturePath); PS_CFG(MainDllPath);
    PS_CFG(OverlayMenu); PS_CFG(UseHQFont); PS_CFG(FontSize); PS_CFG(TTFFontPath);
    PS_CFG(DontUseNTShared); PS_CFG(DisableReactiveMask); PS_CFG(Dx11DelayedInit);
    PS_CFG(SharpnessShader); PS_CFG(LogLevel); PS_CFG(ColorResourceBarrier); PS_CFG(MVResourceBarrier);
#undef PS_CFG
#define PS_STATE(name) Capture(out, #name, state.name)
    PS_STATE(NVNGX_ApplicationId); PS_STATE(NVNGX_ApplicationDataPath); PS_STATE(NVNGX_ProjectId);
    PS_STATE(NVNGX_Version); PS_STATE(NVNGX_Engine); PS_STATE(NVNGX_EngineVersion);
    PS_STATE(NVNGX_DLSS_Path); PS_STATE(gameEngine);
    PS_STATE(dlssPresetsOverriddenExternally); PS_STATE(dlssRenderPresetExternal);
#undef PS_STATE
    Capture(out, "forceUnreal", bool(state.gameQuirks & GameQuirk::ForceUnrealEngine));
    Capture(out, "modulePath", NVNGXProxy::NVNGXModule_Path());
    // GetFeatureCommonInfo appends DLSS's directory to this list at provider Init.
    // Drop repeated identical search directories, preserving first-match order;
    // include the pending append so the first successful Init is not a mismatch.
    std::vector<std::wstring> paths;
    auto addPath = [&](const std::wstring& path) {
        if (std::find(paths.begin(), paths.end(), path) == paths.end()) paths.push_back(path);
    };
    for (const auto& path : state.NVNGX_FeatureInfo_Paths) addPath(path);
    if (state.NVNGX_DLSS_Path) addPath(std::filesystem::path(*state.NVNGX_DLSS_Path).parent_path().wstring());
    for (size_t i = 0; i < paths.size(); ++i) Capture(out, "searchPath." + std::to_string(i), paths[i]);
    return out;
}
inline bool CallbackName(const std::string& name)
{
    return name == NVSDK_NGX_Parameter_DLSSOptimalSettingsCallback ||
           name == NVSDK_NGX_EParameter_DLSSOptimalSettingsCallback ||
           name == NVSDK_NGX_Parameter_DLSSGetStatsCallback || name == "DLSSDOptimalSettingsCallback";
}
inline bool EvaluateResourceName(const std::string& name)
{
    // Exactly the resources translated from the CURRENT Evaluate table by
    // IFeature_Dx11wDx12::Evaluate/PrepareUpscalerResources. No prefix matching:
    // other objects, encoded aliases and unsupported GBuffer pointers reject.
    const char* names[] = { NVSDK_NGX_Parameter_Color, NVSDK_NGX_Parameter_Depth,
        NVSDK_NGX_Parameter_MotionVectors, NVSDK_NGX_Parameter_Output,
        NVSDK_NGX_Parameter_ExposureTexture, NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_Mask };
    return std::any_of(std::begin(names), std::end(names), [&](const char* value) { return name == value; });
}
inline size_t NullEvaluateResourceIndex(const std::string& name)
{
    // SDK NGX_*_EVALUATE_DLSS_EXT optional resources not translated by this
    // DX11-on-DX12 bridge. Only a typed null is equivalent to absence at Create;
    // non-null and wrong-type values must reject persistent admission below.
    // Exact names only: future/unknown creation dependencies stay in the key.
    const char* names[] = { NVSDK_NGX_Parameter_TransparencyMask,
        NVSDK_NGX_Parameter_GBuffer_Albedo,
        NVSDK_NGX_Parameter_GBuffer_Roughness,
        NVSDK_NGX_Parameter_GBuffer_Metallic,
        NVSDK_NGX_Parameter_GBuffer_Specular,
        NVSDK_NGX_Parameter_GBuffer_Subsurface,
        NVSDK_NGX_Parameter_GBuffer_Normals,
        NVSDK_NGX_Parameter_GBuffer_ShadingModelId,
        NVSDK_NGX_Parameter_GBuffer_MaterialId,
        NVSDK_NGX_Parameter_GBuffer_Atrrib_8,
        NVSDK_NGX_Parameter_GBuffer_Atrrib_9,
        NVSDK_NGX_Parameter_GBuffer_Atrrib_10,
        NVSDK_NGX_Parameter_GBuffer_Atrrib_11,
        NVSDK_NGX_Parameter_GBuffer_Atrrib_12,
        NVSDK_NGX_Parameter_GBuffer_Atrrib_13,
        NVSDK_NGX_Parameter_GBuffer_Atrrib_14,
        NVSDK_NGX_Parameter_GBuffer_Atrrib_15,
        NVSDK_NGX_Parameter_MotionVectors3D,
        NVSDK_NGX_Parameter_IsParticleMask,
        NVSDK_NGX_Parameter_AnimatedTextureMask,
        NVSDK_NGX_Parameter_DepthHighRes,
        NVSDK_NGX_Parameter_Position_ViewSpace,
        NVSDK_NGX_Parameter_RayTracingHitDistance,
        NVSDK_NGX_Parameter_MotionVectorsReflection };
    return size_t(std::find_if(std::begin(names), std::end(names), [&](const char* value) { return name == value; }) - std::begin(names));
}
inline bool NullEvaluateResourceName(const std::string& name)
{
    return NullEvaluateResourceIndex(name) < 24;
}
inline bool EvaluateScalarName(const std::string& name)
{
    // SDK NGX_*_EVALUATE_DLSS_EXT fields and legacy MV offsets initialized in
    // the parameter table alongside the Evaluate motion-vector scales.
    // Keep creation dimensions/flags, output-subrect ENABLE, node masks, presets
    // and OptiScaler.SupportsUpscaleSize/FSR.upscaleSize.* in the create map.
    const char* names[] = { NVSDK_NGX_Parameter_Jitter_Offset_X, NVSDK_NGX_Parameter_Jitter_Offset_Y,
        NVSDK_NGX_Parameter_Sharpness, NVSDK_NGX_EParameter_Sharpness,
        NVSDK_NGX_Parameter_Reset, NVSDK_NGX_EParameter_Reset,
        NVSDK_NGX_Parameter_MV_Scale_X, NVSDK_NGX_Parameter_MV_Scale_Y,
        NVSDK_NGX_EParameter_MV_Scale_X, NVSDK_NGX_EParameter_MV_Scale_Y,
        NVSDK_NGX_Parameter_MV_Offset_X, NVSDK_NGX_Parameter_MV_Offset_Y,
        NVSDK_NGX_EParameter_MV_Offset_X, NVSDK_NGX_EParameter_MV_Offset_Y,
        NVSDK_NGX_Parameter_FrameTimeDeltaInMsec, NVSDK_NGX_Parameter_TonemapperType,
        NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X, NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y,
        NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X, NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y,
        NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X, NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y,
        NVSDK_NGX_Parameter_DLSS_Input_Translucency_SubrectBase_X, NVSDK_NGX_Parameter_DLSS_Input_Translucency_SubrectBase_Y,
        NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_SubrectBase_X,
        NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_SubrectBase_Y,
        NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X, NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y,
        NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height,
        NVSDK_NGX_Parameter_DLSS_Pre_Exposure, NVSDK_NGX_Parameter_DLSS_Exposure_Scale,
        NVSDK_NGX_Parameter_DLSS_Indicator_Invert_X_Axis, NVSDK_NGX_Parameter_DLSS_Indicator_Invert_Y_Axis };
    return std::any_of(std::begin(names), std::end(names), [&](const char* value) { return name == value; });
}
inline bool CanonicalizeCreateMap(Raw& raw)
{
    // Sanitize the actual map that will be cloned for private native Create,
    // not just its comparison key. Never pin borrowed per-frame resources.
    for (const auto& [name, value] : raw)
    {
        const bool resource = EvaluateResourceName(name), scalar = EvaluateScalarName(name);
        const bool optionalNullResource = NullEvaluateResourceName(name);
        if (!resource && !scalar && !optionalNullResource) continue;
        const bool pointer = value.key == typeid(void*).hash_code();
        if (optionalNullResource && (!pointer || value.values.vp != nullptr))
        {
            LOG_INFO("PersistentSR recipe-reject reason=unsupported-optional-evaluate-resource parameter={} typeKey={}",
                     name, value.key);
            return false;
        }
        const bool numeric = value.key == typeid(float).hash_code() || value.key == typeid(double).hash_code() ||
            value.key == typeid(int).hash_code() || value.key == typeid(unsigned).hash_code() ||
            value.key == typeid(unsigned long long).hash_code();
        if ((resource && !pointer) || (scalar && !numeric))
        {
            LOG_INFO("PersistentSR recipe-reject reason=evaluate-field-type parameter={} typeKey={}", name, value.key);
            return false;
        }
    }
    // Capture the exact proven-null Evaluate residue before erasing it, so
    // successful canonicalization remains observable. One exact 24-bit mask
    // per changed presence pattern; never prints non-null borrowed resources.
    uint32_t optionalNullMask = 0;
    for (const auto& [name, value] : raw)
    {
        const auto index = NullEvaluateResourceIndex(name);
        if (index < 24) optionalNullMask |= uint32_t(1) << index;
    }
    static uint32_t lastOptionalNullMask = UINT32_MAX;
    if (optionalNullMask != lastOptionalNullMask)
    {
        lastOptionalNullMask = optionalNullMask;
        size_t rows = 0;
        for (const auto& [name, value] : raw)
            if (NullEvaluateResourceName(name))
            {
                LOG_INFO("PersistentSR optional-null-snapshot mask={:06X} row={} name={} type=void* size={} null=true action=omit-private-create-and-key",
                         optionalNullMask, rows++, name, sizeof(void*));
            }
        LOG_INFO("PersistentSR optional-null-snapshot-end mask={:06X} count={} limit=24 cache=last-presence-mask",
                 optionalNullMask, rows);
    }
    raw.erase(std::remove_if(raw.begin(), raw.end(), [](const auto& item) {
        return EvaluateResourceName(item.first) || EvaluateScalarName(item.first) || NullEvaluateResourceName(item.first);
    }), raw.end());
    // GetDynamicOutputResolution initializes this local control flag to int0
    // before Get, and its only write consumes a nonzero value into int0. Missing
    // and explicit int0 therefore share one native Create map and comparison
    // recipe. Keep all present values/types exact, including nonzero controls.
    if (std::none_of(raw.begin(), raw.end(), [](const auto& item) {
        return item.first == "OptiScaler.SupportsUpscaleSize";
    }))
    {
        Parameter value; value = int(0);
        raw.emplace_back("OptiScaler.SupportsUpscaleSize", value);
    }
    // IFeature and the SDK Create structs consume these values as unsigned
    // dimensions/node masks. Canonicalize only these known fields, not unknown
    // numeric data. Missing required fields are still rejected by Freeze.
    const char* unsignedNames[] = { NVSDK_NGX_Parameter_Width, NVSDK_NGX_Parameter_Height,
        NVSDK_NGX_Parameter_OutWidth, NVSDK_NGX_Parameter_OutHeight,
        NVSDK_NGX_Parameter_CreationNodeMask,
        NVSDK_NGX_Parameter_VisibilityNodeMask };
    for (auto& [name, value] : raw)
    {
        if (std::any_of(std::begin(unsignedNames), std::end(unsignedNames), [&](const char* key) { return name == key; }) &&
            value.key == typeid(int).hash_code())
            value = static_cast<unsigned>(value.values.i);
    }
    // Local InitNGXParameters and DLSSFeature::ProcessInitParams use Default=0
    // for absent preset hints. Native Create receives this same explicit map.
    // Nondefault presets remain part of the immutable creation contract.
    const char* presets[] = { NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA,
        NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality,
        NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
        NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced,
        NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
        NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance };
    for (const auto* name : presets)
    {
        auto it = std::find_if(raw.begin(), raw.end(), [&](const auto& item) { return item.first == name; });
        if (it == raw.end()) { Parameter value; value = unsigned(NVSDK_NGX_DLSS_Hint_Render_Preset_Default); raw.emplace_back(name, value); }
        else if (it->second.key == typeid(int).hash_code()) it->second = static_cast<unsigned>(it->second.values.i);
        else if (it->second.key != typeid(unsigned).hash_code())
        {
            LOG_INFO("PersistentSR recipe-reject reason=preset-type parameter={} typeKey={}", name, it->second.key);
            return false;
        }
    }
    std::sort(raw.begin(), raw.end(), [](const auto& left, const auto& right) { return left.first < right.first; });
    return true;
}
inline bool Freeze(NVSDK_NGX_Parameter* parameters, Raw& raw, Recipe& recipe)
{
    if (!parameters) return false;
    unsigned allocation = 0;
    const auto allocationResult = parameters->Get(NGX_AllocTypes::AllocKey.data(), &allocation);
    if (allocationResult != NVSDK_NGX_Result_Success ||
        (allocation != NGX_AllocTypes::InternDynamic && allocation != NGX_AllocTypes::InternPersistent))
    {
        LOG_INFO("PersistentSR recipe-reject reason=allocation-provenance allocation={} result={:X}",
                 allocation, unsigned(allocationResult));
        return false;
    }
    if (!NVNGX_Parameters::PersistentSrSnapshotIfOwned(parameters, raw))
    {
        LOG_INFO("PersistentSR recipe-reject reason=unregistered-object allocation={}", allocation);
        return false;
    }
    if (!CanonicalizeCreateMap(raw)) return false;
    for (const auto& [name, p] : raw)
    {
        if (p.key == typeid(float).hash_code()) recipe.parameters.push_back(PersistentSr::Scalar(name, 1, p.values.f));
        else if (p.key == typeid(double).hash_code()) recipe.parameters.push_back(PersistentSr::Scalar(name, 2, p.values.d));
        else if (p.key == typeid(int).hash_code()) recipe.parameters.push_back(PersistentSr::Scalar(name, 3, p.values.i));
        else if (p.key == typeid(unsigned).hash_code()) recipe.parameters.push_back(PersistentSr::Scalar(name, 4, p.values.ui));
        else if (p.key == typeid(unsigned long long).hash_code()) recipe.parameters.push_back(PersistentSr::Scalar(name, 5, p.values.ull));
        else if (p.key == typeid(void*).hash_code())
        {
            // Known current-frame inputs were removed from both maps above.
            // Other non-null objects cannot become borrowed process-lifetime
            // creation dependencies. Known callbacks are
            // code pointers, included exactly in the immutable recipe.
            if (p.values.vp && !CallbackName(name))
            {
                LOG_INFO("PersistentSR pointer-recipe-fallback parameter={} type=void* typeKey={}", name, p.key);
                LogCounts("pointer-recipe-fallback"); return false;
            }
            recipe.parameters.push_back(PersistentSr::Scalar(name, 6, p.values.vp));
        }
        else
        {
            LOG_INFO("PersistentSR recipe-reject reason=unsupported-type parameter={} typeKey={}", name, p.key);
            return false;
        }
    }
    const char* required[] = { NVSDK_NGX_Parameter_Width, NVSDK_NGX_Parameter_Height,
        NVSDK_NGX_Parameter_OutWidth, NVSDK_NGX_Parameter_OutHeight,
        NVSDK_NGX_Parameter_PerfQualityValue, NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags };
    for (const auto* name : required)
    {
        const auto it = std::find_if(raw.begin(), raw.end(), [&](const auto& p) { return p.first == name; });
        if (it == raw.end() || (it->second.key != typeid(unsigned).hash_code() && it->second.key != typeid(int).hash_code()))
        {
            LOG_INFO("PersistentSR recipe-reject reason=required-field parameter={} present={}", name, it != raw.end());
            return false;
        }
    }
    recipe.configuration = Configuration();
    return true;
}
inline size_t FirstDifference(const std::vector<Atom>& saved, const std::vector<Atom>& current)
{
    const auto common = std::min(saved.size(), current.size());
    size_t i = 0;
    while (i < common && saved[i] == current[i]) ++i;
    return i;
}
inline std::string AtomHex(const Atom* atom)
{
    if (!atom) return "absent";
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    const auto size = std::min<size_t>(atom->value.size(), 64);
    for (size_t i = 0; i < size; ++i)
    {
        const auto byte = atom->value[i];
        out.push_back(digits[byte >> 4]); out.push_back(digits[byte & 15]);
    }
    if (size != atom->value.size()) out += "...";
    return out;
}
inline uint64_t RecipeDiagnosticSignature(const std::vector<Atom>& saved, const std::vector<Atom>& current)
{
    // Diagnostic suppression only. Admission still compares complete typed
    // bytes above; this non-security signature never grants owner reuse.
    uint64_t hash = 14695981039346656037ULL;
    auto byte = [&](uint8_t value) { hash = (hash ^ value) * 1099511628211ULL; };
    auto number = [&](uint64_t value) { for (unsigned i = 0; i < 8; ++i) byte(uint8_t(value >> (i * 8))); };
    auto append = [&](const std::vector<Atom>& atoms) {
        number(atoms.size());
        for (const auto& atom : atoms)
        {
            number(atom.name.size()); for (const auto c : atom.name) byte(uint8_t(c));
            number(atom.type); number(atom.value.size()); for (const auto c : atom.value) byte(c);
        }
    };
    append(saved); append(current); return hash;
}
template<class Visitor> inline size_t VisitRecipeDifferences(const std::vector<Atom>& saved,
    const std::vector<Atom>& current, Visitor visit)
{
    size_t count = 0;
    auto changed = [&](const Atom* left, const Atom* right) {
        if (count < 64) visit(count, left, right);
        ++count;
    };
    // Compare by exact name, so insertion does not report every later atom as
    // changed. Configuration atoms need not be sorted; no key mutation occurs.
    for (const auto& right : current)
    {
        const auto left = std::find_if(saved.begin(), saved.end(), [&](const auto& atom) { return atom.name == right.name; });
        if (left == saved.end()) changed(nullptr, &right);
        else if (*left != right) changed(&*left, &right);
    }
    for (const auto& left : saved)
        if (std::none_of(current.begin(), current.end(), [&](const auto& atom) { return atom.name == left.name; }))
            changed(&left, nullptr);
    return count;
}
inline void LogRecipeDifference(uint32_t backend, const char* kind,
                                const std::vector<Atom>& saved, const std::vector<Atom>& current)
{
    if (saved == current) return;
    const auto index = FirstDifference(saved, current);
    const auto* left = index < saved.size() ? &saved[index] : nullptr;
    const auto* right = index < current.size() ? &current[index] : nullptr;
    LOG_INFO("PersistentSR key-diff backend={} kind={} index={} savedCount={} currentCount={} "
             "savedName={} savedType={} savedSize={} savedHex={} currentName={} currentType={} currentSize={} currentHex={}",
             backend, kind, index, saved.size(), current.size(), left ? left->name : "absent",
             left ? left->type : 0, left ? left->value.size() : 0, AtomHex(left),
             right ? right->name : "absent", right ? right->type : 0, right ? right->value.size() : 0, AtomHex(right));
    // Two pinned owners and three recipe kinds. Keep only the last signature
    // per owner/kind: fixed storage, first/changed signature snapshots only.
    struct Snapshot { uint32_t backend = 0; const char* kind = nullptr; uint64_t signature = 0; bool seen = false; };
    static std::array<Snapshot, 6> snapshots {};
    auto cached = std::find_if(snapshots.begin(), snapshots.end(), [&](const auto& item) {
        return item.seen && item.backend == backend && std::strcmp(item.kind, kind) == 0;
    });
    if (cached == snapshots.end()) cached = std::find_if(snapshots.begin(), snapshots.end(), [](const auto& item) { return !item.seen; });
    if (cached == snapshots.end()) return;
    const auto signature = RecipeDiagnosticSignature(saved, current);
    if (cached->seen && cached->signature == signature) return;
    *cached = Snapshot { backend, kind, signature, true };
    const auto count = VisitRecipeDifferences(saved, current, [&](size_t row, const Atom* old, const Atom* now) {
        auto isNull = [](const Atom* atom) {
            return atom && atom->type == 6 && std::all_of(atom->value.begin(), atom->value.end(), [](auto b) { return b == 0; });
        };
        LOG_INFO("PersistentSR key-snapshot backend={} kind={} signature={:016X} row={} "
                 "savedName={} savedType={} savedSize={} savedNull={} savedHex={} "
                 "currentName={} currentType={} currentSize={} currentNull={} currentHex={}",
                 backend, kind, signature, row, old ? old->name.substr(0, 256) : "absent", old ? old->type : 0,
                 old ? old->value.size() : 0, isNull(old), AtomHex(old),
                 now ? now->name.substr(0, 256) : "absent", now ? now->type : 0, now ? now->value.size() : 0,
                 isNull(now), AtomHex(now));
    });
    LOG_INFO("PersistentSR key-snapshot-end backend={} kind={} signature={:016X} differences={} emitted={} truncated={} "
             "signatureUse=diagnostic-only cache=last-per-owner-kind",
             backend, kind, signature, count, std::min<size_t>(count, 64), count > 64);
}

inline bool Bind(Owner& owner, ID3D11DeviceContext* context, PersistentSr::Binding& binding)
{
    if (!context) return false;
    owner.context11 = context; context->GetDevice(owner.device11.GetAddressOf());
    if (!owner.device11 || !WithDx12::PrepareD3D12ForD3D11(owner.device11.Get())) return false;
    owner.device12 = WithDx12::GetD3D12Device(); owner.queue12 = WithDx12::GetD3D12CommandQueue();
    if (!owner.device12 || !owner.queue12 || FAILED(owner.device11->GetDeviceRemovedReason()) ||
        FAILED(owner.device12->GetDeviceRemovedReason())) return false;
    if (FAILED(owner.device11.As(&owner.identity11)) || FAILED(owner.context11.As(&owner.identityContext)) ||
        FAILED(owner.device12.As(&owner.identity12)) || FAILED(owner.queue12.As(&owner.identityQueue))) return false;
    binding = { uintptr_t(owner.identity11.Get()), uintptr_t(owner.identityContext.Get()),
                uintptr_t(owner.identity12.Get()), uintptr_t(owner.identityQueue.Get()),
                uintptr_t(NVNGXProxy::NVNGXModule()), NVNGXProxy::Dx12ProviderGeneration() };
    return true;
}
inline bool Healthy(Pool::Slot& slot, ID3D11DeviceContext* context)
{
    auto& owner = *slot.owner;
    ComPtr<IUnknown> identity;
    return context && SUCCEEDED(context->QueryInterface(IID_PPV_ARGS(identity.GetAddressOf()))) &&
        identity.Get() == owner.identityContext.Get() &&
        WithDx12::GetD3D12Device() == owner.device12.Get() && WithDx12::GetD3D12CommandQueue() == owner.queue12.Get() &&
        slot.key.binding.module == uintptr_t(NVNGXProxy::NVNGXModule()) &&
        slot.key.binding.providerGeneration == NVNGXProxy::Dx12ProviderGeneration() &&
        SUCCEEDED(owner.device11->GetDeviceRemovedReason()) && SUCCEEDED(owner.device12->GetDeviceRemovedReason()) &&
        owner.context.feature && owner.context.feature->IsInited() && owner.context.feature->ModuleLoaded();
}
inline bool Initialize(Pool::Slot& slot)
{
    auto& owner = *slot.owner;
    owner.privateParams = NVNGX_Parameters::PersistentSrClone(API::DX11, owner.requested);
    State::Instance().autoExposure = slot.key.autoExposure;
    owner.context.feature = std::make_unique<DLSSFeatureDx11on12>(slot.backendId, owner.privateParams.get());
    if (!owner.context.feature->ModuleLoaded() ||
        !owner.context.feature->Init(owner.device11.Get(), owner.context11.Get(), owner.privateParams.get()) ||
        !owner.context.feature->IsInited())
    {
        Instance().FailReservation(&slot); ++Stats().failures; LogCounts("init-failed-pinned"); return false;
    }
    // Initial proxy Init and shutdown-function validation can advance the epoch.
    slot.key.binding.providerGeneration = NVNGXProxy::Dx12ProviderGeneration();
    owner.effectiveConfiguration = Configuration();
    return true;
}
inline bool ConfigurationMatches(const Pool::Slot& slot, const std::vector<Atom>& current)
{
    return PersistentSr::ConfigurationMatches(slot.key.recipe.configuration,
        slot.owner->effectiveConfiguration, current);
}
inline void LogKeyMiss(Pool& pool, const Key& key, const std::vector<Atom>& currentConfiguration,
                       unsigned configurationAliasBackend)
{
    for (size_t i = 0; i < pool.OwnerCount(); ++i)
    {
        const auto& saved = pool.OwnerAt(i);
        const auto& old = saved.key.binding; const auto& now = key.binding;
        LOG_INFO("PersistentSR key-miss owner={} backend={} active={} reusable={} parametersMatch={} "
                 "requestedConfigMatch={} effectiveConfigMatch={} bindingMatch={} savedAuto={} currentAuto={} "
                 "configurationAliasBackend={} candidateRequestedConfigMatch={} "
                 "savedDevice11={} currentDevice11={} savedContext11={} currentContext11={} savedDevice12={} currentDevice12={} "
                 "savedQueue12={} currentQueue12={} savedModule={} currentModule={} savedGeneration={} currentGeneration={}",
                 i, saved.backendId, saved.active, saved.reusable, saved.key.recipe.parameters == key.recipe.parameters,
                 saved.key.recipe.configuration == currentConfiguration,
                 saved.owner->effectiveConfiguration == currentConfiguration, old == now,
                 saved.key.autoExposure ? int(*saved.key.autoExposure) : -1, key.autoExposure ? int(*key.autoExposure) : -1,
                 configurationAliasBackend, saved.key.recipe.configuration == key.recipe.configuration,
                 old.device11, now.device11, old.context11, now.context11, old.device12, now.device12,
                 old.queue12, now.queue12, old.module, now.module, old.providerGeneration, now.providerGeneration);
        LogRecipeDifference(saved.backendId, "parameters", saved.key.recipe.parameters, key.recipe.parameters);
        LogRecipeDifference(saved.backendId, "requested-config", saved.key.recipe.configuration, currentConfiguration);
        LogRecipeDifference(saved.backendId, "effective-config", saved.owner->effectiveConfiguration, currentConfiguration);
    }
}
inline void ObserveEvaluate(Pool::Slot& slot, std::optional<bool> beforeAuto, bool beforeChanged,
                            Upscaler beforeBackend)
{
    auto& state = State::Instance();
    if (PersistentSr::MissingExposureTransition(beforeAuto.value_or(false), beforeChanged,
        state.autoExposure.value_or(false), state.changeBackend[slot.backendId],
        slot.owner->context.feature->AutoExposure(), beforeBackend == state.newBackend,
        ConfigurationMatches(slot, Configuration())))
    {
        slot.owner->missingExposurePending = true;
        slot.owner->exposureBackendRequest = state.newBackend;
        LogCounts("missing-exposure-observed");
    }
}
inline std::optional<NVSDK_NGX_Result> TryCreate(ID3D11DeviceContext* context,
    NVSDK_NGX_Feature feature, NVSDK_NGX_Parameter* parameters, NVSDK_NGX_Handle** handle)
{
    if (!IsStarRail()) return std::nullopt;
    auto fallback = []() -> std::optional<NVSDK_NGX_Result> { ++Stats().fallbacks; return std::nullopt; };
    auto& pool = Instance(); auto& cfg = *Config::Instance();
    if (feature != NVSDK_NGX_Feature_SuperSampling || !context || !parameters || !handle ||
        cfg.Dx11Upscaler.value_or_default() != Upscaler::DLSS_on12 || !cfg.DLSSEnabled.value_or_default() ||
        !IdentifyGpu::getPrimaryGpu().dlssCapable || !State::Instance().NVNGX_DLSS_Path || pool.Closed()) return fallback();
    // A pending restoration uses saved presets from another creation contract.
    if (!cfg.RenderPresetOverride.value_or_default() && State::Instance().dlssPresetsOverridenByOpti) return fallback();
    if (!pool.HasSpareHandle()) { ++Stats().metadataFull; LogCounts("metadata-full"); return NVSDK_NGX_Result_Fail; }
    if (!NVNGXProxy::NVNGXModule()) NVNGXProxy::InitNVNGX();
    auto freshOwner = std::make_unique<Owner>(); Key key;
    if (!Freeze(parameters, freshOwner->requested, key.recipe)) { LogCounts("opaque-or-incomplete-recipe-fallback"); return fallback(); }
    if (!Bind(*freshOwner, context, key.binding)) { LogCounts("binding-fallback"); return fallback(); }
    // Diagnostic input must survive the admission-only requested/effective alias below.
    const auto originalConfiguration = key.recipe.configuration;
    unsigned configurationAliasBackend = 0;
    // Keep the requested recipe immutable. Init may clamp output scaling or set
    // engine barriers: either exact requested or recorded effective config maps
    // to that same contract; every other configuration still misses.
    for (size_t i = 0; i < pool.OwnerCount(); ++i)
    {
        const auto& saved = pool.OwnerAt(i);
        if (saved.reusable && saved.key.recipe.parameters == key.recipe.parameters &&
            saved.key.binding == key.binding && ConfigurationMatches(saved, key.recipe.configuration))
        { key.recipe.configuration = saved.key.recipe.configuration; configurationAliasBackend = saved.backendId; break; }
    }
    // Learn only for this immutable requested recipe and exact live binding.
    // Ordinary Create resets global autoExposure; it must not erase this learning.
    for (size_t i = 0; i < pool.OwnerCount(); ++i)
    {
        const auto& saved = pool.OwnerAt(i);
        if (saved.reusable && saved.key.recipe == key.recipe && saved.key.binding == key.binding &&
            saved.key.autoExposure == std::optional<bool>(true)) key.autoExposure = true;
    }
    const auto logicalId = IFeature::GetNextHandleId();
    if (!logicalId) return NVSDK_NGX_Result_Fail;
    auto acquired = pool.Acquire(key, logicalId);
    if (acquired.status == Status::Reused)
    {
        if (!Healthy(*acquired.slot, context) || State::Instance().changeBackend[acquired.slot->backendId])
        { pool.Retire(acquired.handle); return fallback(); }
        acquired.slot->owner->resetPending = true;
        acquired.slot->owner->resetTraceRecorded = false;
        *handle = acquired.handle; State::Instance().api = DX11;
        State::Instance().autoExposure = key.autoExposure;
        State::Instance().currentFeature = acquired.slot->owner->context.feature.get();
        ++Stats().hits; LogCounts("reuse"); return NVSDK_NGX_Result_Success;
    }
    LogKeyMiss(pool, key, originalConfiguration, configurationAliasBackend);
    // Reserve the second slot exclusively for the learned exposure generation.
    // Full/incompatible creates follow the original lifecycle outside this pool.
    if (pool.OwnerCount() != 0) { LogCounts(acquired.status == Status::Full ? "owners-full-fallback" : "exact-key-miss-fallback"); return fallback(); }
    const auto backendId = IFeature::GetNextHandleId();
    auto reserved = pool.Reserve(key, backendId, freshOwner);
    if (reserved.status != Status::Reserved) return NVSDK_NGX_Result_Fail;
    if (!Initialize(*reserved.slot)) return NVSDK_NGX_Result_Fail;
    auto published = pool.Publish(reserved.slot, logicalId);
    if (published.status != Status::Created) { pool.FailReservation(reserved.slot); return NVSDK_NGX_Result_Fail; }
    *handle = published.handle; State::Instance().api = DX11;
    State::Instance().currentFeature = published.slot->owner->context.feature.get();
    ++Stats().initialCreates; LogCounts("created"); return NVSDK_NGX_Result_Success;
}
inline bool PrepareEvaluate(Pool::Result& found, const NVSDK_NGX_Handle* handle, ID3D11DeviceContext* context);
inline bool Evaluate(Pool::Result& found, ID3D11DeviceContext* context, NVSDK_NGX_Parameter* parameters)
{
    auto& owner = *found.slot->owner;
    if (!owner.resetPending) return owner.context.feature->Evaluate(context, parameters);
    // Only this call owns the temporary table. Preserve the caller's original
    // Reset presence, value AND type, plus the current frame's resources. The
    // persistent private Create table never supplies Evaluate input resources.
    Raw current;
    if (!NVNGX_Parameters::PersistentSrSnapshotIfOwned(parameters, current))
    {
        LOG_INFO("PersistentSR lease-reset-reject reason=unregistered-evaluate-table");
        return false;
    }
    auto first = NVNGX_Parameters::PersistentSrClone(API::DX11, current);
    first->Set(NVSDK_NGX_Parameter_Reset, 1);
    const bool succeeded = owner.context.feature->Evaluate(context, first.get());
    if (succeeded)
    {
        owner.resetPending = false;
        LOG_INFO("PersistentSR lease-reset-consumed backend={}", found.slot->backendId);
    }
    // One diagnostic per logical lease, including a failed first attempt. Do
    // not query/convert/mutate the caller table just to report Reset's origin.
    if (!owner.resetTraceRecorded)
    {
        const auto reset = std::find_if(current.begin(), current.end(), [](const auto& p) {
            return p.first == NVSDK_NGX_Parameter_Reset;
        });
        const bool present = reset != current.end();
        auto report = [&](const char* type, const auto& value) {
            LOG_INFO("PersistentSR lease-reset-origin logical={} owner={} backend={} gameResetPresent={} "
                     "gameResetType={} gameResetValue={} leaseReset=1 effectiveReset=1 success={} pending={}",
                     uintptr_t(found.handle), uintptr_t(&owner), found.slot->backendId, present,
                     type, value, succeeded, owner.resetPending);
        };
        if (!present) report("absent", "absent");
        else if (reset->second.key == typeid(int).hash_code()) report("int", reset->second.values.i);
        else if (reset->second.key == typeid(unsigned).hash_code()) report("unsigned", reset->second.values.ui);
        else if (reset->second.key == typeid(float).hash_code()) report("float", reset->second.values.f);
        else if (reset->second.key == typeid(double).hash_code()) report("double", reset->second.values.d);
        else if (reset->second.key == typeid(unsigned long long).hash_code()) report("unsigned-long-long", reset->second.values.ull);
        else if (reset->second.key == typeid(void*).hash_code()) report("pointer", uintptr_t(reset->second.values.vp));
        else report("unsupported-type-key", reset->second.key);
        owner.resetTraceRecorded = true;
    }
    return succeeded;
}
inline std::optional<NVSDK_NGX_Result> TryRelease(const NVSDK_NGX_Handle* handle)
{
    if (!IsStarRail()) return std::nullopt;
    auto& pool = Instance(); auto found = pool.Lookup(handle);
    if (found.status == Status::Unknown) return std::nullopt;
    if (found.status == Status::Stale) { ++Stats().stale; return NVSDK_NGX_Result_FAIL_FeatureNotFound; }
    if (found.slot->owner->missingExposurePending &&
        !PrepareEvaluate(found, handle, found.slot->owner->context11.Get()))
        return NVSDK_NGX_Result_FAIL_FeatureNotFound;
    pool.Release(handle); ++Stats().parked; LogCounts("parked");
    // Retain currentFeature identity for the parent's independent current-picture
    // producer. A parked owner does not claim fresh SR frames or FG submissions.
    return NVSDK_NGX_Result_Success;
}
inline bool PrepareEvaluate(Pool::Result& found, const NVSDK_NGX_Handle* handle, ID3D11DeviceContext* context)
{
    auto& pool = Instance(); auto& state = State::Instance();
    if (found.status != Status::Reused || !Healthy(*found.slot, context))
    { pool.Retire(handle); ++Stats().stale; return false; }
    auto* slot = found.slot;
    const auto changed = state.changeBackend.find(slot->backendId);
    if (changed == state.changeBackend.end() || !changed->second) return true;
    // Only the bridge's first missing-exposure discovery can create a retained
    // replacement. Changed configuration/backend requests explicitly retire.
    const bool sameBackend = slot->owner->missingExposurePending &&
        state.newBackend == slot->owner->exposureBackendRequest;
    if (!sameBackend || state.autoExposure != std::optional<bool>(true) || slot->owner->context.feature->AutoExposure() ||
        !ConfigurationMatches(*slot, Configuration()) || !pool.HasSpareOwner())
    { pool.Retire(handle); ++Stats().failures; LogCounts("backend-change-retired"); return false; }
    auto freshOwner = std::make_unique<Owner>(); freshOwner->requested = slot->owner->requested;
    Key key = slot->key; key.autoExposure = true;
    if (!Bind(*freshOwner, context, key.binding) || key.binding != slot->key.binding)
    { pool.Retire(handle); return false; }
    const auto backendId = IFeature::GetNextHandleId();
    auto reserved = pool.Reserve(key, backendId, freshOwner);
    if (reserved.status != Status::Reserved || !Initialize(*reserved.slot))
    { pool.Retire(handle); return false; }
    const auto oldId = slot->backendId;
    const auto* oldFeature = slot->owner->context.feature.get();
    auto replacement = pool.PublishReplacement(handle, reserved.slot);
    if (replacement.status != Status::Created)
    { pool.FailReservation(reserved.slot); pool.Retire(handle); return false; }
    state.changeBackend.erase(oldId); state.changeBackend.erase(backendId);
    if (state.currentFeature == oldFeature)
        state.currentFeature = replacement.slot->owner->context.feature.get();
    state.autoExposure = true;
    found = replacement; ++Stats().replacements; LogCounts("learned-auto-exposure"); return true;
}
inline void Shutdown()
{
    if (!IsStarRail()) return;
    Instance().Shutdown(); LogCounts("shutdown-closed");
}
}

// Standalone CPU contract tests; no Windows/device, SDK entry points or GPU.
// Lead can compile as C++20 with external/nvngx_dlss_sdk on the include path.
// Tests exercise the production header, not a copied parameter resolver.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../UpscalerFrameParameters.h"
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <variant>
#include <vector>

using namespace fg_inputs;

namespace
{
// Synthetic table with the real SDK parameter interface. Pointer values are
// inert test identities: no COM call, resource access, or writer proof occurs.
struct Parameters final : NVSDK_NGX_Parameter
{
    using Value = std::variant<unsigned long long, float, double, unsigned int, int,
                               ID3D11Resource*, ID3D12Resource*, void*>;
    std::unordered_map<std::string, Value> values;
    mutable std::unordered_map<std::string, unsigned> reads;
    mutable unsigned reads11 = 0, reads12 = 0, readsVoid = 0;
    bool writeOnFailure = false;

    template <class T> NVSDK_NGX_Result Read(const char* key, T* value) const
    {
        ++reads[key];
        if constexpr (std::is_same_v<T, ID3D11Resource*>) ++reads11;
        if constexpr (std::is_same_v<T, ID3D12Resource*>) ++reads12;
        if constexpr (std::is_same_v<T, void*>) ++readsVoid;
        const auto it = values.find(key);
        if (it != values.end())
        {
            if (const auto* stored = std::get_if<T>(&it->second))
            {
                *value = *stored;
                return NVSDK_NGX_Result_Success;
            }
        }
        if (writeOnFailure)
        {
            if constexpr (std::is_pointer_v<T>)
                *value = reinterpret_cast<T>(uintptr_t { 0xbad });
            else
                *value = static_cast<T>(123);
        }
        return NVSDK_NGX_Result_FAIL_InvalidParameter;
    }

#define TEST_PARAMETER_OVERLOAD(T) \
    void Set(const char* key, T value) override { values.insert_or_assign(key, value); } \
    NVSDK_NGX_Result Get(const char* key, T* value) const override { return Read(key, value); }
    TEST_PARAMETER_OVERLOAD(unsigned long long)
    TEST_PARAMETER_OVERLOAD(float)
    TEST_PARAMETER_OVERLOAD(double)
    TEST_PARAMETER_OVERLOAD(unsigned int)
    TEST_PARAMETER_OVERLOAD(int)
    TEST_PARAMETER_OVERLOAD(ID3D11Resource*)
    TEST_PARAMETER_OVERLOAD(ID3D12Resource*)
    TEST_PARAMETER_OVERLOAD(void*)
#undef TEST_PARAMETER_OVERLOAD

    void Reset() override { values.clear(); }
    unsigned ReadCount(const char* key) const
    {
        const auto it = reads.find(key);
        return it == reads.end() ? 0 : it->second;
    }
};

constexpr CameraParameterKeys cameraKeys {
    "FSR.cameraNear", "FSR.cameraFar", "FSR.cameraFovAngleVertical", "FSR.viewSpaceToMetersFactor"
};

FeatureParameters Feature()
{
    FeatureParameters feature {};
    feature.render = { 1000, 500 };
    feature.target = { 2000, 1000 };
    feature.display = { 3000, 2000 };
    feature.lowResolutionMotionVectors = true;
    return feature;
}

CameraPolicy Policy()
{
    return { true, 0.1f, 100000.0f, 70.0f, 0.0f };
}

void SetCamera(Parameters& parameters)
{
    parameters.Set(cameraKeys.nearPlane, 1.25f);
    parameters.Set(cameraKeys.farPlane, 450.0f);
    parameters.Set(cameraKeys.verticalFov, 1.125f); // already radians
    parameters.Set(cameraKeys.viewSpaceToMeters, 0.01f);
}

void SetTemporal(Parameters& parameters)
{
    parameters.Set(NVSDK_NGX_Parameter_MV_Scale_X, 1000.0f);
    parameters.Set(NVSDK_NGX_Parameter_MV_Scale_Y, -500.0f);
    parameters.Set(NVSDK_NGX_Parameter_Jitter_Offset_X, -0.375f);
    parameters.Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, 0.125f);
    parameters.Set(NVSDK_NGX_Parameter_Reset, 7);
}

void InputUnitsAndPriority()
{
    Parameters parameters;
    SetCamera(parameters);
    SetTemporal(parameters);
    auto feature = Feature();
    feature.invertedDepth = true;
    const auto frame = ReadNgxFrameParameters(parameters, feature, Policy(), 16.25, cameraKeys);
    assert(frame.cameraNear.value == 1.25f && frame.cameraFar.value == 450.0f);
    assert(frame.cameraNear.origin == RequestOrigin::NgxParameter);
    assert(frame.cameraFar.origin == RequestOrigin::NgxParameter); // no second inversion
    assert(frame.verticalFovRadians.value == 1.125f);
    assert(frame.verticalFovRadians.origin == RequestOrigin::NgxParameter);
    assert(frame.viewSpaceToMeters.value == 0.01f);
    assert(frame.viewSpaceToMeters.origin == RequestOrigin::NgxParameter);
    assert(frame.aspectRatio == 1.5f); // display aspect, not target/render aspect
    assert(frame.frameTimeDeltaMs == 16.25); // no seconds conversion
    assert(frame.motionScaleX.value == 1000.0f && frame.motionScaleY.value == -500.0f);
    assert(frame.jitterX.value == -0.375f && frame.jitterY.value == 0.125f);
    assert(frame.reset.value == 7); // preserve request; no invented reset policy
    assert(parameters.ReadCount(cameraKeys.viewSpaceToMeters) == 1);
}

void DisabledInputPolicyDoesNotReadFsrCameraValues()
{
    Parameters parameters;
    SetCamera(parameters);
    SetTemporal(parameters);
    auto policy = Policy();
    policy.useInputValues = false;
    for (const bool inverted : { false, true })
    {
        auto feature = Feature();
        feature.invertedDepth = inverted;
        const auto frame = ReadNgxFrameParameters(parameters, feature, policy, 0.0, cameraKeys);
        assert(frame.cameraNear.value == (inverted ? policy.farPlane : policy.nearPlane));
        assert(frame.cameraFar.value == (inverted ? policy.nearPlane : policy.farPlane));
        assert(frame.cameraNear.origin == RequestOrigin::Configuration);
        assert(frame.cameraFar.origin == RequestOrigin::Configuration);
        assert(std::abs(frame.verticalFovRadians.value - 1.2217304764f) < 0.000001f);
        assert(frame.verticalFovRadians.origin == RequestOrigin::Configuration);
        assert(frame.viewSpaceToMeters.value == 0.0f);
        assert(frame.viewSpaceToMeters.origin == RequestOrigin::Unavailable);
        assert(frame.motionScaleX.value == 1000.0f); // temporal inputs are independent
    }
    for (const auto* key : { cameraKeys.nearPlane, cameraKeys.farPlane, cameraKeys.verticalFov,
                             cameraKeys.viewSpaceToMeters })
        assert(parameters.ReadCount(key) == 0);
}

void MissingOrPartialCameraUsesCompleteFallback()
{
    // None, near only, far only, both zero. Partial reads used to silently mix
    // a requested plane with a fabricated zero, including on failed Get.
    for (unsigned scenario = 0; scenario != 4; ++scenario)
    {
        Parameters parameters;
        parameters.writeOnFailure = true;
        if (scenario == 1) parameters.Set(cameraKeys.nearPlane, 3.0f);
        if (scenario == 2) parameters.Set(cameraKeys.farPlane, 12.0f);
        if (scenario == 3)
        {
            parameters.Set(cameraKeys.nearPlane, 0.0f);
            parameters.Set(cameraKeys.farPlane, 0.0f);
        }
        for (const bool inverted : { false, true })
        {
            auto feature = Feature();
            feature.invertedDepth = inverted;
            const auto policy = Policy();
            const auto frame = ReadNgxFrameParameters(parameters, feature, policy, 0.0, cameraKeys);
            assert(frame.cameraNear.origin == RequestOrigin::Configuration);
            assert(frame.cameraFar.origin == RequestOrigin::Configuration);
            assert(frame.cameraNear.value == (inverted ? policy.farPlane : policy.nearPlane));
            assert(frame.cameraFar.value == (inverted ? policy.nearPlane : policy.farPlane));
        }
    }
}

void SuppliedCameraValuesAreRequestsNotValidation()
{
    Parameters parameters;
    parameters.Set(cameraKeys.nearPlane, 0.0f);
    parameters.Set(cameraKeys.farPlane, std::numeric_limits<float>::infinity());
    parameters.Set(cameraKeys.verticalFov, 0.0f);
    const auto frame = ReadNgxFrameParameters(parameters, Feature(), Policy(), 0.0, cameraKeys);
    assert(frame.cameraNear.origin == RequestOrigin::NgxParameter && frame.cameraNear.value == 0.0f);
    assert(frame.cameraFar.origin == RequestOrigin::NgxParameter && std::isinf(frame.cameraFar.value));
    assert(frame.verticalFovRadians.origin == RequestOrigin::NgxParameter);
    assert(frame.verticalFovRadians.value == 0.0f); // no silently "validated" FOV
    // The descriptor does not synthesize InfiniteDepth, a projection or basis.
}

void FovFallbacksUseDegreesAndTargetExtent()
{
    Parameters parameters;
    auto policy = Policy();
    auto frame = ReadNgxFrameParameters(parameters, Feature(), policy, 0.0, cameraKeys);
    assert(std::abs(frame.verticalFovRadians.value - 1.2217304764f) < 0.000001f);
    assert(frame.verticalFovRadians.origin == RequestOrigin::Configuration);
    policy.verticalFovDegrees.reset();
    policy.horizontalFovDegrees = 90.0f;
    frame = ReadNgxFrameParameters(parameters, Feature(), policy, 0.0, cameraKeys);
    assert(std::abs(frame.verticalFovRadians.value - 2.0f * std::atan(0.5f)) < 0.000001f);
    assert(frame.verticalFovRadians.origin == RequestOrigin::Configuration);
    policy.horizontalFovDegrees = 0.0f;
    frame = ReadNgxFrameParameters(parameters, Feature(), policy, 0.0, cameraKeys);
    assert(std::abs(frame.verticalFovRadians.value - 1.0471975512f) < 0.000001f);
    assert(frame.verticalFovRadians.origin == RequestOrigin::CompatibilityDefault);
}

void MissingExtentCannotEstablishHorizontalFov()
{
    Parameters parameters;
    auto policy = Policy();
    policy.verticalFovDegrees.reset();
    policy.horizontalFovDegrees = 90.0f;
    for (const auto extent : { Extent { 0, 1000 }, Extent { 2000, 0 }, Extent {} })
    {
        auto feature = Feature();
        feature.target = extent;
        feature.display.height = 0;
        const auto frame = ReadNgxFrameParameters(parameters, feature, policy, 0.0, cameraKeys);
        assert(frame.verticalFovRadians.value == 0.0f);
        assert(frame.verticalFovRadians.origin == RequestOrigin::Unavailable);
        assert(frame.aspectRatio == 0.0f && std::isfinite(frame.aspectRatio));
        assert(frame.feature.target.width == extent.width && frame.feature.target.height == extent.height);
    }
}

void MeterFactorPolicyAndMissingValue()
{
    Parameters parameters;
    auto frame = ReadNgxFrameParameters(parameters, Feature(), Policy(), 0.0, cameraKeys);
    assert(frame.viewSpaceToMeters.value == 0.0f && frame.viewSpaceToMeters.origin == RequestOrigin::Unavailable);
    for (const float requested : { 0.0f, 0.01f, 100.0f })
    {
        parameters.Set(cameraKeys.viewSpaceToMeters, requested);
        frame = ReadNgxFrameParameters(parameters, Feature(), Policy(), 0.0, cameraKeys);
        assert(frame.viewSpaceToMeters.value == requested);
        assert(frame.viewSpaceToMeters.origin == RequestOrigin::NgxParameter);
    }
}

void MissingTemporalValuesAreNotEncoreConstants()
{
    Parameters parameters;
    const auto frame = ReadNgxFrameParameters(parameters, Feature(), Policy(), 0.0, cameraKeys);
    for (const auto value : { frame.motionScaleX, frame.motionScaleY, frame.jitterX, frame.jitterY })
    {
        assert(value.value == 0.0f);
        assert(value.origin == RequestOrigin::Unavailable);
    }
    assert(frame.reset.value == 0 && frame.reset.origin == RequestOrigin::Unavailable);
    parameters.Set(NVSDK_NGX_Parameter_Jitter_Offset_X, 0.0f);
    parameters.Set(NVSDK_NGX_Parameter_MV_Scale_X, 0.0f);
    parameters.Set(NVSDK_NGX_Parameter_Reset, 0);
    const auto zero = ReadNgxFrameParameters(parameters, Feature(), Policy(), 0.0, cameraKeys);
    assert(zero.jitterX.value == 0.0f && zero.jitterX.origin == RequestOrigin::NgxParameter);
    assert(zero.motionScaleX.value == 0.0f && zero.motionScaleX.origin == RequestOrigin::NgxParameter);
    assert(zero.reset.value == 0 && zero.reset.origin == RequestOrigin::NgxParameter);
}

void ExposureScalarsRemainDistinctRequests()
{
    Parameters parameters;
    auto frame = ReadNgxFrameParameters(parameters, Feature(), Policy(), 0.0, cameraKeys);
    assert(frame.preExposure.value == 1.0f && frame.exposureScale.value == 1.0f);
    assert(frame.preExposure.origin == RequestOrigin::CompatibilityDefault);
    assert(frame.exposureScale.origin == RequestOrigin::CompatibilityDefault);
    parameters.Set(NVSDK_NGX_Parameter_DLSS_Pre_Exposure, 2.5f);
    frame = ReadNgxFrameParameters(parameters, Feature(), Policy(), 0.0, cameraKeys);
    assert(frame.preExposure.value == 2.5f && frame.preExposure.origin == RequestOrigin::NgxParameter);
    assert(frame.exposureScale.value == 1.0f && frame.exposureScale.origin == RequestOrigin::CompatibilityDefault);
    parameters.Set(NVSDK_NGX_Parameter_DLSS_Exposure_Scale, 0.25f);
    frame = ReadNgxFrameParameters(parameters, Feature(), Policy(), 0.0, cameraKeys);
    assert(frame.preExposure.value == 2.5f && frame.exposureScale.value == 0.25f);
    assert(frame.exposureScale.origin == RequestOrigin::NgxParameter);
    parameters.Set(NVSDK_NGX_Parameter_DLSS_Pre_Exposure, 0.0f);
    frame = ReadNgxFrameParameters(parameters, Feature(), Policy(), 0.0, cameraKeys);
    assert(frame.preExposure.value == 0.0f && frame.preExposure.origin == RequestOrigin::NgxParameter);
    assert(parameters.ReadCount(NVSDK_NGX_Parameter_ExposureTexture) == 0);
}

void NonfiniteRequestsAreNotSilentlyCertifiedOrRewritten()
{
    Parameters parameters;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    parameters.Set(NVSDK_NGX_Parameter_Jitter_Offset_X, nan);
    parameters.Set(NVSDK_NGX_Parameter_DLSS_Pre_Exposure, nan);
    const auto frame = ReadNgxFrameParameters(parameters, Feature(), Policy(), 0.0, cameraKeys);
    assert(std::isnan(frame.jitterX.value) && frame.jitterX.origin == RequestOrigin::NgxParameter);
    assert(std::isnan(frame.preExposure.value) && frame.preExposure.origin == RequestOrigin::NgxParameter);
    // Origin records Get success only; no Valid/Verified/Accepted claim exists.
}

void FailingGetterCannotLeakItsOutput()
{
    Parameters parameters;
    parameters.writeOnFailure = true;
    const auto frame = ReadNgxFrameParameters(parameters, Feature(), Policy(), 0.0, cameraKeys);
    assert(frame.cameraNear.value == Policy().nearPlane && frame.cameraFar.value == Policy().farPlane);
    assert(std::abs(frame.verticalFovRadians.value - 1.2217304764f) < 0.000001f);
    assert(frame.viewSpaceToMeters.value == 0.0f && frame.motionScaleX.value == 0.0f);
    assert(frame.motionScaleY.value == 0.0f && frame.jitterX.value == 0.0f && frame.jitterY.value == 0.0f);
    assert(frame.reset.value == 0 && frame.preExposure.value == 1.0f && frame.exposureScale.value == 1.0f);
}

// Deliberately different enum values from FG_Flags: common mapping must use
// the named flags, not assume either test masks or production enum ordinals.
enum class Flag : uint64_t
{
    Async = 0x20, DisplayResolutionMVs = 0x08, JitteredMVs = 0x40,
    InvertedDepth = 0x02, InfiniteDepth = 0x80, Hdr = 0x100
};

struct Constants
{
    struct Flags
    {
        uint64_t mask = 0;
        void operator|=(Flag value) { mask |= static_cast<uint64_t>(value); }
    } flags;
    uint32_t displayWidth = 0, displayHeight = 0;
};

void EveryFlagCombinationUsesFeatureRequests()
{
    for (unsigned mask = 0; mask != 32; ++mask)
    {
        UpscalerFrameParameters frame {};
        frame.feature = Feature();
        frame.feature.hdr = (mask & 1) != 0;
        frame.feature.invertedDepth = (mask & 2) != 0;
        frame.feature.jitteredMotionVectors = (mask & 4) != 0;
        frame.feature.lowResolutionMotionVectors = (mask & 8) == 0;
        frame.feature.async = (mask & 16) != 0;
        const auto constants = MakeFgConstants<Constants, Flag>(frame);
        const std::array<Flag, 5> flags { Flag::Hdr, Flag::InvertedDepth, Flag::JitteredMVs,
                                        Flag::DisplayResolutionMVs, Flag::Async };
        for (unsigned bit = 0; bit != flags.size(); ++bit)
            assert(((constants.flags.mask & static_cast<uint64_t>(flags[bit])) != 0) == ((mask & (1u << bit)) != 0));
        assert((constants.flags.mask & static_cast<uint64_t>(Flag::InfiniteDepth)) == 0);
        assert(constants.displayWidth == 3000 && constants.displayHeight == 2000);
    }
}

struct FgSink
{
    enum class Setter { Camera, FrameTime, Motion, Jitter, Reset, Rect };
    std::array<float, 5> camera {};
    std::array<float, 2> motion {}, jitter {};
    double frameTime = 0.0;
    unsigned int reset = 0;
    uint64_t width = 0;
    uint32_t height = 0;
    std::vector<Setter> calls;
    void SetCameraValues(float nearPlane, float farPlane, float fov, float aspect, float meters)
    { camera = { nearPlane, farPlane, fov, aspect, meters }; calls.push_back(Setter::Camera); }
    void SetFrameTimeDelta(double value) { frameTime = value; calls.push_back(Setter::FrameTime); }
    void SetMVScale(float x, float y) { motion = { x, y }; calls.push_back(Setter::Motion); }
    void SetJitter(float x, float y) { jitter = { x, y }; calls.push_back(Setter::Jitter); }
    void SetReset(unsigned int value) { reset = value; calls.push_back(Setter::Reset); }
    void SetInterpolationRect(uint64_t x, uint32_t y) { width = x; height = y; calls.push_back(Setter::Rect); }
};

void SettersUseOneSnapshotWithoutResourceOrProviderOperations()
{
    Parameters parameters;
    SetCamera(parameters);
    SetTemporal(parameters);
    const auto frame = ReadNgxFrameParameters(parameters, Feature(), Policy(), 16.123456789, cameraKeys);
    const auto reads = parameters.reads;
    parameters.Reset(); // applying the descriptor must not re-query a later table
    FgSink sink;
    ApplyFrameParameters(sink, frame);
    const std::array<float, 5> expectedCamera { 1.25f, 450.0f, 1.125f, 1.5f, 0.01f };
    const std::array<float, 2> expectedMotion { 1000.0f, -500.0f }, expectedJitter { -0.375f, 0.125f };
    assert(sink.camera == expectedCamera && sink.motion == expectedMotion && sink.jitter == expectedJitter);
    assert(sink.frameTime == 16.123456789 && sink.reset == 7);
    assert(sink.width == 3000 && sink.height == 2000);
    const std::vector<FgSink::Setter> expectedCalls {
        FgSink::Setter::Camera, FgSink::Setter::FrameTime, FgSink::Setter::Motion,
        FgSink::Setter::Jitter, FgSink::Setter::Reset, FgSink::Setter::Rect
    };
    assert(sink.calls == expectedCalls && parameters.reads == reads);
    // No exposure setter, source receipt, slot advance or GPU API is available
    // to this sink. Requests cannot acquire those authorities through Apply.
}

void NextFrameDoesNotInheritTemporalOrExposureRequests()
{
    Parameters parameters;
    SetTemporal(parameters);
    parameters.Set(NVSDK_NGX_Parameter_DLSS_Pre_Exposure, 2.0f);
    FgSink sink;
    ApplyFrameParameters(sink, ReadNgxFrameParameters(parameters, Feature(), Policy(), 5.0, cameraKeys));
    assert(sink.reset == 7 && sink.motion[0] == 1000.0f);
    parameters.Reset();
    const auto next = ReadNgxFrameParameters(parameters, Feature(), Policy(), 0.0, cameraKeys);
    ApplyFrameParameters(sink, next);
    assert(sink.reset == 0 && sink.motion[0] == 0.0f && sink.motion[1] == 0.0f);
    assert(sink.jitter[0] == 0.0f && sink.jitter[1] == 0.0f && sink.frameTime == 0.0);
    assert(next.reset.origin == RequestOrigin::Unavailable);
    assert(next.preExposure.value == 1.0f && next.preExposure.origin == RequestOrigin::CompatibilityDefault);
}

template <class T> struct Option
{
    T defaultValue;
    std::optional<T> explicitValue;
    explicit Option(T value) : defaultValue(value) {}
    T value_or_default() const { return explicitValue.value_or(defaultValue); }
    bool has_value() const { return explicitValue.has_value(); }
    T value() const { return explicitValue.value(); }
};

struct Configuration
{
    Option<bool> FsrUseFsrInputValues { true }, FGAsync { true };
    Option<float> FsrCameraNear { 0.1f }, FsrCameraFar { 100000.0f };
    Option<float> FsrVerticalFov { 60.0f }, FsrHorizontalFov { 0.0f };
};

struct NativeFeature
{
    unsigned RenderWidth() { return 1000; }
    unsigned RenderHeight() { return 500; }
    unsigned TargetWidth() { return 2000; }
    unsigned TargetHeight() { return 1000; }
    unsigned DisplayWidth() { return 3000; }
    unsigned DisplayHeight() { return 2000; }
    bool IsHdr() { return true; }
    bool DepthInverted() { return true; }
    bool JitteredMV() { return true; }
    bool LowResMV() { return false; }
};
struct InteropFeature : NativeFeature {};

void BothFeatureContractsUseSameParameterAndFlagAdapter()
{
    Parameters parameters;
    SetTemporal(parameters);
    Configuration config;
    config.FsrUseFsrInputValues.explicitValue = false;
    config.FsrCameraNear.explicitValue = 0.25f;
    config.FsrCameraFar.explicitValue = 8000.0f;
    config.FsrHorizontalFov.explicitValue = 90.0f;
    NativeFeature native;
    InteropFeature interop;
    const auto nativeFrame = ReadFrameParameters(parameters, native, config, 10.0, cameraKeys);
    const auto interopFrame = ReadFrameParameters(parameters, interop, config, 10.0, cameraKeys);
    FgSink nativeSink, interopSink;
    ApplyFrameParameters(nativeSink, nativeFrame);
    ApplyFrameParameters(interopSink, interopFrame);
    assert(nativeSink.camera == interopSink.camera && nativeSink.motion == interopSink.motion);
    assert(nativeSink.jitter == interopSink.jitter && nativeSink.reset == interopSink.reset);
    assert(nativeSink.calls == interopSink.calls && nativeSink.frameTime == interopSink.frameTime);
    assert(nativeSink.camera[0] == 8000.0f && nativeSink.camera[1] == 0.25f);
    assert(std::abs(nativeSink.camera[2] - 2.0f * std::atan(0.5f)) < 0.000001f);
    const auto nativeConstants = MakeFgConstants<Constants, Flag>(nativeFrame);
    const auto interopConstants = MakeFgConstants<Constants, Flag>(interopFrame);
    assert(nativeConstants.flags.mask == interopConstants.flags.mask);
    assert(nativeConstants.flags.mask == (0x100 | 0x02 | 0x40 | 0x08 | 0x20));
    assert(nativeFrame.feature.render.width == 1000 && interopFrame.feature.target.height == 1000);
    config.FsrVerticalFov.explicitValue = 80.0f;
    const auto configuredVertical = ReadFrameParameters(parameters, native, config, 10.0, cameraKeys);
    assert(std::abs(configuredVertical.verticalFovRadians.value - 1.3962634016f) < 0.000001f);
}

void ResourceGetterUsesActualApiOverloadAndNoCachedIdentity()
{
    Parameters parameters;
    int first = 0, second = 0;
    auto* dx11 = reinterpret_cast<ID3D11Resource*>(&first);
    auto* dx12 = reinterpret_cast<ID3D12Resource*>(&second);
    const auto* key = NVSDK_NGX_Parameter_Depth;
    parameters.Set(key, dx11);
    assert(ReadResource<ID3D11Resource>(parameters, key) == dx11);
    assert(parameters.reads11 == 1 && parameters.reads12 == 0 && parameters.readsVoid == 0);
    parameters.Set(key, dx12);
    assert(ReadResource<ID3D12Resource>(parameters, key) == dx12);
    assert(parameters.reads11 == 1 && parameters.reads12 == 1 && parameters.readsVoid == 0);
    // A different API's typed entry is not a resource conversion.
    assert(ReadResource<ID3D11Resource>(parameters, key) == nullptr);
    parameters.Set(key, static_cast<void*>(&first));
    assert(ReadResource<ID3D12Resource>(parameters, key) == reinterpret_cast<ID3D12Resource*>(&first));
    parameters.Set(key, static_cast<void*>(&second));
    assert(ReadResource<ID3D11Resource>(parameters, key) == reinterpret_cast<ID3D11Resource*>(&second));
    parameters.Reset();
    assert(ReadResource<ID3D12Resource>(parameters, key) == nullptr);
}

void ResourceFailuresCannotLeakAndTypedNullIsNotFallback()
{
    Parameters parameters;
    parameters.writeOnFailure = true;
    const auto* key = NVSDK_NGX_Parameter_MotionVectors;
    assert(ReadResource<ID3D11Resource>(parameters, key) == nullptr);
    assert(ReadResource<ID3D12Resource>(parameters, key) == nullptr);
    const auto fallbackReads = parameters.readsVoid;
    parameters.Set(key, static_cast<ID3D12Resource*>(nullptr));
    assert(ReadResource<ID3D12Resource>(parameters, key) == nullptr);
    assert(parameters.readsVoid == fallbackReads); // success/null retains old semantics
    int storage = 0;
    parameters.Set(key, static_cast<void*>(&storage));
    assert(ReadResource<ID3D12Resource>(parameters, key) == reinterpret_cast<ID3D12Resource*>(&storage));
}
} // namespace

int main()
{
    const std::array tests {
        InputUnitsAndPriority, DisabledInputPolicyDoesNotReadFsrCameraValues,
        MissingOrPartialCameraUsesCompleteFallback, SuppliedCameraValuesAreRequestsNotValidation,
        FovFallbacksUseDegreesAndTargetExtent, MissingExtentCannotEstablishHorizontalFov,
        MeterFactorPolicyAndMissingValue, MissingTemporalValuesAreNotEncoreConstants,
        ExposureScalarsRemainDistinctRequests, NonfiniteRequestsAreNotSilentlyCertifiedOrRewritten,
        FailingGetterCannotLeakItsOutput, EveryFlagCombinationUsesFeatureRequests,
        SettersUseOneSnapshotWithoutResourceOrProviderOperations, NextFrameDoesNotInheritTemporalOrExposureRequests,
        BothFeatureContractsUseSameParameterAndFlagAdapter, ResourceGetterUsesActualApiOverloadAndNoCachedIdentity,
        ResourceFailuresCannotLeakAndTypedNullIsNotFallback
    };
    for (const auto test : tests) test();
    std::printf("PASS %zu upscaler frame-parameter CPU test groups (synthetic inputs only)\n", tests.size());
}

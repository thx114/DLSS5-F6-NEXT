#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <nvsdk_ngx_params.h>
#include "../../MathUtils.h"

namespace fg_inputs
{
// These are request origins, NOT camera/producer validation or SDK acceptance.
// In particular, a successful NGX Get says nothing about the writer, frame,
// resource identity, physical units or GPU completion.
enum class RequestOrigin { Unavailable, NgxParameter, Configuration, CompatibilityDefault };

template <class T> struct RequestedValue
{
    T value {};
    RequestOrigin origin = RequestOrigin::Unavailable;
};

struct Extent
{
    uint32_t width = 0;
    uint32_t height = 0;
};

// Bind the existing OptiKeys at the API boundary. Keeping those four names out
// of this header avoids importing Config/State/logging into the CPU contract.
struct CameraParameterKeys
{
    const char* nearPlane;
    const char* farPlane;
    const char* verticalFov;
    const char* viewSpaceToMeters;
};

struct CameraPolicy
{
    bool useInputValues = false;
    float nearPlane = 0.0f;
    float farPlane = 0.0f;
    std::optional<float> verticalFovDegrees;
    float horizontalFovDegrees = 0.0f;
};

struct FeatureParameters
{
    Extent render;
    Extent target;
    Extent display;
    bool hdr = false;
    bool invertedDepth = false;
    bool jitteredMotionVectors = false;
    bool lowResolutionMotionVectors = false;
    bool async = false;
};

struct UpscalerFrameParameters
{
    FeatureParameters feature;
    RequestedValue<float> cameraNear;
    RequestedValue<float> cameraFar;
    RequestedValue<float> verticalFovRadians;
    RequestedValue<float> viewSpaceToMeters;
    float aspectRatio = 0.0f;
    double frameTimeDeltaMs = 0.0;
    // Preserve the NGX input convention. Neither native D3D12 nor D3D11-on-12
    // authorizes a jitter sign/bias change or an extra MV/extent conversion.
    RequestedValue<float> motionScaleX;
    RequestedValue<float> motionScaleY;
    RequestedValue<float> jitterX;
    RequestedValue<float> jitterY;
    RequestedValue<int> reset;
    // Distinct NGX scalars, never exposure-texture contents or each other's
    // aliases. IFGFeature has no exposure setter; these remain request metadata.
    RequestedValue<float> preExposure;
    RequestedValue<float> exposureScale;
};

template <class T>
inline RequestedValue<T> ReadRequestedValue(const NVSDK_NGX_Parameter& parameters, const char* key,
                                           T fallback = {}, RequestOrigin fallbackOrigin = RequestOrigin::Unavailable)
{
    T value {};
    if (parameters.Get(key, &value) == NVSDK_NGX_Result_Success)
        return { value, RequestOrigin::NgxParameter };
    // A failing external Get may still have touched its output argument.
    return { fallback, fallbackOrigin };
}

inline UpscalerFrameParameters ReadNgxFrameParameters(const NVSDK_NGX_Parameter& parameters,
                                                     const FeatureParameters& feature, const CameraPolicy& policy,
                                                     double frameTimeDeltaMs, const CameraParameterKeys& keys)
{
    UpscalerFrameParameters frame {};
    frame.feature = feature;
    frame.frameTimeDeltaMs = frameTimeDeltaMs;
    if (feature.display.height != 0)
        frame.aspectRatio = static_cast<float>(feature.display.width) / static_cast<float>(feature.display.height);

    // Only a complete requested pair replaces the configured fallback. Do not
    // manufacture a missing plane by mixing one NGX value with a default zero.
    auto nearPlane = RequestedValue<float> {};
    auto farPlane = RequestedValue<float> {};
    if (policy.useInputValues)
    {
        nearPlane = ReadRequestedValue<float>(parameters, keys.nearPlane);
        farPlane = ReadRequestedValue<float>(parameters, keys.farPlane);
    }
    if (nearPlane.origin == RequestOrigin::NgxParameter && farPlane.origin == RequestOrigin::NgxParameter &&
        (nearPlane.value != 0.0f || farPlane.value != 0.0f))
    {
        frame.cameraNear = nearPlane;
        frame.cameraFar = farPlane;
    }
    else
    {
        // Config near/far describe the non-inverted convention; NGX input
        // values already carry their convention and must not be swapped again.
        frame.cameraNear = { feature.invertedDepth ? policy.farPlane : policy.nearPlane,
                             RequestOrigin::Configuration };
        frame.cameraFar = { feature.invertedDepth ? policy.nearPlane : policy.farPlane,
                            RequestOrigin::Configuration };
    }

    if (policy.useInputValues)
        frame.verticalFovRadians = ReadRequestedValue<float>(parameters, keys.verticalFov);
    if (frame.verticalFovRadians.origin != RequestOrigin::NgxParameter)
    {
        if (policy.verticalFovDegrees.has_value())
            frame.verticalFovRadians = { OptiMath::GetRadiansFromDeg(*policy.verticalFovDegrees),
                                         RequestOrigin::Configuration };
        else if (policy.horizontalFovDegrees > 0.0f)
        {
            // Keep the existing target-viewport conversion; a missing extent
            // cannot establish a vertical FOV. Leave its origin unavailable.
            if (feature.target.width != 0 && feature.target.height != 0)
                frame.verticalFovRadians = {
                    OptiMath::GetVerticalFovFromHorizontal(OptiMath::GetRadiansFromDeg(policy.horizontalFovDegrees),
                                                         static_cast<float>(feature.target.width),
                                                         static_cast<float>(feature.target.height)),
                    RequestOrigin::Configuration };
        }
        else
            frame.verticalFovRadians = { OptiMath::GetRadiansFromDeg(60.0f), RequestOrigin::CompatibilityDefault };
    }

    // This is an FSR input value, just like the camera planes and FOV. The old
    // adapters read it only when useInputValues was false (the opposite policy).
    if (policy.useInputValues)
        frame.viewSpaceToMeters = ReadRequestedValue<float>(parameters, keys.viewSpaceToMeters);

    frame.motionScaleX = ReadRequestedValue<float>(parameters, NVSDK_NGX_Parameter_MV_Scale_X);
    frame.motionScaleY = ReadRequestedValue<float>(parameters, NVSDK_NGX_Parameter_MV_Scale_Y);
    frame.jitterX = ReadRequestedValue<float>(parameters, NVSDK_NGX_Parameter_Jitter_Offset_X);
    frame.jitterY = ReadRequestedValue<float>(parameters, NVSDK_NGX_Parameter_Jitter_Offset_Y);
    frame.reset = ReadRequestedValue<int>(parameters, NVSDK_NGX_Parameter_Reset);
    frame.preExposure = ReadRequestedValue<float>(parameters, NVSDK_NGX_Parameter_DLSS_Pre_Exposure, 1.0f,
                                                 RequestOrigin::CompatibilityDefault);
    frame.exposureScale = ReadRequestedValue<float>(parameters, NVSDK_NGX_Parameter_DLSS_Exposure_Scale, 1.0f,
                                                   RequestOrigin::CompatibilityDefault);
    return frame;
}

// Feature/Config are deliberately only accessor contracts. Both real API
// adapters use this overload, while CPU tests need no device or global state.
template <class Feature, class Config>
inline UpscalerFrameParameters ReadFrameParameters(const NVSDK_NGX_Parameter& parameters, Feature& feature,
                                                  const Config& config, double frameTimeDeltaMs,
                                                  const CameraParameterKeys& keys)
{
    FeatureParameters inputs {};
    inputs.render = { feature.RenderWidth(), feature.RenderHeight() };
    inputs.target = { feature.TargetWidth(), feature.TargetHeight() };
    inputs.display = { feature.DisplayWidth(), feature.DisplayHeight() };
    inputs.hdr = feature.IsHdr();
    inputs.invertedDepth = feature.DepthInverted();
    inputs.jitteredMotionVectors = feature.JitteredMV();
    inputs.lowResolutionMotionVectors = feature.LowResMV();
    inputs.async = config.FGAsync.value_or_default();

    CameraPolicy camera {};
    camera.useInputValues = config.FsrUseFsrInputValues.value_or_default();
    camera.nearPlane = config.FsrCameraNear.value_or_default();
    camera.farPlane = config.FsrCameraFar.value_or_default();
    if (config.FsrVerticalFov.has_value())
        camera.verticalFovDegrees = config.FsrVerticalFov.value();
    camera.horizontalFovDegrees = config.FsrHorizontalFov.value_or_default();
    return ReadNgxFrameParameters(parameters, inputs, camera, frameTimeDeltaMs, keys);
}

// Instantiate with FG_Constants/FG_Flags at the two production call sites.
// No game identity, provider selection, activation or resource admission here.
template <class Constants, class Flags> inline Constants MakeFgConstants(const UpscalerFrameParameters& frame)
{
    Constants constants {};
    constants.displayWidth = frame.feature.display.width;
    constants.displayHeight = frame.feature.display.height;
    if (frame.feature.hdr)
        constants.flags |= Flags::Hdr;
    if (frame.feature.invertedDepth)
        constants.flags |= Flags::InvertedDepth;
    if (frame.feature.jitteredMotionVectors)
        constants.flags |= Flags::JitteredMVs;
    if (!frame.feature.lowResolutionMotionVectors)
        constants.flags |= Flags::DisplayResolutionMVs;
    if (frame.feature.async)
        constants.flags |= Flags::Async;
    return constants;
}

// Call only after the edge adapter has acquired any required Inputs guard and
// started the frame. This neither reserves a source nor certifies these values.
template <class Fg> inline void ApplyFrameParameters(Fg& fg, const UpscalerFrameParameters& frame)
{
    fg.SetCameraValues(frame.cameraNear.value, frame.cameraFar.value, frame.verticalFovRadians.value,
                       frame.aspectRatio, frame.viewSpaceToMeters.value);
    fg.SetFrameTimeDelta(frame.frameTimeDeltaMs);
    fg.SetMVScale(frame.motionScaleX.value, frame.motionScaleY.value);
    fg.SetJitter(frame.jitterX.value, frame.jitterY.value);
    fg.SetReset(static_cast<unsigned int>(frame.reset.value));
    fg.SetInterpolationRect(frame.feature.display.width, frame.feature.display.height);
}

// Borrow the current typed NGX resource using the API's actual interface. The
// fallback is for parameter-table overload compatibility, not a D3D11/12 cast.
// Ownership, QI/device checks, selected-source joins and queue/fence leases stay
// with the edge adapters/provider. Never cache this pointer as a source receipt.
template <class Resource>
inline Resource* ReadResource(const NVSDK_NGX_Parameter& parameters, const char* key)
{
    Resource* resource = nullptr;
    if (parameters.Get(key, &resource) == NVSDK_NGX_Result_Success)
        return resource;
    void* untyped = nullptr;
    if (parameters.Get(key, &untyped) == NVSDK_NGX_Result_Success)
        return static_cast<Resource*>(untyped);
    return nullptr;
}
} // namespace fg_inputs

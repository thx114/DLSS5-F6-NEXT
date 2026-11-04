// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>

namespace GenshinLegacy
{
struct Settings
{
    bool enabled=true, depthInverted=true, motionJittered=false, positiveMotionScale=true;
    bool depthFlipY=false, motionFlipY=false, jitterDelay=false, forceReset=false;
    bool familySkip=false;
    // Explicit source-observation control. Unknown metadata never enables NR/FG.
    bool captureOnlyObservation=false;
    uint32_t familyExpireMs=500;
    unsigned jitterMode=3;
    bool clampJitterHalf=true;
    bool sharpening=false,autoExposure=true;
    float sharpness=0,motionScale=1,fovScale=1;
    // Legacy defaults are controls, not proof of captured camera/exposure semantics.
    float cameraNear=0.25f, cameraFar=6000.0f, fovRadians=0.7853981634f, preExposure=1.0f;
    bool operator==(const Settings&)const=default;
};
struct ControlsSnapshot
{
    uint32_t version=1,bytes=sizeof(ControlsSnapshot);
    uint64_t epoch=1;
    Settings settings{};
    bool enableFsr2Inputs=true,useFsr2Inputs=true;
    bool hookCpuTiming=false; // Observation only; excluded from Settings/route equality and epoch.
    bool Active()const noexcept{return settings.enabled && enableFsr2Inputs && useFsr2Inputs;}
};
inline bool ValidSettings(const Settings& s)noexcept
{
    return s.familyExpireMs>=100 && s.familyExpireMs<=5000 && s.jitterMode<=5 && std::isfinite(s.motionScale) && s.motionScale>=0 && s.motionScale<=4 &&
        std::isfinite(s.fovScale) && s.fovScale>=0.1f && s.fovScale<=3 &&
        std::isfinite(s.cameraNear) && s.cameraNear>=0.001f && s.cameraNear<=100 &&
        std::isfinite(s.cameraFar) && s.cameraFar>=10 && s.cameraFar<=100000 && s.cameraFar>s.cameraNear &&
        std::isfinite(s.sharpness) && s.sharpness>=0 && s.sharpness<=1 &&
        std::isfinite(s.fovRadians) && s.fovRadians>0 && s.fovRadians*s.fovScale<3.141592654f &&
        std::isfinite(s.preExposure) && s.preExposure>0;
}
template<class T> inline T ParseControl(const char* raw,T fallback)noexcept
{
    if(!raw)return fallback;
    std::string_view text(raw);
    const auto first=text.find_first_not_of(" \t\r\n"),last=text.find_last_not_of(" \t\r\n");
    if(first==text.npos)return fallback;text=text.substr(first,last-first+1);
    T value{};const auto result=std::from_chars(text.data(),text.data()+text.size(),value);
    if(result.ec!=std::errc{} || result.ptr!=text.data()+text.size())return fallback;
    if constexpr(std::is_floating_point_v<T>){if(!std::isfinite(value))return fallback;}
    return value;
}
template<class Ini> inline Settings LoadControls(const Ini& ini)
{
    Settings s;
    const auto value=[&](const char* key){return ini.GetValue("Dx11FsrBridge",key,nullptr);};
    const auto boolean=[&](const char* key,bool fallback){const char* raw=value(key);
        if(raw && std::string_view(raw)=="true")return true;if(raw && std::string_view(raw)=="false")return false;
        return ParseControl<int>(raw,fallback?1:0)!=0;};
    s.enabled=boolean("Enabled",true);s.depthInverted=boolean("Ffx12DepthInverted",true);
    s.motionJittered=boolean("Fsr2MotionVectorsJittered",false);
    s.positiveMotionScale=ParseControl<int>(value("Fsr2MotionVectorScaleMode"),1)==1;
    s.depthFlipY=boolean("Ffx12DepthFlipY",false);s.motionFlipY=boolean("Ffx12MotionFlipY",false);
    s.jitterDelay=boolean("Ffx12JitterDelay",false);s.forceReset=boolean("Ffx12ForceReset",false);
    s.familySkip=boolean("Fsr2FamilySkip",false);
    s.captureOnlyObservation=boolean("CaptureOnlyObservation",false);
    s.familyExpireMs=static_cast<uint32_t>(std::clamp(ParseControl<int>(value("Fsr2FamilyExpireMs"),500),100,5000));
    s.jitterMode=static_cast<unsigned>(std::clamp(ParseControl<int>(value("Ffx12JitterMode"),3),0,5));
    s.autoExposure=boolean("Ffx12AutoExposure",true);
    s.motionScale=std::clamp(ParseControl<float>(value("Ffx12MotionScale"),1),0.0f,4.0f);
    s.fovScale=std::clamp(ParseControl<float>(value("Ffx12FovScale"),1),0.1f,3.0f);
    s.cameraNear=std::clamp(ParseControl<float>(value("Ffx12CameraNear"),0.25f),0.001f,100.0f);
    s.cameraFar=std::clamp(ParseControl<float>(value("Ffx12CameraFar"),6000),10.0f,100000.0f);
    s.sharpness=std::clamp(ParseControl<int>(value("Fsr2SharpnessPercent"),0),0,100)/100.0f;
    s.sharpening=s.sharpness>0;
    if(s.cameraFar<=s.cameraNear){s.cameraNear=0.25f;s.cameraFar=6000;}
    return s;
}
template<class Ini> inline void SaveControls(Ini& ini,const Settings& s)
{
    const auto set=[&](const char* key,auto value){
        char text[64]{};
        using T=decltype(value);
        if constexpr(std::is_floating_point_v<T>){const auto result=std::to_chars(text,text+63,value,std::chars_format::general,std::numeric_limits<T>::max_digits10);*result.ptr=0;}
        else {const auto result=std::to_chars(text,text+63,static_cast<int>(value));*result.ptr=0;}
        ini.SetValue("Dx11FsrBridge",key,text);
    };
    set("Enabled",s.enabled);set("Ffx12DepthInverted",s.depthInverted);set("Fsr2MotionVectorsJittered",s.motionJittered);
    set("Fsr2MotionVectorScaleMode",s.positiveMotionScale?1:0);set("Ffx12DepthFlipY",s.depthFlipY);
    set("Ffx12MotionFlipY",s.motionFlipY);set("Ffx12JitterDelay",s.jitterDelay);set("Ffx12ForceReset",s.forceReset);
    set("Fsr2FamilySkip",s.familySkip);set("Fsr2FamilyExpireMs",s.familyExpireMs);
    set("CaptureOnlyObservation",s.captureOnlyObservation);
    set("Ffx12JitterMode",s.jitterMode);set("Ffx12AutoExposure",s.autoExposure);set("Ffx12MotionScale",s.motionScale);
    set("Ffx12FovScale",s.fovScale);set("Ffx12CameraNear",s.cameraNear);set("Ffx12CameraFar",s.cameraFar);
    set("Fsr2SharpnessPercent",static_cast<int>(std::lround((s.sharpening?s.sharpness:0)*100)));
}
}

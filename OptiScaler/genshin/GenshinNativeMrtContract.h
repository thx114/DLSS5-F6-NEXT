// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "GenshinLegacyControls.h"
#include "GenshinNrProfile.h"
#include "GenshinGameMotionReference.h"
#include "legacy/Il2CppCallSiteHook.h"
#include <inputs/GenshinFsrContract.h>
#include <Logger.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <climits>
#include <limits>

namespace GenshinLegacy::NativeMrt
{
constexpr uint32_t Revision=1;
constexpr uint64_t AccumulateShader=0x78057A29AF6C2D99ull;
enum class Reason : uint8_t { Ready, MissingToken, ReplayToken, WrongOwner, WrongShader, WrongLayout,
    Alias, UnsupportedView, WrongFormat, StaleGeneration, NativeDrawMissing, BindingsChanged,
    SourceSchemaUnknown, JitterPhaseUnknown, OutputRoleUnknown, InvalidParameters, ProviderRejected };
// The fixed Bridge transforms are an operational compatibility profile. They
// do not certify incoming colour/depth/MV meaning or a downstream colour role.
enum class SourceSchema : uint8_t { Unknown, PinnedShaderResourceSchema };
enum class JitterPhase : uint8_t { Unknown, SameRenderForConsumedInputs };
enum class OutputRole : uint8_t { Unknown, NativeRtv1ColourWithCompanionPreserved };
struct Schema
{
    uint32_t revision=0;
    uint64_t shaderIdentity=0,deviceEpoch=0,ownerEpoch=0,configEpoch=0,instance=0,generation=0,drawSerial=0;
    Settings controls{};
    // Selected working colour domain must be stated by the schema adapter.
    bool hdr=false;
    SourceSchema sourceRoles=SourceSchema::Unknown;
    JitterPhase jitterPhase=JitterPhase::Unknown;
    OutputRole selectedOutputRole=OutputRole::Unknown;
    uintptr_t selectedOutputResource=0,selectedOutputView=0,companionResource=0,companionView=0;
    uintptr_t outputConsumerContext=0;
    uint64_t outputConsumerGeneration=0;
    bool companionPreserved=false;
};
struct Surface
{
    uintptr_t resource=0,view=0,device=0;
    D3D11_TEXTURE2D_DESC desc{};
    uint32_t viewFormat=0,viewDimension=0,firstMip=0,mipCount=0;
};
struct Frame
{
    uintptr_t device=0,context=0,constantBuffer=0;
    uint64_t deviceEpoch=0,ownerEpoch=0,configEpoch=0,thread=0,drawSerial=0;
    uint64_t instance=0,generation=0,renderContext=0,tokenTick=0,shader=0,shaderIdentity=0;
    std::shared_ptr<const GenshinNr::ShaderProfile> nrProfile;
    std::shared_ptr<const GenshinGameMotion::NativeReference> gameMotionReference;
    float rawJitterX=0,rawJitterY=0;
    uint32_t frame=0,renderWidth=0,renderHeight=0,outputWidth=0,outputHeight=0;
    std::array<Surface,3> inputs{};
    std::array<Surface,2> outputs{};
    std::array<uint32_t,3> inputSlots{0,2,3};
    uint32_t selectedOutput=1;
    Settings controls{};
    bool immediate=false;
};
// The local Bridge's Genshin profile is concrete runtime behavior, not an INI
// hint: accumulate shader 0x78057A29AF6C2D99, SRV slots t0/t2/t3, RTV1 as the
// selected colour target, and the companion RTV left intact by the native draw.
// Resource/owner/size/format checks below remain mandatory before this profile
// can open the existing internal Dx11Owner consumer.
inline bool Same(const Surface& a,const Surface& b)noexcept
{
    const auto& x=a.desc;const auto& y=b.desc;
    return a.resource==b.resource && a.view==b.view && a.device==b.device && a.viewFormat==b.viewFormat &&
        a.viewDimension==b.viewDimension && a.firstMip==b.firstMip && a.mipCount==b.mipCount &&
        x.Width==y.Width && x.Height==y.Height && x.MipLevels==y.MipLevels && x.ArraySize==y.ArraySize &&
        x.Format==y.Format && x.SampleDesc.Count==y.SampleDesc.Count && x.SampleDesc.Quality==y.SampleDesc.Quality &&
        x.Usage==y.Usage && x.BindFlags==y.BindFlags && x.CPUAccessFlags==y.CPUAccessFlags && x.MiscFlags==y.MiscFlags;
}
// A view selects one physical mip even when its SRV exposes the remaining chain.
// Consumers use Load(...,0), relative to MostDetailedMip, and a single 2D layer.
inline bool MipExtent(const D3D11_TEXTURE2D_DESC& d,uint32_t mip,uint32_t& width,uint32_t& height)noexcept
{
    if(!d.Width || !d.Height || !d.MipLevels || d.MipLevels>D3D11_REQ_MIP_LEVELS || mip>=d.MipLevels)return false;
    width=(std::max)(1u,d.Width>>mip);height=(std::max)(1u,d.Height>>mip);return true;
}
inline bool Full(const Surface& s,bool output)noexcept
{
    const auto& d=s.desc;uint32_t width=0,height=0;
    if(!s.resource || !s.view || !MipExtent(d,s.firstMip,width,height) || d.ArraySize!=1 ||
       d.SampleDesc.Count!=1 || d.SampleDesc.Quality)return false;
    // Native resources stay on their owning device/context; selected data is
    // copied or sampled into private DEFAULT textures. Native Usage/CPU/Misc
    // allocation flags do not change those legal SRV/copy operations.
    if(output)return s.viewDimension==D3D11_RTV_DIMENSION_TEXTURE2D;
    return s.viewDimension==D3D11_SRV_DIMENSION_TEXTURE2D && s.mipCount &&
        (s.mipCount==UINT_MAX || s.mipCount<=d.MipLevels-s.firstMip);
}
inline bool Fits(const Surface& s,uint32_t width,uint32_t height)noexcept
{
    uint32_t actualWidth=0,actualHeight=0;
    return MipExtent(s.desc,s.firstMip,actualWidth,actualHeight) && actualWidth==width && actualHeight==height;
}
// Bridge Gen1 make_srv maps R32G8X24_TYPELESS to its float depth-plane
// SRV. Both forms are read by PrepareShader's Texture2D<float>.Load into
// private R32_FLOAT storage; stencil is not imported by the SR consumer.
inline bool ReadableDepth(DXGI_FORMAT resource,uint32_t view)noexcept
{
    return (view==DXGI_FORMAT_R32_FLOAT && (resource==DXGI_FORMAT_R32_FLOAT || resource==DXGI_FORMAT_R32_TYPELESS)) ||
        (view==DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS && resource==DXGI_FORMAT_R32G8X24_TYPELESS);
}
inline Reason Validate(const Frame& f)noexcept
{
    if(!f.instance || !f.generation || !f.renderContext || !f.tokenTick || !f.frame ||
        !std::isfinite(f.rawJitterX) || !std::isfinite(f.rawJitterY))return Reason::MissingToken;
    if(!f.device || !f.context || !f.deviceEpoch || !f.ownerEpoch || !f.configEpoch || !f.thread || !f.drawSerial || !f.immediate)
        return Reason::WrongOwner;
    if(f.shader!=AccumulateShader || !f.shaderIdentity)return Reason::WrongShader;
    if(f.inputSlots!=std::array<uint32_t,3>{0,2,3} || f.selectedOutput!=1 || !f.constantBuffer ||
        !f.renderWidth || !f.renderHeight || f.outputWidth<f.renderWidth || f.outputHeight<f.renderHeight)return Reason::WrongLayout;
    for(const auto& s:f.inputs)if(!Full(s,false))return Reason::UnsupportedView;
    if(!Full(f.outputs[f.selectedOutput],true))return Reason::UnsupportedView;
    const auto& companion=f.outputs[1-f.selectedOutput];
    if(!companion.resource || !companion.view || companion.device!=f.device)return Reason::WrongLayout;
    for(const auto& s:f.inputs)if(s.device!=f.device || !Fits(s,f.renderWidth,f.renderHeight))return Reason::WrongLayout;
    const auto& selected=f.outputs[f.selectedOutput];
    if(selected.device!=f.device || !Fits(selected,f.outputWidth,f.outputHeight))return Reason::WrongLayout;
    if(f.outputs[0].resource==f.outputs[1].resource)return Reason::Alias;
    for(size_t i=0;i<f.inputs.size();++i){
        for(size_t j=i+1;j<f.inputs.size();++j)if(f.inputs[i].resource==f.inputs[j].resource)return Reason::Alias;
        for(const auto& output:f.outputs)if(f.inputs[i].resource==output.resource)return Reason::Alias;}
    const auto& c=f.inputs[0];const auto& d=f.inputs[1];const auto& m=f.inputs[2];const auto& o=f.outputs[1];
    const bool color=c.viewFormat==DXGI_FORMAT_R16G16B16A16_FLOAT || c.viewFormat==DXGI_FORMAT_R11G11B10_FLOAT ||
        c.viewFormat==DXGI_FORMAT_R8G8B8A8_UNORM || c.viewFormat==DXGI_FORMAT_R10G10B10A2_UNORM;
    const auto pair=[](const Surface& s){return s.desc.Format==s.viewFormat ||
        (s.desc.Format==DXGI_FORMAT_R8G8B8A8_TYPELESS && s.viewFormat==DXGI_FORMAT_R8G8B8A8_UNORM) ||
        (s.desc.Format==DXGI_FORMAT_R16G16B16A16_TYPELESS && s.viewFormat==DXGI_FORMAT_R16G16B16A16_FLOAT) ||
        (s.desc.Format==DXGI_FORMAT_R10G10B10A2_TYPELESS && s.viewFormat==DXGI_FORMAT_R10G10B10A2_UNORM);};
    const bool depth=ReadableDepth(d.desc.Format,d.viewFormat);
    if(!color || !depth || m.viewFormat!=DXGI_FORMAT_R10G10B10A2_UNORM || !pair(m) || !pair(c) || !pair(o) || o.viewFormat!=c.viewFormat)
        return Reason::WrongFormat;
    return Reason::Ready;
}
inline bool BridgeProfile(const Frame& frame,Schema& schema)noexcept
{
    if(Validate(frame)!=Reason::Ready || frame.shader!=AccumulateShader ||
        frame.inputSlots!=std::array<uint32_t,3>{0,2,3} || frame.selectedOutput!=1)return false;
    schema={};schema.revision=Revision;schema.shaderIdentity=frame.shaderIdentity;
    schema.deviceEpoch=frame.deviceEpoch;schema.ownerEpoch=frame.ownerEpoch;schema.configEpoch=frame.configEpoch;
    schema.instance=frame.instance;schema.generation=frame.generation;schema.drawSerial=frame.drawSerial;
    schema.controls=frame.controls;schema.hdr=false;
    schema.sourceRoles=SourceSchema::PinnedShaderResourceSchema;
    schema.jitterPhase=JitterPhase::SameRenderForConsumedInputs;
    schema.selectedOutputRole=OutputRole::NativeRtv1ColourWithCompanionPreserved;
    schema.selectedOutputResource=frame.outputs[frame.selectedOutput].resource;
    schema.selectedOutputView=frame.outputs[frame.selectedOutput].view;
    const auto companion=frame.selectedOutput==0?1u:0u;
    schema.companionResource=frame.outputs[companion].resource;
    schema.companionView=frame.outputs[companion].view;
    schema.outputConsumerContext=frame.context;
    schema.outputConsumerGeneration=frame.generation;
    schema.companionPreserved=true;
    return true;
}
class Transaction final
{
    Frame frame_{};
    struct Used{uint64_t instance=0,generation=0;};
    std::array<Used,8> used_{};
    bool armed_=false,returned_=false,claimed_=false,copyClaimed_=false,replacement_=false;
    Reason ConsumeGeneration()noexcept
    {
        for(auto& used:used_)if(used.instance==frame_.instance){used.generation=frame_.generation;return Reason::Ready;}
        for(auto& used:used_)if(!used.instance){used={frame_.instance,frame_.generation};return Reason::Ready;}
        return Reason::ReplayToken;
    }
public:
    Reason Arm(const Frame& frame)noexcept
    {
        Revoke();const auto reason=Validate(frame);if(reason!=Reason::Ready)return reason;
        bool capacity=false;
        for(const auto& used:used_){if(used.instance==frame.instance && frame.generation<=used.generation)return Reason::ReplayToken;
            capacity=capacity || !used.instance || used.instance==frame.instance;}
        if(!capacity)return Reason::ReplayToken;
        frame_=frame;armed_=true;return Reason::Ready;
    }
    // Same validated inputs/RTV transaction, but BEFORE any original API call.
    // This grants only SR copyback admission, never a native/guide capability.
    Reason BeginReplacement()noexcept
    {
        if(!armed_ || returned_ || claimed_ || replacement_)return Reason::NativeDrawMissing;
        replacement_=true;return Reason::Ready;
    }
    const Frame* ReplacementFrame()const noexcept
    {return armed_ && replacement_ && !returned_ && !claimed_?&frame_:nullptr;}
    Reason Returned(const Frame& current,uint64_t currentGeneration)noexcept
    {
        if(!armed_ || returned_ || claimed_ || replacement_)return Reason::NativeDrawMissing;
        auto fail=[&](Reason r){Revoke();return r;};
        if(currentGeneration!=frame_.generation || current.instance!=frame_.instance || current.generation!=frame_.generation)
            return fail(Reason::StaleGeneration);
        if(current.device!=frame_.device || current.context!=frame_.context || current.deviceEpoch!=frame_.deviceEpoch ||
            current.ownerEpoch!=frame_.ownerEpoch || current.configEpoch!=frame_.configEpoch || current.thread!=frame_.thread || !current.immediate)
            return fail(Reason::WrongOwner);
        // Arm already validated this exact profile. The owner, token and every
        // immutable surface field are compared below; no second schema scan.
        if(current.drawSerial!=frame_.drawSerial || current.shader!=frame_.shader || current.shaderIdentity!=frame_.shaderIdentity ||
            static_cast<bool>(current.nrProfile)!=static_cast<bool>(frame_.nrProfile) ||
            (current.nrProfile && !current.nrProfile->Matches(frame_.deviceEpoch,frame_.shaderIdentity)) ||
            current.constantBuffer!=frame_.constantBuffer || current.renderContext!=frame_.renderContext || current.frame!=frame_.frame ||
            current.tokenTick!=frame_.tokenTick || current.rawJitterX!=frame_.rawJitterX || current.rawJitterY!=frame_.rawJitterY ||
            current.inputSlots!=frame_.inputSlots || current.selectedOutput!=frame_.selectedOutput ||
            current.renderWidth!=frame_.renderWidth || current.renderHeight!=frame_.renderHeight ||
            current.outputWidth!=frame_.outputWidth || current.outputHeight!=frame_.outputHeight || current.controls!=frame_.controls)
            return fail(Reason::BindingsChanged);
        for(size_t i=0;i<frame_.inputs.size();++i)if(!Same(frame_.inputs[i],current.inputs[i]))return fail(Reason::BindingsChanged);
        for(size_t i=0;i<frame_.outputs.size();++i)if(!Same(frame_.outputs[i],current.outputs[i]))return fail(Reason::BindingsChanged);
        // Optional FG evidence cannot change ordinary SR/NR acceptance.
        if(frame_.gameMotionReference && (!current.gameMotionReference ||
           !frame_.gameMotionReference->Same(*current.gameMotionReference)))frame_.gameMotionReference.reset();
        returned_=true;return Reason::Ready;
    }
    Reason Claim(const Schema& schema)noexcept
    {
        if(!armed_ || !returned_ || claimed_)return Reason::NativeDrawMissing;
        if(schema.sourceRoles!=SourceSchema::PinnedShaderResourceSchema)return Reason::SourceSchemaUnknown;
        if(schema.jitterPhase!=JitterPhase::SameRenderForConsumedInputs)return Reason::JitterPhaseUnknown;
        if(schema.selectedOutputRole!=OutputRole::NativeRtv1ColourWithCompanionPreserved)return Reason::OutputRoleUnknown;
        if(schema.selectedOutputResource!=frame_.outputs[frame_.selectedOutput].resource ||
            schema.selectedOutputView!=frame_.outputs[frame_.selectedOutput].view || !schema.companionPreserved)
            return Reason::OutputRoleUnknown;
        const auto companion=frame_.selectedOutput==0?1u:0u;
        if(schema.companionResource!=frame_.outputs[companion].resource || schema.companionView!=frame_.outputs[companion].view)
            return Reason::OutputRoleUnknown;
        if(schema.outputConsumerContext!=frame_.context || schema.outputConsumerGeneration!=frame_.generation)
            return Reason::StaleGeneration;
        if(schema.revision!=Revision || schema.shaderIdentity!=frame_.shaderIdentity || schema.deviceEpoch!=frame_.deviceEpoch ||
            schema.ownerEpoch!=frame_.ownerEpoch || schema.configEpoch!=frame_.configEpoch || schema.instance!=frame_.instance ||
            schema.generation!=frame_.generation || schema.drawSerial!=frame_.drawSerial || schema.controls!=frame_.controls)
            return Reason::StaleGeneration;
        return ClaimOriginal();
    }
    // Thin adaptation of the known Genshin accumulate into the existing guide
    // consumers. Arm + Returned already establish this fixed original profile;
    // do not construct and certify a second schema at the same draw boundary.
    Reason ClaimOriginal()noexcept
    {
        if(!armed_ || !returned_ || claimed_)return Reason::NativeDrawMissing;
        const auto reason=ConsumeGeneration();claimed_=reason==Reason::Ready;return reason;
    }
    Reason ClaimCopyback(const GenshinFsr::Result& result,const GenshinFsr::Parameters& parameters,
        uintptr_t privateOutput,uintptr_t destinationOutput,uintptr_t consumerContext,
        uint64_t currentGeneration)noexcept
    {
        if(!armed_ || copyClaimed_ || !(replacement_?(!returned_ && !claimed_):(returned_ && claimed_)))
            return Reason::NativeDrawMissing;
        if(currentGeneration!=frame_.generation)return Reason::StaleGeneration;
        if(!privateOutput || result.outputIdentity!=privateOutput || result.instance!=frame_.instance ||
            result.generation!=frame_.generation || result.epoch!=parameters.epoch ||
            parameters.instance!=frame_.instance || parameters.generation!=frame_.generation ||
            parameters.sourceFrame!=frame_.frame || parameters.renderWidth!=frame_.renderWidth ||
            parameters.renderHeight!=frame_.renderHeight || parameters.outputWidth!=frame_.outputWidth ||
            parameters.outputHeight!=frame_.outputHeight || destinationOutput!=frame_.outputs[frame_.selectedOutput].resource ||
            consumerContext!=frame_.context || result.DrawAction()!=GenshinFsr::NativeDrawAction::OmitWithReplacement)
            return Reason::ProviderRejected;
        if(replacement_){const auto reason=ConsumeGeneration();if(reason!=Reason::Ready)return reason;}
        copyClaimed_=true;return Reason::Ready;
    }
    void Revoke()noexcept{frame_={};armed_=returned_=claimed_=copyClaimed_=replacement_=false;}
    // Only the completed, claimed native transaction can qualify a producer profile.
    // A caller-supplied Schema alone is never a guide evidence capability.
    const Frame* ClaimedFrame()const noexcept
    {return armed_ && returned_ && claimed_?&frame_:nullptr;}
    bool Pending()const noexcept{return armed_ && !returned_ && !replacement_;}
    bool NativePreserved()const noexcept{return armed_ && returned_;}
};
// Exact configured transform used by the pinned Bridge path. Unknown schema
// facts are checked separately; finite offsets are never semantic evidence.
inline bool ApplyProfile(const Settings& s,const il2cpp_callsite::RenderToken& token,
    bool havePrevious,float previousX,float previousY,float deltaMs,uint64_t epoch,bool first,bool hdr,
    GenshinFsr::Parameters& p)noexcept
{
    if(!ValidSettings(s))return false;
    const auto& c=token.params;p={};p.instance=c.instance;p.epoch=epoch;p.generation=token.generation;p.sourceFrame=c.frame_index;
    p.renderWidth=c.render_w;p.renderHeight=c.render_h;p.outputWidth=c.display_w;p.outputHeight=c.display_h;
    p.jitterX=s.jitterDelay && havePrevious?previousX:c.jitter_x;p.jitterY=s.jitterDelay && havePrevious?previousY:c.jitter_y;
    auto jitter=[&](float value,unsigned dimension){
        switch(s.jitterMode){case 1:value*=dimension;break;case 2:break;case 3:value=-value*dimension+0.5f;break;
            case 4:value=-value*dimension;break;case 5:value=0;break;case 0:value=value*dimension-0.5f;break;
            default:return std::numeric_limits<float>::quiet_NaN();}
        return s.clampJitterHalf?std::clamp(value,-0.5f,0.5f):value;};
    // Reject invalid raw jitter before clamping could turn infinities into finite values.
    if(!std::isfinite(p.jitterX) || !std::isfinite(p.jitterY))return false;
    p.jitterX=jitter(p.jitterX,p.renderWidth);p.jitterY=jitter(p.jitterY,p.renderHeight);p.deltaMs=deltaMs;
    p.preExposure=s.preExposure;p.cameraNear=s.cameraNear;p.cameraFar=s.cameraFar;p.fovRadians=s.fovRadians*s.fovScale;
    p.motionScale=s.motionScale;p.autoExposure=s.autoExposure;p.depthInverted=s.depthInverted;p.motionJittered=s.motionJittered;
    p.positiveMotionScale=s.positiveMotionScale;p.enableSharpening=s.sharpening;p.sharpness=s.sharpness;p.reset=s.forceReset || first;p.hdr=hdr;
    return GenshinFsr::ValidateParameters(p)==GenshinFsr::Reason::None;
}
inline const char* ReasonText(Reason reason)noexcept
{
    constexpr const char* names[]{"Ready","MissingToken","ReplayToken","WrongOwner","WrongShader","WrongLayout","Alias",
        "UnsupportedView","WrongFormat","StaleGeneration","NativeDrawMissing","BindingsChanged","SourceSchemaUnknown",
        "JitterPhaseUnknown","OutputRoleUnknown","InvalidParameters","ProviderRejected"};
    return names[static_cast<unsigned>(reason)];
}
}

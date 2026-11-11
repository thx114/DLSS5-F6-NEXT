// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "GenshinOutputContract.h"
#include "GenshinFsrProfiles.h"
#include <d3d11.h>
#include <d3d12.h>
#include <nvsdk_ngx.h>
#include <array>
#include <memory>

namespace GenshinLegacy::Family {
struct Shader;
struct ReadObservation;
Shader Read(ID3D11Device*,ID3D11PixelShader*,uint64_t) noexcept;
Shader Read(ID3D11Device*,ID3D11PixelShader*,uint64_t,ReadObservation*) noexcept;
}
namespace GenshinCaller { class BridgeAfterUpscale; }
namespace GenshinNr {
// This capability certifies this reviewed shader's numerical operations and
// bounded UNORM8 layout. It does not certify scene transfer or temporal phase.
class ShaderProfile final {
    uint64_t epoch_=0,identity_=0,shader_=0;
    ShaderProfile(uint64_t epoch,uint64_t identity,uint64_t shader):epoch_(epoch),identity_(identity),shader_(shader){}
    friend GenshinLegacy::Family::Shader GenshinLegacy::Family::Read(
        ID3D11Device*,ID3D11PixelShader*,uint64_t,GenshinLegacy::Family::ReadObservation*) noexcept;
public:
    static constexpr uint32_t Revision=2;
    static constexpr size_t Bytes=GenshinFsrProfiles::Reviewed[0].bytes;
    static constexpr uint64_t Fnv=GenshinFsrProfiles::Reviewed[0].hash;
    inline static constexpr auto Digest=GenshinFsrProfiles::Reviewed[0].digest;
    bool MatchesShader(uint64_t epoch,uint64_t identity,uint64_t shader)const noexcept
    {return Matches(epoch,identity) && shader_==shader && GenshinFsrProfiles::Accumulate(shader);}
    bool Matches(uint64_t epoch,uint64_t identity)const noexcept
    {return epoch && identity && epoch_==epoch && identity_==identity;}
};
// External ColourEncoding 0 means Auto, not identity. The actual admitted
// profile selects this internal processing contract independently of Auto's
// generic transfer inference; no sRGB/scene-light meaning is manufactured.
enum class ColourProcessing : uint8_t { Unknown, NativeBoundedIdentity };
struct Inputs {
    ColourProcessing colourProcessing=ColourProcessing::Unknown;
    std::shared_ptr<const ShaderProfile> profile;
    GenshinGuides::Owner owner;
    std::array<ID3D12Resource*,4> resources{};
    uint32_t renderWidth=0,renderHeight=0,outputWidth=0,outputHeight=0;
    float motionScaleX=0,motionScaleY=0;
};
// Synchronous, private handoff from the actual admitted Bridge callback to
// EvaluateAfterUpscale. No public ready flag or alternate consumer entry point.
class Scope final {
    Inputs inputs_;
    ID3D12GraphicsCommandList* list_=nullptr;
    NVSDK_NGX_Parameter* parameters_=nullptr;
    ID3D12CommandQueue* queue_=nullptr;
    Scope* previous_=nullptr;
    inline static thread_local Scope* current_=nullptr;
    Scope(Inputs inputs,ID3D12GraphicsCommandList* list,NVSDK_NGX_Parameter* params,ID3D12CommandQueue* queue)
        :inputs_(std::move(inputs)),list_(list),parameters_(params),queue_(queue),previous_(current_){current_=this;}
    friend class GenshinCaller::BridgeAfterUpscale;
public:
    Scope(const Scope&)=delete;
    Scope& operator=(const Scope&)=delete;
    ~Scope(){current_=previous_;}
    static const Scope* Current()noexcept{return current_;}
    const Inputs& Get()const noexcept{return inputs_;}
    bool Matches(ID3D12GraphicsCommandList* list,NVSDK_NGX_Parameter* params,ID3D12CommandQueue* queue,
        unsigned long long frame)const noexcept {
        return current_==this && inputs_.profile && GetCurrentThreadId()==inputs_.owner.thread &&
            list==list_ && params==parameters_ && queue==queue_ && frame==inputs_.owner.sourceFrame;
    }
    bool Resources(ID3D12Resource* output,ID3D12Resource* depth,ID3D12Resource* motion)const noexcept
    {return output==inputs_.resources[3] && depth==inputs_.resources[1] && motion==inputs_.resources[2];}
    bool Supports(uint32_t encoding,bool before,bool rayReconstruction,bool runBefore,bool deferred,
        bool finished,bool hold,bool depthFlip,bool motionFlip,bool motionYInverted)const noexcept {
        // Auto delegates to the exact profile; every explicit external transfer
        // and every unknown value must refuse this numerical identity route.
        return encoding==0 && inputs_.profile && inputs_.colourProcessing==ColourProcessing::NativeBoundedIdentity &&
            !before && !rayReconstruction && !runBefore && !deferred &&
            !finished && !hold && !depthFlip && !motionFlip && !motionYInverted;
    }
    // Base NR uses identity encode/resolve; no assertion of sRGB or linear scene light.
    // Phase-dependent histories are deliberately excluded by the actual Dispatch path.
    template<class Frame> bool Apply(Frame& frame)const noexcept {
        if(!inputs_.profile || inputs_.colourProcessing!=ColourProcessing::NativeBoundedIdentity)return false;
        frame.DepthInverted=true; // the reviewed shader chooses the largest numeric Z donor
        frame.MotionVectorsLowResolution=true;
        frame.OutputWidth=inputs_.outputWidth;frame.OutputHeight=inputs_.outputHeight;
        frame.RenderSubrectWidth=inputs_.renderWidth;frame.RenderSubrectHeight=inputs_.renderHeight;
        frame.DepthSubrectBaseX=frame.DepthSubrectBaseY=0;
        frame.MotionSubrectBaseX=frame.MotionSubrectBaseY=0;
        frame.MvScaleX=inputs_.motionScaleX;frame.MvScaleY=inputs_.motionScaleY;
        frame.SubmissionEpoch=inputs_.owner.sourceFrame;
        frame.ColourIsLinearHdr=false;frame.InputEncoding=0;
        // These frame fields request no conversion. External Auto was checked
        // separately and does not certify a transfer function.
        frame.ColourEncoding=0;frame.ColourEncodingForced=false;frame.WhitePointOverride=0;
        return true;
    }
};
}

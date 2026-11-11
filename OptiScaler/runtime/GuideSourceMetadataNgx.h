// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "GuideSourceMetadata.h"
#include <d3d12.h>
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_params.h>

namespace GuideSourceMetadata {
// Called at the translated NGX boundary, once per real Evaluate. No admission.
inline Ptr ReadNgx(const NVSDK_NGX_Parameter& p,Descriptor d,
    ID3D12Resource* depth,ID3D12Resource* motion) noexcept {
    try {
        using F=DlssNr::GuideMetadata::Field;using O=DlssNr::GuideMetadata::Origin;
        const auto read=[&](const char* key,auto& value){
            if(p.Get(key,&value)==NVSDK_NGX_Result_Success)return true;
            value={};return false;
        };
        unsigned w=0,h=0,dx=0,dy=0,mx=0,my=0;
        const bool wk=read(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width,w);
        const bool hk=read(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height,h);
        const bool extent=DlssNr::GuideMetadata::RenderExtent(w,h,wk,hk,
            d.renderWidth,d.renderHeight,d.renderWidth!=0&&d.renderHeight!=0);
        d.renderWidth=w;d.renderHeight=h;
        const auto origin=wk&&hk?O::Evaluate:O::RetainedFeature;
        d.provenance.Set(F::RenderWidth,w,extent,origin);
        d.provenance.Set(F::RenderHeight,h,extent,origin);
        const auto base=[&](F f,const char* key,unsigned& v){
            const bool supplied=read(key,v);
            const bool known=DlssNr::GuideMetadata::OptionalBase(v,supplied,d.rr);
            d.provenance.Set(f,v,known,supplied?O::Evaluate:O::ApiDefault);
        };
        base(F::DepthX,NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X,dx);
        base(F::DepthY,NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y,dy);
        base(F::MotionX,NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X,mx);
        base(F::MotionY,NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y,my);
        const auto scalar=[&](F f,const char* key,float& v,bool nonzero){
            const bool known=read(key,v);
            const bool valid=known&&std::isfinite(v)&&(!nonzero||v!=0);
            d.provenance.Set(f,v,valid,O::Evaluate);return valid;
        };
        scalar(F::ScaleX,NVSDK_NGX_Parameter_MV_Scale_X,d.scaleX,true);
        scalar(F::ScaleY,NVSDK_NGX_Parameter_MV_Scale_Y,d.scaleY,true);
        const bool jx=scalar(F::JitterX,NVSDK_NGX_Parameter_Jitter_Offset_X,d.jitterX,false);
        const bool jy=scalar(F::JitterY,NVSDK_NGX_Parameter_Jitter_Offset_Y,d.jitterY,false);
        d.jitterKnown=jx&&jy;d.tick=GuideDiagnostics::Now();
        d.translatedDepth=reinterpret_cast<uintptr_t>(depth);
        d.translatedMotion=reinterpret_cast<uintptr_t>(motion);
        d.lowRes=(d.flags&NVSDK_NGX_DLSS_Feature_Flags_MVLowRes)!=0;
        d.jittered=(d.flags&NVSDK_NGX_DLSS_Feature_Flags_MVJittered)!=0;
        d.depthInverted=(d.flags&NVSDK_NGX_DLSS_Feature_Flags_DepthInverted)!=0;
        d.provenance.Set(F::CreateFlags,d.flags,d.flagsKnown,O::RetainedFeature);
        const auto resource=[](ID3D12Resource* r,GuideDiagnostics::Rect rect){
            GuideDiagnostics::Resource result;
            if(r){const auto desc=r->GetDesc();
                result={desc.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D,
                    static_cast<uint32_t>(desc.Format),desc.Width,desc.Height,rect};}
            return result;
        };
        d.depth=resource(depth,{dx,dy,w,h});
        d.motion=resource(motion,{mx,my,d.lowRes?w:d.outputWidth,d.lowRes?h:d.outputHeight});
        d.provenance.Set(F::DepthBounds,Bounds(d.depth),true,O::ActualCall);
        d.provenance.Set(F::MotionBounds,Bounds(d.motion),true,O::ActualCall);
        return std::make_shared<const Descriptor>(std::move(d));
    }catch(...){return {};}
}
} // namespace GuideSourceMetadata

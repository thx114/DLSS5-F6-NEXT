// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "GenshinRuntimeCapture.h"
#include "GenshinPresentLifecycle.h"
#include "legacy/Il2CppCallSiteHook.h"

namespace GenshinLegacy::RuntimeCapture {
// Bounded evidence for the missing engine contract. This class never produces
// a Signal, opens admission, changes state or skips an original draw.
class PassEvidence final {
    struct Node {
        ComPtr<ID3D11Resource> resource;
        ResourceProof::Witness witness{};
        uint64_t draw=0,root=0;uint32_t depth=0;
    };
    std::array<Node,MaxTraceNodes> nodes_{};
    std::array<ComPtr<ID3D11Resource>,D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> pendingOutputs_{};
    DrawFact pending_{};
    il2cpp_callsite::RenderToken traceToken_{};
    uint32_t inspected_=0,roots_=0;bool pendingValid_=false;
    std::atomic_bool revoked_{false};

    void ClearRoute()noexcept{nodes_={};traceToken_={};pendingOutputs_={};pendingValid_=false;}
    static bool Current(const il2cpp_callsite::RenderToken& token)noexcept {
        const auto now=GetTickCount64();return token.params.instance && token.generation && token.capture_tick &&
            token.capture_tick<=now && now-token.capture_tick<=250 &&
            il2cpp_callsite::params_generation_for(token.params.instance)==token.generation;
    }
    static bool SameSize(UINT w,UINT h,UINT expectedW,UINT expectedH)noexcept
    {return w && h && w==expectedW && h==expectedH;}
    static unsigned Match(const DrawFact& draw,il2cpp_callsite::RenderToken& result)noexcept {
        uint64_t instances[8]{};size_t count=0;il2cpp_callsite::known_instances(instances,8,count);unsigned matches=0;
        for(size_t i=0;i<count;++i){il2cpp_callsite::CapturedParams params;uint64_t generation=0;
            if(!il2cpp_callsite::last_params_for(instances[i],params,generation))continue;
            il2cpp_callsite::RenderToken token;
            const auto candidates=il2cpp_callsite::match_capture_render_token(params.render_w,params.render_h,
                params.display_w,params.display_h,0,token);
            if(candidates>1)return candidates;
            if(candidates!=1 || token.params.instance!=instances[i] || token.generation!=generation || !Current(token))continue;
            bool renderInput=false,compatibleOutput=false;
            for(size_t slot=0;slot<draw.boundResources.size();++slot)renderInput=renderInput ||
                SameSize(draw.boundWidths[slot],draw.boundHeights[slot],params.render_w,params.render_h);
            for(size_t slot=0;slot<draw.outputResources.size();++slot)compatibleOutput=compatibleOutput ||
                SameSize(draw.outputWidths[slot],draw.outputHeights[slot],params.render_w,params.render_h) ||
                SameSize(draw.outputWidths[slot],draw.outputHeights[slot],params.display_w,params.display_h);
            if(renderInput && compatibleOutput){result=token;++matches;}}
        return matches;
    }
public:
    // Off-thread boundaries only revoke metadata. The owning-thread Poll releases
    // route COM references; GPU readback source/staging/shader/query are separate.
    void Revoke()noexcept{revoked_.store(true,std::memory_order_release);}
    void CollectRevoked()noexcept{if(revoked_.exchange(false,std::memory_order_acq_rel)){ClearRoute();pending_={};}}
    void BeforeDraw(ID3D11Device* device,ID3D11DeviceContext* context,uint64_t deviceEpoch,
        uint64_t ownerEpoch,uint64_t configEpoch,uint64_t serial,UINT count,bool countKnown,ConstantReadback& constants)noexcept {
        pendingValid_=false;pendingOutputs_={};
        if(!Enabled() || configEpoch!=Get().configEpoch){ClearRoute();return;}
        if(!Current(traceToken_)){nodes_={};traceToken_={};}
        const auto shader=Family::Current(context,device,deviceEpoch);
        const bool family=shader.Known() && Expected(shader.hash) && shader.hash!=fsr2_family_takeover::k_smaa_hash;
        if(family)BeginFamilyWindow();
        bool tracing=false;for(const auto& node:nodes_)tracing=tracing || node.resource;
        if(!family && !tracing)return;
        auto& state=Get();
        if(state.passBudgetCount>=MaxPassSamples){++state.passBudgetDrops;return;}
        if(inspected_>=MaxTraceDraws){++state.traceBudgetDrops;ClearRoute();return;}++inspected_;
        if(!device || !context || !deviceEpoch || !ownerEpoch || !configEpoch ||
            context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return;
        try {
            DrawFact draw;draw.observationTick=GetTickCount64();draw.device=reinterpret_cast<uintptr_t>(device);
            draw.context=reinterpret_cast<uintptr_t>(context);draw.deviceEpoch=deviceEpoch;draw.ownerEpoch=ownerEpoch;
            draw.configEpoch=configEpoch;draw.thread=GetCurrentThreadId();draw.drawSerial=serial;
            draw.shaderIdentity=shader.identity;draw.shaderHash=shader.hash;draw.familyPass=family;
            draw.drawCount=count;draw.drawCountKnown=countKnown;
            std::array<ComPtr<ID3D11ShaderResourceView>,D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT> srvs;
            std::array<ID3D11ShaderResourceView*,D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT> rawSrvs{};
            context->PSGetShaderResources(0,static_cast<UINT>(rawSrvs.size()),rawSrvs.data());
            for(size_t i=0;i<srvs.size();++i)srvs[i].Attach(rawSrvs[i]);
            for(size_t i=0;i<srvs.size();++i)if(srvs[i]){
                ComPtr<ID3D11Resource> resource;srvs[i]->GetResource(&resource);if(!resource)continue;
                D3D11_SHADER_RESOURCE_VIEW_DESC view{};srvs[i]->GetDesc(&view);
                draw.boundViewDescriptions[i]=view;
                draw.boundResources[i]=reinterpret_cast<uintptr_t>(resource.Get());draw.boundViews[i]=reinterpret_cast<uintptr_t>(srvs[i].Get());
                draw.boundFormats[i]=view.Format;draw.boundWrites[i]=ResourceProof::Identify(context,resource.Get(),srvs[i].Get());
                ComPtr<ID3D11Texture2D> texture;if(SUCCEEDED(resource.As(&texture))){D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);
                    draw.boundWidths[i]=desc.Width;draw.boundHeights[i]=desc.Height;draw.boundDescriptions[i]=desc;}
                for(const auto& node:nodes_)if(node.resource.Get()==resource.Get() &&
                    node.witness.resourceLifetime==draw.boundWrites[i].resourceLifetime &&
                    node.witness.subresource==draw.boundWrites[i].subresource &&
                    ResourceProof::CurrentIdentityPair(node.witness,draw.boundWrites[i])){
                    if(draw.readEdgeCount==draw.readEdges.size()){draw.readEdgesTruncated=true;break;}
                    draw.readEdges[draw.readEdgeCount++]={node.draw,node.root,node.witness.resourceLifetime,static_cast<uint32_t>(i),node.depth+1};}}
            draw.consumerRoute=draw.readEdgeCount!=0;
            if(!family && !draw.consumerRoute)return;
            std::array<ComPtr<ID3D11RenderTargetView>,D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> rtvs;
            std::array<ID3D11RenderTargetView*,D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> rawRtvs{};
            context->OMGetRenderTargets(static_cast<UINT>(rawRtvs.size()),rawRtvs.data(),nullptr);
            for(size_t i=0;i<rtvs.size();++i)rtvs[i].Attach(rawRtvs[i]);
            for(size_t i=0;i<rtvs.size();++i)if(rtvs[i]){
                rtvs[i]->GetResource(&pendingOutputs_[i]);if(!pendingOutputs_[i])continue;
                D3D11_RENDER_TARGET_VIEW_DESC view{};rtvs[i]->GetDesc(&view);
                draw.outputViewDescriptions[i]=view;
                draw.outputResources[i]=reinterpret_cast<uintptr_t>(pendingOutputs_[i].Get());
                draw.outputViews[i]=reinterpret_cast<uintptr_t>(rtvs[i].Get());draw.outputFormats[i]=view.Format;
                draw.outputWrites[i]=ResourceProof::Identify(context,pendingOutputs_[i].Get(),rtvs[i].Get());
                ComPtr<ID3D11Texture2D> texture;if(SUCCEEDED(pendingOutputs_[i].As(&texture))){D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);
                    draw.outputWidths[i]=desc.Width;draw.outputHeights[i]=desc.Height;draw.outputDescriptions[i]=desc;}}
            ComPtr<ID3D11PixelShader> pixel;context->PSGetShader(&pixel,nullptr,nullptr);draw.shaderObject=reinterpret_cast<uintptr_t>(pixel.Get());
            UINT creationBytes=sizeof(draw.creation);if(pixel && (FAILED(pixel->GetPrivateData(CreateKey,&creationBytes,&draw.creation)) ||
                creationBytes!=sizeof(draw.creation) || draw.creation.hash!=draw.shaderHash || draw.creation.sessionTick!=state.startTick))draw.creation={};
            UINT viewportCount=D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
            D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
            context->RSGetViewports(&viewportCount,viewports);draw.viewportCount=viewportCount;
            if(viewportCount==1)draw.viewport=viewports[0];
            ComPtr<ID3D11Buffer> cb;context->PSGetConstantBuffers(0,1,&cb);
            if(cb){D3D11_BUFFER_DESC desc{};cb->GetDesc(&desc);draw.cbBytes=desc.ByteWidth;draw.cbIdentity=reinterpret_cast<uintptr_t>(cb.Get());}
            il2cpp_callsite::RenderToken token;
            if(family){const auto matches=Match(draw,token);draw.captureTokenMatches=matches;if(matches!=1){
                    if(matches>1)++state.passTokenAmbiguous;else ++state.passTokenMissing;
                    // Preserve the rejected shape without attributing a Render token.
                    if(state.passBudgetCount<(state.manual?MaxPassSamples:16)){++state.passOffers;++state.passBudgetCount;Job job;job.kind=Job::Kind::Binding;
                        job.eventTick=draw.observationTick;job.draw=draw;Enqueue(std::move(job));
                        if(state.manual && cb)constants.Queue(device,context,cb.Get(),draw);}
                    pendingOutputs_={};return;}}
            else token=traceToken_;
            if(!Current(token)){pendingOutputs_={};return;}
            const auto& params=token.params;draw.instance=params.instance;draw.generation=token.generation;
            draw.frame=params.frame_index;draw.captureTick=token.capture_tick;draw.renderWidth=params.render_w;draw.renderHeight=params.render_h;
            draw.outputWidth=params.display_w;draw.outputHeight=params.display_h;draw.jitterX=params.jitter_x;draw.jitterY=params.jitter_y;
            // A new token cannot inherit the previous token's read edges.
            if(draw.consumerRoute && (traceToken_.generation!=token.generation || traceToken_.params.instance!=params.instance)){
                draw.readEdges={};draw.readEdgeCount=0;draw.consumerRoute=false;nodes_={};}
            if(shader.hash==fsr2_family_takeover::k_accumulate_hash){
                draw.slots={0,2,3};draw.selectedOutputSlot=1;
                for(unsigned i=0;i<3;++i){const auto slot=draw.slots[i];draw.resources[i]=draw.boundResources[slot];draw.views[i]=draw.boundViews[slot];
                    draw.formats[i]=draw.boundFormats[slot];draw.widths[i]=draw.boundWidths[slot];draw.heights[i]=draw.boundHeights[slot];
                    draw.sourceWrites[i]=draw.boundWrites[slot];}draw.outputWitness=draw.outputWrites[1];
                if(roots_<8){nodes_={};traceToken_=token;++roots_;}else pendingOutputs_={};}
            ++state.passOffers;++state.passBudgetCount;Job job;job.kind=Job::Kind::Binding;
            job.eventTick=draw.observationTick;job.draw=draw;Enqueue(std::move(job));
            // The independent binding record survives missing/oversized CBs and a full GPU ring.
            if(family && cb)constants.Queue(device,context,cb.Get(),draw);
            if(state.manual){nodes_={};traceToken_={};pendingOutputs_={};return;}
            pending_=draw;pendingValid_=true;
        }catch(...){pendingOutputs_={};++state.allocationFailures;}
    }
    void AfterOriginalDraw(ID3D11DeviceContext* context,uint64_t serial)noexcept {
        if(!pendingValid_ || serial!=pending_.drawSerial)return;pendingValid_=false;
        if(Enabled() && pending_.instance && il2cpp_callsite::params_generation_for(pending_.instance)==pending_.generation){
            RouteFact route;route.device=pending_.device;route.context=pending_.context;route.deviceEpoch=pending_.deviceEpoch;
            route.ownerEpoch=pending_.ownerEpoch;route.configEpoch=pending_.configEpoch;route.thread=GetCurrentThreadId();
            route.drawSerial=serial;route.instance=pending_.instance;route.generation=pending_.generation;route.frame=pending_.frame;
            route.shaderIdentity=pending_.shaderIdentity;route.shaderHash=pending_.shaderHash;route.originalDrawReturned=true;
            route.output=pending_.outputWitness;ObserveRoute(route);}
        if(!Enabled() || !Current(traceToken_) || traceToken_.generation!=pending_.generation ||
            traceToken_.params.instance!=pending_.instance){pendingOutputs_={};return;}
        const bool root=pending_.shaderHash==fsr2_family_takeover::k_accumulate_hash;
        uint64_t rootDraw=pending_.drawSerial;uint32_t depth=0;
        if(!root){if(!pending_.consumerRoute || pending_.readEdgesTruncated){pendingOutputs_={};return;}
            rootDraw=pending_.readEdges[0].rootDraw;depth=pending_.readEdges[0].depth;
            for(unsigned i=1;i<pending_.readEdgeCount;++i)if(pending_.readEdges[i].rootDraw!=rootDraw){pendingOutputs_={};return;}}
        if(depth>8){pendingOutputs_={};++Get().traceBudgetDrops;return;}
        for(auto& resource:pendingOutputs_)if(resource){
            const auto witness=ResourceProof::Identify(context,resource.Get());if(!witness.resourceLifetime)continue;
            Node* destination=nullptr;for(auto& node:nodes_)if(node.resource.Get()==resource.Get()){destination=&node;break;}
            if(!destination)for(auto& node:nodes_)if(!node.resource){destination=&node;break;}
            if(destination)*destination={resource,witness,pending_.drawSerial,rootDraw,depth};else ++Get().traceBudgetDrops;}
        pendingOutputs_={};
    }
    void NotePresent(const Presentation::Receipt& receipt,const ResourceProof::Witness& backbuffer)noexcept {
        if(!Enabled() || !Current(traceToken_) || !receipt.recorded || !receipt.successfulNonTest)return;
        for(const auto& node:nodes_)if(node.resource && node.witness.resourceLifetime==backbuffer.resourceLifetime &&
            node.witness.subresource==backbuffer.subresource && ResourceProof::CurrentIdentityPair(node.witness,backbuffer)){
            RouteFact route;route.kind=RouteFact::Kind::PresentCompleted;route.device=pending_.device;route.context=pending_.context;
            route.deviceEpoch=pending_.deviceEpoch;route.ownerEpoch=pending_.ownerEpoch;route.configEpoch=pending_.configEpoch;
            route.thread=GetCurrentThreadId();route.drawSerial=node.draw;route.instance=traceToken_.params.instance;
            route.generation=traceToken_.generation;route.frame=traceToken_.params.frame_index;route.swapchain=receipt.ticket.swapchain;
            route.presentSerial=receipt.completionSerial;route.presentResult=static_cast<int32_t>(receipt.rawResult);
            route.presentRecorded=true;route.successfulNonTest=true;route.directIdentityMatch=true;
            route.sameRender=receipt.sourceFrameKnown && receipt.ticket.frame.instance==traceToken_.params.instance &&
                receipt.ticket.frame.generation==traceToken_.generation && receipt.ticket.frame.sourceFrame==traceToken_.params.frame_index;
            route.output=node.witness;route.backbuffer=backbuffer;route.traceRootDraw=node.root;route.traceDepth=node.depth;
            ObserveRoute(route);}
        ClearRoute();
    }
};
}

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "GenshinCpuStageTiming.h"
#include "GenshinTypedGuides.h"
#include "GenshinDepthEvidence.h"
#include "GenshinDepthDraw.h"
#include "GenshinDepthReplayState.h"
#include "GenshinDepthSamplerRecipe.h"
#include "GenshinOwnerIdentity.h"
#include "GenshinQueryScope.h"
#include "GenshinDepthShaderCapabilities.h"
#include "GenshinPictureQueryIsolation.h"
#include <optional>
#include <d3d11shader.h>
#include <d3d11_1.h>
#include <array>
#include <algorithm>
#include <memory>
#include <cmath>

namespace GenshinDepthMapping {
using Microsoft::WRL::ComPtr;
using GenshinGuides::Owner;
inline uintptr_t Identity(IUnknown* object)noexcept {
    ComPtr<IUnknown> id;if(object)object->QueryInterface(IID_PPV_ARGS(&id));return reinterpret_cast<uintptr_t>(id.Get());
}
enum class Reason:unsigned {None,NoExactDraw,NoShaderBytecode,UnsupportedPipeline,NoScene,
    BytecodeNotGenerated,OwnerChanged,UnknownDeviceZ,UnknownJitterPhase,UnprovedClear,WrongPicture,ResourceFailure,
    UnknownVS,SignatureMismatch,AlreadyPublished,UntrackedWrite,UnprovedQueryScope,DeferredInput,DeferredCapacity,NoSelectedInput,NoOutputTarget,TextureLayout,SamplerState,MappingPoolBusy,MappingBudget,DeferredPayloadPending,GeometryStage,TessellationLinkage,ShaderDependency,PortableLayout,PortableGeometry,PortableShader,PortableConstants,IsolatedPacket,IsolatedCapacity,IsolatedSubmission};
inline const char* ReasonText(Reason r)noexcept {
    switch(r){
    case Reason::None:return "READY";case Reason::NoExactDraw:return "NO_EXACT_DRAW_ARGUMENTS";
    case Reason::NoShaderBytecode:return "NO_ACTUAL_DIRECT_SAMPLE_PS_BYTES";
    case Reason::UnsupportedPipeline:return "UNSUPPORTED_RASTER_STATE";case Reason::NoScene:return "NO_RETURNED_SCENE_MAPPING";
    case Reason::BytecodeNotGenerated:return "DEPTH_SHADER_GROUP_NOT_GENERATED_OR_STALE";case Reason::OwnerChanged:return "OWNER_CHANGED";
    case Reason::UnknownDeviceZ:return "DEVICE_Z_PROJECTION_UNPROVED";case Reason::UnknownJitterPhase:return "DEPTH_COLOUR_JITTER_UNPROVED";
    case Reason::UnprovedClear:return "SOURCE_MATCHED_CLEAR_UNPROVED";case Reason::WrongPicture:return "WRONG_PICTURE_OR_OWNER";
    case Reason::UnknownVS:return "ACTUAL_VS_BYTES_UNAVAILABLE_OR_UNQUALIFIED";case Reason::SignatureMismatch:return "VS_PS_SIGNATURE_MISMATCH";
    case Reason::AlreadyPublished:return "MAPPING_ALREADY_PUBLISHED";case Reason::UntrackedWrite:return "UNTRACKED_NONRASTER_WRITE";
    case Reason::UnprovedQueryScope:return "QUERY_SCOPE_ACTIVE_UNKNOWN_OR_UNHOOKED";
    case Reason::DeferredInput:return "DEFERRED_RASTER_INPUT_INVALID_OR_MATERIALIZE_FAILED";
    case Reason::DeferredCapacity:return "DEFERRED_RASTER_CAPACITY";
    case Reason::NoSelectedInput:return "SELECTED_RASTER_INPUT_MISMATCH";
    case Reason::NoOutputTarget:return "NO_TEXTURE_OUTPUT_TARGET";
    case Reason::TextureLayout:return "UNSUPPORTED_MAPPING_TEXTURE_LAYOUT";
    case Reason::SamplerState:return "UNSUPPORTED_MAPPING_SAMPLER";
    case Reason::MappingPoolBusy:return "MAPPING_POOL_RETAINED_OR_IN_FLIGHT";
    case Reason::MappingBudget:return "MAPPING_STORAGE_BUDGET";
    case Reason::DeferredPayloadPending:return "DEFERRED_RASTER_GPU_PAYLOAD_PENDING";
    case Reason::GeometryStage:return "GEOMETRY_STAGE_UNSUPPORTED";
    case Reason::TessellationLinkage:return "TESSELLATION_TOPOLOGY_OR_LINKAGE";
    case Reason::ShaderDependency:return "PRE_RASTER_NON_CB_DEPENDENCY_OR_LINKAGE";
    case Reason::PortableLayout:return "PORTABLE_INPUT_LAYOUT_CREATION_NOT_CAPTURED";
    case Reason::PortableGeometry:return "PORTABLE_IA_BYTES_OR_VERSION_UNAVAILABLE";
    case Reason::PortableShader:return "PORTABLE_VS_HS_DS_BYTES_UNAVAILABLE";
    case Reason::PortableConstants:return "PORTABLE_CB_PAYLOAD_NOT_READY";
    case Reason::IsolatedPacket:return "ISOLATED_CURRENT_PREPARED_DEPTH_PACKET_INVALID";
    case Reason::IsolatedCapacity:return "ISOLATED_PICTURE_LANE_CAPACITY";
    case Reason::IsolatedSubmission:return "ISOLATED_PICTURE_MAPPING_SUBMISSION_FAILED";
    default:return "RESOURCE_OR_STATE_FAILURE";}
}
struct Surface {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11UnorderedAccessView> uav;
};
struct Node {
    // Remains charged through mapping/reader retirement and replacement.
    GenshinDepthRetention::Buffers<96> rasterRetention;
    Node()=default;Node(const Node&)=default;Node(Node&&)noexcept=default;
    Node& operator=(Node other)noexcept {this->~Node();new(this) Node(std::move(other));return *this;}
    Owner owner{};uint32_t width=0,height=0;uint64_t serial=0;
    uintptr_t selected=0,target=0;UINT targetSubresource=0;
    uintptr_t preparedDepth=0;uint64_t contentVersion=0;
    std::shared_ptr<const GenshinGuides::PreparedDepthSource> preparedSource;
    // Direct-context Resolve samples the same immutable packet as FG. Keep
    // its real surface/fence roots even if publication fails after Dispatch.
    std::shared_ptr<const GenshinTransport::QueuedTransfer> preparedTransfer;
    GenshinGuides::DepthConvention depthConvention=GenshinGuides::DepthConvention::Unknown;
    std::array<Surface,2> uv;
    Surface depth;unsigned active=0;bool scene=false,returned=false;
    std::array<ComPtr<ID3D11PixelShader>,3> mappingPrograms;
    ComPtr<ID3D11ComputeShader> resolveProgram;
    ComPtr<ID3D11Buffer> mappingConstants,resolveConstants;
    ComPtr<ID3D11Query> mappingDone;
    uintptr_t executionContext=0;
    std::shared_ptr<GenshinFgQueryIsolation::PictureJob> isolatedJob;
    bool workIssued=false;
    std::array<ComPtr<ID3D11DeviceChild>,96> rasterRoots{};unsigned rootCount=0;
    bool RetainRasterRoot(ID3D11DeviceChild* object)noexcept {
        if(!object)return true;
        for(unsigned i=0;i<rootCount;++i)if(rasterRoots[i].Get()==object)return true;
        if(rootCount==rasterRoots.size())return false;
        ComPtr<ID3D11Buffer> buffer;
        const auto hr=object->QueryInterface(IID_PPV_ARGS(&buffer));
        if(SUCCEEDED(hr)){if(!rasterRetention.Add(buffer.Get()))return false;}
        else if(hr!=E_NOINTERFACE)return false;
        rasterRoots[rootCount++]=object;return true;
    }
    void ClearRasterRoots()noexcept {
        rasterRoots={};rootCount=0;rasterRetention={};preparedSource.reset();preparedTransfer.reset();
    }
};
inline bool Complete(ID3D11DeviceContext* context,const Node& node)noexcept {
    if(node.isolatedJob)return node.isolatedJob->Complete();
    if(!node.workIssued)return true;
    if(!node.mappingDone || node.owner.thread!=GetCurrentThreadId() || Identity(context)!=(node.executionContext?node.executionContext:node.owner.context))return false;
    BOOL done=FALSE;
    return context->GetData(node.mappingDone.Get(),&done,sizeof(done),D3D11_ASYNC_GETDATA_DONOTFLUSH)==S_OK && done;
}
inline void CollectRasterRoots(ID3D11DeviceContext* context,const std::shared_ptr<Node>& node)noexcept {
    // No current mapping or reader may still own the node. Keep surfaces warm.
    if(!node || node.use_count()!=1 || node->owner.thread!=GetCurrentThreadId() ||
       !context || Identity(context)!=(node->executionContext?node->executionContext:node->owner.context) || !Complete(context,*node))return;
    node->ClearRasterRoots();
}
struct Result {
    std::shared_ptr<const Node> owner;
    ComPtr<ID3D11Resource> texture;
    uint32_t width=0,height=0;
    GenshinGuides::DepthConvention convention=GenshinGuides::DepthConvention::Unknown;
    bool Known()const noexcept {return owner && texture && width && height;}
};
// Scalar observations only; never authorize private work or publish a mapping.
struct RequestCost {
    std::atomic<uint64_t> immediate{0},noWork{0},admission{0},refused{0};
};
inline RequestCost requestCost;
class Capture final {
    // Set only inside ResolveIsolated after a genuine single-packet Lane has
    // created and retained its closed context. Never changes QueryScope state.
    ComPtr<ID3D11DeviceContext> isolatedContext_,isolatedObservation_;
    uintptr_t isolatedSource_=0;
    GenshinFgQueryIsolation::PictureLane::Ptr isolatedLane_;
    std::shared_ptr<Node> isolatedResult_;
    std::shared_ptr<Deferred::Version> isolatedTargetVersion_;
    uint64_t isolatedMutation_=0;
    std::shared_ptr<Capture> isolatedMapper_;
    // The private UV/depth scratch is read only by the mapper submission and
    // its copy into PictureJob's distinct output. NR retains that output, not
    // these scratch surfaces. Detach only on the real producer fence; output
    // reader retirement, failed Signal/Wait and quarantine are unchanged.
    struct RasterRoots {std::array<std::shared_ptr<Node>,2> nodes;};
    struct RasterUse {std::shared_ptr<GenshinFgQueryIsolation::PictureJob> job;std::shared_ptr<RasterRoots> roots;};
    mutable std::array<RasterUse,3> rasterUses_;
    static Reason NumericQualification(const GenshinGuides::Facts& facts)noexcept {
        if(facts.preparedSource && !facts.preparedSource->MatchesNumerics(facts))return Reason::UnknownDeviceZ;
        const bool reversed=facts.depthConvention.value==GenshinGuides::DepthConvention::DeviceZReversed;
        if((!reversed && facts.depthConvention.value!=GenshinGuides::DepthConvention::DeviceZForward) ||
           facts.depthConvention.source!=GenshinGuides::Source::ActualRasterDepth)return Reason::UnknownDeviceZ;
        if(facts.depthClear.source!=GenshinGuides::Source::ActualRasterDepth ||
           facts.depthClear.value!=(reversed?0.0f:1.0f))return Reason::UnprovedClear;
        if(facts.depthToOutputJitterX.source!=GenshinGuides::Source::ActualDepthMapping ||
           facts.depthToOutputJitterY.source!=GenshinGuides::Source::ActualDepthMapping ||
           !std::isfinite(facts.depthToOutputJitterX.value) || !std::isfinite(facts.depthToOutputJitterY.value))
            return Reason::UnknownJitterPhase;
        return Reason::None;
    }
    Deferred::PortableCache portableCache_;
    bool Isolated(ID3D11DeviceContext* c)const noexcept {return c&&c==isolatedContext_.Get();}
    uint64_t Mutation(ID3D11DeviceContext* c)const noexcept {
        return GenshinDepthMapping::MutationSequence(Isolated(c)?isolatedObservation_.Get():c);
    }
    bool Quiescent(ID3D11DeviceContext* c)const noexcept {
        return Isolated(c)||GenshinQueryScope::KnownQuiescent(c);
    }
    struct MappingWork {
        std::optional<GenshinQueryScope::PrivateWork> game;
        bool privateLane=false;
        MappingWork(ID3D11DeviceContext* c,bool isolated):privateLane(isolated){if(!isolated)game.emplace(c);}
        explicit operator bool()const noexcept{return privateLane||(game&&static_cast<bool>(*game));}
    };
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11PixelShader> scenePs_,composePs_,invalidatePs_;
    ComPtr<ID3D11ComputeShader> resolveCs_;
    ComPtr<ID3D11DepthStencilState> noDepth_;
    ComPtr<ID3D11BlendState> noBlend_;
    ComPtr<ID3D11Buffer> controls_,resolveControls_;
    std::array<std::shared_ptr<Node>,2> pool_{};
    std::shared_ptr<Node> current_;
    // Make creates three RGBA32_FLOAT surfaces (two UV maps and paired depth).
    // Keep the original 192 MiB / 24-byte pixel capacity after the R825 format change.
    static constexpr uint64_t StorageBudget=384ull*1024*1024;
    static constexpr uint64_t StorageBytes(UINT width,UINT height)noexcept {
        return uint64_t(width)*height*48;
    }
    uint64_t bytes_=0,mutation_=0;Reason reason_=Reason::NoScene;
    bool issued_=false;uintptr_t pendingTarget_=0;UINT pendingSubresource_=0;
    struct DeferredCall {Owner owner{};uintptr_t selected=0;uint64_t ps=0;
        std::unique_ptr<Deferred::Raster> raster;std::shared_ptr<Deferred::Version> targetVersion;};
    static constexpr size_t DeferredCapacity=64,RecoveryBatch=16;
    std::vector<DeferredCall> deferred_;
    // Only an executed prefix, never a published depth/picture receipt. Keep
    // its real target version even after releasing the CPU raster snapshots.
    std::shared_ptr<Deferred::Version> checkpointTarget_;
    uint64_t checkpointLoss_=0;
    Owner deferredOwner_{};uintptr_t deferredTarget_=0;bool recording_=false,replaying_=false;
    // CPU object metadata only, under the same nativeMutex as this Capture.
    // Direct OM slots bound retention to eight views; no binding/content or
    // consumer authority is cached. Strong roots prevent pointer reuse hits.
    struct MappingView {
        ComPtr<ID3D11RenderTargetView> view;
        ComPtr<ID3D11Resource> resource;
        ComPtr<IUnknown> canonical;
    };
    std::array<MappingView,8> mappingViews_{};
    Owner mappingViewOwner_{};
    void ClearMappingViews()noexcept {mappingViews_={};mappingViewOwner_={};}
    uintptr_t MappingViewIdentity(UINT slot,ID3D11RenderTargetView* view,
                                  ComPtr<ID3D11Resource>* output=nullptr)noexcept {
        auto& cached=mappingViews_[slot];
        if(view && cached.view.Get()==view){
            if(output)*output=cached.resource;
            return reinterpret_cast<uintptr_t>(cached.canonical.Get());
        }
        cached={}; // A different/null actual view retires only this CPU entry.
        ComPtr<ID3D11Resource> resource;if(view)view->GetResource(&resource);
        ComPtr<IUnknown> canonical;
        const auto hr=resource?resource->QueryInterface(IID_PPV_ARGS(&canonical)):E_POINTER;
        const auto id=reinterpret_cast<uintptr_t>(canonical.Get());
        // Keep the caller's resource alive across Revoke/slot replacement.
        // Failed queries follow the old result and retry on the next draw.
        if(output)*output=resource;
        if(SUCCEEDED(hr) && canonical){
            cached.view=view;cached.resource=std::move(resource);cached.canonical=std::move(canonical);
        }
        return id;
    }
    ComPtr<ID3DDeviceContextState> scratch_;
    void DropDeferred()noexcept {Deferred::EndExpected(nullptr);deferred_.clear();deferredOwner_={};deferredTarget_=0;recording_=false;checkpointTarget_.reset();checkpointLoss_=0;}
    bool FlushDeferred(ID3D11DeviceContext* context,bool recoverCapacity=false)noexcept {
        GenshinCpuStageTiming::Scope cpuCost(GenshinCpuStageTiming::Stage::MappingReplay);
        if(deferred_.empty())return true;
        reason_=Reason::ResourceFailure; // No failed transaction may retain a prior READY status.
        if(recording_){reason_=Reason::NoExactDraw;return false;}
        if(checkpointTarget_ && (!current_ || !current_->scene || !current_->returned || current_->serial ||
           !GenshinGuides::SameOwner(current_->owner,deferredOwner_) || !Deferred::Intact(checkpointTarget_) ||
           current_->target!=reinterpret_cast<uintptr_t>(checkpointTarget_->identity.Get()) ||
           Mutation(context)!=mutation_)){
            reason_=Reason::DeferredInput;return false;
        }
        // Exact native/query authority remains mandatory. Original draws are
        // already returned; no private work was inserted into their queries.
        if(!Quiescent(context)){reason_=Reason::UnprovedQueryScope;return false;}
        try {
            // Capacity recovery runs only before the next qualifying original
            // draw at a full queue. It must not queue/read back CB payloads or
            // wait here. Only previously Ready payloads can release capacity;
            // the original Resolve path retains its existing Poll opportunity.
            if(!recoverCapacity)GenshinDepthSemantic::Poll(context);
            size_t replayCount=deferred_.size();
            {
                // One nonblocking semantic lock covers every retained call.
                // Pending cannot conceal a known invalid later call/CB slot.
                namespace S=GenshinDepthSemantic::detail;S::Access access;
                if(!access){reason_=Reason::DeferredInput;return false;}
                if(checkpointTarget_ && (checkpointLoss_!=access.s.lossEpoch ||
                   checkpointLoss_!=S::loss.load())){reason_=Reason::DeferredInput;return false;}
                bool pending=false;size_t readyPrefix=0;
                for(const auto& call:deferred_){
                    if(!call.raster || !Deferred::Intact(call.targetVersion) ||
                       !GenshinGuides::SameOwner(call.owner,deferredOwner_)){
                        reason_=Reason::DeferredInput;return false;}
                    const auto check=call.raster->Preflight(context,access.s);
                    if(check.readiness==Deferred::Raster::Readiness::Invalid){
                        call.raster->Refuse(check.stage,check.slot,&access.s);
                        reason_=Reason::DeferredInput;return false;}
                    if(check.readiness==Deferred::Raster::Readiness::Pending)pending=true;
                    if(!pending)++readyPrefix;
                }
                // Invalid anywhere still wins. Resolve keeps the R640 whole-
                // chain Ready barrier; capacity recovery may execute only the
                // contiguous Ready prefix, leaving every later writer queued.
                if(pending && (!recoverCapacity || !readyPrefix)){
                    reason_=Reason::DeferredPayloadPending;return false;}
                if(recoverCapacity)replayCount=(std::min)(readyPrefix,RecoveryBatch);
            }
            for(size_t i=0;i<replayCount;++i){auto& call=deferred_[i];
                if(!Deferred::Intact(call.targetVersion) || !call.raster->Materialize(context)){
                    reason_=Reason::DeferredInput;return false;}}
            // No reader/serial can own an unpublished checkpoint. Retire only
            // CPU roots of completed prior GPU work; keep both UV surfaces,
            // the node, query and target-version proof. An unfinished query
            // keeps every root and the existing 96-root limit may still refuse.
            // At most one DONOTFLUSH completion probe per recovery/Resolve.
            if(checkpointTarget_ && current_.use_count()==2 && Complete(context,*current_))
                current_->ClearRasterRoots();
            ComPtr<ID3D11DeviceContext1> context1;ComPtr<ID3D11Device> device;ComPtr<ID3D11Device1> device1;
            if(FAILED(context->QueryInterface(IID_PPV_ARGS(&context1))))return false;context->GetDevice(&device);
            if(!device || FAILED(device.As(&device1)))return false;
            if(!scratch_){const D3D_FEATURE_LEVEL levels[]{device->GetFeatureLevel()};D3D_FEATURE_LEVEL chosen{};
                const UINT flags=(device->GetCreationFlags()&D3D11_CREATE_DEVICE_SINGLETHREADED)?D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED:0;
                if(FAILED(device1->CreateDeviceContextState(flags,levels,1,D3D11_SDK_VERSION,__uuidof(ID3D11Device),&chosen,&scratch_))){reason_=Reason::ResourceFailure;return false;}
                GenshinQueryScope::NoteD3D11State(scratch_.Get());}
            GenshinDepthSemantic::detail::Internal privateContent;
            ComPtr<ID3DDeviceContextState> previous;context1->SwapDeviceContextState(scratch_.Get(),&previous);
            if(!previous)return false;
            struct Restore {ID3D11DeviceContext1* context;ID3DDeviceContextState* previous;
                ~Restore(){context->SwapDeviceContextState(previous,nullptr);}} restore{context1.Get(),previous.Get()};
            const auto oldDraw=CurrentDraw();
            struct RestoreDraw {DrawArguments draw;~RestoreDraw(){CurrentDraw()=draw;}} restoreDraw{oldDraw};
            replaying_=true;struct ReplayGuard {bool& flag;~ReplayGuard(){flag=false;}} replayGuard{replaying_};
            if(!checkpointTarget_)current_.reset();issued_=false;
            auto checkpoint=checkpointTarget_;
            for(size_t i=0;i<replayCount;++i){auto& call=deferred_[i];
                call.raster->Apply();CurrentDraw()=call.raster->draw;
                BeforeImmediate(context,call.owner,call.selected,call.ps,call.raster->draw.kind,call.raster->draw.count);
                ReturnedImmediate(context);
                if(!current_){static unsigned reports=0;if(reports++<12){
                    ComPtr<ID3D11ShaderResourceView> input;context->PSGetShaderResources(0,1,&input);
                    ComPtr<ID3D11Resource> resource;if(input)input->GetResource(&resource);
                    ComPtr<ID3D11PixelShader> shader;context->PSGetShader(&shader,nullptr,nullptr);
                    UINT bytes=0;const auto hr=shader?shader->GetPrivateData(GenshinDepthEvidence::BytesKey,&bytes,nullptr):E_POINTER;
                    LOG_INFO("R819 replay refusal: pid={} generation={} shader={:X} reason={} input={} selected={} ps_bytes={} ps_bytes_hr={} no_authority=true",
                        GetCurrentProcessId(),call.owner.generation,call.ps,ReasonText(reason_),Identity(resource.Get()),call.selected,bytes,static_cast<long>(hr));
                }DropDeferred();return false;}
                // A touching MRT call need not bind the mapped target at slot
                // zero. Advance the checkpoint only to the actual active
                // scene/compose target, otherwise retain its prior proof.
                if(current_->target==reinterpret_cast<uintptr_t>(call.targetVersion->identity.Get()))
                    checkpoint=call.targetVersion;
            }
            if(!checkpoint || !current_->returned ||
               current_->target!=reinterpret_cast<uintptr_t>(checkpoint->identity.Get())){
                Revoke();DropDeferred();reason_=Reason::DeferredInput;return false;}
            static unsigned reports=0;if(reports++<12)LOG_INFO("R814 deferred mapping: pid={} generation={} calls={} returned={} target={} no_visual_claim=true",GetCurrentProcessId(),current_->owner.generation,replayCount,current_->returned,current_->target);
            if(recoverCapacity){
                checkpointTarget_=std::move(checkpoint);
                checkpointLoss_=deferred_[replayCount-1].raster->loss;
                // All executed inputs have explicit Node roots until query/
                // reader retirement. Only their CPU call records are released.
                // Keep queued target/owner metadata (a later compose may have
                // already advanced it), and every unexecuted writer in order.
                deferred_.erase(deferred_.begin(),deferred_.begin()+replayCount);
                reason_=Reason::None;return true;
            }
            DropDeferred();return current_ && current_->returned;
        }catch(...){reason_=Reason::ResourceFailure;return false;}
    }
    bool Make(Surface& out,UINT width,UINT height,bool raster) noexcept {
        D3D11_TEXTURE2D_DESC d{};d.Width=width;d.Height=height;d.ArraySize=d.MipLevels=d.SampleDesc.Count=1;
        d.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;d.Usage=D3D11_USAGE_DEFAULT;
        d.BindFlags=D3D11_BIND_SHADER_RESOURCE|(raster?D3D11_BIND_RENDER_TARGET:D3D11_BIND_UNORDERED_ACCESS);
        return SUCCEEDED(device_->CreateTexture2D(&d,nullptr,&out.texture)) &&
            SUCCEEDED(device_->CreateShaderResourceView(out.texture.Get(),nullptr,&out.srv)) &&
            (raster?SUCCEEDED(device_->CreateRenderTargetView(out.texture.Get(),nullptr,&out.rtv)):
                    SUCCEEDED(device_->CreateUnorderedAccessView(out.texture.Get(),nullptr,&out.uav)));
    }
    bool Init(ID3D11DeviceContext* context)noexcept {
        ComPtr<ID3D11Device> device;context->GetDevice(&device);
        if(device_ && device.Get()!=device_.Get()){reason_=Reason::OwnerChanged;return false;}
        if(scenePs_ && composePs_ && invalidatePs_ && resolveCs_ && noDepth_ && noBlend_ && controls_ && resolveControls_)return true;
        device_=device;
        const auto* scene=GenshinDepthShaders::Find("BridgeDepth","SceneUv");
        const auto* compose=GenshinDepthShaders::Find("BridgeDepth","ComposeUv");
        const auto* invalid=GenshinDepthShaders::Find("BridgeDepth","InvalidateUv");
        const auto* resolve=GenshinDepthShaders::Find("BridgeDepth","ResolveDepth");
        if(!scene || !compose || !invalid || !resolve){reason_=Reason::BytecodeNotGenerated;return false;}
        // Missing generated entries never trigger a
        // D3DCompile on the draw/Present thread or use unrelated old bytecode.
        if(FAILED(device->CreatePixelShader(scene->code,scene->size,nullptr,&scenePs_)) ||
           FAILED(device->CreatePixelShader(compose->code,compose->size,nullptr,&composePs_)) ||
           FAILED(device->CreatePixelShader(invalid->code,invalid->size,nullptr,&invalidatePs_)) ||
           FAILED(device->CreateComputeShader(resolve->code,resolve->size,nullptr,&resolveCs_)))return false;
        D3D11_DEPTH_STENCIL_DESC ds{};ds.DepthFunc=D3D11_COMPARISON_ALWAYS;
        D3D11_BLEND_DESC blend{};blend.RenderTarget[0].RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;
        D3D11_BUFFER_DESC cb{};cb.ByteWidth=16;cb.Usage=D3D11_USAGE_DEFAULT;cb.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        if(FAILED(device->CreateDepthStencilState(&ds,&noDepth_)) || FAILED(device->CreateBlendState(&blend,&noBlend_)) ||
           FAILED(device->CreateBuffer(&cb,nullptr,&controls_)))return false;
        cb.ByteWidth=32;if(FAILED(device->CreateBuffer(&cb,nullptr,&resolveControls_)))return false;
        return true;
    }
    static bool DirectSample(ID3D11PixelShader* ps,uint64_t hash)noexcept {
        if(hash!=0x773B7BD7A2C6971Full && hash!=0xB58064BC5041A048ull)return false;
        try {UINT size=0;if(!ps || FAILED(ps->GetPrivateData(GenshinDepthEvidence::BytesKey,&size,nullptr)) || !size || size>1024)return false;
            std::vector<unsigned char> code(size);if(FAILED(ps->GetPrivateData(GenshinDepthEvidence::BytesKey,&size,code.data())) || GenshinDepthEvidence::ByteHash(code.data(),size)!=hash)return false;
            return (hash==0x773B7BD7A2C6971Full && size==328) || (hash==0xB58064BC5041A048ull && size==700);
        }catch(...){return false;}
    }
    // Verify actual retained VS outputs against the freshly generated PS
    // inputs. Semantic/register mismatches cannot silently yield bogus UVs.
    struct SignatureEntry {
        ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11HullShader> hs;ComPtr<ID3D11DomainShader> ds;
        D3D11_PRIMITIVE_TOPOLOGY topology=D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
        uint64_t recipe=0;UINT pass=0,fail=0;Reason refusal=Reason::SignatureMismatch;
    };
    // Strong object keys prevent address-reuse hits. Eight entries concern
    // picture composition only, not a second world-draw collector/cache.
    std::array<SignatureEntry,8> signatureCache_{};unsigned signatureCursor_=0;
    bool Signature(ID3D11DeviceContext* context,const char* entry,bool known)noexcept {
        const auto recipe=GenshinDepthShaders::HashSource(GenshinDepthMappingHlsl::Raster);
        const UINT bit=!strcmp(entry,"SceneUv")?1u:!strcmp(entry,"ComposeUv")?2u:4u;
        ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11HullShader> hs;ComPtr<ID3D11DomainShader> ds;ComPtr<ID3D11GeometryShader> gs;
        UINT vc=0,hc=0,dc=0;context->VSGetShader(&vs,nullptr,&vc);context->HSGetShader(&hs,nullptr,&hc);
        context->DSGetShader(&ds,nullptr,&dc);context->GSGetShader(&gs,nullptr,nullptr);
        if(gs){reason_=Reason::GeometryStage;return false;}
        if(!vs || vc || hc || dc){reason_=Reason::ShaderDependency;return false;}
        D3D11_PRIMITIVE_TOPOLOGY topology{};context->IAGetPrimitiveTopology(&topology);
        SignatureEntry* cached=nullptr;
        for(auto& c:signatureCache_)if(c.vs.Get()==vs.Get() && c.hs.Get()==hs.Get() && c.ds.Get()==ds.Get() && c.topology==topology && c.recipe==recipe){cached=&c;break;}
        if(cached){if(cached->pass&bit)return true;if(cached->fail&bit){reason_=cached->refusal;return false;}}
        try{
            using Stage=GenshinDepthSemantic::detail::Stage;
            const auto vertex=GenshinDepthSemantic::detail::Shader(vs.Get(),Stage::ControlVertex);
            const auto hull=hs?GenshinDepthSemantic::detail::Shader(hs.Get(),Stage::Hull):nullptr;
            const auto terminal=ds?GenshinDepthSemantic::detail::Shader(ds.Get(),Stage::Domain):vertex;
            if(!vertex || !vertex->interfaceValid || !terminal || !terminal->interfaceValid || (hs && (!hull || !hull->interfaceValid))){reason_=Reason::ShaderDependency;return false;}
            if(known && vertex->diagnosticByteHash!=(bit==1?GenshinDepthEvidence::SceneVs:GenshinDepthEvidence::PictureVs)){
                reason_=Reason::UnknownVS;return false;
            }
            if(!cached){cached=&signatureCache_[signatureCursor_++%signatureCache_.size()];
                *cached={};cached->vs=vs;cached->hs=hs;cached->ds=ds;cached->topology=topology;cached->recipe=recipe;}
            const auto refuse=[&](Reason reason){cached->fail|=bit;cached->refusal=reason;reason_=reason;return false;};
            const bool patch=topology>=D3D11_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST && topology<=D3D11_PRIMITIVE_TOPOLOGY_32_CONTROL_POINT_PATCHLIST;
            if(bool(hs)!=bool(ds) || bool(ds)!=patch)return refuse(Reason::TessellationLinkage);
            if(ds && !GenshinDepthEvidence::TessellationSignature(vertex->reflection.Get(),hull->reflection.Get(),terminal->reflection.Get(),topology))
                return refuse(Reason::TessellationLinkage);
            const auto* code=GenshinDepthShaders::Find("BridgeDepth",entry);ComPtr<ID3D11ShaderReflection> ps;
            if(!code || FAILED(D3DReflect(code->code,code->size,IID_PPV_ARGS(&ps))))return refuse(Reason::BytecodeNotGenerated);
            if(!GenshinDepthEvidence::MatchingSignature(terminal->reflection.Get(),ps.Get()))return refuse(Reason::SignatureMismatch);
            cached->pass|=bit;return true;
        }catch(...){reason_=Reason::ResourceFailure;return false;}
    }
    bool Replay(ID3D11DeviceContext* context,const DrawArguments& draw,Surface& target,ID3D11PixelShader* ps,
                ID3D11ShaderResourceView* previous,UINT linear,bool strict)noexcept {
        MappingWork queryScope(context,Isolated(context));
        if(!queryScope){reason_=Reason::UnprovedQueryScope;return false;}
        ComPtr<ID3D11GeometryShader> gs;ComPtr<ID3D11HullShader> hs;ComPtr<ID3D11DomainShader> ds;
        UINT hc=0,dc=0;context->GSGetShader(&gs,nullptr,nullptr);context->HSGetShader(&hs,nullptr,&hc);context->DSGetShader(&ds,nullptr,&dc);
        if(gs){reason_=Reason::GeometryStage;return false;}
        if(hc || dc || bool(hs)!=bool(ds)){reason_=Reason::TessellationLinkage;return false;}
        D3D11_PRIMITIVE_TOPOLOGY topology{};context->IAGetPrimitiveTopology(&topology);
        const bool patch=topology>=D3D11_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST && topology<=D3D11_PRIMITIVE_TOPOLOGY_32_CONTROL_POINT_PATCHLIST;
        if(bool(ds)!=patch){reason_=Reason::TessellationLinkage;return false;}
        std::array<ID3D11Buffer*,4> stream{};context->SOGetTargets(4,stream.data());bool so=false;
        for(auto* item:stream)if(item){so=true;item->Release();}if(so)return false;
        ComPtr<ID3D11Predicate> predicate;BOOL predicateValue=FALSE;context->GetPredication(&predicate,&predicateValue);if(predicate)return false;
        std::array<ID3D11UnorderedAccessView*,8> uavs{};context->OMGetRenderTargetsAndUnorderedAccessViews(0,nullptr,nullptr,0,8,uavs.data());
        bool unordered=false;for(auto* item:uavs)if(item){unordered=true;item->Release();}if(unordered)return false;
        ComPtr<ID3D11DepthStencilState> depth;UINT reference=0;context->OMGetDepthStencilState(&depth,&reference);
        D3D11_DEPTH_STENCIL_DESC depthDesc{};depthDesc.DepthEnable=TRUE;depthDesc.DepthFunc=D3D11_COMPARISON_LESS;
        if(depth)depth->GetDesc(&depthDesc);
        if(strict && (depthDesc.StencilEnable || (depthDesc.DepthEnable && depthDesc.DepthFunc!=D3D11_COMPARISON_ALWAYS)))return false;
        ComPtr<ID3D11BlendState> blend;FLOAT factors[4]{};UINT sampleMask=0;context->OMGetBlendState(&blend,factors,&sampleMask);
        D3D11_BLEND_DESC blendDesc{};blendDesc.RenderTarget[0].RenderTargetWriteMask=15;if(blend)blend->GetDesc(&blendDesc);
        if(strict && (blendDesc.AlphaToCoverageEnable || blendDesc.RenderTarget[0].BlendEnable ||
           blendDesc.RenderTarget[0].RenderTargetWriteMask!=15 || sampleMask!=UINT_MAX))return false;
        ComPtr<ID3D11PixelShader> originalPs;std::array<ID3D11ClassInstance*,256> classes{};UINT classCount=256;
        context->PSGetShader(&originalPs,classes.data(),&classCount);
        for(UINT i=0;i<classCount && i<classes.size();++i)if(classes[i])classes[i]->Release();if(classCount)return false;
        std::array<ID3D11RenderTargetView*,8> rtvs{};ComPtr<ID3D11DepthStencilView> dsv;
        context->OMGetRenderTargets(8,rtvs.data(),&dsv);
        ComPtr<ID3D11ShaderResourceView> oldSrv;ComPtr<ID3D11Buffer> oldCb;
        context->PSGetShaderResources(1,1,&oldSrv);context->PSGetConstantBuffers(0,1,&oldCb);
        ComPtr<ID3D11DeviceContext1> context1;context->QueryInterface(IID_PPV_ARGS(&context1));
        UINT first=0,num=4096;if(context1)context1->PSGetConstantBuffers1(0,1,oldCb.ReleaseAndGetAddressOf(),&first,&num);
        const UINT constants[4]{linear,0,0,0};context->UpdateSubresource(controls_.Get(),0,nullptr,constants,0,0);
        // Explicit source/program/IA/CB roots survive replay and storage
        // invalidation until mappingDone and any reader fence complete.
        const auto retain=[&](ID3D11DeviceChild* object){return current_->RetainRasterRoot(object);};
        ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11InputLayout> layout;ComPtr<ID3D11RasterizerState> raster;
        context->VSGetShader(&vs,nullptr,nullptr);context->IAGetInputLayout(&layout);context->RSGetState(&raster);
        bool retained=retain(vs.Get()) && retain(hs.Get()) && retain(ds.Get()) && retain(layout.Get()) && retain(raster.Get());
        std::array<ID3D11Buffer*,32> vertices{};UINT strides[32]{},offsets[32]{};context->IAGetVertexBuffers(0,32,vertices.data(),strides,offsets);
        for(auto* v:vertices)if(v){retained=retain(v) && retained;v->Release();}
        ComPtr<ID3D11Buffer> indices;DXGI_FORMAT indexFormat;UINT indexOffset=0;context->IAGetIndexBuffer(&indices,&indexFormat,&indexOffset);retained=retain(indices.Get()) && retained;
        std::array<ID3D11Buffer*,42> constantsRoots{};context->VSGetConstantBuffers(0,14,constantsRoots.data());
        if(ds){context->HSGetConstantBuffers(0,14,constantsRoots.data()+14);context->DSGetConstantBuffers(0,14,constantsRoots.data()+28);}
        for(auto* v:constantsRoots)if(v){retained=retain(v) && retained;v->Release();}
        // Signature verified every actual stage is CB-only. Merely bound,
        // unread SRVs do not turn into dependencies or force geometry changes.
        if(!retained){for(auto* v:rtvs)if(v)v->Release();return false;}
        auto* cb=controls_.Get();auto* rtv=target.rtv.Get();context->OMSetRenderTargets(1,&rtv,nullptr);
        context->OMSetDepthStencilState(noDepth_.Get(),0);context->OMSetBlendState(noBlend_.Get(),nullptr,UINT_MAX);
        context->PSSetShader(ps,nullptr,0);context->PSSetShaderResources(1,1,&previous);context->PSSetConstantBuffers(0,1,&cb);
        draw.Issue(); // exact original arguments, actual original VS/IA/resources/RS
        ID3D11ShaderResourceView* nullSrv=nullptr;context->PSSetShaderResources(1,1,&nullSrv);
        context->OMSetRenderTargets(8,rtvs.data(),dsv.Get());for(auto* item:rtvs)if(item)item->Release();
        context->OMSetDepthStencilState(depth.Get(),reference);context->OMSetBlendState(blend.Get(),factors,sampleMask);
        context->PSSetShader(originalPs.Get(),nullptr,0);auto* srv=oldSrv.Get();context->PSSetShaderResources(1,1,&srv);
        cb=oldCb.Get();if(context1)context1->PSSetConstantBuffers1(0,1,&cb,&first,&num);else context->PSSetConstantBuffers(0,1,&cb);
        return true;
    }
public:
    Reason Status()const noexcept{return reason_;}
    // Refusal-only routing, never permission for game-context private work.
    // Supported game contexts still take their original full KnownQuiescent /
    // PrivateWork validation at Flush/Replay/Resolve; no extra native sweep.
    static bool RequiresIsolation(ID3D11DeviceContext* context)noexcept {
        GenshinQueryScope::Access access(context);const auto* s=access.Get();
        return !s||!GenshinQueryScope::CurrentHookEpoch(s->certificateEpoch)||
            GenshinQueryScope::coverageLost.load()||s->contention.load()||!s->ranges.KnownQuiescent();
    }
    bool CollectIsolated()const noexcept {
        // Service completed, sole-owned jobs even when this frame's CPU inputs
        // will fail preflight. Collection never releases a published reader,
        // failed Signal/Wait or unknown GPU owner. No extra wait/Flush/readback.
        for(auto& use:rasterUses_)if(use.job && use.job->Complete()){
            if(use.roots)use.roots->nodes={};use={};
        }
        const bool pending=isolatedLane_&&isolatedLane_->Pending();
        if(isolatedMapper_&&isolatedLane_){auto& inner=*isolatedMapper_;
            auto* execution=isolatedLane_->context_.Get();
            // After the job releases Work, pool + current can be the last two
            // private references. No caller consumes this private current_;
            // published readers own the separate PictureJob/output instead.
            if(inner.current_&&inner.current_.use_count()==2&&Complete(execution,*inner.current_))inner.current_.reset();
            for(const auto& node:inner.pool_)CollectRasterRoots(execution,node);
        }
        return pending;
    }
    bool Pending(ID3D11DeviceContext* context)const noexcept {
        bool pending=CollectIsolated();
        for(const auto& node:pool_)if(node){
            if(!Complete(context,*node))pending=true;
            else CollectRasterRoots(context,node);
        }
        return pending;
    }
    void Revoke()noexcept {isolatedResult_.reset();isolatedTargetVersion_.reset();ClearMappingViews();current_.reset();issued_=false;if(!replaying_)DropDeferred();}
    // This is only a raster correspondence receipt. Numerical/jitter/clear
    // evidence is checked separately before any depth/validity is published.
    void Before(ID3D11DeviceContext* context,const Owner& owner,uintptr_t selected,uint64_t ps,
                UINT kind,UINT count)noexcept {
        GenshinCpuStageTiming::Scope cpuCost(GenshinCpuStageTiming::Stage::MappingCapture);
        try {
            if(recording_){Revoke();reason_=Reason::NoExactDraw;return;}
            if(!owner.generation || owner.thread!=GetCurrentThreadId() || Identity(context)!=owner.context){Revoke();reason_=Reason::OwnerChanged;return;}
            if(!deferred_.empty() && !GenshinGuides::SameOwner(deferredOwner_,owner))Revoke();
            if(current_ && (current_->serial || !GenshinGuides::SameOwner(current_->owner,owner)))current_.reset();
            if(!GenshinGuides::SameOwner(mappingViewOwner_,owner)){
                ClearMappingViews();mappingViewOwner_=owner;
            }
            const bool sceneShader=ps==0x773B7BD7A2C6971Full;
            const bool composeShader=ps==0xB58064BC5041A048ull && deferredTarget_;
            // Without a Scene chain there is no mapped target for another draw
            // to affect. Reject before any SRV/RTV/resource identity queries.
            // Once a chain exists, every real target writer is still observed.
            if(!sceneShader && !deferredTarget_)return;
            std::array<ID3D11RenderTargetView*,8> views{};
            context->OMGetRenderTargets(deferredTarget_?8u:1u,views.data(),nullptr);
            ComPtr<ID3D11RenderTargetView> target;target.Attach(views[0]);views[0]=nullptr;
            ComPtr<ID3D11Resource> resource;const auto id=MappingViewIdentity(0,target.Get(),&resource);
            uintptr_t inputId=0;
            if(sceneShader || composeShader){
                ComPtr<ID3D11ShaderResourceView> input;context->PSGetShaderResources(0,1,&input);
                ComPtr<ID3D11Resource> source;if(input)input->GetResource(&source);inputId=Identity(source.Get());
            }
            const bool scene=sceneShader && inputId==selected;
            const bool compose=composeShader && inputId==deferredTarget_;
            bool touches=deferredTarget_ && id==deferredTarget_;
            // Retire changed/null slots even after an earlier target matched.
            for(UINT slot=1;slot<views.size();++slot){
                auto* view=views[slot];if(mappingViews_[slot].view.Get()!=view)mappingViews_[slot]={};
                if(view){touches=touches || MappingViewIdentity(slot,view)==deferredTarget_;view->Release();}
            }
            if(!scene && !compose && !touches)return;
            if(scene){DropDeferred();current_.reset();isolatedResult_.reset();isolatedTargetVersion_.reset();deferredOwner_=owner;}
            if(deferred_.size()>=DeferredCapacity){
                // One bounded recovery, never a retry/poll loop on this draw.
                // SwapDeviceContextState and CurrentDraw restore this exact
                // original invocation before Capture/ExpectRaster below.
                if(!FlushDeferred(context,true)){
                    const auto failure=reason_;Revoke();reason_=failure;return;}
                if(deferred_.size()>=DeferredCapacity){Revoke();reason_=Reason::DeferredCapacity;return;}
            }
            DeferredCall call;call.owner=owner;call.selected=selected;call.ps=ps;
            call.raster=std::make_unique<Deferred::Raster>();call.targetVersion=Deferred::Target(resource.Get())?Deferred::Watch(resource.Get()):nullptr;
            if(!call.targetVersion || !call.raster->Capture(context) || !call.raster->draw.Matches(context,kind,count) ||
               !Deferred::ExpectRaster(context,call.raster->draw,call.raster->rtvs)){
                static unsigned reports=0;if(reports++<12)LOG_INFO("R814 raster capture refusal: pid={} generation={} shader={:X} calls={} no_authority=true",GetCurrentProcessId(),owner.generation,ps,deferred_.size());
                Revoke();reason_=Reason::DeferredInput;return;}
            if(scene || compose)deferredTarget_=id;
            deferred_.push_back(std::move(call));recording_=true;reason_=Reason::None;
            static unsigned reports=0;if(reports++<12)LOG_INFO("R814 raster recorded: pid={} generation={} shader={:X} calls={} no_private_gpu_work=true",GetCurrentProcessId(),owner.generation,ps,deferred_.size());
        }catch(...){Revoke();reason_=Reason::ResourceFailure;}
    }
    void Returned(ID3D11DeviceContext* context)noexcept {
        Deferred::EndExpected(context);
        if(!recording_ || deferred_.empty())return;recording_=false;
        try{if(!deferred_.back().raster->Returned(context)){Revoke();reason_=Reason::DeferredInput;}}
        catch(...){Revoke();reason_=Reason::ResourceFailure;}
    }
    void BeforeImmediate(ID3D11DeviceContext* context,const Owner& owner,uintptr_t selected,uint64_t ps,
                UINT kind,UINT count,const Deferred::Raster* original=nullptr)noexcept {
        issued_=false;reason_=Reason::NoScene;
        requestCost.immediate.fetch_add(1,std::memory_order_relaxed);
        if(current_ && (current_->serial || Mutation(context)!=mutation_))Revoke();
        if(!owner.generation || owner.thread!=GetCurrentThreadId() || Identity(Isolated(context)?isolatedObservation_.Get():context)!=owner.context){reason_=Reason::OwnerChanged;return;}
        ComPtr<ID3D11RenderTargetView> rtv;if(original)rtv=original->rtvs[0];else context->OMGetRenderTargets(1,&rtv,nullptr);
        ComPtr<ID3D11Resource> target;if(rtv)rtv->GetResource(&target);const auto id=Identity(target.Get());
        D3D11_RENDER_TARGET_VIEW_DESC view{};if(rtv)rtv->GetDesc(&view);
        bool touches=current_ && current_->scene && id==current_->target;
        if(current_){std::array<ID3D11RenderTargetView*,8> views{};
            if(original){for(unsigned i=0;i<8;++i)if(original->rtvs[i]){views[i]=original->rtvs[i].Get();views[i]->AddRef();}}
            else context->OMGetRenderTargets(8,views.data(),nullptr);
            for(auto* v:views)if(v){ComPtr<ID3D11Resource> r;v->GetResource(&r);if(Identity(r.Get())==current_->target)touches=true;v->Release();}}
        const auto draw=CurrentDraw();
        if(!draw.Matches(context,kind,count)){if(touches)Revoke();reason_=Reason::NoExactDraw;return;}
        reason_=Reason::ResourceFailure;
        ComPtr<ID3D11ShaderResourceView> input;if(original)input=original->psViews[0];else context->PSGetShaderResources(0,1,&input);
        ComPtr<ID3D11Resource> inputResource;if(input)input->GetResource(&inputResource);
        const auto inputId=Identity(inputResource.Get());
        ComPtr<ID3D11PixelShader> shader;if(original)shader=original->ps;else context->PSGetShader(&shader,nullptr,nullptr);
        const bool scene=ps==0x773B7BD7A2C6971Full && inputId==selected && DirectSample(shader.Get(),ps);
        const bool compose=ps==0xB58064BC5041A048ull && current_ && current_->scene &&
            current_->returned && GenshinGuides::SameOwner(current_->owner,owner) &&
            inputId==current_->target && DirectSample(shader.Get(),ps);
        // The classification above only reads bindings/retained shader bytes.
        // No replay, clear, query End, allocation or mapping publication occurs
        // on this return. Touching draws still require full admission even if
        // their shader cannot produce a valid scene/compose correspondence.
        if(!scene && !compose && !touches){
            requestCost.noWork.fetch_add(1,std::memory_order_relaxed);
            reason_=(ps==0x773B7BD7A2C6971Full && inputId==selected) ||
                (ps==0xB58064BC5041A048ull && current_ && inputId==current_->target)?Reason::NoShaderBytecode:Reason::NoSelectedInput;
            return;
        }
        requestCost.admission.fetch_add(1,std::memory_order_relaxed);
        // Every optional operation below, including Init, fallback clear/End
        // and invalidation replay, retains the complete current admission.
        if(!Quiescent(context)){
            requestCost.refused.fetch_add(1,std::memory_order_relaxed);
            Revoke();reason_=Reason::UnprovedQueryScope;return;
        }
        if(!Init(context)){if(touches)Revoke();return;}
        if(!scene && !compose){
            reason_=(ps==0x773B7BD7A2C6971Full && inputId==selected) ||
                (ps==0xB58064BC5041A048ull && current_ && inputId==current_->target)?Reason::NoShaderBytecode:Reason::NoSelectedInput;
            if(touches && (!Signature(context,"InvalidateUv",false) || !Replay(context,draw,current_->uv[current_->active],invalidatePs_.Get(),nullptr,0,false))){
                const FLOAT invalid[4]{-1,-1,0,0};context->ClearRenderTargetView(current_->uv[current_->active].rtv.Get(),invalid);
            }
            if(touches){context->End(current_->mappingDone.Get());current_->workIssued=true;}
            return;
        }
        if(!Signature(context,scene?"SceneUv":"ComposeUv",true)){Revoke();return;}
        ComPtr<ID3D11Texture2D> texture;if(!target || FAILED(target.As(&texture))){reason_=Reason::NoOutputTarget;return;}
        D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);
        D3D11_SHADER_RESOURCE_VIEW_DESC inputView{};input->GetDesc(&inputView);
        if(!desc.Width || !desc.Height || desc.SampleDesc.Count!=1 || desc.SampleDesc.Quality || desc.ArraySize!=1 || desc.MipLevels!=1 ||
           view.ViewDimension!=D3D11_RTV_DIMENSION_TEXTURE2D || view.Texture2D.MipSlice ||
           inputView.ViewDimension!=D3D11_SRV_DIMENSION_TEXTURE2D || inputView.Texture2D.MostDetailedMip){Revoke();reason_=Reason::TextureLayout;return;}
        ComPtr<ID3D11Texture2D> inputTexture;D3D11_TEXTURE2D_DESC inputDesc{};
        if(!inputResource || FAILED(inputResource.As(&inputTexture))){Revoke();reason_=Reason::TextureLayout;return;}
        inputTexture->GetDesc(&inputDesc);
        if(inputDesc.ArraySize!=1 || inputDesc.MipLevels!=1 || inputDesc.SampleDesc.Count!=1 || inputDesc.SampleDesc.Quality ||
           (inputDesc.MiscFlags&D3D11_RESOURCE_MISC_RESOURCE_CLAMP) ||
           (inputView.Texture2D.MipLevels!=1 && inputView.Texture2D.MipLevels!=UINT(-1))){Revoke();reason_=Reason::TextureLayout;return;}
        ComPtr<ID3D11SamplerState> sampler;if(original)sampler=original->samplers[0];else context->PSGetSamplers(0,1,&sampler);
        const auto sd=SamplerRecipe::Effective(sampler.Get());UINT linear=0;
        const bool supported=scene?SamplerRecipe::ClassifySceneCenter(sd,inputDesc.MipLevels,linear):
            SamplerRecipe::Classify(sd,inputDesc.MipLevels,linear);
        // SceneUv records a geometric center coordinate; it does not emulate
        // the colour filter. ComposeUv keeps unsupported edge taps invalid.
        // This sampler classification grants no single-surface/continuity proof.
        static unsigned samplerReports=0;if(samplerReports++<12)
            LOG_INFO("R821 mapping sampler: pid={} generation={} scene_center={} default={} filter={:X} address={},{},{} bias={} min_lod={} max_lod={} aniso={} compare={} mips={} supported={} linear={} no_visual_claim=true",
                GetCurrentProcessId(),owner.generation,scene,!sampler,static_cast<unsigned>(sd.Filter),static_cast<unsigned>(sd.AddressU),
                static_cast<unsigned>(sd.AddressV),static_cast<unsigned>(sd.AddressW),sd.MipLODBias,sd.MinLOD,sd.MaxLOD,
                sd.MaxAnisotropy,static_cast<unsigned>(sd.ComparisonFunc),inputDesc.MipLevels,supported,linear);
        if(!supported){Revoke();reason_=Reason::SamplerState;return;}
        if(scene){
            if(current_ && GenshinGuides::SameOwner(current_->owner,owner) && current_->scene){Revoke();reason_=Reason::WrongPicture;return;}
            current_.reset();reason_=Reason::MappingPoolBusy;
            for(auto& node:pool_)if(!node || (node.use_count()==1 && Complete(context,*node))){
                if(node && (node->width!=desc.Width || node->height!=desc.Height)){bytes_-=StorageBytes(node->width,node->height);node.reset();}
                const auto size=StorageBytes(desc.Width,desc.Height);
                if(!node){if(bytes_>StorageBudget || size>StorageBudget-bytes_){
                        reason_=Reason::MappingBudget;
                        static unsigned budgetReports=0;if(budgetReports++<8)
                            LOG_INFO("R827 mapping budget: pid={} used={} requested={} limit={} width={} height={} no_authority=true",
                                GetCurrentProcessId(),bytes_,size,StorageBudget,desc.Width,desc.Height);
                        continue;
                    }
                    try{node=std::make_shared<Node>();}catch(...){reason_=Reason::ResourceFailure;return;}
                    if(!Make(node->uv[0],desc.Width,desc.Height,true) || !Make(node->uv[1],desc.Width,desc.Height,true) ||
                       !Make(node->depth,desc.Width,desc.Height,false)){node.reset();reason_=Reason::ResourceFailure;return;}
                    const D3D11_QUERY_DESC query{D3D11_QUERY_EVENT,0};
                    if(FAILED(device_->CreateQuery(&query,&node->mappingDone))){node.reset();reason_=Reason::ResourceFailure;return;}
                    node->width=desc.Width;node->height=desc.Height;bytes_+=size;
                }
                current_=node;break;
            }
            if(!current_)return;
            current_->ClearRasterRoots();current_->owner=owner;current_->executionContext=Identity(context);current_->selected=selected;current_->active=0;current_->serial=0;current_->returned=false;current_->scene=false;mutation_=Mutation(context);
            current_->mappingPrograms={scenePs_,composePs_,invalidatePs_};
            current_->resolveProgram=resolveCs_;current_->mappingConstants=controls_;current_->resolveConstants=resolveControls_;
        }else if(current_->width!=desc.Width || current_->height!=desc.Height){Revoke();reason_=Reason::TextureLayout;return;}
        const unsigned next=compose?1-current_->active:0;const FLOAT invalid[4]{-1,-1,0,0};
        context->ClearRenderTargetView(current_->uv[next].rtv.Get(),invalid);
        auto* previous=compose?current_->uv[current_->active].srv.Get():nullptr;
        const bool replayed=Replay(context,draw,current_->uv[next],compose?composePs_.Get():scenePs_.Get(),previous,linear,true);
        context->End(current_->mappingDone.Get());current_->workIssued=true;
        if(!replayed){Revoke();if(reason_!=Reason::UnprovedQueryScope && reason_!=Reason::GeometryStage && reason_!=Reason::TessellationLinkage && reason_!=Reason::ShaderDependency)reason_=Reason::UnsupportedPipeline;return;}
        current_->active=next;current_->target=id;current_->targetSubresource=0;current_->scene=true;
        current_->returned=false;issued_=true;pendingTarget_=id;pendingSubresource_=0;reason_=Reason::None;
    }
    void ReturnedImmediate(ID3D11DeviceContext* context,const Deferred::Raster* original=nullptr)noexcept {
        if(!issued_ || !current_)return;issued_=false;
        ComPtr<ID3D11RenderTargetView> rtv;if(original)rtv=original->rtvs[0];else context->OMGetRenderTargets(1,&rtv,nullptr);
        ComPtr<ID3D11Resource> target;if(rtv)rtv->GetResource(&target);
        D3D11_RENDER_TARGET_VIEW_DESC view{};if(rtv)rtv->GetDesc(&view);
        current_->returned=Identity(target.Get())==pendingTarget_ && view.ViewDimension==D3D11_RTV_DIMENSION_TEXTURE2D &&
            view.Texture2D.MipSlice==pendingSubresource_;
        if(!current_->returned){Revoke();reason_=Reason::WrongPicture;}
    }
    Result ResolveIsolated(ID3D11DeviceContext* game,ID3D11Resource* source,const GenshinGuides::Facts& facts,
        IUnknown* picture,uint64_t serial,const std::shared_ptr<const GenshinTransport::QueuedTransfer>& packet)noexcept {
        Result result;
        try{
            CollectIsolated(); // retirement does not depend on a new eligible submission
            const GenshinGuides::OwnerIdentity::Context ids(game);Owner owner{};
            if(!serial||!source||!picture||!ids.ToCanonical(facts.owner,owner)||
               owner.thread!=GetCurrentThreadId()||Identity(source)!=facts.textures[1].identity){reason_=Reason::WrongPicture;return result;}
            if(isolatedResult_&&isolatedResult_->serial==serial){
                if(!GenshinGuides::SameOwner(isolatedResult_->owner,owner)||Identity(picture)!=isolatedResult_->target||
                   isolatedResult_->preparedDepth!=facts.textures[1].identity||
                   isolatedResult_->contentVersion!=facts.depthSource.version||isolatedResult_->preparedSource!=facts.preparedSource||
                   isolatedResult_->preparedTransfer!=packet||!Deferred::Intact(isolatedTargetVersion_)||
                   MutationSequence(game)!=isolatedMutation_||!isolatedResult_->isolatedJob||
                   !isolatedResult_->isolatedJob->Texture()){reason_=Reason::WrongPicture;return result;}
                result.owner=isolatedResult_;result.texture=isolatedResult_->depth.texture;
                result.width=isolatedResult_->width;result.height=isolatedResult_->height;result.convention=isolatedResult_->depthConvention;
                reason_=Reason::None;return result;
            }
            // Numeric facts are the cached source result, not a second
            // writer scan. Refuse before private allocation/Wait/replay so a
            // known unsupported source cannot occupy scarce mapper surfaces.
            if(const auto numeric=NumericQualification(facts);numeric!=Reason::None){reason_=numeric;return result;}
            RasterUse* rasterUse=nullptr;
            for(auto& use:rasterUses_)if(!use.job){rasterUse=&use;break;}
            if(!rasterUse){reason_=Reason::IsolatedCapacity;return result;}
            // No query-scope recovery/prefix replay on this route. The ENTIRE
            // returned Scene -> all touching writers -> Compose chain must be
            // retained, target-intact and CPU-ready before a private GPU Wait.
            if(recording_||checkpointTarget_||deferred_.empty()||!GenshinGuides::SameOwner(deferredOwner_,owner)){
                reason_=Reason::DeferredInput;return result;}
            {
                namespace S=GenshinDepthSemantic::detail;S::Access access;
                if(!access){reason_=Reason::DeferredInput;return result;}
                for(const auto& call:deferred_){
                    if(!call.raster||!GenshinGuides::SameOwner(call.owner,owner)||!Deferred::Intact(call.targetVersion)){
                        reason_=Reason::DeferredInput;return result;}
                    const auto& r=*call.raster;
                    if(!r.portable.ready){using F=Deferred::Raster::PortableInputs::Failure;
                        reason_=r.portable.failure==F::Layout||r.portable.failure==F::LayoutSignature||r.portable.failure==F::LayoutFormat?Reason::PortableLayout:
                            r.portable.failure==F::ShaderBytes?Reason::PortableShader:Reason::PortableGeometry;
                        // Resolve-time receipt only, never per-draw IO. A missing
                        // image is not evidence of a particular game upload mode.
                        static uint64_t reportedGeneration=0;static unsigned initialReports=0;
                        if(initialReports<8||owner.generation-reportedGeneration>=300)try{
                            ++initialReports;reportedGeneration=owner.generation;
                            D3D11_BUFFER_DESC d{};const auto slot=r.portable.slot;
                            auto* input=slot==32?r.indices.Get():slot<32?r.vertices[slot].Get():nullptr;
                            if(input)input->GetDesc(&d);
                            const auto retention=GenshinDepthRetention::ReadTelemetry();
                            LOG_INFO("R1056 portable raster refused: pid={} generation={} shader={:X} call={} kind={} slot={} failure={} layout_elements={} used_slots=0x{:X} dependencies_known={} byte_width={} usage={} bind=0x{:X} cpu=0x{:X} misc=0x{:X} payload_bytes={} payload_refused={} private_gpu_work=false",
                                GetCurrentProcessId(),owner.generation,call.ps,r.draw.observationCall,r.draw.kind,slot,
                                Deferred::Raster::PortableInputs::Text(r.portable.failure),r.portable.elementCount,r.portable.slots,r.portable.dependenciesKnown,
                                d.ByteWidth,unsigned(d.Usage),d.BindFlags,d.CPUAccessFlags,d.MiscFlags,retention.payloadBytes,retention.refused);
                            const auto& source=r.portable.sourceStatus;
                            const auto resident=GenshinDepthRetention::ReadSourceTelemetry();
                            const auto refusal=GenshinDepthRetention::ReadRefusalTelemetry();
                            LOG_INFO("R1064 portable source first refusal: pid={} generation={} slot={} source_cause={} record_revision={} image_revision={} image_epoch={} capture_source_epoch={} current_source_epoch={} source_bytes={} ordinary_source_bytes={} source_allocations={} source_refused={} source_evictions={} retention_contention={} retention_capacity={} retention_bytes={} retention_conflict={} retention_allocation={} frozen_at_capture=true no_authority=true",
                                GetCurrentProcessId(),owner.generation,slot,source.attempted?GenshinDepthPortable::Text(source.cause):"NOT_READ",source.revision,source.imageRevision,source.imageEpoch,source.sourceEpoch,
                                GenshinDepthPortable::epoch.load(),resident.bytes,resident.ordinaryBytes,resident.allocations,resident.refused,GenshinDepthPortable::evictions.load(),
                                refusal.contention,refusal.capacity,refusal.bytes,refusal.conflict,refusal.allocation);
                        }catch(...){ } // Diagnostic failure cannot replace the actual refusal.
                        return result;}
                    if(r.Preflight(game,access.s,nullptr,true).readiness!=Deferred::Raster::Readiness::Ready){
                        reason_=Reason::PortableConstants;return result;}
                }
            }
            // Packet factory checks exact owner, source, producer, queue,
            // prepared storage and all real fence/list/allocator roots.
            if(!packet){reason_=Reason::IsolatedPacket;return result;}
            if(isolatedLane_&&!isolatedLane_->Matches(game,packet->Get().queue.Get())){isolatedLane_.reset();isolatedMapper_.reset();}
            if(!isolatedLane_)isolatedLane_=GenshinFgQueryIsolation::PictureLane::Create(game,owner,source,packet);
            if(!isolatedLane_){reason_=Reason::IsolatedPacket;return result;}
            // Packet/source admission and mapper capacity are separate. A
            // retained completed wrapper must first release its own retired
            // roots; neither a use_count exception nor a larger pool is safe.
            unsigned busyJobs=0;
            for(const auto& job:isolatedLane_->jobs_)if(job && (job.use_count()!=1 || !job->Complete()))++busyJobs;
            if(busyJobs==isolatedLane_->jobs_.size()){reason_=Reason::IsolatedCapacity;return result;}
            struct Work {
                std::shared_ptr<Capture> mapper;
                std::vector<DeferredCall> calls;
                std::vector<std::unique_ptr<Deferred::Raster>> clones;
                // Each submission owns its actual mapper surfaces/programs,
                // including a failed partial replay, through its physical fence.
                RasterRoots mappingRoots;
                ComPtr<IUnknown> picture;
                GenshinGuides::Facts facts;
                uint64_t serial=0;bool mapperEntered=false,mappingResolved=false;
            };
            auto work=std::make_shared<Work>();
            if(!isolatedMapper_)isolatedMapper_=std::make_shared<Capture>();work->mapper=isolatedMapper_;
            auto& inner=*work->mapper;inner.isolatedContext_=isolatedLane_->context_;
            inner.isolatedObservation_=game;inner.isolatedSource_=facts.textures[1].identity;
            work->facts=facts;work->serial=serial;work->picture=picture;
            // Metadata is the real target chain, not an invented same-size
            // substitute. The colour resources are never read on this lane.
            ComPtr<ID3D11Texture2D> target;
            for(auto it=deferred_.rbegin();it!=deferred_.rend();++it){
                for(const auto& view:it->raster->rtvs)if(view){ComPtr<ID3D11Resource> r;view->GetResource(&r);
                    if(Identity(r.Get())==Identity(picture)){r.As(&target);break;}}
                if(target)break;
            }
            std::shared_ptr<Deferred::Version> pictureVersion;
            for(const auto& call:deferred_)if(call.targetVersion&&
                reinterpret_cast<uintptr_t>(call.targetVersion->identity.Get())==Identity(picture))pictureVersion=call.targetVersion;
            D3D11_TEXTURE2D_DESC td{};
            if(!target||!Deferred::Intact(pictureVersion)){reason_=Reason::WrongPicture;return result;}target->GetDesc(&td);
            const auto pictureMutation=MutationSequence(game);
            GenshinDepthSemantic::detail::Internal privateContent;
            for(const auto& call:deferred_){auto clone=std::make_unique<Deferred::Raster>();
                if(!call.raster->MaterializePortable(isolatedLane_->device_.Get(),isolatedLane_->context_.Get(),*clone,inner.portableCache_)){
                    reason_=Reason::DeferredInput;return result;}
                work->clones.push_back(std::move(clone));
            }
            work->calls=std::move(deferred_);DropDeferred();isolatedResult_.reset();
            inner.reason_=Reason::IsolatedSubmission; // never report an older successful mapper status
            const auto job=isolatedLane_->Submit(game,owner,source,packet,td.Width,td.Height,work,
                [work](ID3D11Device5*,ID3D11DeviceContext4* c,ID3D11Texture2D* depth)->ComPtr<ID3D11Texture2D>{
                    work->mapperEntered=true;auto& m=*work->mapper;const auto old=CurrentDraw();
                    // The reusable mapper may replace a pool slot on resize.
                    // Pin only this invocation's nodes before Submit signals,
                    // even when Replay/Resolve revokes current_ after GPU work.
                    const auto retain=[&]()noexcept {
                        for(size_t i=0;i<m.pool_.size();++i)if(m.pool_[i]&&m.pool_[i]->workIssued&&
                            GenshinGuides::SameOwner(m.pool_[i]->owner,work->calls.front().owner))
                            work->mappingRoots.nodes[i]=m.pool_[i];
                    };
                    struct RetainOnReturn {const decltype(retain)& fn;~RetainOnReturn(){fn();}} keep{retain};
                    struct RestoreDraw {DrawArguments old;~RestoreDraw(){CurrentDraw()=old;}} restore{old};
                    for(size_t i=0;i<work->calls.size();++i){const auto& call=work->calls[i];const auto& clone=*work->clones[i];
                        if(!Deferred::Intact(call.targetVersion))return {};
                        clone.Apply();CurrentDraw()=clone.draw;
                        m.BeforeImmediate(c,call.owner,call.selected,call.ps,clone.draw.kind,clone.draw.count,call.raster.get());
                        m.ReturnedImmediate(c,call.raster.get());
                        if(!m.current_)return {};
                    }
                    const auto mapped=m.Resolve(c,depth,work->facts,work->picture.Get(),work->serial);
                    work->mappingResolved=mapped.Known();
                    return work->mappingResolved?m.current_->depth.texture:ComPtr<ID3D11Texture2D>{};
                });
            if(!job){
                reason_=inner.Status()==Reason::None?Reason::IsolatedSubmission:inner.Status();
                // Existing Resolve boundary only. Keep the distinction between
                // unavailable output/import admission, raster/Resolve refusal,
                // and a failed post-mapping physical publication. No per-draw IO.
                static unsigned reports=0;static uint64_t previous=0;
                if(reports<8 || owner.generation-previous>=300)try{
                    ++reports;previous=owner.generation;
                    LOG_INFO("R1067 picture submission boundary: pid={} generation={} source_frame={} picture={} reason={} busy_jobs={} mapper_entered={} mapping_resolved={} source_receipt={} packet={} no_completion_inferred=true",
                        GetCurrentProcessId(),owner.generation,owner.sourceFrame,serial,ReasonText(reason_),busyJobs,
                        work->mapperEntered,work->mappingResolved,reinterpret_cast<uintptr_t>(facts.preparedSource.get()),
                        reinterpret_cast<uintptr_t>(packet.get()));
                }catch(...){}
                return result;
            }
            // Alias the existing Work allocation; no extra per-picture roots
            // allocation. A successful job may finish before its NR reader.
            *rasterUse={job,std::shared_ptr<RasterRoots>(work,&work->mappingRoots)};
            if(!Deferred::Intact(pictureVersion)||MutationSequence(game)!=pictureMutation){reason_=Reason::UntrackedWrite;return result;}
            auto node=std::make_shared<Node>();node->owner=owner;node->selected=inner.current_->selected;
            node->target=Identity(picture);node->preparedDepth=facts.textures[1].identity;node->contentVersion=facts.depthSource.version;
            node->preparedSource=facts.preparedSource;node->preparedTransfer=packet;
            node->serial=serial;node->width=td.Width;node->height=td.Height;node->scene=node->returned=true;
            node->depthConvention=facts.depthConvention.value;node->isolatedJob=job;node->depth.texture=job->Texture();
            isolatedTargetVersion_=std::move(pictureVersion);isolatedMutation_=pictureMutation;
            isolatedResult_=node;result.owner=std::move(node);result.texture=isolatedResult_->depth.texture;
            result.width=td.Width;result.height=td.Height;result.convention=facts.depthConvention.value;
            reason_=Reason::None;
            static uint64_t reportedGeneration=0;static unsigned initialReports=0;
            if(initialReports<8||owner.generation-reportedGeneration>=300)try{
                ++initialReports;reportedGeneration=owner.generation;
                const auto& submitted=packet->Get();
                LOG_INFO("R1056 portable picture mapping queued: pid={} instance={} generation={} source_frame={} history_epoch={} device_epoch={} producer_epoch={} config_epoch={} picture_serial={} calls={} prepared={} content_version={} target={} mapped={} width={} height={} producer_queue={} producer_fence={} producer_value={} surface_lifetime={} physical_fence_owned=true cpu_complete=UNKNOWN model_consumed=UNKNOWN pixels=UNKNOWN",
                    GetCurrentProcessId(),owner.instance,owner.generation,owner.sourceFrame,owner.historyEpoch,owner.deviceEpoch,owner.producerEpoch,owner.configEpoch,
                    serial,work->calls.size(),facts.textures[1].identity,facts.depthSource.version,isolatedResult_->target,
                    reinterpret_cast<uintptr_t>(result.texture.Get()),td.Width,td.Height,reinterpret_cast<uintptr_t>(submitted.queue.Get()),
                    reinterpret_cast<uintptr_t>(submitted.submitted.Get()),submitted.submittedValue,reinterpret_cast<uintptr_t>(submitted.surfaceLifetime.get()));
            }catch(...){ } // Publication/physical GPU ownership do not depend on logging.
            return result;
        }catch(...){reason_=Reason::ResourceFailure;return result;}
    }
    Result Resolve(ID3D11DeviceContext* context,ID3D11Resource* source,const GenshinGuides::Facts& facts,
                   IUnknown* picture,uint64_t serial,
                   const std::shared_ptr<const GenshinTransport::QueuedTransfer>& packet={})noexcept {
        Result result;
        // Refuse a known numeric failure before any allocation/private replay.
        // A packet changes the sampled storage, never its original source ID.
        if(const auto numeric=NumericQualification(facts);numeric!=Reason::None){reason_=numeric;return result;}
        ID3D11Resource* sampled=source;
        if(!Isolated(context) && facts.preparedSource){
            if(!packet || !packet->Prepared(1) || !source ||
               !GenshinGuides::SameOwner(packet->Get().owner,facts.owner) ||
               Identity(packet->Get().sources[1].Get())!=facts.preparedSource->Identity() ||
               Identity(source)!=facts.preparedSource->Identity() || !packet->Get().surfaceLifetime){
                reason_=Reason::IsolatedPacket;return result;}
            sampled=packet->Prepared(1);
        }else if(packet){reason_=Reason::IsolatedPacket;return result;}
        if(!Isolated(context)&&!FlushDeferred(context))return result;
        if(!current_)return result; // retain the first raster failure
        if(current_->serial){
            const GenshinGuides::OwnerIdentity::Context ids(Isolated(context)?isolatedObservation_.Get():context);Owner owner{};
            if(current_->serial!=serial || !ids.ToCanonical(facts.owner,owner) ||
               !GenshinGuides::SameOwner(current_->owner,owner) || Identity(picture)!=current_->target ||
               !source || (Isolated(context)?isolatedSource_:Identity(source))!=current_->preparedDepth || facts.textures[1].identity!=current_->preparedDepth ||
               facts.depthSource.version!=current_->contentVersion || facts.preparedSource!=current_->preparedSource ||
               current_->preparedTransfer!=packet || Mutation(context)!=mutation_){reason_=Reason::WrongPicture;return result;}
            result.owner=current_;result.texture=current_->depth.texture;result.width=current_->width;result.height=current_->height;
            result.convention=current_->depthConvention;reason_=Reason::None;return result;
        }
        // A separate private Dispatch can contaminate pipeline statistics
        // even when raster mapping was recorded earlier outside a query.
        MappingWork queryScope(context,Isolated(context));
        if(!queryScope){reason_=Reason::UnprovedQueryScope;return result;}
        if(Mutation(context)!=mutation_){reason_=Reason::UntrackedWrite;return result;}
        const GenshinGuides::OwnerIdentity::Context identities(Isolated(context)?isolatedObservation_.Get():context);Owner canonicalOwner{};
        const bool ownerKnown=identities.ToCanonical(facts.owner,canonicalOwner);
        if(!current_->scene || !current_->returned || !serial || !ownerKnown ||
           !GenshinGuides::SameOwner(current_->owner,canonicalOwner) || Identity(picture)!=current_->target){
            static unsigned reports=0;if(reports++<8)LOG_INFO("R822 mapping owner refusal: pid={} generation={} owner_known={} same_owner={} scene={} returned={} serial={} picture={} target={} fact_context={} canonical_context={} mapping_context={} no_authority=true",
                GetCurrentProcessId(),facts.owner.generation,ownerKnown,ownerKnown && GenshinGuides::SameOwner(current_->owner,canonicalOwner),
                current_->scene,current_->returned,serial,Identity(picture),current_->target,facts.owner.context,canonicalOwner.context,current_->owner.context);
            reason_=Reason::WrongPicture;return result;
        }
        // Preflight already bound the cached source proof before replay.
        const bool reversed=facts.depthConvention.value==GenshinGuides::DepthConvention::DeviceZReversed;
        if(!Init(context) || !source || (Isolated(context)?isolatedSource_:Identity(source))!=facts.textures[1].identity){reason_=Reason::ResourceFailure;return result;}
        reason_=Reason::ResourceFailure;
        ComPtr<ID3D11Texture2D> texture;if(!sampled || FAILED(sampled->QueryInterface(IID_PPV_ARGS(&texture))))return result;
        D3D11_TEXTURE2D_DESC sourceDesc{};texture->GetDesc(&sourceDesc);
        if(sourceDesc.Format!=DXGI_FORMAT_R32_FLOAT || sourceDesc.Width!=facts.renderWidth.value ||
           sourceDesc.Height!=facts.renderHeight.value || sourceDesc.MipLevels!=1 || sourceDesc.ArraySize!=1 || sourceDesc.SampleDesc.Count!=1 || sourceDesc.SampleDesc.Quality){reason_=Reason::TextureLayout;return result;}
        ComPtr<ID3D11ShaderResourceView> sourceView;if(FAILED(device_->CreateShaderResourceView(sampled,nullptr,&sourceView)))return result;
        ComPtr<ID3D11Resource> outputBound;
        std::array<ID3D11RenderTargetView*,8> outputs{};context->OMGetRenderTargets(8,outputs.data(),nullptr);bool aliases=false;
        for(auto* v:outputs)if(v){v->GetResource(&outputBound);aliases=aliases || outputBound.Get()==sampled || outputBound.Get()==current_->depth.texture.Get();v->Release();}
        if(aliases){reason_=Reason::UnsupportedPipeline;return result;}
        ComPtr<ID3D11ComputeShader> oldShader;UINT instances=0;context->CSGetShader(&oldShader,nullptr,&instances);if(instances)return result;
        std::array<ID3D11ShaderResourceView*,2> oldViews{};context->CSGetShaderResources(0,2,oldViews.data());
        std::array<ID3D11UnorderedAccessView*,8> allUavs{};context->CSGetUnorderedAccessViews(0,8,allUavs.data());bool anyUav=false;
        for(auto* v:allUavs)if(v){anyUav=true;v->Release();}
        ComPtr<ID3D11UnorderedAccessView> oldUav;
        // An append/consume UAV counter cannot be observed/restored. Refuse it.
        if(anyUav){for(auto* v:oldViews)if(v)v->Release();return result;}
        ComPtr<ID3D11Buffer> oldCb;context->CSGetConstantBuffers(0,1,&oldCb);
        ComPtr<ID3D11DeviceContext1> context1;context->QueryInterface(IID_PPV_ARGS(&context1));UINT first=0,num=4096;
        if(context1)context1->CSGetConstantBuffers1(0,1,oldCb.ReleaseAndGetAddressOf(),&first,&num);
        struct Constants{UINT w,h,rw,rh;float jx,jy,clear;UINT reversed;} constants{
            current_->width,current_->height,sourceDesc.Width,sourceDesc.Height,
            facts.depthToOutputJitterX.value,facts.depthToOutputJitterY.value,facts.depthClear.value,reversed?1u:0u};
        context->UpdateSubresource(resolveControls_.Get(),0,nullptr,&constants,0,0);
        ID3D11ShaderResourceView* inputs[]{sourceView.Get(),current_->uv[current_->active].srv.Get()};
        auto* cb=resolveControls_.Get();auto* uav=current_->depth.uav.Get();
        context->CSSetShader(resolveCs_.Get(),nullptr,0);context->CSSetShaderResources(0,2,inputs);
        context->CSSetConstantBuffers(0,1,&cb);context->CSSetUnorderedAccessViews(0,1,&uav,nullptr);
        // Retain before the first source-sampling command, including a later
        // failed reader admission. Scratch collection still needs its event.
        current_->preparedSource=facts.preparedSource;current_->preparedTransfer=packet;
        context->Dispatch((current_->width+7)/8,(current_->height+7)/8,1);
        ID3D11UnorderedAccessView* nullUav=nullptr;ID3D11ShaderResourceView* nullViews[2]{};
        context->CSSetUnorderedAccessViews(0,1,&nullUav,nullptr);context->CSSetShaderResources(0,2,nullViews);
        context->CSSetShaderResources(0,2,oldViews.data());for(auto* v:oldViews)if(v)v->Release();
        cb=oldCb.Get();if(context1)context1->CSSetConstantBuffers1(0,1,&cb,&first,&num);else context->CSSetConstantBuffers(0,1,&cb);
        context->CSSetShader(oldShader.Get(),nullptr,0);
        context->End(current_->mappingDone.Get());current_->workIssued=true;
        current_->serial=serial;current_->preparedDepth=facts.textures[1].identity;current_->contentVersion=facts.depthSource.version;
        current_->preparedSource=facts.preparedSource;
        current_->depthConvention=facts.depthConvention.value;result.owner=current_;result.texture=current_->depth.texture;
        result.width=current_->width;result.height=current_->height;result.convention=facts.depthConvention.value;
        reason_=Reason::None;return result;
    }
};
}

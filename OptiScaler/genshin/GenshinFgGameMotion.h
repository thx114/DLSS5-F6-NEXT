#pragma once

// FG-only native-accumulate guide transport. SR and the ordinary NR mapping route
// remain independent. No ReturnedRaster, optical-flow, CPU full-frame scan or wait
// participates in this path.
#include "GenshinDepthMapping.h"
#include "GenshinFsrProfiles.h"
#include "GenshinDx12Transport.h"
#include "GenshinFgGameMotion_Hlsl.h"
#include "GenshinFgLeaseBudget.h"
#include "GenshinFgQueuedIsolation.h"
#include "GenshinOwnerIdentity.h"
#include "GenshinCameraHistory.h"
#include <native/F5LowShaderBytecode.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <DirectXMath.h>
#include <DirectXPackedVector.h>
#include <array>
#include <atomic>
#include <bit>
#include <cstring>
#include <memory>
#include <mutex>

namespace GenshinGameMotion {
using Microsoft::WRL::ComPtr;
using GenshinGuides::Owner;
using Control=GenshinFgQueryIsolation::Control;
enum class Reason { Pending, Ready, MissingSource, WrongOwner, PredecessorGap, PhaseUnproved,
    ShaderUnavailable, Budget, ResourceFailure, QueryScope, SignalFailed, DeviceRemoved,
    MalformedMap, NonIdentity, InvalidMotion, NoValidMotion, WrongReader, UnfencedReader,
    SemanticPending, SemanticUnproved, Discontinuity };
inline const char* Text(Reason r) noexcept {
    switch(r) {
    case Reason::Ready:return "GAME_MOTION_READY";
    case Reason::Pending:return "GAME_MOTION_GPU_SUBMITTED";
    case Reason::PredecessorGap:return "GAME_MOTION_PREDECESSOR_GAP";
    case Reason::PhaseUnproved:return "MOTION_PHASE_UNPROVED";
    case Reason::SemanticPending:return "NATIVE_SEMANTIC_ASYNC_SAMPLE_PENDING";
    case Reason::SemanticUnproved:return "NATIVE_SEMANTIC_CAMERA_ORACLE_UNPROVED";
    case Reason::Discontinuity:return "GAME_MOTION_DISCONTINUITY_RESET";
    case Reason::ShaderUnavailable:return "GAME_MOTION_BYTECODE_UNAVAILABLE_OR_STALE";
    case Reason::Budget:return "GAME_MOTION_RING_EXHAUSTED_OR_BUDGET";
    case Reason::WrongOwner:return "GAME_MOTION_WRONG_OWNER_OR_TICKET";
    case Reason::QueryScope:return "GAME_MOTION_QUERY_SCOPE_UNPROVED";
    case Reason::SignalFailed:return "GAME_MOTION_SIGNAL_FAILED_RETAINED";
    case Reason::DeviceRemoved:return "GAME_MOTION_DEVICE_REMOVED_RETAINED";
    case Reason::WrongReader:return "GAME_MOTION_WRONG_READER";
    case Reason::UnfencedReader:return "GAME_MOTION_READER_UNFENCED_RETAINED";
    default:return "GAME_MOTION_SOURCE_OR_RESOURCE_UNAVAILABLE";
    }
}
inline bool Finite(float v) noexcept {return (std::bit_cast<uint32_t>(v)&0x7f800000u)!=0x7f800000u;}
inline uintptr_t Identity(IUnknown* p) noexcept {ComPtr<IUnknown> id;if(p)p->QueryInterface(IID_PPV_ARGS(&id));return reinterpret_cast<uintptr_t>(id.Get());}
inline constexpr uint64_t SourceHash() noexcept {
    uint64_t h=14695981039346656037ull;
    for(const char* p=GenshinFgGameMotionHlsl::Source;*p;++p){h^=static_cast<unsigned char>(*p);h*=1099511628211ull;}
    return h;
}
inline const F5LowShaderBytecode::Shader* Shader(const char* entry) noexcept {
#if defined(F5_GENSHIN_GAME_MOTION_ABI) && F5_GENSHIN_GAME_MOTION_ABI == 4
    if(!F5LowShaderBytecode::GameMotionSourceMatches(SourceHash()))return nullptr;
    return F5LowShaderBytecode::Find("GenshinGameMotion",entry);
#else
    (void)entry;return nullptr;
#endif
}

class Reference {
    friend class GenshinGuides::Receipt;
    Owner owner{};
    uint64_t shaderIdentity=0,nativeShaderHash=0;
    std::shared_ptr<const NativeReference> nativeEvidence_;
    GenshinGuides::SrOutputGrid acceptedGrid_{};
    float nativeToSrX=0,nativeToSrY=0;
    bool nativeOutputHistoryUv=false,outputReferenceProved=false;
public:
    bool Known(const Owner& o)const noexcept {
        return nativeEvidence_ && acceptedGrid_.Matches(acceptedGrid_.owner) &&
            GenshinGuides::SameOwner(owner,o) && shaderIdentity && GenshinFsrProfiles::Accumulate(nativeShaderHash) &&
            nativeOutputHistoryUv && outputReferenceProved && Finite(nativeToSrX) && Finite(nativeToSrY);
    }
    bool PairCompatible(const Reference& previous)const noexcept {
        if(!nativeEvidence_ || !previous.nativeEvidence_ || !Finite(nativeToSrX) || !Finite(nativeToSrY) ||
           !Finite(previous.nativeToSrX) || !Finite(previous.nativeToSrY) ||
           acceptedGrid_.renderWidth!=previous.acceptedGrid_.renderWidth || acceptedGrid_.renderHeight!=previous.acceptedGrid_.renderHeight ||
           acceptedGrid_.outputWidth!=previous.acceptedGrid_.outputWidth || acceptedGrid_.outputHeight!=previous.acceptedGrid_.outputHeight)
            return false;
        const auto& a=*nativeEvidence_;const auto& b=*previous.nativeEvidence_;
        if(owner.device!=previous.owner.device || owner.context!=previous.owner.context ||
           owner.deviceEpoch!=previous.owner.deviceEpoch || owner.producerEpoch!=previous.owner.producerEpoch ||
           owner.configEpoch!=previous.owner.configEpoch || owner.historyEpoch!=previous.owner.historyEpoch ||
           owner.instance!=previous.owner.instance || owner.thread!=previous.owner.thread)return false;
        // Adjacent accepted transactions from the original producer are the
        // authority. Global game-frame gaps and a replacement CB allocation do
        // not invalidate that producer-local history.
        return a.context_==b.context_ && a.deviceEpoch_==b.deviceEpoch_ && a.configEpoch_==b.configEpoch_ &&
            a.shader_==b.shader_ && a.instance_==b.instance_ && b.generation_!=UINT64_MAX &&
            a.generation_==b.generation_+1 && a.sourceFrame_>b.sourceFrame_;
    }
    void LogPairFailure(const Reference& previous)const noexcept {
        static std::atomic<unsigned> reports{0};
        if(reports.fetch_add(1,std::memory_order_relaxed)>=16 || !nativeEvidence_ || !previous.nativeEvidence_)return;
        const auto& a=*nativeEvidence_;const auto& b=*previous.nativeEvidence_;
        LOG_INFO("R835 native pair refusal: pid={} generation={} source_frame={} previous_generation={} previous_frame={} grid={} owner={} native={} sequence={} phase={} context={} buffer={} device_epoch={} config_epoch={} shader={} instance={} dimensions={} corrections={} current_cb={},{} previous_token={},{} current_token={},{} previous_cb={},{} no_authority=true",
            GetCurrentProcessId(),a.generation_,a.sourceFrame_,b.generation_,b.sourceFrame_,
            acceptedGrid_.renderWidth==previous.acceptedGrid_.renderWidth && acceptedGrid_.renderHeight==previous.acceptedGrid_.renderHeight && acceptedGrid_.outputWidth==previous.acceptedGrid_.outputWidth && acceptedGrid_.outputHeight==previous.acceptedGrid_.outputHeight,
            owner.device==previous.owner.device && owner.context==previous.owner.context && owner.deviceEpoch==previous.owner.deviceEpoch && owner.producerEpoch==previous.owner.producerEpoch && owner.configEpoch==previous.owner.configEpoch && owner.historyEpoch==previous.owner.historyEpoch && owner.instance==previous.owner.instance && owner.thread==previous.owner.thread,
            a.context_==b.context_ && a.deviceEpoch_==b.deviceEpoch_ && a.configEpoch_==b.configEpoch_ && a.shader_==b.shader_ && a.instance_==b.instance_,
            a.generation_>b.generation_ && b.sourceFrame_!=UINT32_MAX && a.sourceFrame_==b.sourceFrame_+1,
            a.SameFramePhase(),a.context_==b.context_,a.buffer_==b.buffer_,
            a.deviceEpoch_==b.deviceEpoch_,a.configEpoch_==b.configEpoch_,a.shader_==b.shader_,a.instance_==b.instance_,
            a.renderWidth_==b.renderWidth_ && a.renderHeight_==b.renderHeight_,Finite(nativeToSrX) && Finite(nativeToSrY) && Finite(previous.nativeToSrX) && Finite(previous.nativeToSrY),
            a.cb28x_,a.cb28y_,b.rawJitterX_,b.rawJitterY_,a.rawJitterX_,a.rawJitterY_,b.cb28x_,b.cb28y_);
    }
    void Correction(float& x,float& y)const noexcept {x=nativeToSrX;y=nativeToSrY;}
};

// Immutable producer facts sealed from the claimed NativeMrt transaction and
// CapturedFrame receipt. Picture identity remains separate from pictureSerial.
struct NativeGuideBundle {
    std::shared_ptr<const GenshinGuides::PreparedDepthSource> preparedSource;
    Owner owner{};
    GenshinLegacy::NativeMrt::Frame frame{};
    GenshinFsr::Parameters parameters{};
    GenshinCameraHistory::Sample camera{};
    Reference reference{};
    GenshinGuides::SrOutputGrid grid{};
    std::array<GenshinGuides::TextureFact,4> textures{};
    GenshinGuides::Source provenance=GenshinGuides::Source::Unknown;
    uint64_t pictureIdentity=0;
};
struct Snapshot {
    Owner owner{};
    std::shared_ptr<const NativeGuideBundle> native;
    std::shared_ptr<const GenshinTransport::QueuedTransfer> transfer;
    std::shared_ptr<const GenshinGuides::Receipt> guides;
    Reference reference;
    uint64_t pictureSerial=0;
    bool discontinuity=false;
};

// One bounded, asynchronous 64x36 sample attests the immutable shader/layout
// contract. Its oracle independently reprojects through the observed current
// and previous camera matrices; it never evaluates the normalization formula.
class Semantics final {
    static constexpr uint32_t W=64,H=36;
    struct Key {
        uint64_t device=0,deviceEpoch=0,producerEpoch=0,configEpoch=0,historyEpoch=0,instance=0,shader=0,shaderIdentity=0;
        uint32_t rw=0,rh=0,ow=0,oh=0;bool reversed=false;
        GenshinLegacy::Settings controls{};
        bool operator==(const Key& b)const noexcept {
            return device==b.device && deviceEpoch==b.deviceEpoch && producerEpoch==b.producerEpoch &&
                configEpoch==b.configEpoch && historyEpoch==b.historyEpoch && instance==b.instance && shader==b.shader &&
                shaderIdentity==b.shaderIdentity && rw==b.rw && rh==b.rh && ow==b.ow && oh==b.oh &&
                reversed==b.reversed && controls==b.controls;
        }
    };
    struct Pending {
        Key key{};Snapshot current,previous;ComPtr<ID3D11DeviceContext4> context;
        ComPtr<ID3D11Texture2D> depth,motion;ComPtr<ID3D11Fence> fence;
        uint32_t x=0,y=0;bool mapped=false;
    };
    struct Store {std::mutex mutex;Key readyKey{};bool ready=false,reversed=false;float clear=1;std::unique_ptr<Pending> pending;};
    static Store& Storage() {static auto* s=new Store;return *s;}
    static Key MakeKey(const Snapshot& s) noexcept {
        const auto& n=*s.native;const auto& f=n.frame;
        Key key;key.device=f.device;key.deviceEpoch=f.deviceEpoch;key.producerEpoch=n.owner.producerEpoch;
        key.configEpoch=f.configEpoch;key.historyEpoch=n.owner.historyEpoch;key.instance=n.owner.instance;
        key.shader=f.shader;key.shaderIdentity=f.shaderIdentity;key.rw=f.renderWidth;key.rh=f.renderHeight;
        key.ow=f.outputWidth;key.oh=f.outputHeight;key.reversed=s.native->camera.reversed;key.controls=f.controls;return key;
    }
    static bool Project(const GenshinCameraHistory::Sample& camera,const float world[4],float& u,float& v) noexcept {
        using namespace DirectX;
        GenshinCameraHistory::Matrix view,clip;
        if(!GenshinCameraHistory::Inverse(camera.viewToWorld,view))return false;
        clip=GenshinCameraHistory::Multiply(view,camera.projectionJittered);
        const auto r=XMVector4Transform(XMVectorSet(world[0],world[1],world[2],1),GenshinCameraHistory::Load(clip));
        const auto w=XMVectorGetW(r);if(!Finite(w)||std::fabs(w)<1.e-8f)return false;
        const auto x=XMVectorGetX(r)/w,y=XMVectorGetY(r)/w;
        if(!Finite(x)||!Finite(y))return false;u=(x+1)*.5f;v=(1-y)*.5f;return Finite(u)&&Finite(v);
    }
    static bool Unproject(const GenshinCameraHistory::Sample& camera,float u,float v,float z,float world[4]) noexcept {
        using namespace DirectX;
        GenshinCameraHistory::Matrix view,clip,inverse;
        if(!GenshinCameraHistory::Inverse(camera.viewToWorld,view))return false;
        clip=GenshinCameraHistory::Multiply(view,camera.projectionJittered);
        if(!GenshinCameraHistory::Inverse(clip,inverse))return false;
        auto r=XMVector4Transform(XMVectorSet(u*2-1,1-v*2,z,1),GenshinCameraHistory::Load(inverse));
        const auto w=XMVectorGetW(r);if(!Finite(w)||std::fabs(w)<1.e-12f)return false;
        r=XMVectorScale(r,1/w);world[0]=XMVectorGetX(r);world[1]=XMVectorGetY(r);world[2]=XMVectorGetZ(r);world[3]=1;
        return Finite(world[0])&&Finite(world[1])&&Finite(world[2]);
    }
    static bool StartLocked(Store& s,const Snapshot& current,const Snapshot& previous,ID3D11DeviceContext* context) {
        if(!context||current.owner.thread!=GetCurrentThreadId()||!current.transfer)return false;
        const auto& transfer=current.transfer->Get();const auto& n=*current.native;
        if(n.frame.renderWidth<W||n.frame.renderHeight<H||!transfer.sources[1]||!transfer.sources[2])return false;
        auto job=std::make_unique<Pending>();job->key=MakeKey(current);job->current=current;job->previous=previous;
        if(FAILED(context->QueryInterface(IID_PPV_ARGS(&job->context))))return false;
        ComPtr<ID3D11Device> device;context->GetDevice(&device);ComPtr<ID3D11Device5> device5;
        if(!device||FAILED(device.As(&device5))||FAILED(device5->CreateFence(0,D3D11_FENCE_FLAG_NONE,IID_PPV_ARGS(&job->fence))))return false;
        D3D11_TEXTURE2D_DESC d{};d.Width=W;d.Height=H;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;
        d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        d.Format=DXGI_FORMAT_R32_FLOAT;if(FAILED(device->CreateTexture2D(&d,nullptr,&job->depth)))return false;
        d.Format=DXGI_FORMAT_R16G16_FLOAT;if(FAILED(device->CreateTexture2D(&d,nullptr,&job->motion)))return false;
        GenshinQueryScope::PrivateWork authority(context);if(!authority)return false;
        // Four bounded tiles cover all screen quadrants; centered-only sampling
        // can miss sky/clear and restart forever without proving the contract.
        for(uint32_t qy=0;qy<2;++qy)for(uint32_t qx=0;qx<2;++qx){
            const uint32_t regionW=n.frame.renderWidth/2,regionH=n.frame.renderHeight/2,tw=W/2,th=H/2;
            const uint32_t ox=qx*regionW+(regionW-tw)/2,oy=qy*regionH+(regionH-th)/2;
            D3D11_BOX box{ox,oy,0,ox+tw,oy+th,1};
            job->context->CopySubresourceRegion(job->depth.Get(),0,qx*tw,qy*th,0,transfer.sources[1].Get(),0,&box);
            job->context->CopySubresourceRegion(job->motion.Get(),0,qx*tw,qy*th,0,transfer.sources[2].Get(),0,&box);
        }
        if(FAILED(job->context->Signal(job->fence.Get(),1)))return false;job->context->Flush();s.pending=std::move(job);return true;
    }
    // Returns true only when a terminal classification was made.
    static bool PollLocked(Store& s,bool& accepted,bool& reversed,float& clear) {
        accepted=false;if(!s.pending)return false;auto& j=*s.pending;
        if(j.fence->GetCompletedValue()<1)return false;
        D3D11_MAPPED_SUBRESOURCE dm{},mm{};
        if(FAILED(j.context->Map(j.depth.Get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&dm))||!dm.pData)return false;
        if(FAILED(j.context->Map(j.motion.Get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&mm))||!mm.pData){j.context->Unmap(j.depth.Get(),0);return false;}
        // Use the D3D helper for exact FP16 conversion, including subnormals.
        const auto decode=[](uint16_t h){return DirectX::PackedVector::XMConvertHalfToFloat(h);};
        const auto& cur=*j.current.native;const auto& prev=*j.previous.native;
        float dc[2],dp[2];cur.reference.Correction(dc[0],dc[1]);prev.reference.Correction(dp[0],dp[1]);
        int score[2]{},eligible=0,clearSamples=0,clearConsistent=0;float parallax=0;
        const float expectedClear=cur.camera.reversed?0.f:1.f;
        for(uint32_t y=0;y<H;++y)for(uint32_t x=0;x<W;++x){
            const auto* dpix=reinterpret_cast<const uint8_t*>(dm.pData)+y*dm.RowPitch+x*4;
            const auto* mpix=reinterpret_cast<const uint8_t*>(mm.pData)+y*mm.RowPitch+x*4;
            float z;std::memcpy(&z,dpix,4);uint16_t mh[2];std::memcpy(mh,mpix,4);
            const float mx=decode(mh[0]),my=decode(mh[1]);if(!Finite(z)||!Finite(mx)||!Finite(my))continue;
            if(z==0||z==1){if(z==expectedClear){++clearSamples;if(std::fabs(mx)+std::fabs(my)<1.e-6f)++clearConsistent;}continue;}
            if(z<0||z>1)continue;
            const uint32_t qx=x/(W/2),qy=y/(H/2),tw=W/2,th=H/2;
            const uint32_t regionW=cur.frame.renderWidth/2,regionH=cur.frame.renderHeight/2;
            const uint32_t sx=qx*regionW+(regionW-tw)/2+x%tw,sy=qy*regionH+(regionH-th)/2+y%th;
            const float nu=(sx+.5f)/cur.frame.renderWidth,nv=(sy+.5f)/cur.frame.renderHeight;
            const float ru=nu-dc[0],rv=nv-dc[1];if(ru<0||ru>=1||rv<0||rv>=1)continue;
            float world[4],pu,pv;if(!Unproject(cur.camera,ru,rv,z,world)||!Project(prev.camera,world,pu,pv))continue;
            pu+=dp[0];pv+=dp[1];const float ex=(pu-nu)*cur.frame.outputWidth,ey=(pv-nv)*cur.frame.outputHeight;
            const float ax=mx*cur.frame.outputWidth,ay=my*cur.frame.outputHeight;
            parallax+=(std::fabs(ex)+std::fabs(ey));++eligible;
            for(int sign=0;sign<2;++sign){const float direction=sign?1.f:-1.f;
                if(std::fabs(ax-ex*direction)<=.75f&&std::fabs(ay-ey*direction)<=.75f)++score[sign];}
        }
        j.context->Unmap(j.motion.Get(),0);j.context->Unmap(j.depth.Get(),0);j.mapped=true;
        const int best=score[1]>=score[0]?1:0;
        const bool qualified=QualifySemanticSample(eligible,parallax,clearSamples,score[0],score[1]);
        static std::atomic<unsigned> sampleReports{0};
        if(sampleReports.fetch_add(1,std::memory_order_relaxed)<64)
            LOG_INFO("R835 native semantic sample: pid={} generation={} source_frame={} previous_generation={} previous_frame={} eligible={} parallax={} clear_samples={} clear_consistent={} score={},{} best={} accepted={} expected_clear={} sky_motion_ignored=true no_authority=true",
                GetCurrentProcessId(),cur.frame.generation,cur.frame.frame,prev.frame.generation,prev.frame.frame,
                eligible,parallax,clearSamples,clearConsistent,score[0],score[1],best,qualified,expectedClear);
        // A static camera/zero-motion sample cannot prove depth or direction.
        // Keep the contract pending and try a later real pair instead.
        if(qualified){
            s.ready=true;s.readyKey=j.key;s.reversed=cur.camera.reversed;s.clear=expectedClear;
            accepted=true;reversed=s.reversed;clear=s.clear;s.pending.reset();return true;
        }
        s.pending.reset();return true;
    }
public:
    struct Result {bool ready=false,reversed=false;float clear=1;Reason reason=Reason::SemanticPending;};
    static Result Admit(ID3D11DeviceContext* context,const Snapshot& current,const Snapshot& previous) noexcept {
        try {
            auto& s=Storage();std::lock_guard lock(s.mutex);const auto key=MakeKey(current);
            if(s.ready&&s.readyKey==key)return {true,s.reversed,s.clear,Reason::Ready};
            if(s.pending&&!(s.pending->key==key))s.pending.reset();
            bool accepted=false,reversed=false;float clear=1;
            if(s.pending&&!PollLocked(s,accepted,reversed,clear))return {false,false,1,Reason::SemanticPending};
            if(s.ready&&s.readyKey==key)return {true,s.reversed,s.clear,Reason::Ready};
            if(!StartLocked(s,current,previous,context))return {false,false,1,Reason::SemanticUnproved};
            return {false,false,1,Reason::SemanticPending};
        }catch(...){return {false,false,1,Reason::SemanticUnproved};}
    }
};

class Lease;
struct Pool {
    std::mutex mutex;std::array<std::shared_ptr<Lease>,3> slots,recycled;Snapshot previous;
    Reason last=Reason::MissingSource;bool discontinuity=true;
    GenshinFgLeaseBudget::Counters counters;
    std::unique_ptr<GenshinFgQueryIsolation::PacketLane> isolatedLane;
};
inline Pool& Storage() {static Pool* pool=new Pool;return *pool;}
inline Reason Last() noexcept {auto& p=Storage();std::lock_guard lock(p.mutex);return p.last;}

class Lease final {
    friend std::shared_ptr<Lease> Prepare(ID3D11DeviceContext*,Snapshot,const Owner&);
    friend void Collect() noexcept;
    Snapshot current_,previous_;ComPtr<ID3D11Device> device11_;ComPtr<ID3D11DeviceContext4> context_;
    ComPtr<ID3D11ComputeShader> shader_;std::array<ComPtr<ID3D11ShaderResourceView>,3> inputs_;
    ComPtr<ID3D11Buffer> constants_;ComPtr<ID3D11UnorderedAccessView> motionUav_,depthUav_;
    ComPtr<ID3D11Texture2D> motion11_,depth11_;ComPtr<ID3D11Fence> ready11_;
    ComPtr<ID3D12Device> readerDevice_;ComPtr<ID3D12CommandQueue> readerQueue_;ComPtr<ID3D12GraphicsCommandList> readerList_;
    ComPtr<ID3D12Resource> motion12_,depth12_;ComPtr<ID3D12Fence> ready12_,readerDone_;
    uint64_t readyValue_=0,serial_=0,readerValue_=0,bytes_=0;uint32_t width_=0,height_=0;
    Reason reason_=Reason::Pending;bool issued_=false,signalled_=false,certified_=false,quarantined_=false;
    bool readerStarted_=false,readerEnded_=false,sourceRootsPruned_=false;
    bool Initialize(ID3D11DeviceContext*,uint32_t,uint32_t) noexcept;
    std::shared_ptr<GenshinFgQueryIsolation::Job> isolatedJob_;
    bool InitializeGameContext(ID3D11DeviceContext*,uint32_t,uint32_t) noexcept;
    bool RecordOnGameContext(const Control&) noexcept; // Legacy only; never an isolation fallback.
    bool Record() noexcept;
    bool Complete()const noexcept {
        if(quarantined_||!device11_||FAILED(device11_->GetDeviceRemovedReason())||
           (readerDevice_&&FAILED(readerDevice_->GetDeviceRemovedReason())))return false;
        if(isolatedJob_)return isolatedJob_->Complete();
        if(!issued_)return true;if(!signalled_||!ready11_||!readyValue_)return false;
        const auto ready=ready11_->GetCompletedValue();if(ready==UINT64_MAX||ready<readyValue_)return false;
        if(!readerStarted_)return true;if(!readerEnded_||!readerDone_||!readerValue_)return false;
        const auto done=readerDone_->GetCompletedValue();return done!=UINT64_MAX&&done>=readerValue_;
    }
public:
    void Prune() noexcept {
        if(current_.owner.thread!=GetCurrentThreadId()||sourceRootsPruned_||!Complete())return;
        current_.transfer.reset();previous_.transfer.reset();current_.guides.reset();previous_.guides.reset();
        current_.native.reset();previous_.native.reset();inputs_={};sourceRootsPruned_=true;
    }
    Reason Status()const noexcept{return quarantined_?reason_:(certified_?Reason::Ready:reason_);}
    const std::array<uint32_t,16>& Certificate()const noexcept {static const std::array<uint32_t,16> empty{};return empty;}
    const Owner& PreviousOwner()const noexcept{return previous_.owner;}
    uint64_t PictureSerial()const noexcept{return current_.pictureSerial;}
    bool Discontinuity()const noexcept{return current_.discontinuity;}
    ID3D12Resource* Motion()const noexcept{return certified_?(isolatedJob_?isolatedJob_->Motion():motion12_.Get()):nullptr;}
    ID3D12Resource* Depth()const noexcept{return certified_?(isolatedJob_?isolatedJob_->Depth():depth12_.Get()):nullptr;}
    bool Matches(const Owner& owner,uint64_t pictureSerial)const noexcept {
        return certified_&&!quarantined_&&GenshinGuides::SameOwner(current_.owner,owner)&&pictureSerial==current_.pictureSerial;
    }
    bool Reusable(ID3D11Device* device,uint32_t width,uint32_t height)const noexcept {
        return device11_.Get()==device&&width_==width&&height_==height&&!motion11_&&!depth11_;
    }
    bool ImportReady(ID3D12Device*,ID3D12CommandQueue*) noexcept;
    bool BeginRead(ID3D12GraphicsCommandList*,ID3D12CommandQueue*) noexcept;
    void EndRead(ID3D12GraphicsCommandList*) noexcept;
    bool BindCompletion(ID3D12Fence*,uint64_t) noexcept;
};

inline void Collect() noexcept {
    auto& p=Storage();std::lock_guard lock(p.mutex);
    for(auto& slot:p.slots)if(slot){slot->Prune();if(slot.use_count()==1&&slot->Complete()){
        // Isolated outputs are owned by Job, not game-device cached textures.
        // Release only this exclusive completed holder, then collect its lane.
        if(slot->isolatedJob_) {
            // V3-R041: exclusive + positively complete above is the only
            // transition to an empty isolated wrapper. Without clearing
            // issued_, Complete would poll the unused legacy ready11_ fence
            // after dropping Job, stranding every cached wrapper forever.
            slot->isolatedJob_.reset();
            slot->issued_=slot->signalled_=slot->certified_=false;
        }
        slot->bytes_=0;
        for(auto& free:p.recycled)if(!free){free=std::move(slot);break;}slot.reset();}}
    if(p.isolatedLane)p.isolatedLane->Collect();GenshinFgQueryIsolation::Lane::ReapRetired();
}
inline std::shared_ptr<Lease> Prepare(ID3D11DeviceContext* context,Snapshot current,const Owner& predecessor) {
    Collect();auto& pool=Storage();std::lock_guard lock(pool.mutex);
    // Every refused pair is a temporal boundary. A later real pair must submit
    // its discontinuity reset; successful Record alone clears that obligation.
    const auto refuse=[&](Reason r)->std::shared_ptr<Lease>{pool.last=r;pool.discontinuity=true;return {};};
    if(!context||!current.native||!current.transfer||!current.guides||!current.pictureSerial||
       current.owner.thread!=GetCurrentThreadId()||current.native->provenance!=GenshinGuides::Source::ActualNativeAccumulate||
       !GenshinGuides::SameOwner(current.native->owner,current.owner))return refuse(Reason::WrongOwner);
    for(const auto& slot:pool.slots)if(slot&&slot->Matches(current.owner,current.pictureSerial))return slot;
    if(!current.reference.Known(current.owner)){pool.previous={};pool.discontinuity=true;return refuse(Reason::PhaseUnproved);}
    Snapshot previous=pool.previous;pool.previous=current;
    if(!previous.native||!previous.transfer||!GenshinGuides::SameOwner(previous.owner,predecessor)||
       current.owner.sourceFrame<=previous.owner.sourceFrame||previous.owner.generation==UINT64_MAX||
       current.owner.generation!=previous.owner.generation+1||current.owner.historyEpoch!=previous.owner.historyEpoch){
        pool.discontinuity=true;return refuse(Reason::PredecessorGap);}
    if(!current.reference.PairCompatible(previous.reference)){current.reference.LogPairFailure(previous.reference);return refuse(Reason::PhaseUnproved);}
    if(!current.native->preparedSource)return refuse(Reason::MissingSource);
    const uint32_t width=current.native->preparedSource->Grid().outputWidth,
        height=current.native->preparedSource->Grid().outputHeight;
    if(!GenshinFgQueryIsolation::TextureExtent(width,height))return refuse(Reason::NonIdentity);
    constexpr uint64_t budget=192ull*1024*1024;const uint64_t bytes=uint64_t(width)*height*12;
    uint64_t used=0;std::shared_ptr<Lease>* vacant=nullptr;
    for(auto& slot:pool.slots)if(slot)used+=slot->bytes_;else if(!vacant)vacant=&slot;
    for(auto& slot:pool.recycled)if(slot)used+=slot->bytes_;
    ComPtr<ID3D11Device> device;context->GetDevice(&device);if(!device)return refuse(Reason::DeviceRemoved);
    std::shared_ptr<Lease>* reusable=nullptr;
    unsigned deviceMismatch=0,extentMismatch=0,unfinished=0,external=0,active=0,cached=0;
    for(auto& slot:pool.recycled)if(slot){
        if(slot->device11_.Get()!=device.Get())++deviceMismatch;
        if(slot->width_!=width||slot->height_!=height)++extentMismatch;
        if(!reusable&&slot.use_count()==1&&slot->Complete()&&slot->Reusable(device.Get(),width,height))reusable=&slot;
    }
    for(const auto& slot:pool.slots)if(slot){
        if(slot->device11_.Get()!=device.Get())++deviceMismatch;
        if(slot->width_!=width||slot->height_!=height)++extentMismatch;
    }
    auto& counts=pool.counters;
    if(deviceMismatch)GenshinFgLeaseBudget::Increment(counts.deviceMismatch);
    if(extentMismatch)GenshinFgLeaseBudget::Increment(counts.extentMismatch);
    // A device/extent transition must not leave completed, incompatible cache
    // allocations permanently consuming the new pair's budget. Reclaim only
    // as needed, only with a vacant active slot, and never an eligible reuse.
    if(vacant&&!reusable&&!GenshinFgLeaseBudget::Fits(used,bytes,false,budget)){
        for(auto& slot:pool.recycled)if(slot&&GenshinFgLeaseBudget::Discardable(
            slot.use_count()==1,slot->Complete(),slot->Reusable(device.Get(),width,height))){
            const auto released=slot->bytes_;slot.reset();used-=released;
            GenshinFgLeaseBudget::Increment(counts.recycledDiscarded);
            if(GenshinFgLeaseBudget::Fits(used,bytes,false,budget))break;
        }
    }
    // Isolated recycling retains only CPU wrappers. Each new Job allocation
    // still counts against this budget and the lane global physical budget.
    // Test before moving. Active/unfinished/external owners remain untouched;
    // the only releases above are completed sole-owned incompatible cache.
    const auto admission=GenshinFgLeaseBudget::Classify(vacant!=nullptr,used,bytes,false,budget);
    if(admission!=GenshinFgLeaseBudget::Admission::Ready){
        const bool noVacant=admission==GenshinFgLeaseBudget::Admission::NoVacantSlot;
        GenshinFgLeaseBudget::Increment(noVacant?counts.noVacantSlot:counts.additionalAllocationBudget);
        const auto inspect=[&](const auto& slots,bool inCache){for(const auto& slot:slots)if(slot){
            if(inCache)++cached;else ++active;
            if(!slot->Complete())++unfinished;
            if(slot.use_count()!=1)++external;
        }};
        inspect(pool.slots,false);inspect(pool.recycled,true);
        if(unfinished)GenshinFgLeaseBudget::Increment(counts.unfinishedHolders);
        if(external)GenshinFgLeaseBudget::Increment(counts.externalHolders);
        GenshinFgLeaseBudget::Increment(counts.refused);
        const uint32_t mask=(noVacant?1u:2u)|(deviceMismatch?4u:0u)|(extentMismatch?8u:0u)|
            (unfinished?16u:0u)|(external?32u:0u);
        // Finite output even if every frame is refused; counters saturate rather
        // than wrap. These observations never authorize release or reuse.
        if(counts.reports<32&&(counts.reports<4||(mask&~counts.seenMask)||counts.refused%300==0)){
            GenshinFgLeaseBudget::Increment(counts.reports);counts.seenMask|=mask;
            LOG_INFO("R845 game motion pool refusal: pid={} generation={} source_frame={} cause={} retained_bytes={} requested_bytes={} reuse={} active={} cached={} unfinished={} external={} device_mismatch={} extent_mismatch={} no_vacant_count={} allocation_budget_count={} unfinished_count={} external_count={} device_mismatch_count={} extent_mismatch_count={} discarded_cache_count={} reused_count={} allocated_count={} refused_count={} predecessor_retained=true no_authority=true",
                GetCurrentProcessId(),current.owner.generation,current.owner.sourceFrame,
                noVacant?"no-vacant-slot":"additional-allocation-budget",used,bytes,reusable!=nullptr,
                active,cached,unfinished,external,deviceMismatch,extentMismatch,counts.noVacantSlot,
                counts.additionalAllocationBudget,counts.unfinishedHolders,counts.externalHolders,
                counts.deviceMismatch,counts.extentMismatch,counts.recycledDiscarded,counts.reused,counts.allocated,counts.refused);
            for(unsigned i=0;i<pool.slots.size();++i)if(const auto& slot=pool.slots[i])
                LOG_INFO("R845 game motion active holder: pid={} slot={} generation={} source_frame={} references={} issued={} signalled={} quarantined={} reader_started={} reader_ended={} reader_completion_bound={} extent={}x{} same_device={} no_authority=true",
                    GetCurrentProcessId(),i,slot->current_.owner.generation,slot->current_.owner.sourceFrame,slot.use_count(),
                    slot->issued_,slot->signalled_,slot->quarantined_,slot->readerStarted_,slot->readerEnded_,
                    slot->readerDone_&&slot->readerValue_,slot->width_,slot->height_,slot->device11_.Get()==device.Get());
        }
        // Capacity is not invalid producer history: pool.previous already holds
        // this authenticated current snapshot. Keep it so the next adjacent pair
        // can recover immediately, while refuse still requires an FG reset.
        return refuse(Reason::Budget);
    }
    std::shared_ptr<Lease> lease;
    if(reusable){lease=std::move(*reusable);GenshinFgLeaseBudget::Increment(counts.reused);}
    if(!lease){lease=std::make_shared<Lease>();if(!lease->Initialize(context,width,height)){pool.previous={};return refuse(lease->reason_);}
        GenshinFgLeaseBudget::Increment(counts.allocated);}
    current.discontinuity=pool.discontinuity;
    lease->current_=std::move(current);lease->previous_=std::move(previous);lease->bytes_=bytes;
    *vacant=lease;
    if(!lease->Record()){
        pool.last=lease->reason_;
        // A capacity refusal did not invalidate the authentic current packet.
        // Retain it for the next exact adjacent pair, as at outer admission;
        // GPU owners still remain in the original slot until physical retirement.
        if(lease->reason_!=Reason::Budget)pool.previous={};
        pool.discontinuity=true;return {};
    }
    pool.discontinuity=false;
    pool.last=Reason::Pending;return lease;
}

inline bool Lease::Initialize(ID3D11DeviceContext* context,uint32_t width,uint32_t height) noexcept {
    if(!context||context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE||
       FAILED(context->QueryInterface(IID_PPV_ARGS(&context_)))){reason_=Reason::ResourceFailure;return false;}
    context->GetDevice(&device11_);
    if(!device11_||FAILED(device11_->GetDeviceRemovedReason())){reason_=Reason::DeviceRemoved;return false;}
    width_=width;height_=height;return true; // Identity only; no game GPU allocation/command.
}
inline bool Lease::InitializeGameContext(ID3D11DeviceContext* context,uint32_t width,uint32_t height) noexcept {
    const auto fail=[&](Reason r){reason_=r;return false;};
    if(!context||FAILED(context->QueryInterface(IID_PPV_ARGS(&context_))))return fail(Reason::ResourceFailure);
    context->GetDevice(&device11_);if(!device11_||FAILED(device11_->GetDeviceRemovedReason()))return fail(Reason::DeviceRemoved);
    const auto* code=Shader("NormalizeNative");if(!code||FAILED(device11_->CreateComputeShader(code->code,code->size,nullptr,&shader_)))return fail(Reason::ShaderUnavailable);
    D3D11_TEXTURE2D_DESC d{};d.Width=width;d.Height=height;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;d.Usage=D3D11_USAGE_DEFAULT;
    d.BindFlags=D3D11_BIND_UNORDERED_ACCESS|D3D11_BIND_SHADER_RESOURCE;d.MiscFlags=D3D11_RESOURCE_MISC_SHARED_NTHANDLE|D3D11_RESOURCE_MISC_SHARED;
    d.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;
    if(FAILED(device11_->CreateTexture2D(&d,nullptr,&motion11_))||FAILED(device11_->CreateUnorderedAccessView(motion11_.Get(),nullptr,&motionUav_)))return fail(Reason::ResourceFailure);
    d.Format=DXGI_FORMAT_R32_FLOAT;
    if(FAILED(device11_->CreateTexture2D(&d,nullptr,&depth11_))||FAILED(device11_->CreateUnorderedAccessView(depth11_.Get(),nullptr,&depthUav_)))return fail(Reason::ResourceFailure);
    D3D11_BUFFER_DESC b{};b.ByteWidth=192;b.Usage=D3D11_USAGE_DEFAULT;b.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    if(FAILED(device11_->CreateBuffer(&b,nullptr,&constants_)))return fail(Reason::ResourceFailure);
    ComPtr<ID3D11Device5> device5;if(FAILED(device11_.As(&device5))||FAILED(device5->CreateFence(0,D3D11_FENCE_FLAG_SHARED,IID_PPV_ARGS(&ready11_))))return fail(Reason::ResourceFailure);
    width_=width;height_=height;bytes_=uint64_t(width)*height*12;return true;
}
inline bool Lease::Record() noexcept {
    const auto fail=[&](Reason r){reason_=r;return false;};
    issued_=signalled_=certified_=quarantined_=readerStarted_=readerEnded_=sourceRootsPruned_=false;
    isolatedJob_.reset();readerDevice_.Reset();readerQueue_.Reset();motion12_.Reset();depth12_.Reset();ready12_.Reset();
    readerList_.Reset();readerDone_.Reset();readerValue_=0;reason_=Reason::Pending;
    const auto& n=*current_.native;const auto& transfer=current_.transfer->Get();GenshinGuides::Owner canonical{};
    if(!n.preparedSource || !current_.guides || !n.preparedSource->Matches(current_.guides->Get()) ||
       !GenshinGuides::SameOwner(n.preparedSource->OwnerCanonical(),n.owner))return fail(Reason::WrongOwner);
    if(!GenshinGuides::OwnerIdentity::Context(context_.Get()).ToCanonical(transfer.owner,canonical)||
       !GenshinGuides::SameOwner(canonical,n.owner)||!GenshinGuides::SameOwner(canonical,current_.owner)||Identity(transfer.sources[1].Get())!=n.textures[1].identity||
       Identity(transfer.sources[2].Get())!=n.textures[2].identity||n.textures[1].format.value!=DXGI_FORMAT_R32_FLOAT||
       n.textures[2].format.value!=DXGI_FORMAT_R16G16_FLOAT||!previous_.transfer)return fail(Reason::WrongOwner);
    const auto& previousTransfer=previous_.transfer->Get();
    if(Identity(previousTransfer.sources[1].Get())!=previous_.native->textures[1].identity)return fail(Reason::WrongOwner);
    // The bridge's source scratch is reused at the next SR invocation. Read
    // the distinct transport-owned copies from these exact two transactions.
    if(!current_.transfer->Prepared(1) || !current_.transfer->Prepared(2) || !previous_.transfer->Prepared(1) ||
       Identity(current_.transfer->Prepared(1))==Identity(previous_.transfer->Prepared(1)))return fail(Reason::WrongOwner);
    float cur[2],prev[2];n.reference.Correction(cur[0],cur[1]);previous_.native->reference.Correction(prev[0],prev[1]);
    // Preserve the existing FG normalization's operational camera/donor
    // contract. It is not an NR DeviceZ/clear certificate. FG consumes the
    // same source/packet even while its independent numeric proof is absent.
    const auto& source=*n.preparedSource;
    const bool sourceZ=source.NumericsKnown();
    const bool reversed=sourceZ?source.Convention()==GenshinGuides::DepthConvention::DeviceZReversed:source.Camera().reversed;
    const float clear=sourceZ?source.ClearValue():(reversed?0.f:1.f);
    GenshinCameraHistory::Matrix currentView,currentClip,clipToWorld,previousView,previousClip;
    if(!GenshinCameraHistory::Inverse(source.Camera().viewToWorld,currentView)||
       !GenshinCameraHistory::Inverse(previous_.native->camera.viewToWorld,previousView))return fail(Reason::SemanticUnproved);
    currentClip=GenshinCameraHistory::Multiply(currentView,source.Camera().projectionJittered);
    previousClip=GenshinCameraHistory::Multiply(previousView,previous_.native->camera.projectionJittered);
    if(!GenshinCameraHistory::Inverse(currentClip,clipToWorld))return fail(Reason::SemanticUnproved);
    const Control values{width_,height_,source.Width(),source.Height(),
        cur[0],cur[1],prev[0],prev[1],clear,reversed?1u:0u,0,0,1.f/width_,1.f/height_,0,0,clipToWorld,previousClip};
    // Seal authentic packet + owner + exact producer device/queue authority.
    // UnknownPast on the game context is neither inspected nor upgraded. This
    // route submits zero commands to that context and is not its PrivateWork.
    try {
        auto pair=GenshinFgQueryIsolation::PacketPair::Open(context_.Get(),current_.transfer,previous_.transfer,
            current_.owner,previous_.owner);
        if(!pair)return fail(Reason::WrongOwner);
        auto& lane=Storage().isolatedLane; // Prepare holds Pool::mutex.
        if(lane&&!lane->Matches(*pair))lane.reset();
        if(!lane){
            const auto* code=Shader("NormalizeNative");if(!code)return fail(Reason::ShaderUnavailable);
            lane=GenshinFgQueryIsolation::PacketLane::Create(*pair,code->code,code->size);
            if(!lane)return fail(Reason::ResourceFailure);
        }
        isolatedJob_=lane->Submit(*pair,values);
        if(!isolatedJob_){
            const auto failure=lane->LastFailure();
            const bool capacity=failure==GenshinFgQueryIsolation::Failure::Capacity ||
                failure==GenshinFgQueryIsolation::Failure::ByteCapacity;
            // Existing refused-source boundary only; no per-draw logging or IO.
            static thread_local uint64_t calls=0;
            static thread_local unsigned reports=0;
            static thread_local auto last=GenshinFgQueryIsolation::Failure::None;
            const auto count=++calls;
            if(reports<32 && (reports<3 || failure!=last || count%300==0)){
                ++reports;last=failure;
                const char* cause=failure==GenshinFgQueryIsolation::Failure::Capacity?"native-job-slots-retained":
                    failure==GenshinFgQueryIsolation::Failure::ByteCapacity?"shared-physical-byte-budget":"native-normalization-refused";
                LOG_INFO("R1063 FG normalization refused: pid={} generation={} source_frame={} cause={} lane_failure={} shared_retained_bytes={} requested_bytes={} shared_limit_bytes={} source=actual_PreparedMotion provider_dispatch=not-reached no_authority=true",
                    GetCurrentProcessId(),current_.owner.generation,current_.owner.sourceFrame,cause,unsigned(failure),
                    GenshinFgQueryIsolation::Detail::bytes.load(),uint64_t(width_)*height_*12,
                    GenshinFgQueryIsolation::Detail::ByteLimit);
            }
            return fail(capacity?Reason::Budget:Reason::ResourceFailure);
        }
        issued_=signalled_=certified_=true;reason_=Reason::Ready;
        static std::atomic<unsigned> reports{0};
        if(reports.fetch_add(1,std::memory_order_relaxed)<16)
            LOG_INFO("R34 isolated game motion: pid={} generation={} previous_generation={} source_frame={} game_context_commands=0 packet_owner_device_queue_sealed=true normalize_native_submitted=true ready_fence_ordered=true cpu_completion_not_claimed=true query_certificate_issued=false elapsed_time_neutral_not_claimed=true opaque_vendor_global_counter_neutral_not_claimed=true",
                GetCurrentProcessId(),current_.owner.generation,previous_.owner.generation,current_.owner.sourceFrame);
        return true;
    }catch(...){return fail(Reason::ResourceFailure);}
}
// Retained legacy implementation. Never used as a fallback when the isolated
// path refuses. Its original game-context authority remains mandatory, and is
// now acquired before UpdateSubresource as well as all other game GPU commands.
inline bool Lease::RecordOnGameContext(const Control& values) noexcept {
    const auto fail=[&](Reason r){reason_=r;return false;};
    GenshinQueryScope::PrivateWork authority(context_.Get());if(!authority)return fail(Reason::QueryScope);
    if(!shader_&&!InitializeGameContext(context_.Get(),width_,height_))return false;
    if(FAILED(device11_->CreateShaderResourceView(current_.transfer->Prepared(2),nullptr,&inputs_[0]))||
       FAILED(device11_->CreateShaderResourceView(current_.transfer->Prepared(1),nullptr,&inputs_[1]))||
       FAILED(device11_->CreateShaderResourceView(previous_.transfer->Prepared(1),nullptr,&inputs_[2])))return fail(Reason::ResourceFailure);
    context_->UpdateSubresource(constants_.Get(),0,nullptr,&values,0,0);
    ComPtr<ID3D11ComputeShader> oldShader;UINT instances=0;context_->CSGetShader(&oldShader,nullptr,&instances);if(instances)return fail(Reason::ResourceFailure);
    std::array<ComPtr<ID3D11ShaderResourceView>,3> oldInputs;for(UINT i=0;i<3;++i)context_->CSGetShaderResources(i,1,&oldInputs[i]);
    for(UINT i=0;i<8;++i){ComPtr<ID3D11UnorderedAccessView> old;context_->CSGetUnorderedAccessViews(i,1,&old);if(old)return fail(Reason::ResourceFailure);}
    ComPtr<ID3D11Buffer> oldCb;UINT first=0,count=0;context_->CSGetConstantBuffers1(0,1,&oldCb,&first,&count);
    struct Restore {ID3D11DeviceContext4* c;ID3D11ComputeShader* shader;ID3D11Buffer* cb;UINT first,count;std::array<ComPtr<ID3D11ShaderResourceView>,3>& inputs;
        ~Restore(){ID3D11UnorderedAccessView* u[2]{};c->CSSetUnorderedAccessViews(0,2,u,nullptr);ID3D11ShaderResourceView* v[3]{};
            for(unsigned i=0;i<3;++i)v[i]=inputs[i].Get();c->CSSetShaderResources(0,3,v);c->CSSetConstantBuffers1(0,1,&cb,&first,&count);c->CSSetShader(shader,nullptr,0);}
    } restore{context_.Get(),oldShader.Get(),oldCb.Get(),first,count,oldInputs};
    if(!current_.transfer->OrderRead(context_.Get()) || !previous_.transfer->OrderRead(context_.Get()))return fail(Reason::SignalFailed);
    static unsigned packetReadReports=0;if(packetReadReports++<12)
        LOG_INFO("R840 Bridge packet read: pid={} generation={} previous_generation={} depth={} previous_depth={} motion={} distinct_depth=true gpu_wait_ordered=true cpu_completion_not_claimed=true",
            GetCurrentProcessId(),current_.owner.generation,previous_.owner.generation,
            Identity(current_.transfer->Prepared(1)),Identity(previous_.transfer->Prepared(1)),Identity(current_.transfer->Prepared(2)));
    issued_=true;ID3D11ShaderResourceView* inputs[]{inputs_[0].Get(),inputs_[1].Get(),inputs_[2].Get()};ID3D11UnorderedAccessView* outputs[]{motionUav_.Get(),depthUav_.Get()};
    auto* cb=constants_.Get();context_->CSSetShaderResources(0,3,inputs);context_->CSSetUnorderedAccessViews(0,2,outputs,nullptr);
    context_->CSSetConstantBuffers(0,1,&cb);context_->CSSetShader(shader_.Get(),nullptr,0);context_->Dispatch((width_+7)/8,(height_+7)/8,1);
    readyValue_=++serial_;signalled_=SUCCEEDED(context_->Signal(ready11_.Get(),readyValue_));context_->Flush();
    if(!signalled_){quarantined_=true;return fail(Reason::SignalFailed);}certified_=true;reason_=Reason::Ready;return true;
}
inline bool Open(IUnknown* source,ID3D12Device* device,ComPtr<ID3D12Resource>& target) noexcept {
    ComPtr<IDXGIResource1> resource;if(!source||FAILED(source->QueryInterface(IID_PPV_ARGS(&resource))))return false;
    HANDLE handle=nullptr;if(FAILED(resource->CreateSharedHandle(nullptr,DXGI_SHARED_RESOURCE_READ|DXGI_SHARED_RESOURCE_WRITE,nullptr,&handle)))return false;
    const HRESULT hr=device->OpenSharedHandle(handle,IID_PPV_ARGS(&target));CloseHandle(handle);return SUCCEEDED(hr);
}
inline bool Lease::ImportReady(ID3D12Device* device,ID3D12CommandQueue* queue) noexcept {
    if(isolatedJob_)return current_.owner.thread==GetCurrentThreadId()&&!quarantined_&&isolatedJob_->ImportReady(device,queue);
    if(!device||!queue||!issued_||!signalled_||quarantined_||FAILED(device->GetDeviceRemovedReason()))return false;
    ComPtr<ID3D12Device> queueDevice;if(FAILED(queue->GetDevice(IID_PPV_ARGS(&queueDevice)))||Identity(queueDevice.Get())!=Identity(device)||
       queue->GetDesc().Type!=D3D12_COMMAND_LIST_TYPE_DIRECT)return false;
    if(!readerDevice_){
        HANDLE handle=nullptr;if(!Open(motion11_.Get(),device,motion12_)||!Open(depth11_.Get(),device,depth12_)||
           FAILED(ready11_->CreateSharedHandle(nullptr,GENERIC_ALL,nullptr,&handle)))return false;
        const HRESULT hr=device->OpenSharedHandle(handle,IID_PPV_ARGS(&ready12_));CloseHandle(handle);if(FAILED(hr))return false;
        readerDevice_=device;readerQueue_=queue;
    }else if(Identity(readerDevice_.Get())!=Identity(device)||Identity(readerQueue_.Get())!=Identity(queue))return false;
    if(FAILED(queue->Wait(ready12_.Get(),readyValue_))){quarantined_=true;reason_=Reason::SignalFailed;return false;}
    certified_=true;return true;
}
inline void Transition(ID3D12GraphicsCommandList* list,ID3D12Resource* resource,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after) noexcept {
    D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition.pResource=resource;
    b.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;b.Transition.StateBefore=before;b.Transition.StateAfter=after;list->ResourceBarrier(1,&b);
}
inline bool Lease::BeginRead(ID3D12GraphicsCommandList* list,ID3D12CommandQueue* queue) noexcept {
    if(isolatedJob_){
        if(current_.owner.thread!=GetCurrentThreadId()||!certified_||quarantined_||readerStarted_)return false;
        if(!isolatedJob_->BeginRead(list,queue))return false;
        readerStarted_=true;return true;
    }
    if(current_.owner.thread!=GetCurrentThreadId()||!certified_||quarantined_||readerStarted_||!readerDevice_||!list||Identity(queue)!=Identity(readerQueue_.Get()))return false;
    ComPtr<ID3D12Device> actual;if(FAILED(list->GetDevice(IID_PPV_ARGS(&actual)))||Identity(actual.Get())!=Identity(readerDevice_.Get()))return false;
    readerStarted_=true;readerList_=list;Transition(list,motion12_.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Transition(list,depth12_.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);return true;
}
inline void Lease::EndRead(ID3D12GraphicsCommandList* list) noexcept {
    if(isolatedJob_){if(isolatedJob_->EndRead(list))readerEnded_=true;return;}
    if(!readerStarted_||readerEnded_||list!=readerList_.Get())return;
    Transition(list,motion12_.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
    Transition(list,depth12_.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);readerEnded_=true;
}
inline bool Lease::BindCompletion(ID3D12Fence* fence,uint64_t value) noexcept {
    if(isolatedJob_){
        if(!readerStarted_)return true;
        if(!quarantined_&&isolatedJob_->BindCompletion(fence,value)){readerDone_=fence;readerValue_=value;return true;}
        quarantined_=true;reason_=Reason::UnfencedReader;return false;
    }
    if(!readerStarted_)return true;
    if(current_.owner.thread!=GetCurrentThreadId()||quarantined_||!readerEnded_||readerDone_||!fence||!value||value==UINT64_MAX){quarantined_=true;reason_=Reason::UnfencedReader;return false;}
    ComPtr<ID3D12Device> device;if(FAILED(fence->GetDevice(IID_PPV_ARGS(&device)))||Identity(device.Get())!=Identity(readerDevice_.Get())){quarantined_=true;reason_=Reason::WrongReader;return false;}
    readerDone_=fence;readerValue_=value;return true;
}
} // namespace GenshinGameMotion

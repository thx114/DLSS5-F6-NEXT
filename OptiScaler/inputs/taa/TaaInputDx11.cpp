#include "pch.h"
#include "TaaInputDx11.h"
#include "TaaSourceOutput.h"
#include "TaaPassDx11.h"
#include <inputs/BeforeSrInput.h>
#include "TaaAutoDx11.h"
#include "TaaDeliveryPolicy.h"
#include "TaaTemporalEvidenceDx11.h"
#include <inputs/F6EvalReceiptV1.h>
#include <inputs/NVNGX_DLSS.h>
#include <NVNGX_Parameter.h>
#include <native/DepthFinderCore.h>
#include <resource_tracking/DrawObservation_Dx11.h>
#include <Util.h>
#include <fstream>
#include <mutex>
#include <set>
#include <unordered_map>

extern "C" NVSDK_NGX_Result NVSDK_CONV F6_NGX_D3D11_EvaluateFeatureV1(ID3D11DeviceContext*,
    const NVSDK_NGX_Handle*,NVSDK_NGX_Parameter*,PFN_NVSDK_NGX_ProgressCallback,uint64_t,neo::ngx::F6EvalReceiptV1*);

namespace taa {
namespace {
struct Request {
    Mode mode=Mode::Off;
    uint64_t revision=0;
    std::string path,error;
    std::filesystem::path evidenceDirectory;
    CandidateKey selected{};
    std::optional<Profile> profile;
};
std::atomic<std::shared_ptr<const Request>> request;
std::atomic_bool needsHooks{false},consumerPending{false};
auto& ownsInput=detail::inputOwned;
std::atomic<uint64_t> revisions{0},requests{0};
thread_local unsigned nesting=0;
// Diagnostic jobs contain CPU shader bytes only. No context, GPU resource or
// registry lock is borrowed by the I/O worker, and a module reference keeps its
// callback valid until Windows has returned from it.
struct EvidencePart { std::wstring suffix;std::vector<unsigned char> bytes; };
EvidencePart EvidenceText(const wchar_t* suffix,std::string_view text) {
    return {suffix,{text.begin(),text.end()}};
}
struct ShaderEvidence {
    HMODULE module=nullptr;
    std::filesystem::path stem;
    std::vector<EvidencePart> parts;
    std::string metadata;
    size_t size=0;
};
std::atomic_size_t pendingEvidenceBytes{0};
void CALLBACK SaveShaderEvidence(PTP_CALLBACK_INSTANCE instance,void* data) noexcept {
    std::unique_ptr<ShaderEvidence> evidence(static_cast<ShaderEvidence*>(data));
    FreeLibraryWhenCallbackReturns(instance,evidence->module);
    const size_t size=evidence->size;
    try {
        std::filesystem::create_directories(evidence->stem.parent_path());
        const auto write=[&](const wchar_t* suffix,const void* bytes,size_t length) {
            auto path=evidence->stem;path+=suffix;
            std::ofstream file;file.exceptions(std::ios::failbit|std::ios::badbit);
            file.open(path,std::ios::binary|std::ios::trunc);
            file.write(static_cast<const char*>(bytes),std::streamsize(length));file.close();
        };
        for(const auto& part:evidence->parts)write(part.suffix.c_str(),part.bytes.data(),part.bytes.size());
        // Publish the completion marker only after every file is closed. An
        // observer must never mistake an open/truncated metadata file for a
        // complete bytecode/assembly record.
        write(L".meta.txt.part",evidence->metadata.data(),evidence->metadata.size());
        auto pending=evidence->stem;pending+=L".meta.txt.part";
        auto published=evidence->stem;published+=L".meta.txt";
        if(!MoveFileExW(pending.c_str(),published.c_str(),MOVEFILE_REPLACE_EXISTING))
            throw std::runtime_error("cannot publish the completed shader evidence");
        LOG_INFO("TAA shader evidence: saved={}",evidence->stem.string());
    } catch(const std::exception& error) {
        LOG_WARN("TAA shader evidence: write failed: {}",error.what());
    } catch(...) {LOG_WARN("TAA shader evidence: write failed");}
    pendingEvidenceBytes.fetch_sub(size,std::memory_order_relaxed);
}
bool QueueEvidence(const Request& wanted,const std::string& name,std::vector<EvidencePart>&& parts,std::string&& metadata) noexcept {
    size_t size=metadata.size();for(const auto& part:parts)size+=part.bytes.size();
    constexpr size_t budget=16*1024*1024;
    if(pendingEvidenceBytes.fetch_add(size,std::memory_order_relaxed)+size>budget) {
        pendingEvidenceBytes.fetch_sub(size,std::memory_order_relaxed);
        LOG_WARN("TAA shader evidence: skipped={} pending CPU export budget is full",name);return false;
    }
    HMODULE module=nullptr;
    try {
        auto evidence=std::make_unique<ShaderEvidence>();
        evidence->stem=wanted.evidenceDirectory/name;
        evidence->metadata=std::move(metadata);evidence->parts=std::move(parts);evidence->size=size;
        if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,reinterpret_cast<LPCWSTR>(&SaveShaderEvidence),&module))
            throw std::runtime_error("cannot retain the export callback module");
        evidence->module=module;
        if(!TrySubmitThreadpoolCallback(&SaveShaderEvidence,evidence.get(),nullptr))
            throw std::runtime_error("cannot submit the shader export callback");
        evidence.release();return true;
    } catch(const std::exception& error) {LOG_WARN("TAA shader evidence: queue failed for {}: {}",name,error.what());}
    catch(...) {LOG_WARN("TAA shader evidence: queue failed for {}",name);}
    if(module)FreeLibrary(module);
    pendingEvidenceBytes.fetch_sub(size,std::memory_order_relaxed);return false;
}
bool QueueShaderEvidence(const Request& wanted,CandidateKey key,uint64_t frame,const std::string& bindings,
    std::vector<unsigned char>&& bytes,std::string&& assembly,const analysis::Result& result,const char* stage=nullptr) {
    const auto* name=stage?stage:key.stage==Stage::Pixel?"PS":"CS";
    auto metadata=std::format("schema=2\npid={}\ncandidate={}:{:016X}\nsource_frame={}\nbytes={}\nrecipes={}\nreason={}\nunsupported_instruction={}\nbindings={}\n",
        GetCurrentProcessId(),name,key.hash,frame,bytes.size(),result.recipes.size(),result.reason,result.unsupportedInstruction,bindings);
    for(const auto& recipe:result.recipes)metadata+=std::format("temporal_roles color=t{} history=t{} motion=t{} output={} jitter_in_resolve={} motion_scale={},{} sr_hdr_flag={} sr_hdr_declared={} color_transfer=unknown\n",
        recipe.profile.colorSlot,recipe.historySlot,recipe.profile.motionSlot,recipe.profile.outputSlot,recipe.jitterInResolve,recipe.profile.motionScaleX,recipe.profile.motionScaleY,recipe.profile.hdr,recipe.profile.hdrDeclared);
    std::vector<EvidencePart> parts;parts.push_back({L".dxbc",std::move(bytes)});parts.push_back(EvidenceText(L".asm.txt",assembly));
    return QueueEvidence(wanted,std::format("{}-{:016X}",name,key.hash),std::move(parts),std::move(metadata));
}
struct RasterRequest {
    evidence::RasterWatch watch;
    std::shared_ptr<const Request> wanted;
};
std::atomic<std::shared_ptr<const RasterRequest>> rasterRequest;
thread_local evidence::RasterWindow rasterWindow;
thread_local evidence::RasterMailbox rasterMailbox;
void RasterBefore(ID3D11DeviceContext* context,const DrawObservationDx11::Draw& draw,const DrawObservationDx11::Owner& owner,
    DrawObservationDx11::CapturedState& state,uint64_t& cookie) noexcept {
    try {
        const auto current=rasterRequest.load(std::memory_order_acquire);if(!current)return;
        // Do not enter the TAA registry here: a replacement transaction may
        // hold that lock across this very original draw.
        cookie=rasterWindow.Before(current->watch,context,draw,owner,state,
            [&](evidence::ShaderStage stage,uint64_t hash,auto&& bytes,auto&& assembly,const analysis::Result& result) {
                return QueueShaderEvidence(*current->wanted,{Stage::Pixel,hash},owner.frame,"raster binding match",
                    std::move(bytes),std::move(assembly),result,stage==evidence::ShaderStage::Vertex?"VS":"PS");
            });
        if(const auto report=rasterWindow.Report(owner.frame))LOG_INFO("TAA raster capture: {}",*report);
    }catch(...) {cookie=0;}
}
void RasterAfter(ID3D11DeviceContext*,const DrawObservationDx11::Draw&,const DrawObservationDx11::Owner& owner,
    uint64_t cookie,DrawObservationDx11::Completion completion) noexcept {
    try {
        const auto current=rasterRequest.load(std::memory_order_acquire);
        if(!current){rasterWindow.Reset();rasterMailbox.Reset();return;}
        const auto receipt=rasterWindow.After(cookie,completion,current->watch,owner,[&](uint64_t pixel,uint64_t vertex,evidence::Frame&& frame) {
            std::vector<EvidencePart> parts;parts.push_back({L".constants.bin",std::move(frame.buffers)});
            return QueueEvidence(*current->wanted,std::format("raster/PS-{:016X}-VS-{:016X}-{:08}",pixel,vertex,frame.recordId),std::move(parts),std::move(frame.metadata));
        });
        if(receipt)rasterMailbox.Publish(*receipt);
    }catch(...) {}
}
void RasterRevoke() noexcept {rasterWindow.Reset();rasterMailbox.Reset();}
const DrawObservationDx11::Consumer RasterConsumer{&RasterBefore,&RasterAfter,nullptr,&RasterRevoke,nullptr};
thread_local jitter::Tracker projectionJitter;
void ProjectionBefore(ID3D11DeviceContext* context,const DrawObservationDx11::Draw& draw,const DrawObservationDx11::Owner& owner,
    DrawObservationDx11::CapturedState& state,uint64_t& cookie) noexcept {
    try {
        const auto wanted=request.load(std::memory_order_acquire);
        if(!wanted||wanted->mode==Mode::Off||wanted->profile||!wanted->error.empty()||
            (draw.kind!=DrawObservationDx11::Kind::Draw&&draw.kind!=DrawObservationDx11::Kind::Indexed)||draw.count!=3)return;
        const auto id=state.PixelIdentity();if((id.hash!=0x137B32A04AB724F0ull||id.bytes!=1060)&&(id.hash!=0xAE2AA729C623837Aull||id.bytes!=1456))return;
        const auto ps=automatic::Find(state.PixelShader()),vs=automatic::Find(state.VertexShader());
        if(!ps||!vs)return;
        cookie=projectionJitter.Before(context,draw,owner,wanted->revision,ps->projection,vs->projection);
        if(!cookie&&ps->projection==jitter::Program::PackedCameraPixel&&(owner.frame<=4||owner.frame%300==0)) {
            const auto& d=projectionJitter.PackedDiagnostics();
            LOG_INFO("TAA packed projection rejected: owner_frame={} owner_epoch={} revision={} calls={} reason={} "
                     "examined={} render={}x{} cb_write={} cb_epoch={} dot={} norms={},{},{} "
                     "depth_ab={},{} near_far={},{} product_error={} raster_error={} offset={},{}",
                     owner.frame,owner.epoch,wanted->revision,projectionJitter.Calls(),projectionJitter.Error(),
                     d.examined,d.width,d.height,d.writeSerial,d.constantEpoch,d.offAxisDot,d.normX,d.normY,d.normW,
                     d.depthA,d.depthB,d.nearPlane,d.farPlane,d.productError,d.rasterError,d.offsetX,d.offsetY);
        }
    }catch(...){projectionJitter.Reset();cookie=0;}
}
void ProjectionAfter(ID3D11DeviceContext*,const DrawObservationDx11::Draw&,const DrawObservationDx11::Owner&,
    uint64_t cookie,DrawObservationDx11::Completion completion) noexcept {
    try {
        const auto wanted=request.load(std::memory_order_acquire);
        projectionJitter.After(cookie,completion,wanted?wanted->revision:0);
        if(cookie&&projectionJitter.Current()) {
            const auto& s=*projectionJitter.Current();
            if(s.frame<=4||s.frame%300==0)LOG_INFO("TAA projection jitter: owner_frame={} owner_epoch={} write_serial={} render={}x{} pixels={},{}",
                s.frame,s.epoch,s.writeSerial,s.width,s.height,s.offset.x,s.offset.y);
        }
    }catch(...){projectionJitter.Reset();}
}
void ProjectionPresent(ID3D11DeviceContext*,const DrawObservationDx11::Owner&) noexcept {projectionJitter.Reset();}
void ProjectionRevoke() noexcept {projectionJitter.Reset();}
const DrawObservationDx11::Consumer ProjectionConsumer{&ProjectionBefore,&ProjectionAfter,&ProjectionPresent,&ProjectionRevoke,nullptr};
struct Session {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    std::array<std::unique_ptr<PassDx11>,3> passes;
    std::array<std::unique_ptr<consumer::Chain>,3> chains;
    struct Pending {unsigned slot=0;uint64_t frame=0,revision=0;Profile profile;bool observing=false,evaluated=false;};
    uint64_t cancelledConsumers=0;
    std::optional<Pending> pending;
    void CancelPending(std::string_view reason="abandoned",bool completed=false) {
        if(pending) {
            const auto& activity=chains[pending->slot]->Activity();
            if(!completed && (++cancelledConsumers<=4 || pending->frame%300==0))
                LOG_INFO("TAA consumer cancelled: owner_frame={} observing={} reason={} draws={} linked={} disconnected={} invalidated={} pixel={:016X} vertex={:016X} input={} last_refusal={} refusal_draw={} refusal_pixel={:016X} refusal_vertex={:016X} refusal_input={} refusal_output={} refusal_extent={}x{} refusal_parent={}",
                    pending->frame,pending->observing,reason,activity.draws,activity.linked,activity.disconnected,activity.invalidated,activity.pixel,activity.vertex,activity.input,activity.reason,
                    activity.refused.ordinal,activity.refused.pixel,activity.refused.vertex,activity.refused.input,
                    activity.refused.output,activity.refused.width,activity.refused.height,activity.refused.parent);
            chains[pending->slot]->AbandonBinding();passes[pending->slot]->Abandon();pending.reset();
        }
        consumerPending.store(false,std::memory_order_release);
    }
    std::unique_ptr<NVNGX_Parameters> parameters;
    NVSDK_NGX_Handle stableHandle{}; // value ID; never borrow a provider's freed handle across Shutdown/backend changes
    NVSDK_NGX_Handle* handle=nullptr;
    std::vector<std::tuple<uint64_t,uint64_t,std::string>> consumerEvidence;
    unsigned consumerEvidenceAttempts=0;
    uint64_t revision=0,lastAttempt=0,lastEvaluatedSource=0,lastDeliveredSource=0;
    unsigned featureFlags=0;
    static unsigned CreateFlags(const Profile& profile) {
        return NVSDK_NGX_DLSS_Feature_Flags_AutoExposure|NVSDK_NGX_DLSS_Feature_Flags_MVLowRes|
            (profile.depthInverted?NVSDK_NGX_DLSS_Feature_Flags_DepthInverted:0u)|
            (profile.hdr?NVSDK_NGX_DLSS_Feature_Flags_IsHDR:0u)|
            (profile.motionJittered?NVSDK_NGX_DLSS_Feature_Flags_MVJittered:0u);
    }
    unsigned width=0,height=0,outWidth=0,outHeight=0;
    DXGI_FORMAT format=DXGI_FORMAT_UNKNOWN;
    bool rebuild=false;
    explicit Session(ID3D11DeviceContext* c,uint64_t r) : context(c),revision(r) {c->GetDevice(&device);}
    bool Idle() { for (auto& pass:passes) if (pass && !pass->Idle()) return false; for(auto& chain:chains)if(chain&&!chain->Idle())return false; return true; }
    bool Release() {
        CancelPending();
        if (!Idle()) return false;
        if (handle && !NgxInput::ReleaseTaaDx11Feature(stableHandle.Id)) return false;
        NgxInput::DetachTaaDx11Input(device.Get());
        handle=nullptr;parameters.reset();return true;
    }
    bool PrepareLayout(unsigned renderWidth,unsigned renderHeight,unsigned outputWidth,unsigned outputHeight,DXGI_FORMAT outputFormat) {
        if(handle&&(width!=renderWidth||height!=renderHeight||outWidth!=outputWidth||outHeight!=outputHeight||format!=outputFormat)) {rebuild=true;return false;}
        return true;
    }
    bool Evaluate(const Frame& frame,const Profile& profile,uint64_t sourceFrame) {
        if (handle && !NgxInput::HasTaaDx11Context(stableHandle.Id)) {
            // Release an abandoned context before its borrowed parameter table.
            // A live rebuild retains this same handle through all three phases.
            if (!NgxInput::ReleaseTaaDx11Feature(stableHandle.Id)) return false;
            handle=nullptr;parameters.reset();
        }
        D3D11_TEXTURE2D_DESC desc{};frame.output->GetDesc(&desc);
        // A layout change is handled between transactions by the controller.
        // Releasing here would race the just-recorded input copies.
        if(!PrepareLayout(frame.renderWidth,frame.renderHeight,frame.outputWidth,frame.outputHeight,desc.Format))return false;
        if(handle&&featureFlags!=CreateFlags(profile)){rebuild=true;return false;}
        const bool reset=frame.reset||!handle;
        native::SyntheticUpscalerCallScope synthetic;
        if (!handle) {
            if (!NgxInput::AttachTaaDx11Input(device.Get())) return false;
            parameters=std::make_unique<NVNGX_Parameters>(API::DX11,false);
            const unsigned flags=CreateFlags(profile);
            parameters->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,flags);
            parameters->Set(NVSDK_NGX_Parameter_Width,frame.renderWidth);
            parameters->Set(NVSDK_NGX_Parameter_Height,frame.renderHeight);
            parameters->Set(NVSDK_NGX_Parameter_OutWidth,frame.outputWidth);
            parameters->Set(NVSDK_NGX_Parameter_OutHeight,frame.outputHeight);
            parameters->Set(NVSDK_NGX_Parameter_PerfQualityValue,frame.renderWidth==frame.outputWidth && frame.renderHeight==frame.outputHeight
                ? NVSDK_NGX_PerfQuality_Value_DLAA : NVSDK_NGX_PerfQuality_Value_MaxQuality);
            source_output::CreationScope creation(reinterpret_cast<uintptr_t>(context.Get()),reinterpret_cast<uintptr_t>(parameters.get()));
            NVSDK_NGX_Handle* created=nullptr;
            if (NVSDK_NGX_D3D11_CreateFeature(context.Get(),NVSDK_NGX_Feature_SuperSampling,parameters.get(),&created)!=NVSDK_NGX_Result_Success || !created) return false;
            stableHandle=*created;handle=&stableHandle;featureFlags=flags;
            width=frame.renderWidth;height=frame.renderHeight;outWidth=frame.outputWidth;outHeight=frame.outputHeight;format=desc.Format;
        }
        auto& p=*parameters;
        p.Set(NVSDK_NGX_Parameter_Color,static_cast<ID3D11Resource*>(frame.color));
        p.Set(NVSDK_NGX_Parameter_Depth,static_cast<ID3D11Resource*>(frame.depth));
        p.Set(NVSDK_NGX_Parameter_MotionVectors,static_cast<ID3D11Resource*>(frame.motion));
        p.Set(NVSDK_NGX_Parameter_Output,static_cast<ID3D11Resource*>(frame.output));
        p.Set(NVSDK_NGX_Parameter_Jitter_Offset_X,frame.jitterX);p.Set(NVSDK_NGX_Parameter_Jitter_Offset_Y,frame.jitterY);
        p.Set(NVSDK_NGX_Parameter_MV_Scale_X,1.0f);p.Set(NVSDK_NGX_Parameter_MV_Scale_Y,1.0f);
        p.Set(NVSDK_NGX_Parameter_Reset,reset?1u:0u);
        p.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width,frame.renderWidth);
        p.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height,frame.renderHeight);
        p.Set(NVSDK_NGX_Parameter_DLSS_Pre_Exposure,1.0f);p.Set(NVSDK_NGX_Parameter_Sharpness,0.0f);
        p.Set(NVSDK_NGX_Parameter_ExposureTexture,static_cast<ID3D11Resource*>(nullptr));
        const uint64_t id=requests.fetch_add(1,std::memory_order_relaxed)+1;
        neo::ngx::F6EvalReceiptV1 receipt;
        source_output::Scope sourceCall({reinterpret_cast<uintptr_t>(context.Get()),reinterpret_cast<uintptr_t>(&p),
            reinterpret_cast<uintptr_t>(frame.color),reinterpret_cast<uintptr_t>(frame.depth),
            reinterpret_cast<uintptr_t>(frame.motion),reinterpret_cast<uintptr_t>(frame.output),handle->Id,id});
        State::Instance().setInputApiName=ApiUpscalerInput::TAA_DX11;
        interop::BeforeSrInput input;
        input.query=interop::BeforeSrQuery::Matched;input.origin=interop::SrInputOrigin::Taa;
        input.parameters=&p;input.feature=handle->Id;input.request=id;
        auto& source=input.frame;
        source.context=context.Get();source.color=frame.color;source.depth=frame.depth;source.motion=frame.motion;
        source.frame=sourceFrame;source.instance=reinterpret_cast<uintptr_t>(this);source.generation=revision;
        source.render_width=frame.renderWidth;source.render_height=frame.renderHeight;
        source.display_width=frame.outputWidth;source.display_height=frame.outputHeight;
        source.color_storage_format=frame.colorStorage;source.color_view_format=frame.colorView;
        source.source_color=frame.sourceColor;source.source_storage_format=frame.sourceStorage;source.source_view_format=frame.sourceView;
        source.effect_domain=interop::EffectDomain::SourceValues; // no inferred transfer or exposure
        source.sr_hdr_declared=profile.hdrDeclared;
        source.guides_known=true;
        source.flags=(profile.depthInverted?interop::k_sr_depth_inverted:0) |
            (profile.motionJittered?interop::k_sr_motion_jittered:0) | (reset?interop::k_sr_reset:0);
        source.jitter_x=frame.jitterX;source.jitter_y=frame.jitterY;
        const auto result=NgxInput::EvaluateDx11Input(context.Get(),handle,&p,id,&receipt,input);
        const bool current=result==NVSDK_NGX_Result_Success &&
            neo::ngx::f6_output_status(receipt,id,handle,context.Get(),frame.output)==neo::ngx::F6OutputStatus::current_output_ordered;
        if(current) {
            lastEvaluatedSource=sourceFrame;
            if(reset||sourceFrame<=4||sourceFrame%300==0)
                LOG_INFO("TAA continuity: source_frame={} feature={} reset={} render={}x{} output={}x{} current=true",
                    sourceFrame,stableHandle.Id,reset,frame.renderWidth,frame.renderHeight,frame.outputWidth,frame.outputHeight);
        }
        return current;
    }
};
struct Registry {
    std::mutex mutex;
    Status status;
    std::filesystem::path base;
    ComPtr<ID3D11DeviceContext> owner;
    DWORD thread=0;
    uint64_t present=0;
    uint64_t appliedRevision=0,generation=0;
    std::optional<Profile> activeProfile;
    std::shared_ptr<Session> session;
    consumer::DeliveryPolicy delivery;
    std::vector<std::shared_ptr<Session>> retiring;
    std::set<std::pair<Stage,uint64_t>> evidenceQueued;
    struct EvidenceTrack { evidence::SampleBudget budget;bool temporal=false;uint64_t observedFrame=~uint64_t(0); };
    std::map<std::pair<Stage,uint64_t>,EvidenceTrack> temporalEvidence;
    std::set<uint64_t> vertexEvidence;
    size_t temporalEvidenceBytes=0;
    std::vector<evidence::RasterTarget> rasterTargets;
    evidence::CaptureWindow capture;
    struct ExtentTrack { uint64_t frame=~uint64_t(0),probeMs=0;bool confirm=false;unsigned width=0,height=0,pendingWidth=0,pendingHeight=0,stable=0; };
    std::map<std::pair<Stage,uint64_t>,ExtentTrack> extentTracks;
    uint64_t extentDeadlineMs=0;
    unsigned evidenceBurst=~0u,captureQueued=0,captureUnknown=0,captureRejected=0;
    bool captureFinished=false;
    void ResetEvidence() {
        temporalEvidence.clear();rasterTargets.clear();temporalEvidenceBytes=0;
        capture={};extentTracks.clear();extentDeadlineMs=0;evidenceBurst=~0u;captureQueued=captureUnknown=captureRejected=0;captureFinished=false;
    }
};
// Graphics roots are explicitly retired by their context events plus the
// existing provider's queue/fence shutdown. Process teardown cannot invent a
// completed GPU tail by running static COM destructors in DllMain.
Registry& StateOf() { static auto* value=new Registry;return *value; }
void CaptureTemporalEvidence(Registry& r,const std::shared_ptr<const Request>& wanted,ID3D11DeviceContext* context,CandidateKey key,const analysis::Result& analyzed,const Invocation& call) {
    // These hints request diagnostic CPU copies only. They grant no feature,
    // resource lifetime, motion-encoding or zero-jitter authority.
    if(analyzed.recipes.empty()&&!analyzed.observation&&!analyzed.vectorOutput)return;
    const auto now=GetTickCount64();
    // One colour-view probe per candidate/present also observes late render-scale
    // changes. Never compare resource IDs: ordinary ping-pong is not a resize.
    if(!r.extentDeadlineMs)r.extentDeadlineMs=now+300000;
    if(now<r.extentDeadlineMs &&
       r.capture.epoch<evidence::CaptureWindow::MaxRearms && r.temporalEvidenceBytes<8*1024*1024 &&
       (!analyzed.recipes.empty()||analyzed.observation)) {
        const auto id=std::pair{key.stage,key.hash};
        auto found=r.extentTracks.find(id);
        if(found==r.extentTracks.end()&&r.extentTracks.size()>=8) {
            const auto oldest=std::min_element(r.extentTracks.begin(),r.extentTracks.end(),
                [](const auto& a,const auto& b){return a.second.frame<b.second.frame;});
            if(oldest->second.probeMs+1500<now)r.extentTracks.erase(oldest);
        }
        if(found!=r.extentTracks.end()||r.extentTracks.size()<8) {
            auto& extent=r.extentTracks[id];
            if(extent.frame!=r.present && (!r.capture.Complete(now)||extent.confirm||now-extent.probeMs>=500)) {
                const auto previous=extent.frame;extent.frame=r.present;extent.probeMs=now;
                std::vector<unsigned> slots;
                for(const auto& recipe:analyzed.recipes)
                    if(std::find(slots.begin(),slots.end(),recipe.profile.colorSlot)==slots.end())slots.push_back(recipe.profile.colorSlot);
                if(analyzed.observation)for(const auto& role:analyzed.observation->roles)
                    if(std::string_view(role.name)=="color_hint")slots.push_back(role.slot);
                // Coordinate analysis may offer depth-as-colour alternatives.
                // Select a typed colour view, never the first recipe blindly.
                unsigned width=0,height=0;bool ambiguous=false;
                for(const unsigned slot:slots) {
                    ComPtr<ID3D11ShaderResourceView> view;
                    { DrawObservationDx11::InternalScope internal;
                      if(key.stage==Stage::Pixel)context->PSGetShaderResources(slot,1,view.GetAddressOf());
                      else context->CSGetShaderResources(slot,1,view.GetAddressOf()); }
                    ComPtr<ID3D11Resource> resource;ComPtr<ID3D11Texture2D> texture;
                    if(view)view->GetResource(&resource);
                    if(!resource||FAILED(resource.As(&texture)))continue;
                    D3D11_TEXTURE2D_DESC d{};D3D11_SHADER_RESOURCE_VIEW_DESC v{};
                    texture->GetDesc(&d);view->GetDesc(&v);
                    if(!automatic::Color(v.Format))continue;
                    const auto mip=v.ViewDimension==D3D11_SRV_DIMENSION_TEXTURE2D?v.Texture2D.MostDetailedMip:0;
                    const unsigned w=(std::max)(1u,d.Width>>(std::min)(mip,31u)),h=(std::max)(1u,d.Height>>(std::min)(mip,31u));
                    if(width&&(width!=w||height!=h))ambiguous=true;
                    width=w;height=h;
                }
                if(width&&!ambiguous) {
                    if(!extent.width){extent.width=width;extent.height=height;}
                    if(extent.pendingWidth==width&&extent.pendingHeight==height&&previous+1==r.present)++extent.stable;
                    else {extent.pendingWidth=width;extent.pendingHeight=height;extent.stable=1;}
                    extent.confirm=extent.width!=width||extent.height!=height;
                    if(extent.stable>=2&&(extent.width!=width||extent.height!=height)&&r.capture.Rearm(now)) {
                        extent.confirm=false;r.captureFinished=false;r.rasterTargets.clear();
                        LOG_INFO("TAA capture rearmed: epoch={} reason=input-extent candidate={} extent={}x{} bytes={} maximum_rearms={}",
                            r.capture.epoch,KeyText(key),width,height,r.temporalEvidenceBytes,evidence::CaptureWindow::MaxRearms);
                    }
                } else {
                    // Unknown/ambiguous colour views break consecutive-frame
                    // confirmation; they never validate an observed extent.
                    extent.pendingWidth=extent.pendingHeight=extent.stable=0;extent.confirm=false;
                }
            }
        }
    }
    const auto burst=r.capture.Active(now);
    if(!burst||r.temporalEvidenceBytes>=8*1024*1024)return;
    if(r.evidenceBurst!=*burst){r.temporalEvidence.clear();r.evidenceBurst=*burst;}
    const auto identity=std::pair{key.stage,key.hash};auto track=r.temporalEvidence.find(identity);
    const bool sampleComplete=track!=r.temporalEvidence.end()&&track->second.budget.samples>=evidence::CaptureWindow::Frames;
    if(r.temporalEvidenceBytes>=8*1024*1024)return;
    if(track==r.temporalEvidence.end()) {
        const bool temporal=!analyzed.recipes.empty()||analyzed.observation.has_value();
        if(!temporal&&std::count_if(r.temporalEvidence.begin(),r.temporalEvidence.end(),[](const auto& entry){return !entry.second.temporal;})>=4)return;
        if(r.temporalEvidence.size()>=8) {
            if(!temporal)return;
            const auto hint=std::find_if(r.temporalEvidence.begin(),r.temporalEvidence.end(),[](const auto& entry){return !entry.second.temporal;});
            if(hint==r.temporalEvidence.end())return;r.temporalEvidence.erase(hint);
        }
        track=r.temporalEvidence.emplace(identity,Registry::EvidenceTrack{}).first;
        track->second.temporal=temporal;
    }
    const bool seedThenJoined=analyzed.observation&&analyzed.observation->capture==analysis::CapturePolicy::SeedThenJoined;
    // Refresh discovery independently of export admission. Ordinary recipe
    // captures keep their existing full snapshots; unresolved observations
    // query only the declared role slots once per owner frame.
    if(seedThenJoined&&track->second.observedFrame==r.present)return;
    if(!seedThenJoined&&(sampleComplete||!track->second.budget.CanBegin(r.present)))return;
    DrawObservationDx11::InternalScope internal;
    evidence::Frame frame;
    if(seedThenJoined){track->second.observedFrame=r.present;frame.RoleBindings(context,key.stage,*analyzed.observation);}
    else frame.Bindings(context,key.stage);
    if(!analyzed.recipes.empty()||analyzed.observation) {
        const auto targets=evidence::Targets(frame,analyzed,key,r.present);
        if(!targets.empty()) {
                for(const auto& target:targets) {
                    const auto found=std::find_if(r.rasterTargets.begin(),r.rasterTargets.end(),[&](const auto& old){return old.resource==target.resource&&old.candidate==target.candidate&&std::string_view(old.role)==target.role;});
                    if(found!=r.rasterTargets.end())*found=target;
                    else {if(r.rasterTargets.size()>=48)r.rasterTargets.erase(std::min_element(r.rasterTargets.begin(),r.rasterTargets.end(),[](const auto& a,const auto& b){return a.sourceFrame<b.sourceFrame;}));r.rasterTargets.push_back(target);}
                }
                auto next=std::make_shared<RasterRequest>();next->wanted=wanted;
                next->watch={context,wanted->revision,r.rasterTargets,r.capture.startedMs,r.capture.epoch};rasterRequest.store(std::move(next),std::memory_order_release);
        }
    }
    std::string joined;
    if(seedThenJoined) {
        const auto* owner=DrawObservationDx11::owner?&*DrawObservationDx11::owner:nullptr;
        if(owner&&owner->Matches(context))
            joined=rasterMailbox.Join({wanted->revision,r.capture.startedMs,r.capture.epoch,*burst},evidence::OwnerIdentity::From(*owner),key,frame);
        // The first successful record establishes a watch. Afterwards only
        // same-owner, successfully queued raster records spend the old budget.
        if(track->second.budget.samples&&joined.empty())return;
    }
    if(!track->second.budget.Begin(r.present,seedThenJoined?analysis::CapturePolicy::SeedThenJoined:analysis::CapturePolicy::Consecutive,!joined.empty()))return;
    if(seedThenJoined){frame={};frame.Bindings(context,key.stage);}
    frame.metadata=std::format("schema=2\npid={}\ncandidate={}\nsource_frame={}\nsample={}\nkind={}\nphase=before_original\nconstant_source=observed_cpu_writes_only\n",
        GetCurrentProcessId(),KeyText(key),r.present,track->second.budget.samples+1,analyzed.observation?"unresolved_temporal_observation":analyzed.recipes.empty()?"vector_output_hint":"temporal_reprojection")+frame.metadata;
    frame.metadata+=std::format("config_revision={} context_id={} thread_id={} replacement_authority=false\n",
        wanted->revision,reinterpret_cast<uintptr_t>(context),GetCurrentThreadId());
    frame.metadata+=std::format("invocation direct={} count={} instances={} groups={},{},{}\n",call.direct,call.count,call.instances,call.groupsX,call.groupsY,call.groupsZ);
    if(analyzed.observation)frame.metadata+=std::format("observation_contract={}\nreplacement_authority=false\n",analyzed.observation->name);
    frame.metadata+=std::format("capture_burst={} capture_elapsed_ms={} capture_epoch={} capture_reason={}\n",*burst,now-r.capture.startedMs,r.capture.epoch,r.capture.epoch?"input-extent":"initial");
    if(const auto owner=DrawObservationDx11::owner;owner&&owner->Matches(context))
        frame.metadata+=std::format("owner_frame={}\nowner_epoch={}\n",owner->frame,owner->epoch);
    if(seedThenJoined)frame.metadata+=std::format("capture_selection={}\n{}",joined.empty()?"seed":"same_owner_joined",joined);
    frame.Constants(context,key.stage==Stage::Pixel?evidence::ShaderStage::Pixel:evidence::ShaderStage::Compute,analyzed);
    if(key.stage==Stage::Pixel) {
        ComPtr<ID3D11VertexShader> vertex;context->VSGetShader(&vertex,nullptr,nullptr);
        const auto vertexIdentity=DrawObservationDx11::Identity(vertex.Get());
        frame.metadata+=std::format("vertex_shader={:016X}\n",vertexIdentity.hash);
        if(const auto program=automatic::Find(vertex.Get())) {
            const auto& description=program->Analyze(Stage::Pixel,vertexIdentity.hash,[&](auto&& bytes,auto&& assembly,const analysis::Result& result) {
                if(r.vertexEvidence.contains(vertexIdentity.hash))return true;
                if(r.vertexEvidence.size()>=8)return false;
                if(!QueueShaderEvidence(*wanted,{Stage::Pixel,vertexIdentity.hash},r.present,"companion vertex shader",std::move(bytes),std::move(assembly),result,"VS"))return false;
                r.vertexEvidence.emplace(vertexIdentity.hash);return true;
            },{wanted->revision,*burst});
            frame.Constants(context,evidence::ShaderStage::Vertex,description);
        } else frame.metadata+="vertex_bytecode=unavailable\n";
    }
    const bool known=frame.knownBytes!=0;
    if(!known)++r.captureUnknown;
    if(!track->second.budget.Wants(known,!joined.empty()))return;
    const auto size=frame.buffers.size()+frame.metadata.size();
    if(size>8*1024*1024-r.temporalEvidenceBytes)return;
    std::vector<EvidencePart> parts;parts.push_back({L".constants.bin",std::move(frame.buffers)});
    if(QueueEvidence(*wanted,std::format("temporal/{}-{:016X}-{:08}",key.stage==Stage::Pixel?"PS":"CS",key.hash,r.present),std::move(parts),std::move(frame.metadata)))
        {
            r.temporalEvidenceBytes+=size;track->second.budget.Queued(known);++r.captureQueued;
            // A successful capture, not merely an active wall-clock window,
            // confirms this extent. A 34s change with no burst still rearms later.
            const auto extent=r.extentTracks.find(identity);
            if(extent!=r.extentTracks.end()) {
                extent->second.width=extent->second.pendingWidth;extent->second.height=extent->second.pendingHeight;
                extent->second.confirm=false;
            }
        }
    else ++r.captureRejected;
}
void Detail(Registry& r,const std::string& text) {
    if (r.status.detail==text) return;
    r.status.detail=text;LOG_INFO("TAA input: {}",text);
}
std::string Describe(ID3D11DeviceContext* context,Stage stage) {
    std::string text;
    std::array<ID3D11ShaderResourceView*,16> views{};
    if(stage==Stage::Pixel)context->PSGetShaderResources(0,UINT(views.size()),views.data());
    else context->CSGetShaderResources(0,UINT(views.size()),views.data());
    for (unsigned i=0;i<16;++i) {
        ComPtr<ID3D11ShaderResourceView> view;view.Attach(views[i]);
        if (!view) continue;
        ComPtr<ID3D11Resource> resource;view->GetResource(&resource);ComPtr<ID3D11Texture2D> texture;
        if (FAILED(resource.As(&texture))) continue;
        D3D11_TEXTURE2D_DESC d{};D3D11_SHADER_RESOURCE_VIEW_DESC v{};texture->GetDesc(&d);view->GetDesc(&v);
        text+=std::format("t{}={}x{}:{} ",i,d.Width,d.Height,unsigned(v.Format));
    }
    return text;
}
void Retire(Registry& r) {
    for (auto it=r.retiring.begin();it!=r.retiring.end();) {
        if ((*it)->Release()) it=r.retiring.erase(it);else ++it;
    }
}
} // namespace

void Configure(Mode mode,const std::string& path,const std::filesystem::path& base,std::string_view candidate) {
    auto next=std::make_shared<Request>();next->mode=mode;next->revision=++revisions;
    auto& r=StateOf();std::lock_guard lock(r.mutex);
    if (!base.empty()) r.base=base;
    if (r.base.empty()) r.base=Util::DllPath().parent_path();
    next->evidenceDirectory=std::filesystem::absolute(r.base)/
        L"taa-shaders"/std::to_wstring(GetCurrentProcessId());
    r.status.evidenceDirectory=next->evidenceDirectory.string();
    next->path=path;
    const auto key=ParseKey(candidate);
    if(key)next->selected=*key;else next->error="Invalid TAA candidate selection";
    if (!path.empty()) {
        auto file=std::filesystem::path(std::u8string(path.begin(),path.end()));if (file.is_relative()) file=r.base/file;
        CSimpleIniA ini;ini.SetUnicode();
        if (ini.LoadFile(file.c_str())<0) next->error="Cannot read TAA profile";
        else {auto parsed=ReadProfile(ini);next->profile=std::move(parsed.profile);next->error=std::move(parsed.error);}
    }
    if(next->profile&&next->selected.hash&&next->selected!=CandidateKey{next->profile->stage,next->profile->shaderHash})
        next->error="The selected TAA candidate conflicts with the explicit profile";
    // Preserve the observed list across UI changes. A stable stage/hash pin
    // survives sorting; a list index or a borrowed shader pointer cannot.
    r.status.requested=mode;r.status.profilePath=path;r.status.profileError=next->error;
    r.status.selected=next->selected;r.status.detail.clear();
    r.ResetEvidence();
    rasterRequest.store({},std::memory_order_release);
    DrawObservationDx11::Subscribe(DrawObservationDx11::Slot::TaaEvidence,&RasterConsumer,mode!=Mode::Off);
    DrawObservationDx11::Subscribe(DrawObservationDx11::Slot::TaaJitter,&ProjectionConsumer,mode!=Mode::Off&&path.empty());
    constants::InvalidateConstants();ownsInput=false;source_output::Reset();
    request.store(std::move(next),std::memory_order_release);
    needsHooks.store(mode!=Mode::Off,std::memory_order_release);
    LOG_INFO("TAA configuration: mode={} candidate={} profile={} error={} evidence={}",ModeName(mode),KeyText(r.status.selected),path,r.status.profileError,r.status.evidenceDirectory);
}
void RememberShader(ID3D11DeviceChild* shader,const void* bytes,size_t size) noexcept {automatic::Remember(shader,bytes,size);}
void DepthCleared(ID3D11DeviceContext* context,ID3D11DepthStencilView* view,UINT flags,float depth) noexcept {
    if(NeedsHooks()&&!DrawObservationDx11::Internal())automatic::DepthCleared(context,view,flags,depth);
}
bool NeedsHooks() noexcept {return needsHooks.load(std::memory_order_acquire);}
Status ReadStatus() {auto& r=StateOf();std::lock_guard lock(r.mutex);return r.status;}
void OnPresent(ID3D11DeviceContext* context,bool writesCovered) noexcept {
    try {
        const auto wanted=request.load(std::memory_order_acquire);if (!wanted) return;
        auto& r=StateOf();std::lock_guard lock(r.mutex);
        DrawObservationDx11::InternalScope internal;
        const bool ownerChanged=r.owner&&r.owner.Get()!=context;
        if(ownerChanged){r.status.candidates.clear();r.ResetEvidence();rasterRequest.store({},std::memory_order_release);}
        std::optional<Profile> selected;
        if(wanted->mode!=Mode::Off&&wanted->error.empty()) {
            if(wanted->profile) {
                const CandidateKey key{wanted->profile->stage,wanted->profile->shaderHash};
                if(!wanted->selected.hash||wanted->selected==key)selected=wanted->profile;
            } else if(const auto key=ChooseCandidate(r.status.candidates,r.present,wanted->selected,r.status.active)) {
                const auto it=std::find_if(r.status.candidates.begin(),r.status.candidates.end(),[&](const Candidate& c){return c.Key()==*key;});
                if(it!=r.status.candidates.end())selected=it->inferred;
            }
        }
        const bool configurationChanged=ownerChanged||r.appliedRevision!=wanted->revision;
        const bool flagsChanged=r.session&&r.session->handle&&selected&&
            r.session->featureFlags!=Session::CreateFlags(*selected);
        const bool adapterChanged=r.activeProfile!=selected;
        if(r.session)r.session->CancelPending("next Present without delivery");
        if(configurationChanged)r.delivery.Revoke();
        if(r.session&&(configurationChanged||flagsChanged||wanted->mode!=Mode::Replace))
            r.retiring.push_back(std::move(r.session));
        if(configurationChanged||flagsChanged)++r.generation;
        // Candidate availability controls this frame only. Shader/CB adapters
        // do not own the stable NGX feature or its proven output extent.
        r.activeProfile=selected;r.appliedRevision=wanted->revision;
        r.status.active=selected?CandidateKey{selected->stage,selected->shaderHash}:CandidateKey{};
        if(adapterChanged&&selected)LOG_INFO("TAA selection: candidate={} source={} generation={} feature_preserved={}",
            KeyText(r.status.active),wanted->profile?"profile":"automatic",r.generation,bool(r.session&&r.session->handle));
        ++r.present;
        Retire(r);
        if (wanted->mode==Mode::Off) {r.owner.Reset();r.status.ownerReady=false;ownsInput=false;rasterRequest.store({},std::memory_order_release);return;}
        if(r.capture.Complete(GetTickCount64())) {
            rasterRequest.store({},std::memory_order_release);
            if(!r.captureFinished){r.captureFinished=true;LOG_INFO("TAA capture window complete: epoch={} queued={} unknown={} queue_rejected={} bytes={} duration_ms={}",r.capture.epoch,r.captureQueued,r.captureUnknown,r.captureRejected,r.temporalEvidenceBytes,r.capture.epoch?evidence::CaptureWindow::RearmDuration:evidence::CaptureWindow::Duration);}
        }
        r.owner=context;r.thread=GetCurrentThreadId();r.status.ownerReady=context && writesCovered;
        const bool owns=wanted->mode==Mode::Replace&&r.activeProfile&&r.status.ownerReady&&!native::GameUpscalerCalledRecently();
        ownsInput.store(owns,std::memory_order_release);
        if (!writesCovered && wanted->mode!=Mode::Off) Detail(r,"D3D11 constant-buffer write hooks are unavailable");
        else if(!selected&&wanted->error.empty()) Detail(r,r.status.candidates.empty()
            ?"Waiting for TAA candidates":"Candidates observed; no replaceable temporal input contract yet");
    } catch (...) {ownsInput=false;}
}
struct Scope::Transaction {
    Registry& registry;
    std::unique_lock<std::mutex> lock;
    std::shared_ptr<Session> session;
    std::shared_ptr<const Request> wanted;
    Profile profile;
    PassDx11* pass=nullptr;
    Transaction(Registry& r) : registry(r),lock(r.mutex) {}
    ~Transaction() {if (pass) {DrawObservationDx11::InternalScope internal;pass->Abandon();}}
};
Scope::Scope(ID3D11DeviceContext* context,const Invocation& call,bool eligible) noexcept {
    if (!eligible || !NeedsHooks() || DrawObservationDx11::Internal()) return;
    entered_=true;outer_=nesting++==0;
    if (!outer_) return;
    try {
        const auto wanted=request.load(std::memory_order_acquire);if (!wanted || wanted->mode==Mode::Off) return;
        ComPtr<ID3D11DeviceChild> shader;
        if (call.stage==Stage::Pixel) {ComPtr<ID3D11PixelShader> ps;context->PSGetShader(&ps,nullptr,nullptr);shader=ps;}
        else {ComPtr<ID3D11ComputeShader> cs;context->CSGetShader(&cs,nullptr,nullptr);shader=cs;}
        const auto identity=DrawObservationDx11::Identity(shader.Get());if (!identity.hash) return;
        auto tx=std::make_unique<Transaction>(StateOf());auto& r=tx->registry;
        if (r.owner.Get()!=context || r.thread!=GetCurrentThreadId() || !r.status.ownerReady || r.appliedRevision!=wanted->revision) return;
        const CandidateKey key{call.stage,identity.hash};
        const bool matches=r.activeProfile&&r.activeProfile->stage==call.stage&&r.activeProfile->shaderHash==identity.hash;
        const bool candidate=call.stage==Stage::Compute || (call.direct && call.instances==1 && (call.count==3 || call.count==4 || call.count==6));
        if (!matches && !candidate) return;
        auto it=std::find_if(r.status.candidates.begin(),r.status.candidates.end(),[&](const Candidate& c){return c.Key()==key;});
        if(it==r.status.candidates.end()&&r.status.candidates.size()>=256) {
            auto victim=r.status.candidates.end();
            for(auto entry=r.status.candidates.begin();entry!=r.status.candidates.end();++entry) {
                if(entry->Key()==r.status.selected||entry->Key()==r.status.active)continue;
                if(entry->inferred&&entry->lastSeen+2>=r.present)continue;
                if(victim==r.status.candidates.end()||entry->lastSeen<victim->lastSeen)victim=entry;
            }
            if(victim!=r.status.candidates.end())r.status.candidates.erase(victim);
            it=r.status.candidates.end();
        }
        if(it==r.status.candidates.end()&&r.status.candidates.size()<256) {
            Candidate row;row.hash=identity.hash;row.stage=call.stage;row.bindings=Describe(context,call.stage);
            r.status.candidates.push_back(std::move(row));it=std::prev(r.status.candidates.end());
            LOG_INFO("TAA candidate (not verified TAA): stage={} fnv64={:016X} bytes={} {}",call.stage==Stage::Pixel?"pixel":"compute",identity.hash,identity.bytes,it->bindings);
        }
        if(it!=r.status.candidates.end()) {
            ++it->calls;it->frameCalls=it->lastSeen==r.present?it->frameCalls+1:1;it->lastSeen=r.present;
            std::string instruction;
            if(wanted->profile) {it->reason="Explicit TAA profile";it->inferred=wanted->profile->shaderHash==identity.hash?wanted->profile:std::nullopt;}
            else if(const auto program=automatic::Find(shader.Get())) {
                const auto& analyzed=program->Analyze(call.stage,identity.hash,[&](auto&& bytes,auto&& assembly,const analysis::Result& result) {
                    if(r.evidenceQueued.contains({key.stage,key.hash}))return true;
                    if(r.evidenceQueued.size()>=256)return false;
                    if(!QueueShaderEvidence(*wanted,key,r.present,it->bindings,std::move(bytes),std::move(assembly),result))return false;
                    r.evidenceQueued.emplace(key.stage,key.hash);return true;
                },{wanted->revision,r.capture.startedMs?r.capture.Burst(GetTickCount64()):0u});
                instruction=analyzed.unsupportedInstruction;
                CaptureTemporalEvidence(r,wanted,context,key,analyzed,call);
                if(!analyzed.recipes.empty()) {
                    const jitter::Input jitter{projectionJitter.Current(),wanted->revision};
                    auto resolved=automatic::Resolve(context,call.stage,analyzed,jitter);
                    if(resolved.profile) {
                        ComPtr<ID3D11Device> device;context->GetDevice(&device);PassDx11 probe(device.Get(),context);
                        if(!probe.Probe(*resolved.profile,call,jitter)){resolved.profile.reset();resolved.reason=probe.Error();}
                    }
                    it->inferred=resolved.profile;it->width=resolved.width;it->height=resolved.height;
                    it->bindings=std::move(resolved.bindings);
                    it->reason=std::move(resolved.reason);
                } else {it->inferred.reset();it->reason=analyzed.reason;}
            } else {it->inferred.reset();it->reason="Shader bytecode is unavailable; recreate the shader to analyze it";}
            if(it->AnalysisDue(r.present)) {
                LOG_INFO("TAA analysis: candidate={} ready={} reason={} unsupported_instruction={}",KeyText(key),bool(it->inferred),it->reason,instruction);
                it->RecordAnalysis(r.present);
            }
        }
        if (!matches || wanted->mode!=Mode::Replace) return;
        if(!wanted->profile&&(it==r.status.candidates.end()||it->inferred!=r.activeProfile)) {
            Detail(r,"Current TAA candidate analysis is unavailable or changed; keeping the original result");return;
        }
        ++r.status.matches;
        if (native::GameUpscalerCalledRecently()) {Detail(r,"The game is already calling an upscaler");return;}
        Retire(r);
        // Do not run a new provider while an old provider's real tail retires.
        if (!r.retiring.empty()) {Detail(r,"Waiting for the previous TAA provider's GPU tail");return;}
        if (!r.session) r.session=std::make_shared<Session>(context,r.generation);
        auto& session=*r.session;
        if (session.revision!=r.generation || session.lastAttempt==r.present) return;
        session.lastAttempt=r.present;
        DrawObservationDx11::InternalScope internal;
        for (unsigned index=0;index<session.passes.size();++index) {
            auto& slot=session.passes[index];
            if (!slot) slot=std::make_unique<PassDx11>(session.device.Get(),context);
            if (!slot->Idle() || (session.chains[index]&&!session.chains[index]->Idle())) continue;
            if (!slot->Begin(*r.activeProfile,call,!session.lastEvaluatedSource || session.lastEvaluatedSource+1!=r.present,{projectionJitter.Current(),wanted->revision})) {
                Detail(r,slot->Error());return;
            }
            tx->pass=slot.get();tx->session=r.session;tx->wanted=wanted;tx->profile=*r.activeProfile;transaction_=std::move(tx);return;
        }
        Detail(r,"TAA inputs are still in GPU use; keeping the original TAA result");
    } catch (...) {}
}
Scope::~Scope() {transaction_.reset();if (entered_) --nesting;}
void Scope::Returned() noexcept {
    if (!transaction_) return;
    auto& tx=*transaction_;auto& r=tx.registry;
    try {
        DrawObservationDx11::InternalScope internal;
        const auto& frozen=tx.pass->FrozenFrame();
        const bool observe=tx.profile.outputRoute==OutputRoute::ObserveUpscale &&
            frozen.renderWidth==frozen.outputWidth&&frozen.renderHeight==frozen.outputHeight;
        const auto trackConsumer=[&](bool observation) {
            unsigned index=0;while(index<tx.session->passes.size()&&tx.session->passes[index].get()!=tx.pass)++index;
            if(index==tx.session->passes.size())return false;
            if(!tx.session->chains[index])tx.session->chains[index]=std::make_unique<consumer::Chain>(tx.session->context.Get());
            tx.session->chains[index]->Reset(tx.pass->OriginalTarget(),observation);
            tx.session->pending=Session::Pending{index,r.present,tx.wanted->revision,tx.profile,observation};
            consumerPending.store(true,std::memory_order_release);return true;
        };
        // Keep evaluating current valid inputs with the stable proven layout.
        // Binding remains a separate transaction: even a known extent cannot
        // authorize this frame's consumer or reuse an earlier frame's pixels.
        if(observe&&r.delivery.Defer(tx.profile.outputRoute,frozen.renderWidth,frozen.renderHeight)&&trackConsumer(false)) {
            D3D11_TEXTURE2D_DESC output{};tx.pass->CurrentOutput()->GetDesc(&output);
            if(!tx.session->PrepareLayout(frozen.renderWidth,frozen.renderHeight,r.delivery.OutputWidth(),r.delivery.OutputHeight(),output.Format)) {
                tx.session->CancelPending("provider layout rebuild");++r.generation;r.retiring.push_back(std::move(r.session));
                transaction_.reset();return;
            }
            ++r.status.evaluations;
            const bool current=tx.pass->Resolve(r.delivery.OutputWidth(),r.delivery.OutputHeight(),
                [&](const Frame& frame){return tx.session->Evaluate(frame,tx.profile,r.present);},false);
            tx.session->pending->evaluated=current;
            if(current)tx.pass=nullptr;
            else {Detail(r,tx.pass->Error());tx.session->CancelPending("full-resolution evaluation failed");}
            if(tx.session->rebuild&&r.session==tx.session){++r.generation;r.retiring.push_back(std::move(r.session));}
            transaction_.reset();return;
        }
        ++r.status.evaluations;
        const bool ran=tx.pass->Finish([&](const Frame& frame){return tx.session->Evaluate(frame,tx.profile,r.present);});
        if (ran) {++r.status.outputs;tx.session->lastDeliveredSource=r.present;Detail(r,"Current TAA inputs evaluated and copied back");if(observe)trackConsumer(true);}
        else Detail(r,tx.pass->Error());
        if (tx.session->rebuild && r.session==tx.session) {++r.generation;r.retiring.push_back(std::move(r.session));}
        if (r.status.evaluations<=4 || r.status.evaluations%300==0)
            LOG_INFO("TAA evaluate: matches={} attempts={} current_outputs={} copied_back={} profile={}",r.status.matches,r.status.evaluations,r.status.outputs,ran,tx.wanted->path);
    } catch (...) {DrawObservationDx11::InternalScope internal;tx.pass->Abandon();Detail(r,"TAA evaluation failed; keeping the original result");}
    transaction_.reset();
}
struct ConsumerScope::Transaction {
    Registry& registry;std::unique_lock<std::mutex> lock;
    std::shared_ptr<Session> session;consumer::Chain* chain=nullptr;bool resolving=false,returned=false;
    explicit Transaction(Registry& r):registry(r),lock(r.mutex){}
    ~Transaction(){if(chain&&!returned){DrawObservationDx11::InternalScope internal;chain->AbandonBinding();if(session)session->CancelPending("physical consumer abandoned");}}
};
ConsumerScope::ConsumerScope(ID3D11DeviceContext* context,const ConsumerCommand& command,bool eligible) noexcept {
    if(!eligible||DrawObservationDx11::Internal()||!consumerPending.load(std::memory_order_acquire))return;
    try {
        auto tx=std::make_unique<Transaction>(StateOf());auto& r=tx->registry;
        if(!r.session||!r.session->pending)return;
        const auto wanted=request.load(std::memory_order_acquire);auto& session=*r.session;const auto pending=*session.pending;
        if(!wanted||wanted->revision!=pending.revision||wanted->mode!=Mode::Replace||r.owner.Get()!=context||r.thread!=GetCurrentThreadId()||pending.frame!=r.present)return;
        DrawObservationDx11::InternalScope internal;
        const auto ps=[&]{ComPtr<ID3D11PixelShader> shader;context->PSGetShader(&shader,nullptr,nullptr);return automatic::Find(shader.Get());}();
        const auto vs=[&]{ComPtr<ID3D11VertexShader> shader;context->VSGetShader(&shader,nullptr,nullptr);return automatic::Find(shader.Get());}();
        auto& chain=*session.chains[pending.slot];const auto& frozen=session.passes[pending.slot]->FrozenFrame();
        const bool accepted=chain.Before({command.indexed,command.count,command.first,command.instances,command.firstInstance,command.base},
            ps?ps->postPixel:consumer::Program{},vs?vs->postVertex:consumer::Program{},frozen.renderWidth,frozen.renderHeight);
        if(chain.LinkedThisDraw() && session.consumerEvidence.size()<16 && session.consumerEvidenceAttempts<32 && r.temporalEvidenceBytes<8*1024*1024) {
            const auto& node=chain.Activity().current;
            const auto key=std::tuple{node.pixel,node.vertex,chain.Error()};
            if(std::find(session.consumerEvidence.begin(),session.consumerEvidence.end(),key)==session.consumerEvidence.end()) {
                ++session.consumerEvidenceAttempts;
                evidence::Frame frame;frame.Bindings(context,Stage::Pixel);
                frame.metadata=std::format("schema=3\npid={} source_frame={} revision={} kind=consumer_link phase=before_original replacement_authority=false\n"
                    "draw={} parent={} accepted={} refusal={} pixel={:016X} vertex={:016X} input={} output={} extent={}x{}\n"
                    "indexed={} count={} first={} instances={} first_instance={} base={}\n",
                    GetCurrentProcessId(),r.present,pending.revision,node.ordinal,node.parent,accepted,chain.Error(),
                    node.pixel,node.vertex,node.input,node.output,node.width,node.height,
                    command.indexed,command.count,command.first,command.instances,command.firstInstance,command.base)+frame.metadata;
                const auto capture=[&](const ComPtr<automatic::Program>& program,const char* stage,uint64_t hash,evidence::ShaderStage shaderStage) {
                    if(!program)return;
                    const auto& analyzed=program->Analyze(Stage::Pixel,hash,[&](auto&& bytes,auto&& assembly,const analysis::Result& result) {
                        return QueueShaderEvidence(*wanted,{Stage::Pixel,hash},r.present,frame.metadata,
                            std::move(bytes),std::move(assembly),result,stage);
                    },{pending.revision,1});
                    frame.Constants(context,shaderStage,analyzed);
                };
                capture(ps,"PS",node.pixel,evidence::ShaderStage::Pixel);
                capture(vs,"VS",node.vertex,evidence::ShaderStage::Vertex);
                const auto bytes=frame.buffers.size();std::vector<EvidencePart> parts;
                parts.push_back({L".constants.bin",std::move(frame.buffers)});
                if(QueueEvidence(*wanted,std::format("consumer/PS-{:016X}-VS-{:016X}-{:08}-{:02}",node.pixel,node.vertex,r.present,session.consumerEvidence.size()),std::move(parts),std::move(frame.metadata))) {
                    session.consumerEvidence.push_back(key);r.temporalEvidenceBytes+=bytes;
                }
            }
        }
        if(!accepted)return;
        tx->session=r.session;tx->chain=&chain;
        if(chain.NeedsResolve()&&!pending.observing) {
            // The current physical edge may reveal a real output resize. Learn
            // that extent before retirement so the next transaction cannot
            // repeatedly rebuild the old layout. No pixels bind in this frame.
            r.delivery.Observe(frozen.renderWidth,frozen.renderHeight,chain.Width(),chain.Height());
            // Keep the old provider output alive until its own real tail retires.
            // Resolve may replace that texture, so classify layout first.
            D3D11_TEXTURE2D_DESC output{};session.passes[pending.slot]->CurrentOutput()->GetDesc(&output);
            if(!session.PrepareLayout(frozen.renderWidth,frozen.renderHeight,chain.Width(),chain.Height(),output.Format)) {
                session.CancelPending("provider layout rebuild");tx->chain=nullptr;
                ++r.generation;r.retiring.push_back(std::move(r.session));return;
            }
            tx->resolving=true;
            bool current=pending.evaluated;
            if(!current) {
                ++r.status.evaluations;
                current=session.passes[pending.slot]->Resolve(chain.Width(),chain.Height(),
                    [&](const Frame& frame){return session.Evaluate(frame,pending.profile,r.present);},false);
            }
            if(!current||!chain.Bind(session.passes[pending.slot]->CurrentOutput())) {
                Detail(r,current?chain.Error():session.passes[pending.slot]->Error());session.CancelPending(session.rebuild?"provider layout rebuild":"resolve or binding failed");tx->chain=nullptr;
                if(session.rebuild&&r.session==tx->session){++r.generation;r.retiring.push_back(std::move(r.session));}return;
            }
        }
        transaction_=std::move(tx);
    }catch(...){}
}
ConsumerScope::~ConsumerScope(){transaction_.reset();}
void ConsumerScope::Returned() noexcept {
    if(!transaction_)return;
    auto& tx=*transaction_;auto& r=tx.registry;
    try {
        DrawObservationDx11::InternalScope internal;
        const auto pending=*tx.session->pending;
        const bool observed=pending.observing&&tx.chain->NeedsResolve();
        const bool delivered=tx.chain->Returned();tx.returned=true;
        if(observed) {
            const auto& frame=tx.session->passes[pending.slot]->FrozenFrame();
            if(r.delivery.Observe(frame.renderWidth,frame.renderHeight,tx.chain->Width(),tx.chain->Height()))
                LOG_INFO("TAA consumer route observed: render={}x{} output={}x{} owner_frame={} scheduling_only=true",frame.renderWidth,frame.renderHeight,tx.chain->Width(),tx.chain->Height(),r.present);
            tx.session->CancelPending("consumer route observed",true);
        }
        if(delivered) {
            ++r.status.outputs;tx.session->lastDeliveredSource=r.present;
            const auto& frame=tx.session->passes[pending.slot]->FrozenFrame();
            if(r.status.outputs<=4||r.status.outputs%300==0)
                LOG_INFO("TAA consumer delivered: render={}x{} output={}x{} replay_nodes={} attempts={} current_outputs={} owner_frame={} physical_returned=true",
                    frame.renderWidth,frame.renderHeight,frame.outputWidth,frame.outputHeight,tx.chain->ReplayCount(),r.status.evaluations,r.status.outputs,r.present);
            Detail(r,"Current full-resolution SR output consumed by the game's original compositor");
            tx.session->CancelPending("output delivered",true);
        }
    }catch(...){DrawObservationDx11::InternalScope internal;tx.session->CancelPending("consumer return failed");tx.returned=true;}
    transaction_.reset();
}
void ConsumerWrite(ID3D11DeviceContext* context,ID3D11Resource* resource,std::string_view reason) noexcept {
    if(DrawObservationDx11::Internal()||!consumerPending.load(std::memory_order_acquire))return;
    try {auto& r=StateOf();std::lock_guard lock(r.mutex);if(!r.session||!r.session->pending||r.owner.Get()!=context||r.thread!=GetCurrentThreadId())return;
        DrawObservationDx11::InternalScope internal;
        if(resource)r.session->chains[r.session->pending->slot]->Invalidate(resource);else {r.session->CancelPending(reason); }
    }catch(...){}
}
void ConsumerDispatch(ID3D11DeviceContext* context) noexcept {
    if(DrawObservationDx11::Internal()||!consumerPending.load(std::memory_order_acquire))return;
    try {auto& r=StateOf();std::lock_guard lock(r.mutex);
        if(!r.session||!r.session->pending||r.owner.Get()!=context||r.thread!=GetCurrentThreadId())return;
        DrawObservationDx11::InternalScope internal;
        ComPtr<ID3D11Device> device;context->GetDevice(&device);
        const UINT count=device->GetFeatureLevel()>=D3D_FEATURE_LEVEL_11_1?D3D11_1_UAV_SLOT_COUNT:D3D11_PS_CS_UAV_REGISTER_COUNT;
        std::array<ID3D11UnorderedAccessView*,D3D11_1_UAV_SLOT_COUNT> raw{};
        context->CSGetUnorderedAccessViews(0,count,raw.data());
        for(UINT i=0;i<count;++i)if(raw[i]){ComPtr<ID3D11UnorderedAccessView> view;view.Attach(raw[i]);ComPtr<ID3D11Resource> resource;view->GetResource(&resource);r.session->chains[r.session->pending->slot]->Invalidate(resource.Get());}
    }catch(...){ConsumerWrite(context,nullptr,"compute write capture failed");}
}
void RevokeOwner() noexcept {
    try {auto& r=StateOf();std::lock_guard lock(r.mutex);DrawObservationDx11::InternalScope internal;
        if(r.session)r.session->CancelPending("source Present failed");
        // Keep the last context identity: recovery on a different context must
        // retire its old Session through the ordinary GPU completion path.
        r.delivery.Revoke();r.status.ownerReady=false;ownsInput=false;
        Detail(r,"Source Present failed; current TAA observation owner revoked");
    }catch(...) {ownsInput=false;}
}
void ConsumerCopy(ID3D11DeviceContext* context,ID3D11Resource* destination,ID3D11Resource* source,bool full) noexcept {
    if(DrawObservationDx11::Internal()||!consumerPending.load(std::memory_order_acquire))return;
    try {auto& r=StateOf();std::lock_guard lock(r.mutex);if(!r.session||!r.session->pending||r.owner.Get()!=context||r.thread!=GetCurrentThreadId())return;
        DrawObservationDx11::InternalScope internal;r.session->chains[r.session->pending->slot]->Copy(destination,source,full);
    }catch(...){}
}
} // namespace taa

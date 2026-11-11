// SPDX-License-Identifier: GPL-3.0-or-later
#include "GenshinFsrDx11.h"
#include <genshin/GenshinCpuStageTiming.h>
#include <genshin/GenshinFsrSubstageProgress.h>
#include <genshin/GenshinProducerTransport.h>
#include "FSR2_Dx11_Internal.h"
#include <Config.h>
#include <atomic>
#include <array>
#include <d3d11_1.h>
#include <wrl/client.h>
#include <vector>
#include <list>
#include <mutex>

namespace GenshinFsr
{
using Microsoft::WRL::ComPtr;
// R453: a null invocation is a native-only capability, not a DX12 lease.
// The sealed NVNGX_DLSS_Dx11 Evaluate gate requires BeginObservationTransport
// before EVERY IsWithDx12 Evaluate; that method requires the same producer and
// guides. Without them, DX12 Evaluate is rejected before any recording. Native
// D3D11 Evaluate remains available without guides on this immediate stream.
enum class InvocationOrdering { Unproven, NativeImmediate, AuthenticatedTransport };
struct InvocationProof {
    InvocationOrdering ordering=InvocationOrdering::Unproven;
    uintptr_t device=0,context=0;
    DWORD thread=0;
    Parameters parameters{};
    ResourceSet identities{};
    std::shared_ptr<const GenshinGuides::Receipt> guides;
    std::shared_ptr<GenshinTransport::ProducerFrame> transport;
};
static bool MatchingInvocation(const InvocationProof& proof, uintptr_t device, uintptr_t context,
                               DWORD thread, bool requireLive) noexcept
{
    if(!device || !context || !thread || proof.device!=device || proof.context!=context ||
       proof.thread!=thread)return false;
    // This is an equally strong no-cross-API-work proof ONLY for the sealed
    // authenticated dispatch above. It is not a general public NGX assumption.
    if(!proof.transport)return true;
    if(!proof.guides || proof.transport->Guides()!=proof.guides ||
       (requireLive && !proof.transport->Live()))return false;
    const auto& o=proof.transport->Owner();const auto& p=proof.parameters;
    if(!GenshinGuides::SameOwner(o,proof.guides->Get().owner) ||
       o.device!=device || o.context!=context || o.thread!=thread ||
       !o.deviceEpoch || !o.producerEpoch || !o.configEpoch ||
       o.instance!=p.instance || o.historyEpoch!=p.epoch || o.generation!=p.generation ||
       o.sourceFrame!=p.sourceFrame)return false;
    const auto& textures=proof.guides->Get().textures;const auto& r=proof.identities;
    return textures[0].identity==r.color.identity && textures[1].identity==r.depth.identity &&
           textures[2].identity==r.motion.identity && textures[3].identity==r.output.identity;
}
static bool RetainedInvocationOrdered(const InvocationProof& prior, const Parameters& current,
                                      uintptr_t device, uintptr_t context, DWORD thread) noexcept
{
    if(prior.parameters.instance!=current.instance || prior.parameters.epoch!=current.epoch ||
       !prior.parameters.generation || prior.parameters.generation>current.generation)return false;
    if(prior.ordering==InvocationOrdering::NativeImmediate)
        return !prior.transport && MatchingInvocation(prior,device,context,thread,false);
    // Retiring/revoking guides removes content authority, not the immutable
    // matching owner/fence used to retire an already submitted invocation.
    return prior.ordering==InvocationOrdering::AuthenticatedTransport && prior.transport &&
           MatchingInvocation(prior,device,context,thread,false);
}
struct Dx11Owner::Storage
{
    Discovery evidence;
    Reason reason = Reason::CaptureUnproven;
    DWORD thread = GetCurrentThreadId();
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11Device1> device1;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11DeviceContext1> context1;
    ComPtr<ID3DDeviceContextState> scratch;
    struct Submission : InvocationProof {
        ComPtr<ID3D11Query> completion;
        std::array<ComPtr<ID3D11Texture2D>,7> resources;
        bool lifecyclePending=true;
    };
    std::list<Submission> submissions;
    std::array<ComPtr<ID3D11Query>,3> completedQueries;
    struct InspectedResource {
        ComPtr<ID3D11Texture2D> texture;
        ResourceInfo info;
    };
    std::array<InspectedResource,7> inspectedResources;
    ResourceInfo Inspect(ID3D11Texture2D* texture,size_t index)
    {
        auto& cached=inspectedResources[index];
        if(cached.texture.Get()==texture)return cached.info;
        cached={};
        if(!texture)return {};
        ComPtr<IUnknown> identity;
        if(FAILED(texture->QueryInterface(IID_PPV_ARGS(&identity))))return {};
        ResourceInfo info;
        info.identity=reinterpret_cast<uintptr_t>(identity.Get());
        info.resourcePointer=reinterpret_cast<uintptr_t>(texture);texture->GetDesc(&info.desc);
        ComPtr<ID3D11Device> device;texture->GetDevice(&device);
        info.deviceIdentity=reinterpret_cast<uintptr_t>(device.Get());
        // Immutable descriptor/device/COM identity; the retained reference
        // prevents address reuse. A different texture always takes this path.
        cached.texture=texture;cached.info=info;return info;
    }
    // GPU completion releases submission roots; content authority belongs to
    // the last actual authenticated submission until its next input overwrite.
    // One receipt only. Completion order can never republish an older frame.
    std::atomic<std::shared_ptr<const GenshinGuides::Receipt>> currentContent;
    void RevokeCurrentContent() noexcept {
        auto receipt=currentContent.exchange({},std::memory_order_acq_rel);
        if(receipt)receipt->Retire();
    }
    ComPtr<ID3D11Texture2D> retainedOutput;
    Parameters lastParameters {};
    std::array<uintptr_t,7> lastResourceIdentities{};
    FfxFsr2Context key {};
    FfxFsr2ContextDescription creation {};
    Lifecycle lifecycle;
    bool registered = false, cleanupOwned = false, accepted = false;

    // Process-lifetime quarantine retains a failed/pending owner rather than prematurely releasing GPU resources.
    static std::vector<Storage*>& Retired() { static auto* owners=new std::vector<Storage*>; return *owners; }
    static std::mutex& Mutex() { static auto* mutex=new std::mutex; return *mutex; }
    bool ReleaseProvider()
    {
        RevokeCurrentContent();
        if (!cleanupOwned) return true;
        if (DestroyGenshinFsr2Dx11(&key) != FFX_OK) return false;
        cleanupOwned=false; registered=false; retainedOutput.Reset(); return true;
    }
    Reason Poll()
    {
        if (GetCurrentThreadId()!=thread || !context || !device) return Reason::InvalidIdentity;
        if (FAILED(device->GetDeviceRemovedReason()))
        {
            // Device removal is an explicit cancellation, not evidence of a successful output.
            RevokeCurrentContent();
            lifecycle.DeviceRemoved();
            for(auto& submission:submissions)if(submission.guides)submission.guides->Retire();
            submissions.clear();retainedOutput.Reset();
            return reason=Reason::DeviceLost;
        }
        for(auto it=submissions.begin();it!=submissions.end();)
        {
            BOOL finished=FALSE;
            const HRESULT result=context->GetData(it->completion.Get(),&finished,sizeof(finished),D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if(FAILED(result)) {RevokeCurrentContent();lifecycle.Complete(false);accepted=false;return reason=Reason::CompletionFailure;}
            if(result==S_FALSE || !finished || (it->transport && !it->transport->Poll())){++it;continue;}
            // Query completion alone does not cover executed but unfenced DX12 work.
            // Keep the singular provider and its pass resources until both retire.
            if(it->lifecyclePending)lifecycle.Complete(true);
            const bool current=it->guides && it->guides==currentContent.load(std::memory_order_acquire);
            if(it->guides && !current){
                static unsigned retirementReports=0;if(retirementReports<32){++retirementReports;
                    LOG_INFO("R827 guide retirement: pid={} generation={} source_frame={} receipt={} transport_receipt={} transport={} thread={} query_done={} no_authority=true",
                        GetCurrentProcessId(),it->parameters.generation,it->parameters.sourceFrame,
                        reinterpret_cast<uintptr_t>(it->guides.get()),reinterpret_cast<uintptr_t>(it->transport?it->transport->Guides().get():nullptr),
                        bool(it->transport),GetCurrentThreadId(),finished!=FALSE);
                }
                it->guides->Retire();
            }
            for(auto& query:completedQueries)if(!query){query=std::move(it->completion);break;}
            it=submissions.erase(it);
        }
        return lifecycle.Pending() && reason==Reason::None ? Reason::Pending : reason;
    }
};
static std::wstring CurrentExecutable()
{
    wchar_t path[32768] {};
    const DWORD size=GetModuleFileNameW(nullptr,path,static_cast<DWORD>(std::size(path)));
    return size && size<std::size(path) ? std::wstring(path,size) : std::wstring();
}
Dx11Owner::Dx11Owner(ID3D11DeviceContext* context, Discovery evidence) : storage_(std::make_unique<Storage>())
{
    auto& s=*storage_; s.evidence=evidence; s.reason=Discover(CurrentExecutable(),evidence);
    if (s.reason!=Reason::None) return;
    if (!context || context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE) {s.reason=Reason::InvalidIdentity;return;}
    s.context=context; context->GetDevice(&s.device);
    if (!s.device || FAILED(s.device.As(&s.device1)) || FAILED(s.context.As(&s.context1)))
    {s.reason=Reason::StateUnavailable;return;}
    const D3D_FEATURE_LEVEL level=s.device->GetFeatureLevel(); D3D_FEATURE_LEVEL chosen {};
    const UINT flags=(s.device->GetCreationFlags()&D3D11_CREATE_DEVICE_SINGLETHREADED)
        ? D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED : 0;
    if (FAILED(s.device1->CreateDeviceContextState(flags,&level,1,D3D11_SDK_VERSION,__uuidof(ID3D11Device),&chosen,&s.scratch)))
        s.reason=Reason::StateUnavailable;
}
Dx11Owner::~Dx11Owner()
{
    if (!storage_) return;
    auto& s=*storage_;
    RevokeGuides();s.lifecycle.Invalidate(); s.Poll();
    if (!s.lifecycle.CanDestroy() || GetCurrentThreadId()!=s.thread || !s.ReleaseProvider())
    {std::lock_guard lock(Storage::Mutex()); Storage::Retired().push_back(storage_.release());}
}
void Dx11Owner::CollectRetired()
{
    std::lock_guard lock(Storage::Mutex()); auto& owners=Storage::Retired();
    for (auto it=owners.begin();it!=owners.end();)
    {
        auto* s=*it;
        if (GetCurrentThreadId()!=s->thread) {++it;continue;}
        s->Poll();
        if (s->lifecycle.CanDestroy() && s->ReleaseProvider()) {delete s;it=owners.erase(it);} else ++it;
    }
}
Result Dx11Owner::Submit(const CapturedFrame& f)
{
    GenshinCpuStageTiming::Scope cpu(GenshinCpuStageTiming::Stage::BridgeSubmit);
    auto& s=*storage_; const auto& p=f.parameters;
    Result failure; failure.instance=p.instance;failure.epoch=p.epoch;failure.generation=p.generation;
    failure.outputIdentity=reinterpret_cast<uintptr_t>(f.output);
    GenshinSubstage::Event(GenshinSubstage::Stage::SrAdmission,GenshinSubstage::State::Running);
    auto fail=[&](Reason r) {
        if(r!=Reason::Pending && r!=Reason::Draining)GenshinSubstage::Event(
            GenshinSubstage::Stage::SrAdmission,GenshinSubstage::State::Failed,
            GenshinSubstage::Resource::Inputs,GenshinSubstage::Domain::Fsr,static_cast<int64_t>(r));
        failure.reason=r;return failure;};
    if (s.reason!=Reason::None && s.reason!=Reason::Pending) return fail(s.reason);
    if (GetCurrentThreadId()!=s.thread) return fail(Reason::InvalidIdentity);
    // Poll without forcing another immediate-context submission. Ordered
    // retained invocations can advance while prior outputs await their real
    // next input/consumer/Present submit. Their queries and leases stay alive.
    s.Poll(); if (s.reason!=Reason::None) return fail(s.reason);
    const Reason parameters=ValidateParameters(p); if (parameters!=Reason::None) return fail(parameters);
    ResourceSet resources {s.Inspect(f.color,0),s.Inspect(f.depth,1),s.Inspect(f.motion,2),s.Inspect(f.output,3),
                           s.Inspect(f.exposure,4),s.Inspect(f.reactive,5),s.Inspect(f.transparency,6)};
    // Authenticate the exact current producer BEFORE asking the descriptor
    // contract to admit shared prepared inputs. Ordinary/native calls receive
    // an empty capability and retain MiscFlags==0 for every resource.
    InvocationProof invocation;
    invocation.parameters=p;invocation.identities=resources;invocation.guides=f.guides;
    invocation.transport=GenshinTransport::ProducerInvocation::Current();
    const auto device=reinterpret_cast<uintptr_t>(s.device.Get());
    const auto context=reinterpret_cast<uintptr_t>(s.context.Get());
    invocation.device=device;invocation.context=context;invocation.thread=s.thread;
    if(!MatchingInvocation(invocation,device,context,s.thread,true))return fail(Reason::InvalidIdentity);
    PreparedResourceAdmission prepared;
    if(invocation.transport && Config::Instance()->GenshinControlsCurrent(invocation.transport->Owner().configEpoch))
        prepared=invocation.transport->PreparedAdmission(s.context.Get(),p,resources);
    ResourceRefusal refusal;
    const Reason resourceReason=ValidateResources(p,resources,device,context,&prepared,&refusal);
    if (resourceReason!=Reason::None) {
        static std::atomic<unsigned> resourceRefusalReports{0};
        if(resourceRefusalReports.fetch_add(1,std::memory_order_relaxed)<8)try {
            const ResourceInfo* rejected=nullptr;
            if(refusal.role==std::string_view("color"))rejected=&resources.color;
            else if(refusal.role==std::string_view("depth"))rejected=&resources.depth;
            else if(refusal.role==std::string_view("motion"))rejected=&resources.motion;
            else if(refusal.role==std::string_view("output"))rejected=&resources.output;
            else if(refusal.role==std::string_view("exposure"))rejected=&resources.exposure;
            else if(refusal.role==std::string_view("reactive"))rejected=&resources.reactive;
            else if(refusal.role==std::string_view("transparency"))rejected=&resources.transparency;
            const auto d=rejected?rejected->desc:D3D11_TEXTURE2D_DESC{};
            LOG_WARN("R044 SR admission refused: pid={} instance={} epoch={} generation={} source_frame={} role={} field={} reason={} resource={} identity={} misc={} bind={} format={} size={}x{} mip={} array={} samples={} quality={} usage={} cpu={} before_provider=true",
                GetCurrentProcessId(),p.instance,p.epoch,p.generation,p.sourceFrame,refusal.role,refusal.field,
                ReasonText(resourceReason),rejected?rejected->resourcePointer:0,rejected?rejected->identity:0,
                d.MiscFlags,d.BindFlags,static_cast<unsigned>(d.Format),d.Width,d.Height,d.MipLevels,d.ArraySize,
                d.SampleDesc.Count,d.SampleDesc.Quality,static_cast<unsigned>(d.Usage),d.CPUAccessFlags);
        }catch(...){} // Diagnostics never change the exact refusal.
        return fail(resourceReason);
    }
    const auto creation=MakeContext(p,s.device.Get());
    std::array<uintptr_t,7> identities {resources.color.identity,resources.depth.identity,resources.motion.identity,
        resources.output.identity,resources.exposure.identity,resources.reactive.identity,resources.transparency.identity};
    if (p.generation==s.lastParameters.generation && p.epoch==s.lastParameters.epoch &&
        (!SameParameters(p,s.lastParameters) || identities!=s.lastResourceIdentities)) return fail(Reason::IdentityChanged);
    if (!f.replaySafe) return fail(Reason::ReplayUnsafe);
    // This authenticated Bridge leases four resources and has no supported
    // manual ExposureTexture input. Reject an unsupported initial mode before
    // FSR context registration/dispatch. The outer NGX wrapper may still be
    // created by the game, but this path never reports a provider-ready SR.
    // Keep the owner retryable when the user restores true/auto.
    if (!s.registered && !Config::Instance()->AutoExposure.value_or(p.autoExposure)) {
        failure.reason=Reason::InvalidParameters;
        failure.unsubmittedManualExposure=true;
        s.accepted=false;s.RevokeCurrentContent();
        GenshinSubstage::Event(GenshinSubstage::Stage::SrAdmission,GenshinSubstage::State::Failed,
            GenshinSubstage::Resource::Inputs,GenshinSubstage::Domain::Fsr,
            static_cast<int64_t>(Reason::InvalidParameters),GenshinSubstage::Issue::Rejected);
        LOG_WARN("R048 SR exposure unsupported: pid={} instance={} epoch={} generation={} requested_auto=false provider_registered=false provider_created=false submitted=false native_fallback=true restore_auto=true_or_auto",
            GetCurrentProcessId(),p.instance,p.epoch,p.generation);
        return failure;
    }
    bool orderedRetained=true;
    for(const auto& prior:s.submissions)
        if(!RetainedInvocationOrdered(prior,p,device,context,s.thread)){orderedRetained=false;break;}
    if (s.registered)
    {
        if (creation.flags!=s.creation.flags || creation.maxRenderSize.width!=s.creation.maxRenderSize.width ||
            creation.maxRenderSize.height!=s.creation.maxRenderSize.height || creation.displaySize.width!=s.creation.displaySize.width ||
            creation.displaySize.height!=s.creation.displaySize.height) return fail(Reason::Draining);
        auto checked=s.lifecycle.Check(p,resources.output.identity,orderedRetained);
        // A retained no-provider-work query can block orderedRetained without
        // making this fresh invocation a failed history. Preserve all old roots;
        // report retryable backpressure only before this invocation owns a lease.
        if(invocation.transport)checked.NoteUnsubmittedRecordingPending(
            invocation.transport->Live(),invocation.transport->LeaseCreated());
        if (checked.reason!=Reason::None || checked.disposition==Disposition::QueuedOutput ||
            checked.disposition==Disposition::ReusedOutput) return checked;
    }
    // Match the existing recording capacity. Nonblocking backpressure must
    // not turn missed frames into an unbounded list of query/resource owners.
    // Keep every retained invocation; refuse before queuing another one.
    if(s.submissions.size() >= FramePipeline::GpuWorkDomain::RecordingCount) {
        failure.unsubmittedRecordingPending=true;
        return fail(Reason::Pending);
    }
    GenshinSubstage::Event(GenshinSubstage::Stage::SrAdmission,GenshinSubstage::State::Succeeded);
    // Capture must already prove safe native replay. Refuse unsafe partial-output fallback before submission.
    D3D11_QUERY_DESC queryDesc {D3D11_QUERY_EVENT,0};
    ComPtr<ID3D11Query> completion;
    for(auto& query:s.completedQueries)if(query){completion=std::move(query);break;}
    if(!completion && FAILED(s.device->CreateQuery(&queryDesc,&completion)))return fail(Reason::CompletionFailure);
    if (!s.registered)
    {
        auto open=s.lifecycle.Open(p.instance,p.epoch);
        if (open!=Reason::None) return fail(open);
    }
    // Allocate and retain before recording any work; a normal pending query never
    // overwrites the owners of a previous generation. The provider stays singular.
    try {
        Storage::Submission submission;
        static_cast<InvocationProof&>(submission)=invocation;
        submission.completion=completion;
        submission.resources={f.color,f.depth,f.motion,f.output,f.exposure,f.reactive,f.transparency};
        s.submissions.push_back(std::move(submission));
    } catch(...) {return fail(Reason::StateUnavailable);}
    s.RevokeCurrentContent();
    ComPtr<ID3DDeviceContextState> previous;
    s.context1->SwapDeviceContextState(s.scratch.Get(),&previous);
    FfxErrorCode result=FFX_OK;
    Transition transition=Transition::None;
    if(s.registered)GenshinSubstage::Event(GenshinSubstage::Stage::ContextRegistration,
        GenshinSubstage::State::Succeeded,GenshinSubstage::Resource::Context);
    try
    {
    if (!s.registered)
    {
        GenshinSubstage::Event(GenshinSubstage::Stage::ContextRegistration,GenshinSubstage::State::Running);
        GenshinCpuStageTiming::Scope cpu(GenshinCpuStageTiming::Stage::BridgeRegister);
        result=RegisterGenshinFsr2Dx11(&s.key,creation,p.instance,p.epoch,s.cleanupOwned);
        GenshinSubstage::Event(GenshinSubstage::Stage::ContextRegistration,
            result==FFX_OK?GenshinSubstage::State::Succeeded:GenshinSubstage::State::Failed,
            GenshinSubstage::Resource::Context,GenshinSubstage::Domain::Fsr,result);
        if (result==FFX_OK) {s.registered=true;s.creation=creation;}
    }
    if (result==FFX_OK)
    {
        auto dispatch=MakeDispatch(p,s.context.Get(),resources,s.lifecycle.ForceReset());
        GenshinSrReceipt::Identity identity;
        identity.instance=p.instance;identity.epoch=p.epoch;identity.generation=p.generation;identity.sourceFrame=p.sourceFrame;
        identity.device=reinterpret_cast<uint64_t>(s.device.Get());identity.context=reinterpret_cast<uint64_t>(s.context.Get());
        identity.output=resources.output.identity;
        {GenshinCpuStageTiming::Scope cpu(GenshinCpuStageTiming::Stage::BridgeDispatch);
        const auto dispatched=DispatchGenshinFsr2Dx11(&s.key,dispatch,identity,f.guides);
        result=dispatched.status;transition=dispatched.transition;}
    }
    }
    catch (...) { result=FFX_ERROR_BACKEND_API_ERROR; }
    // Dispatch returns only after the SR receipt has accepted the exact actual
    // Evaluate and finished its transport scope. Failure never grants ordering.
    if(result==FFX_OK)s.submissions.back().ordering=invocation.transport
        ? InvocationOrdering::AuthenticatedTransport : InvocationOrdering::NativeImmediate;
    s.context1->SwapDeviceContextState(previous.Get(),nullptr);
    s.context->End(completion.Get());
    const bool recordingPending=GenshinTransport::ConsumeRecordingPending();
    const bool retryablePending=result!=FFX_OK && recordingPending && invocation.transport &&
        !invocation.transport->LeaseCreated();
    const bool backendRebuild=result!=FFX_OK && transition==Transition::UnsubmittedBackendRebuild &&
        (!invocation.transport || (invocation.transport->Live() && !invocation.transport->LeaseCreated()));
    Result submitted;
    if(retryablePending || backendRebuild) {
        // Keep preparation resources and the real EVENT until normal retirement;
        // this invocation added no provider work or lifecycle pending count.
        s.submissions.back().lifecyclePending=false;
        if(backendRebuild)s.lifecycle.BackendRebuildPending();
        submitted.instance=p.instance;submitted.epoch=p.epoch;submitted.generation=p.generation;
        submitted.outputIdentity=resources.output.identity;submitted.reason=Reason::Pending;
        submitted.disposition=Disposition::RunNative;submitted.unsubmittedRecordingPending=retryablePending;
        if(backendRebuild)submitted.transition=Transition::UnsubmittedBackendRebuild;
        if(retryablePending && p.generation%300==1)LOG_INFO("SR recording pending excludes lifecycle: pid={} instance={} generation={} lease_created=false native_replay=true",
            GetCurrentProcessId(),p.instance,p.generation);
    } else submitted=s.lifecycle.Submitted(p,resources.output.identity,result==FFX_OK,true,f.replaySafe,true);
    s.lastParameters=p;s.lastResourceIdentities=std::move(identities);s.retainedOutput=f.output;
    s.accepted=submitted.MaySkipOriginalDraw();
    // Backend failure invalidates history; query/leases stay alive until completion, then a new epoch is mandatory.
    if (result!=FFX_OK) {
        RevokeGuides();
        if(retryablePending || backendRebuild) {
            submitted.reason=Reason::Pending;
            submitted.disposition=Disposition::RunNative;
            submitted.pendingLease=false;
        } else {s.reason=Reason::BackendFailure;s.lifecycle.Invalidate();}
    }
    // Only this fresh successful authenticated submission selects current
    // authority. Reused/queued returns above and Poll never select a receipt.
    if(result==FFX_OK && s.accepted && invocation.transport && invocation.guides &&
       MatchingInvocation(invocation,device,context,s.thread,true))
        s.currentContent.store(invocation.guides,std::memory_order_release);
    return submitted;
}
Reason Dx11Owner::Poll() {return storage_->Poll();}
void Dx11Owner::RevokeGuides() noexcept
{
    auto& s=*storage_;s.accepted=false;s.RevokeCurrentContent();
    for(auto& submission:s.submissions)if(submission.guides)submission.guides->Retire();
}
Reason Dx11Owner::Invalidate()
{
    auto& s=*storage_;RevokeGuides();
    if (GetCurrentThreadId()!=s.thread) return Reason::InvalidIdentity;
    GenshinTransport::FinishPendingOutput(s.device.Get(),GenshinTransport::OutputFlushReason::Revoke);
    s.lifecycle.Invalidate();
    const auto result=s.Poll();
    if (!s.lifecycle.CanDestroy()) return result==Reason::CompletionFailure ? result : Reason::Pending;
    if (!s.ReleaseProvider()) return s.reason=Reason::BackendFailure;
    return s.reason=Reason::Draining;
}
RouteStatus Dx11Owner::Status() const noexcept
{
    const auto& s=*storage_; RouteStatus status;
    status.reason=s.reason;status.captureProven=s.reason!=Reason::WrongGame &&
        s.evidence.captureEntryProven && s.evidence.semanticsProven && s.evidence.orderedOutputConsumerProven && s.evidence.inputLeaseProven;
    status.providerRegistered=s.registered; status.srAccepted=s.accepted && !s.lifecycle.NeedsDrain();
    status.pending=s.lifecycle.Pending();return status;
}
}

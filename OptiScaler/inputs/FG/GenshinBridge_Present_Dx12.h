// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "GenshinFgTicket.h"
#include <ui_mask/DrawUiMask.h>
#include <dlssnr/FinishedPictureOutcome.h>
#include <genshin/GenshinPresentNr.h>
#include <genshin/GenshinFgPreview.h>
#include <inputs/GenshinFsrContract.h>
#include <framegen/dlssg/DLSSG_Dx12.h>
#include <native/OpticalFgNrPolicy.h>
#include <Config.h>
#include <genshin/GenshinFgConsumer.h>
#include <SysUtils.h>
#include <State.h>
#include <Util.h>
#include <bit>
#include <sstream>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <utility>
#include <array>
#include <atomic>

namespace GenshinLegacy { class CapturePipeline; }
namespace GenshinBridgeFg {
using Microsoft::WRL::ComPtr;
inline uint64_t Identity(IUnknown* p) noexcept {return GenshinPresentNr::Identity(p);}
inline bool OnDevice(ID3D12DeviceChild* p,ID3D12Device* d) noexcept {
    ComPtr<ID3D12Device> actual;
    return p && d && SUCCEEDED(p->GetDevice(IID_PPV_ARGS(&actual))) && Identity(actual.Get())==Identity(d);
}
// Bit-preserving SDR copies only: no gamma shader, swizzle, alpha or HDR conversion.
inline bool SdrCopyFormat(DXGI_FORMAT a,DXGI_FORMAT b) noexcept {
    const auto family=[](DXGI_FORMAT f) {
        if(f==DXGI_FORMAT_R8G8B8A8_UNORM || f==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)return 1;
        if(f==DXGI_FORMAT_B8G8R8A8_UNORM || f==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)return 2;
        return 0;
    };
    return family(a) && family(a)==family(b);
}
inline bool SdrCopy11(const D3D11_TEXTURE2D_DESC& a,const D3D11_TEXTURE2D_DESC& b) noexcept {
    return a.Width==b.Width && a.Height==b.Height && a.Width && a.Height &&
        a.ArraySize==1 && b.ArraySize==1 && a.MipLevels==1 && b.MipLevels==1 &&
        a.SampleDesc.Count==1 && b.SampleDesc.Count==1 && !a.SampleDesc.Quality && !b.SampleDesc.Quality &&
        SdrCopyFormat(a.Format,b.Format);
}
inline bool SdrCopy12(const D3D12_RESOURCE_DESC& a,const D3D12_RESOURCE_DESC& b) noexcept {
    return a.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D && a.Dimension==b.Dimension &&
        a.Width==b.Width && a.Height==b.Height && a.Width && a.Height &&
        a.DepthOrArraySize==1 && b.DepthOrArraySize==1 && a.MipLevels==1 && b.MipLevels==1 &&
        a.SampleDesc.Count==1 && b.SampleDesc.Count==1 && !a.SampleDesc.Quality && !b.SampleDesc.Quality &&
        a.Layout==D3D12_TEXTURE_LAYOUT_UNKNOWN && b.Layout==D3D12_TEXTURE_LAYOUT_UNKNOWN &&
        SdrCopyFormat(a.Format,b.Format);
}
inline bool SdrCopy11To12(const D3D11_TEXTURE2D_DESC& a,const D3D12_RESOURCE_DESC& b) noexcept {
    return SdrCopy11(a,a) && SdrCopy12(b,b) && a.Width==b.Width && a.Height==b.Height && SdrCopyFormat(a.Format,b.Format);
}
inline bool Finite(float f) noexcept {
    return f!=sl::INVALID_FLOAT && (std::bit_cast<uint32_t>(f)&0x7f800000u)!=0x7f800000u;
}
inline bool Plane(float f) noexcept {return f>0 && (Finite(f) || std::bit_cast<uint32_t>(f)==0x7f800000u);}
inline bool Matrix(const sl::float4x4& m) noexcept {
    float maximum=0;
    for(unsigned i=0;i<4;++i)for(float v:{m[i].x,m[i].y,m[i].z,m[i].w}) {
        if(!Finite(v))return false;
        if(v<0)v=-v;if(v>maximum)maximum=v;
    }
    return maximum>0;
}
inline Key MakeKey(const GenshinPresentNr::Ticket& t) noexcept {
    const auto& s=t.source;
    return {s.device,s.context,s.deviceEpoch,s.ownerEpoch,s.configEpoch,s.instance,s.generation,
        s.sourceFrame,t.pictureSerial,t.presentEpoch,s.selectedOutput,Identity(t.backbuffer.Get()),t.swapchain,
        s.thread,t.pictureWidth,t.pictureHeight,s.selectedSubresource,s.submitSerial,s.copyOrderedSerial,s.historyEpoch};
}
// R723: expose the first policy refusal without changing any admission gate.
inline const char* RequestRefusal() {
    const auto& s=State::Instance();const auto& c=*Config::Instance();
    if(s.isShuttingDown)return "ShuttingDown";
    if(!GenshinFsr::IsGenshinExecutable(Util::ExePath().wstring()))return "NotGenshinExecutable";
    if(s.activeFgInput!=FGInput::Upscaler)return "UnsupportedFgInput";
    if(s.activeFgOutput!=FGOutput::DLSSG)return "UnsupportedFgOutput";
    if(!c.FGEnabled.value_or_default())return "FrameGenerationDisabled";
    if(!GenshinFgConsumer::NewWorkRequested())return "NoFgConsumerSelected";
    // R727: Requested only opens discovery. Provider tagging still requires the
    // exact Capture-issued normalized-guide contract, even when ResourceFlip is false.
    if(native::OpticalFgNrEnabled())return "OpticalFgNrUnsupported";
    if(s.swapchainInteropApi==SwapchainInteropApi::VkwDx12)return "VulkanInteropUnsupported";
    return nullptr;
}
inline bool Requested() {return RequestRefusal()==nullptr;}
using PictureKind=GenshinPresentNr::FgPictureKind;
inline PictureKind CurrentPictureKind() {
    const auto& c=*Config::Instance();
    return GenshinPresentNr::SelectFgPicture(c.DlssNrEnabled.value_or_default(),c.DlssNrFinishedPicture.value_or_default(),
        c.DlssNrApplyModel.value_or_default(),c.DlssNrRunBeforeSr.value_or_default(),c.DlssNrDeferredDlss.value_or_default());
}
// The supplied matrices must be observed at the actual camera/render boundary,
// for this exact source. RequestedControl/PinnedBridgeProfile cannot sign them.
// Current production has no such writer. Missing camera is an integration gap,
// never permission to install a zero camera or identity previous-frame matrix.
struct CameraObservation {
    GenshinGuides::Owner owner{},predecessor{};
    bool PredecessorMatches(const Key& k)const noexcept {
        const auto& p=predecessor;
        return p.device==k.device && p.context==k.context && p.deviceEpoch==k.deviceEpoch &&
            p.producerEpoch==k.ownerEpoch && p.configEpoch==k.configEpoch && p.instance==k.instance &&
            p.generation==k.generation && p.historyEpoch==k.historyEpoch && p.sourceFrame==k.sourceFrame && p.thread==k.thread;
    }
    GenshinGuides::Source source=GenshinGuides::Source::Unknown;
    sl::Constants constants{};
    bool Matches(const GenshinPresentNr::Ticket& t) const noexcept {
        const auto& s=t.source;
        return source==GenshinGuides::Source::ActualRenderToken && owner.device==s.device &&
            owner.context==s.context && owner.deviceEpoch==s.deviceEpoch && owner.producerEpoch==s.ownerEpoch &&
            owner.configEpoch==s.configEpoch && owner.instance==s.instance && owner.generation==s.generation &&
            owner.historyEpoch==s.historyEpoch && owner.sourceFrame==s.sourceFrame && owner.thread==s.thread &&
            predecessor.device==owner.device && predecessor.context==owner.context &&
            predecessor.deviceEpoch==owner.deviceEpoch && predecessor.producerEpoch==owner.producerEpoch &&
            predecessor.configEpoch==owner.configEpoch && predecessor.instance==owner.instance &&
            predecessor.historyEpoch==owner.historyEpoch && predecessor.thread==owner.thread &&
            predecessor.generation<owner.generation && predecessor.sourceFrame<owner.sourceFrame &&
            Matrix(constants.cameraViewToClip) && Matrix(constants.clipToCameraView) &&
            Matrix(constants.clipToPrevClip) && Matrix(constants.prevClipToClip) &&
            (constants.reset==sl::Boolean::eTrue || constants.reset==sl::Boolean::eFalse) &&
            !constants.next && Plane(constants.cameraNear) && Plane(constants.cameraFar) &&
            constants.cameraNear!=constants.cameraFar && Finite(constants.cameraFOV) && constants.cameraFOV>0 &&
            constants.cameraFOV<3.141592654f && Finite(constants.cameraAspectRatio) && constants.cameraAspectRatio>0 &&
            Finite(constants.cameraPos.x) && Finite(constants.cameraPos.y) && Finite(constants.cameraPos.z) &&
            Finite(constants.cameraUp.x) && Finite(constants.cameraUp.y) && Finite(constants.cameraUp.z) &&
            Finite(constants.cameraRight.x) && Finite(constants.cameraRight.y) && Finite(constants.cameraRight.z) &&
            Finite(constants.cameraFwd.x) && Finite(constants.cameraFwd.y) && Finite(constants.cameraFwd.z) &&
            (constants.orthographicProjection==sl::Boolean::eTrue || constants.orthographicProjection==sl::Boolean::eFalse);
    }
};
// Acquired on the native owner thread from the already validated successful
// History. Camera remains optional until the real render writer is integrated.
// An optional FG offer is separate from a normal motion-only NR ticket.
// Seal binds a completed actual SR transfer, exact selected picture and real
// camera. Its absence must never consume another ticket or gate native NR.
inline void ObserveGuideOwners(const GenshinPresentNr::Ticket&) noexcept;
inline void CollectPreviewGuideOwner() noexcept;
class GenshinFgOffer final {
    std::shared_ptr<const GenshinTransport::QueuedTransfer> transfer_;
    std::optional<CameraObservation> sealedCamera_;
    native::BridgeDepthLeasePtr depthLease_;
    std::shared_ptr<const GenshinDepthMapping::Node> depthMapping_;
    Key key_{};
    uintptr_t provider_=0,providerSwapchain_=0;
    PictureKind kind_=PictureKind::Unknown;
public:
    GenshinPresentNr::Ticket ticket;
    std::shared_ptr<GenshinTransport::ProducerFrame> producer;
    std::optional<CameraObservation> camera;
    uintptr_t provider=0,providerSwapchain=0;
    PictureKind pictureKind=PictureKind::Unknown;
    bool ProducerMatches() const noexcept {
        if(pictureKind==PictureKind::Unknown || !ticket.DepthIdentityMatches() || !producer || !producer->Live() ||
           !producer->Guides() || !provider || !providerSwapchain || ticket.source.thread!=GetCurrentThreadId())return false;
        GenshinGuides::Owner o{};const auto& s=ticket.source;
        if(!GenshinGuides::OwnerIdentity::Context(ticket.context.Get()).ToCanonical(producer->Owner(),o))return false;
        return o.device==s.device && o.context==s.context && o.deviceEpoch==s.deviceEpoch && o.producerEpoch==s.ownerEpoch &&
            o.configEpoch==s.configEpoch && o.instance==s.instance && o.generation==s.generation &&
            o.historyEpoch==s.historyEpoch && o.sourceFrame==s.sourceFrame && o.thread==s.thread &&
            producer->Guides()->Get().textures[3].identity==s.privateOutput;
    }
    bool TransferMatches() const noexcept {
        if(!transfer_ || transfer_!=ticket.depth.nativeTransfer || !ProducerMatches())return false;
        const auto& v=transfer_->Get();const auto& f=producer->Guides()->Get();
        if(!GenshinGuides::SameOwner(v.owner,producer->Owner()) || !v.queue || !v.list || !v.allocator ||
           !v.inputs || !v.submitted || !v.returned || !v.inputValue || !v.submittedValue || !v.returnedValue)return false;
        for(size_t i=0;i<f.textures.size();++i){
            const auto& t=f.textures[i];const auto& d=v.descriptors[i];
            if(Identity(v.sources[i].Get())!=t.identity || !v.imported[i] || t.device!=v.owner.device ||
               t.width.source!=GenshinGuides::Source::TextureDescriptor || t.height.source!=GenshinGuides::Source::TextureDescriptor ||
               t.format.source!=GenshinGuides::Source::TextureDescriptor || d.Width!=t.width.value || d.Height!=t.height.value ||
               d.Format!=t.format.value || d.MipLevels!=1 || d.ArraySize!=1 || d.SampleDesc.Count!=1 || d.SampleDesc.Quality)return false;
        }
        // The mapping output is a distinct RG texture. Authenticate both edges
        // of the actual Resolve transaction; descriptor equality cannot join it.
        GenshinGuides::Owner canonicalOwner{};
        if(!GenshinGuides::OwnerIdentity::Context(ticket.context.Get()).ToCanonical(v.owner,canonicalOwner))return false;
        if(Identity(ticket.depth.producerSource.Get())!=f.textures[1].identity ||
           v.descriptors[1].Format!=DXGI_FORMAT_R32_FLOAT || v.descriptors[2].Format!=DXGI_FORMAT_R16G16_FLOAT)return false;
        if(ticket.depth.nativeGuide)return ticket.depth.NativeIdentityKnown() &&
            GenshinGuides::SameOwner(ticket.depth.nativeGuide->owner,canonicalOwner) &&
            ticket.depth.nativeGuide->pictureIdentity==ticket.source.privateOutput;
        return ticket.depth.mappingOwner && GenshinGuides::SameOwner(ticket.depth.mappingOwner->owner,canonicalOwner) &&
            Identity(ticket.depth.source.Get())==Identity(ticket.depth.mappingOwner->depth.texture.Get());
    }
    bool Seal() noexcept {
        if(transfer_ || !ticket.DepthMatches() || !ProducerMatches() || !camera || !camera->Matches(ticket))return false;
        transfer_=ticket.depth.nativeTransfer;
        if(!TransferMatches()){transfer_.reset();return false;}
        key_=MakeKey(ticket);provider_=provider;providerSwapchain_=providerSwapchain;kind_=pictureKind;
        sealedCamera_=camera;depthLease_=ticket.depth.lease;depthMapping_=ticket.depth.mappingOwner;
        // Optional preview/UI demand cannot own a completed motion slot forever.
        // This checks only an existing copy's exact positive completion receipt.
        CollectPreviewGuideOwner();
        ticket.gameMotion=producer->PrepareGameMotion(ticket.context.Get(),ticket.depth,camera->predecessor,transfer_);
        ticket.gameMotionStatus=ticket.gameMotion?ticket.gameMotion->Status():GenshinGameMotion::Last();
        if(!ticket.gameMotion && ticket.gameMotionStatus==GenshinGameMotion::Reason::Budget)
            ObserveGuideOwners(ticket);
        static unsigned reports=0;
        if(reports<16){++reports;
            LOG_INFO("R827 game motion offer: pid={} generation={} source_frame={} picture={} predecessor_generation={} predecessor_frame={} lease={} reason={} source=actual_PreparedMotion no_optical_fallback=true",
                GetCurrentProcessId(),ticket.source.generation,ticket.source.sourceFrame,ticket.pictureSerial,
                camera->predecessor.generation,camera->predecessor.sourceFrame,bool(ticket.gameMotion),
                GenshinGameMotion::Text(ticket.gameMotionStatus));}
        return Known();
    }
    bool Known() const noexcept {
        return key_.Known() && key_==MakeKey(ticket) && provider_==provider && providerSwapchain_==providerSwapchain &&
            kind_==pictureKind && depthLease_==ticket.depth.lease && depthMapping_==ticket.depth.mappingOwner &&
            sealedCamera_ && sealedCamera_->Matches(ticket) && TransferMatches() &&
            Config::Instance()->GenshinControlsCurrent(ticket.source.configEpoch);
    }
    const std::shared_ptr<const GenshinTransport::QueuedTransfer>& Transfer() const noexcept {return transfer_;}
    const CameraObservation* Camera() const noexcept {return sealedCamera_?&*sealedCamera_:nullptr;}
};
using SenderInput=GenshinFgOffer;

inline void Barrier(ID3D12GraphicsCommandList* cmd,ID3D12Resource* r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b) {
    if(a==b)return;
    D3D12_RESOURCE_BARRIER x{};x.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    x.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,a,b};cmd->ResourceBarrier(1,&x);
}
inline bool Done(ID3D12Fence* f,uint64_t v) noexcept {
    if(!f || !v)return false;const auto n=f->GetCompletedValue();return n!=UINT64_MAX && n>=v;
}
enum class Result : unsigned char { NotRequested, NoFrame, WrongTicket, MissingDepth, MissingCamera,
    Busy, UnknownOutcome, MappingMismatch, ProviderRefused, Submitted, PresentFailed, Revoked };

// Only the operation that opens the actual shared texture can construct this
// association. Reopening the same allocation and comparing COM addresses is
// invalid; comparing dimensions would not prove its identity either.
class ColorImport final {
    ComPtr<ID3D11Texture2D> source_;
    ComPtr<ID3D12Resource> opened_;
    ColorImport()=default;
public:
    // The wrapper owns the one CreateSharedHandle operation. Open exactly that
    // handle once, keeping ordinary presentation independent of allocation of
    // this optional immutable association. No reopen/COM-address inference.
    static std::shared_ptr<const ColorImport> Open(ID3D12Device* device,ID3D11Texture2D* source,
        HANDLE handle,ID3D12Resource** ordinary,HRESULT& result) noexcept {
        result=E_INVALIDARG;
        if(!device || !source || !handle || !ordinary || *ordinary)return {};
        result=device->OpenSharedHandle(handle,IID_PPV_ARGS(ordinary));
        if(FAILED(result) || !*ordinary)return {};
        try {
            auto r=std::shared_ptr<ColorImport>(new ColorImport);
            r->source_=source;r->opened_=*ordinary;return r;
        } catch(const std::bad_alloc&) {return {};}
    }
    ID3D12Resource* Resource() const noexcept {return opened_.Get();}
    bool Matches(ID3D11Texture2D* source,ID3D12Resource* opened) const noexcept {
        return source && opened && Identity(source)==Identity(source_.Get()) && Identity(opened)==Identity(opened_.Get());
    }
};

// A private immutable copy of FINAL-picture guides, recorded on the actual
// native producer list after its guide dispatch. Never a borrowed persistent
// depth/MV pointer, nor a relabelled RG depth-validity pair.
struct Frame final {
    Lifecycle lifecycle;
    GenshinPresentNr::Ticket ticket;
    std::shared_ptr<GenshinTransport::ProducerFrame> producer;
    std::shared_ptr<const GenshinTransport::QueuedTransfer> origin;
    std::shared_ptr<GenshinGameMotion::Lease> gameMotion;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> producerQueue,providerQueue;
    ComPtr<ID3D12GraphicsCommandList> producerList;
    ComPtr<ID3D12Resource> sourceDepth,sourceMotion,depth,motion,finalColor,importedColor;
    ComPtr<ID3D11Texture2D> sharedColor11;
    std::shared_ptr<GenshinPresentNr::SnapshotLease> snapshotLease;
    std::shared_ptr<DrawUiMask::Frame> uiMask;
    ComPtr<ID3D12Resource> uiHudless;
    bool uiRecorded=false;
    ComPtr<ID3D12Fence> produced,interopCopied,nrFinished;
    std::shared_ptr<const DLSSG_Dx12::GenshinFenceReceipt> waitRetirement;
    std::shared_ptr<const DLSSG_Dx12::GenshinNormalizedGuides> normalizedGuides;
    std::shared_ptr<const DLSSG_Dx12::GenshinInputRetirement> providerRetirement;
    uint64_t producedValue=0,bytes=0,guideEpoch=0,guideCutSerial=0,uiCopyValue=0;
    // R093: observation-only descriptor/immutable owner scalars, never admission
    // authority or SDK format/options. Capture reuses its existing two GetDesc calls.
    DXGI_FORMAT diagnosticDepthFormat=DXGI_FORMAT_UNKNOWN,diagnosticMotionFormat=DXGI_FORMAT_UNKNOWN;
    uint64_t diagnosticDepthWidth=0,diagnosticMotionWidth=0,diagnosticProviderOwner=0;
    uint32_t diagnosticDepthHeight=0,diagnosticMotionHeight=0;
    PictureKind pictureKind=PictureKind::Unknown;
    sl::Constants constants{};
    CameraObservation cameraLineage{};
    // Phase20 observations only; never consulted for admission/reset authority.
    bool diagnosticCameraReset=false,diagnosticScopeReset=false;
    uint32_t diagnosticNativeResetReasons=UINT_MAX;
    bool reserved=false,recorded=false,waitIssued=false,waitFenced=false;
    bool touchedProvider=false;
    Frame* next=nullptr;
    // Called only for retired records, never while Present admission can still
    // inspect their producer ticket. Vendor-retained immutable copies need no
    // source map after both the actual native reads and depth reader retire.
    void PruneSources() noexcept {
        if(!gameMotion || ticket.source.thread!=GetCurrentThreadId() || !recorded ||
           !Done(produced.Get(),producedValue) || (ticket.depth.lease && !ticket.depth.lease->ReaderRetired()))return;
        gameMotion->Prune();
        ticket.depth.mappingOwner.reset();ticket.depth.lease.reset();
        ticket.depth.source.Reset();ticket.depth.producerSource.Reset();
        producer.reset();origin.reset();sourceDepth.Reset();sourceMotion.Reset();
    }
    bool Complete() const noexcept {
        if(recorded && !Done(produced.Get(),producedValue))return false;
        // Failed copy-list Signal must keep the actual optional GPU owners.
        if(uiRecorded && (!interopCopied || !Done(interopCopied.Get(),uiCopyValue)))return false;
        if(waitIssued && (!waitFenced || !waitRetirement || !waitRetirement->Completed()))return false;
        if(snapshotLease && !snapshotLease->CopiesComplete())return false;
        // A queue signal after SetResource is NOT a vendor-resource retirement
        // witness. Its provider receipt also needs vendor input completion,
        // or actual successful Off/drain/free, before this frame can retire.
        // Provider-issued, immutable owner/epoch/frame receipt survives both
        // scope and provider failure. Later successful real teardown updates it.
        return !touchedProvider || (providerRetirement && providerRetirement->Retired());
    }
};
// Only an actual dispatched child Present may commit this predecessor. It is
// used to set the real Streamline reset constant, never to retire a GPU owner.
struct PresentedGuides {
    Key key{};
    uint64_t epoch=0,cut=0;
    // Non-owning identities of the actual successful child submission.
    uintptr_t provider=0,providerSwapchain=0,presentQueue=0;
    PictureKind kind=PictureKind::Unknown;
    bool known=false;
    bool resetRequested=false;
    bool Continues(const Frame& f,Key* observedCurrent=nullptr) const noexcept {
        const auto b=MakeKey(f.ticket);
        if(observedCurrent)*observedCurrent=b; // Existing computed key, no extra COM query.
        return known && f.cameraLineage.PredecessorMatches(key) && epoch==f.guideEpoch && cut==f.guideCutSerial && kind==f.pictureKind &&
            b.TemporalSuccessorOf(key);
    }
    void Commit(const Frame& f) noexcept {
        key=MakeKey(f.ticket);epoch=f.guideEpoch;cut=f.guideCutSerial;kind=f.pictureKind;known=true;
    }
};
// All callers hold Pool::mutex. Scalar diagnostics cannot establish a predecessor.
inline std::string DiagnosticKey(const Key& k) {
    std::ostringstream s;s<<k.device<<','<<k.context<<','<<k.deviceEpoch<<','<<k.ownerEpoch<<','<<k.configEpoch<<','
        <<k.instance<<','<<k.generation<<','<<k.sourceFrame<<','<<k.picture<<','<<k.present<<','<<k.selected<<','
        <<k.backbuffer<<','<<k.swapchain<<','<<k.thread<<','<<k.width<<','<<k.height<<','<<k.selectedSubresource<<','
        <<k.submitSerial<<','<<k.copyOrderedSerial<<','<<k.historyEpoch;return s.str();
}
inline std::string DiagnosticOwner(const GenshinGuides::Owner& o) {
    std::ostringstream s;s<<o.device<<','<<o.context<<','<<o.deviceEpoch<<','<<o.producerEpoch<<','<<o.configEpoch<<','
        <<o.instance<<','<<o.generation<<','<<o.historyEpoch<<','<<o.sourceFrame<<','<<o.thread;return s.str();
}
inline bool DiagnosticBudget(unsigned category,uint64_t signature) noexcept {
    struct Budget {uint64_t calls=0,last=UINT64_MAX,tick=0;unsigned reports=0,changes=0;};
    static Budget budgets[3]{};auto& b=budgets[category];
    if(b.reports>=256)return false;
    const auto now=GetTickCount64();const bool changed=signature!=b.last;
    const bool report=b.calls++<32 || (changed && b.changes<128) || now-b.tick>=5000;
    b.last=signature;if(!report)return false;
    if(changed)++b.changes;++b.reports;b.tick=now;return true;
}
inline void ObserveContinuity(const PresentedGuides& prior,const Frame& f,const Key& b,
    bool continues,uint64_t providerFrame) noexcept {
    try {
        const auto& a=prior.key;
        // Bit0 unknown,1 camera predecessor,2 epoch,3 cut,4 kind,5 Present adjacency,
        // 6 picture order,7 source reversal,8 generation reversal,9..19 owner/layout,21 subresource. Bit20 reserved.
        const uint64_t mask=(!prior.known?1ull:0) | (!f.cameraLineage.PredecessorMatches(a)?1ull<<1:0) |
            (prior.epoch!=f.guideEpoch?1ull<<2:0) | (prior.cut!=f.guideCutSerial?1ull<<3:0) |
            (prior.kind!=f.pictureKind?1ull<<4:0) | (!(a.present<UINT64_MAX && b.present==a.present+1)?1ull<<5:0) |
            (!(b.picture>a.picture)?1ull<<6:0) | (b.sourceFrame<a.sourceFrame?1ull<<7:0) |
            (b.generation<a.generation?1ull<<8:0) | (a.device!=b.device?1ull<<9:0) |
            (a.context!=b.context?1ull<<10:0) | (a.deviceEpoch!=b.deviceEpoch?1ull<<11:0) |
            (a.ownerEpoch!=b.ownerEpoch?1ull<<12:0) | (a.configEpoch!=b.configEpoch?1ull<<13:0) |
            (a.instance!=b.instance?1ull<<14:0) | (a.historyEpoch!=b.historyEpoch?1ull<<15:0) |
            (a.swapchain!=b.swapchain?1ull<<16:0) | (a.thread!=b.thread?1ull<<17:0) |
            (a.width!=b.width?1ull<<18:0) | (a.height!=b.height?1ull<<19:0) |
            (a.selectedSubresource!=b.selectedSubresource?1ull<<21:0);
        const uint64_t signature=mask | (uint64_t(f.constants.reset==sl::Boolean::eTrue)<<32);
        if(!DiagnosticBudget(0,signature))return;
        LOG_INFO("R827 phase21 FG continuity: pid={} provider_frame={} mask={:X} continues={} mask_consistent={} selected_allocation_changed={} known={} normalized_reset={} camera_reset={} scope_reset={} native_reset_reasons={} previous_key={} current_key={} camera_owner={} camera_predecessor={} previous_epoch={} current_epoch={} previous_cut={} current_cut={} previous_kind={} current_kind={} key_schema=device,context,deviceEpoch,ownerEpoch,configEpoch,instance,generation,sourceFrame,picture,present,selected,backbuffer,swapchain,thread,width,height,selectedSubresource,submitSerial,copyOrderedSerial,historyEpoch owner_schema=device,context,deviceEpoch,producerEpoch,configEpoch,instance,generation,historyEpoch,sourceFrame,thread no_authority=true",
            GetCurrentProcessId(),providerFrame,mask,continues,continues==(mask==0),a.selected!=b.selected,prior.known,
            (int)f.constants.reset,f.diagnosticCameraReset,f.diagnosticScopeReset,f.diagnosticNativeResetReasons,
            DiagnosticKey(a),DiagnosticKey(b),DiagnosticOwner(f.cameraLineage.owner),DiagnosticOwner(f.cameraLineage.predecessor),
            prior.epoch,f.guideEpoch,prior.cut,f.guideCutSerial,(unsigned)prior.kind,(unsigned)f.pictureKind);
    }catch(...){}
}
inline void ObserveCommit(const PresentedGuides& before,const PresentedGuides& after,const Frame* f,
    HRESULT result,bool admitted,bool dispatched,bool commit,const char* stage) noexcept {
    try {
        const unsigned lifecycle=f?(unsigned)f->lifecycle.State():0;
        const uint64_t signature=uint64_t(commit)|(uint64_t(admitted)<<1)|(uint64_t(dispatched)<<2)|
            (uint64_t(before.known)<<3)|(uint64_t(lifecycle)<<8)|(uint64_t(uint32_t(result))<<16);
        const unsigned category=stage[0]=='R'?2u:1u;
        if(!DiagnosticBudget(category,signature))return;
        LOG_INFO("R827 phase20 FG commit: pid={} stage={} result={:X} admitted={} dispatched={} commit={} before_known={} after_known={} lifecycle={} previous_key={} committed_key={} frame_key={} guide_epoch={} guide_cut={} kind={} no_authority=true",
            GetCurrentProcessId(),stage,(uint32_t)result,admitted,dispatched,commit,before.known,after.known,lifecycle,
            DiagnosticKey(before.key),DiagnosticKey(after.key),DiagnosticKey(f?f->lifecycle.Identity():Key{}),
            f?f->guideEpoch:0,f?f->guideCutSerial:0,f?(unsigned)f->pictureKind:0);
    }catch(...){}
}
struct Pool {
    PresentedGuides presented;
    std::mutex mutex;
    std::unique_ptr<Frame> ready;
    Frame* retired=nullptr;
    size_t reservations=0;
    uint64_t bytes=0;
};
inline Pool& Storage() {static auto* p=new Pool;return *p;}
inline void ReleaseLocked(Pool& p,Frame* f) {
    // All callers have observed Complete(): native, copy, wait AND provider.
    if(f->snapshotLease)f->snapshotLease->FrameRetired();
    --p.reservations;p.bytes-=f->bytes;delete f;
}
inline void CollectLocked(Pool& p) {
    auto** n=&p.retired;
    while(*n) {auto* f=*n;f->PruneSources();if(f->Complete()){*n=f->next;ReleaseLocked(p,f);}else n=&f->next;}
}
inline void Retire(std::unique_ptr<Frame> f) noexcept {
    if(!f || !f->reserved)return;
    // Recording a private HUDless copy does not prove its optional provider tag
    // was accepted/submitted. Preserve a genuine submitted/completed receipt;
    // otherwise this no-longer-admissible recording has unknown consumption.
    if(f->uiRecorded)DrawUiMask::Observe(f->uiMask,DrawUiMask::Consumer::Fg,DrawUiMask::Lifecycle::Unknown,0,0,0);
    // Retirement invalidates this receipt. GPU commands reference the private
    // guides/shared snapshot/child color only; native B is a CPU identity witness.
    // Keeping B here would let an uncertain optional provider block native Resize.
    f->ticket.backbuffer.Reset();
    auto& p=Storage();std::lock_guard lock(p.mutex);
    if(f->Complete())ReleaseLocked(p,f.release());
    else {f->next=p.retired;p.retired=f.release();}
    CollectLocked(p);
}
inline void Collect() noexcept {auto& p=Storage();std::lock_guard lock(p.mutex);CollectLocked(p);}
// R849: join actual shared_ptr CONTROL BLOCKS, not just matching generations,
// resource addresses or reference counts. Observation never clears an owner,
// polls preview/readback, or grants motion/provider retirement authority.
template<class A,class B>
inline bool SameGuideOwner(const std::shared_ptr<A>& a,const std::shared_ptr<B>& b) noexcept {
    return a && b && !a.owner_before(b) && !b.owner_before(a);
}
// Pending::owner is an opaque keepalive; preview never dereferences it. Retain
// the ring allocation through its GPU COPY, not through later UI Map/display.
// The Frame separately retains both ring roots through real vendor retirement.
struct PreviewGuideOwner {
    std::shared_ptr<GenshinGameMotion::Lease> lease;
    DWORD thread=GetCurrentThreadId();
};
struct PreviewGuideOwners {
    std::mutex mutex;
    // Preview admits at most one pending copy. A weak observer neither extends
    // that job's lifetime nor adds another ring owner after ordinary Poll.
    std::weak_ptr<PreviewGuideOwner> pending;
};
inline PreviewGuideOwners& PreviewOwners() {static auto* p=new PreviewGuideOwners;return *p;}
// The UI holds preview.mutex while mapping/sampling a completed readback.
// Never wait behind it from a provider execution guard on the Present thread.
// One receipt per source thread is enough: a new copy cannot be admitted until
// the previous result has been published. This retains no GPU/resource owners.
struct PreviewDispatchReceipt {
    uint64_t generation=0,providerFrame=0;
    bool submitted=false;
};
inline PreviewDispatchReceipt& PendingPreviewDispatch() noexcept {
    static thread_local PreviewDispatchReceipt receipt;return receipt;
}
inline bool FlushPreviewDispatchResult() noexcept {
    auto& receipt=PendingPreviewDispatch();if(!receipt.generation)return true;
    auto& preview=GenshinFgPreview::Storage();
    std::unique_lock lock(preview.mutex,std::try_to_lock);if(!lock.owns_lock())return false;
    const auto update=[&](GenshinFgPreview::Stamp& stamp) {
        if(stamp.generation==receipt.generation && stamp.providerFrame==receipt.providerFrame)
            stamp.submitted=receipt.submitted;
    };
    if(preview.pending)update(preview.pending->stamp);update(preview.ready.stamp);
    receipt={};return true;
}
inline void RecordPreviewDispatchResult(uint64_t generation,uint64_t providerFrame,bool submitted) noexcept {
    PendingPreviewDispatch()={generation,providerFrame,submitted};
    FlushPreviewDispatchResult(); // Contention defers observation, never FG.
}
inline void CollectPreviewGuideOwner() noexcept {
    FlushPreviewDispatchResult(); // Also retries after the UI moved pending to ready.
    auto& owners=PreviewOwners();std::unique_lock ownerLock(owners.mutex,std::try_to_lock);
    if(!ownerLock.owns_lock())return;
    const auto owner=owners.pending.lock();
    if(!owner || !owner->lease || owner->thread!=GetCurrentThreadId())return;
    auto& preview=GenshinFgPreview::Storage();std::unique_lock previewLock(preview.mutex,std::try_to_lock);
    if(!previewLock.owns_lock() || !preview.pending)return;
    const auto& copy=*preview.pending;
    // An exact control-block join associates this ring keepalive with the real
    // submitted copy. Neither elapsed time, a retired flag, native completion
    // nor any provider fence can stand in for this copy's positive receipt.
    if(!SameGuideOwner(owner,copy.owner) || !copy.signalled || !copy.fence || !copy.fenceValue ||
       !copy.device || FAILED(copy.device->GetDeviceRemovedReason()) ||
       !Done(copy.fence.Get(),copy.fenceValue))return;
    // Only OUR completed-copy ring reference is released. Do not reset the
    // preview job/resources or either Frame/provider owner; no pool slot is
    // forcibly cleared and GPU-pending/error/unknown copies remain retained.
    const uint64_t generation=copy.stamp.generation,source=copy.stamp.sourceFrame,value=copy.fenceValue;
    const auto references=owner->lease.use_count();
    owner->lease.reset();
    previewLock.unlock();ownerLock.unlock();
    static std::atomic<unsigned> reports{0};auto report=reports.load(std::memory_order_relaxed);
    while(report<16)if(reports.compare_exchange_weak(report,report+1,std::memory_order_relaxed)) {
        LOG_INFO("R849 FG completed preview owner: pid={} generation={} source_frame={} copy_fence_value={} ring_references_before={} released=copy-keepalive-only control_block_join=true positive_copy_completion=true provider_retirement_unchanged=true",
            GetCurrentProcessId(),generation,source,value,references);
        break;
    }
}
inline bool CaptureGuidePreview(ID3D12Device* device,ID3D12CommandQueue* queue,ID3D12Resource* depth,
    ID3D12Resource* motion,const std::shared_ptr<GenshinGameMotion::Lease>& lease,GenshinFgPreview::Stamp stamp) noexcept {
    try {
        if(!FlushPreviewDispatchResult())return false; // Keep the single deferred receipt exact.
        if(!lease)return false;
        auto& preview=GenshinFgPreview::Storage();const auto now=GetTickCount64();
        const auto wanted=preview.wantedUntil.load(std::memory_order_acquire);
        const auto visible=preview.menuVisible.load(std::memory_order_acquire);
        if(!wanted || now>wanted || (visible && !visible())) {
            // Preserve disabled-demand tracing without allocating a keepalive.
            // Capture rechecks demand before every real submission.
            return GenshinFgPreview::Capture(device,queue,depth,motion,{},stamp);
        }
        auto& owners=PreviewOwners();std::unique_lock ownerLock(owners.mutex,std::try_to_lock);
        if(!ownerLock.owns_lock())return false;
        // An unresolved existing job must never be overwritten by another
        // tracker. Ordinary preview Poll may still complete/release that job.
        const auto pending=owners.pending.lock();
        if(pending && pending->lease)return false;
        {
            std::unique_lock previewLock(preview.mutex,std::try_to_lock);
            if(!previewLock.owns_lock() || (preview.pending && preview.pending->retired) ||
               (preview.lastCapture && now-preview.lastCapture<500))return false;
        }
        auto owner=std::make_shared<PreviewGuideOwner>();owner->lease=lease;
        owners.pending=owner; // install BEFORE Capture can execute any commands
        return GenshinFgPreview::Capture(device,queue,depth,motion,std::move(owner),stamp);
    }catch(...){return false;} // optional observer failure cannot change FG
}
inline void ObserveGuideOwners(const GenshinPresentNr::Ticket& ticket) noexcept {
    // One process-wide finite report budget, with sparse per-thread sampling.
    static std::atomic<unsigned> reports{0};
    static thread_local uint64_t calls=0,tick=0;
    if(reports.load(std::memory_order_relaxed)>=32)return;
    const auto now=GetTickCount64();const auto call=++calls;
    if(call>4 && call%300 && now-tick<5000)return;
    auto report=reports.load(std::memory_order_relaxed);
    do {if(report>=32)return;}
    while(!reports.compare_exchange_weak(report,report+1,std::memory_order_relaxed));
    tick=now;
    struct Row {
        bool occupied=false,previewOwner=false,previewRetired=false,previewSignalled=false;
        bool previewPositiveComplete=false;
        long references=0;
        unsigned readyRoots=0,retiredRoots=0,nativePending=0,waitPending=0,providerPending=0,completeFrames=0;
        uint64_t frameGeneration=0,frameSource=0,previewGeneration=0,previewSource=0;
    };
    std::array<Row,3> rows{};
    bool framesKnown=false,motionKnown=false,previewKnown=false;
    try {
        // Diagnostics must never block a producer/menu thread or introduce a
        // lock-order edge. Failed try_lock means UNKNOWN, never absent owner.
        auto& frames=Storage();auto& motion=GenshinGameMotion::Storage();auto& preview=GenshinFgPreview::Storage();
        {
            std::unique_lock frameLock(frames.mutex,std::try_to_lock);
            std::unique_lock motionLock(motion.mutex,std::try_to_lock);
            auto& owners=PreviewOwners();std::unique_lock ownerLock(owners.mutex,std::try_to_lock);
            std::unique_lock previewLock(preview.mutex,std::try_to_lock);
            framesKnown=frameLock.owns_lock();motionKnown=motionLock.owns_lock();previewKnown=previewLock.owns_lock();
            const auto previewOwner=ownerLock.owns_lock()?owners.pending.lock():nullptr;
            previewKnown=previewKnown && ownerLock.owns_lock();
            if(motionKnown)for(unsigned i=0;i<rows.size();++i) {
                const auto& slot=motion.slots[i];auto& row=rows[i];
                if(!slot)continue;row.occupied=true;row.references=slot.use_count();
                const auto inspect=[&](const Frame* frame,bool ready) {
                    if(!frame)return;
                    const unsigned roots=unsigned(SameGuideOwner(slot,frame->gameMotion))+
                        unsigned(SameGuideOwner(slot,frame->ticket.gameMotion));
                    if(!roots)return;
                    (ready?row.readyRoots:row.retiredRoots)+=roots;
                    row.frameGeneration=frame->ticket.source.generation;row.frameSource=frame->ticket.source.sourceFrame;
                    const bool native=!frame->recorded || Done(frame->produced.Get(),frame->producedValue);
                    const bool wait=!frame->waitIssued || (frame->waitFenced && frame->waitRetirement &&
                        frame->waitRetirement->Completed());
                    const bool provider=!frame->touchedProvider || (frame->providerRetirement && frame->providerRetirement->Retired());
                    row.nativePending+=!native;row.waitPending+=!wait;row.providerPending+=!provider;
                    row.completeFrames+=native && wait && provider;
                };
                if(framesKnown) {
                    inspect(frames.ready.get(),true);
                    // Frame reservations already cap this list at three. Keep
                    // the observation bounded even if that invariant regresses.
                    unsigned seen=0;
                    for(auto* frame=frames.retired;frame && seen<3;frame=frame->next,++seen)inspect(frame,false);
                }
                if(previewKnown && preview.pending && (SameGuideOwner(slot,preview.pending->owner) ||
                   (previewOwner && SameGuideOwner(previewOwner,preview.pending->owner) && SameGuideOwner(slot,previewOwner->lease)))) {
                    const auto& pending=*preview.pending;row.previewOwner=true;
                    row.previewGeneration=pending.stamp.generation;row.previewSource=pending.stamp.sourceFrame;
                    row.previewRetired=pending.retired;row.previewSignalled=pending.signalled;
                    row.previewPositiveComplete=pending.signalled && Done(pending.fence.Get(),pending.fenceValue);
                }
            }
        } // Release every diagnostic lock before logging.
        for(unsigned i=0;i<rows.size();++i) {
            const auto& row=rows[i];
            if(!row.occupied && motionKnown)continue;
            // A residual reference is unidentified, not proof of a leak. No
            // temporary shared_ptr copies distort the observed reference count.
            const auto observed=1+row.readyRoots+row.retiredRoots+unsigned(row.previewOwner);
            const long unassigned=motionKnown && framesKnown && previewKnown?row.references-long(observed):-1;
            LOG_INFO("R849 FG guide owner-stage: pid={} request_generation={} request_source={} slot={} motion_known={} frames_known={} preview_known={} occupied={} references={} ready_frame_roots={} retired_frame_roots={} frame_generation={} frame_source={} native_pending={} wait_pending={} provider_pending={} complete_frames={} preview_owner={} preview_generation={} preview_source={} preview_retired={} preview_signalled={} preview_positive_complete={} unassigned_references={} control_block_join=true no_authority=true",
                GetCurrentProcessId(),ticket.source.generation,ticket.source.sourceFrame,i,motionKnown,framesKnown,previewKnown,
                row.occupied,row.references,row.readyRoots,row.retiredRoots,row.frameGeneration,row.frameSource,
                row.nativePending,row.waitPending,row.providerPending,row.completeFrames,row.previewOwner,
                row.previewGeneration,row.previewSource,row.previewRetired,row.previewSignalled,row.previewPositiveComplete,unassigned);
        }
    }catch(...){} // Observation cannot change the refused allocation's outcome.
}
inline void Revoke(bool resetHistory=true,bool requestReset=false) noexcept {
    std::unique_ptr<Frame> f;
    {auto& p=Storage();std::lock_guard lock(p.mutex);f=std::move(p.ready);
        if(resetHistory) {
            const auto before=p.presented;p.presented.known=false;p.presented.resetRequested=false;
            ObserveCommit(before,p.presented,f.get(),E_PENDING,false,false,false,"Revoke");
        }
        if(requestReset)p.presented.resetRequested=true;
    }
    if(f)f->lifecycle.Revoke();Retire(std::move(f));Collect();
}

class Recording final {
    std::unique_ptr<Frame> frame_;
    explicit Recording(std::unique_ptr<Frame> f):frame_(std::move(f)){}
public:
    ~Recording(){
        try {
        // Observe only existing CPU state before the original retirement path.
        const unsigned stage=!frame_?0u:!frame_->reserved?1u:!frame_->recorded?2u:!frame_->producedValue?3u:4u;
        static thread_local uint64_t calls=0,lastReport=0,seenFailures=0,pending=0;
        static thread_local unsigned lastStage=~0u,transitions=0;
        const auto count=++calls;if(count%256==1)transitions=0;
        if(stage!=lastStage)++pending;lastStage=stage;
        const bool failure=stage>=1 && stage<=3;
        const bool firstFailure=failure && !(seenFailures&(1ull<<stage));
        if(failure)seenFailures|=1ull<<stage;
        if(count<=3 || firstFailure || count%1024==0 || (pending && transitions<4)) {
            ++transitions;
            const auto* t=frame_?&frame_->ticket:nullptr;
            LOG_INFO("R827 FG late recording: pid={} calls={} stage={} first_failure={} transitions={} since_report={} generation={} picture={} source_frame={} present_epoch={} owner_epoch={} config_epoch={} lifecycle={} no_authority=true",
                GetCurrentProcessId(),count,stage,firstFailure,pending,count-lastReport,
                t?t->source.generation:0,t?t->pictureSerial:0,t?t->source.sourceFrame:0,t?t->presentEpoch:0,
                t?t->source.ownerEpoch:0,t?t->source.configEpoch:0,frame_?static_cast<unsigned>(frame_->lifecycle.State()):0);
            pending=0;lastReport=count;
        }
        }catch(...){} // Diagnostics must never bypass the original cleanup.
        Retire(std::move(frame_));
    }
    Recording(const Recording&)=delete;Recording& operator=(const Recording&)=delete;
    // Integration seam (not inserted into NativeDriver by R504): call inside
    // the real native consumer while Scope::MatchesConsumer is true, AFTER NR
    // has consumed the same guides and before this actual cmd is submitted.
    static std::unique_ptr<Recording> Capture(ID3D12GraphicsCommandList* cmd,ID3D12CommandQueue* queue,
        const native::NativeFrame& authenticatedFrame,const native::NativeFrame& nativeFrame,const GenshinFgOffer& offer) {
        if(!offer.Known())return {};
        auto* provider=dynamic_cast<DLSSG_Dx12*>(State::Instance().currentFG);
        if(!provider || offer.provider!=reinterpret_cast<uintptr_t>(State::Instance().currentFG) ||
           offer.providerSwapchain!=Identity(State::Instance().currentFGSwapchain) ||
           State::Instance().swapchainInteropApi!=SwapchainInteropApi::Dx11wDx12)return {};
        const auto& camera=*offer.Camera();auto producer=offer.producer;
        auto* scope=GenshinPresentNr::Scope::Current();
        if(!scope || MakeKey(scope->ticket)!=MakeKey(offer.ticket))return {};
        if(!nativeFrame.guideEpoch || nativeFrame.guideEpoch==UINT64_MAX || nativeFrame.guideCutSerial==UINT64_MAX ||
           !Requested() || !scope || !scope->ticket.Known() || !scope->ticket.DepthMatches() ||
           !scope->MatchesConsumer(cmd,queue,authenticatedFrame) || (!nativeFrame.depth && !offer.ticket.depth.nativeGuide) || !camera.Matches(scope->ticket))return {};
        GenshinGuides::Owner canonicalOwner{};
        if(!producer || !producer->Live() || !producer->Guides() ||
           !GenshinGuides::OwnerIdentity::Context(scope->ticket.context.Get()).ToCanonical(producer->Owner(),canonicalOwner) ||
           !GenshinGuides::SameOwner(canonicalOwner,camera.owner) ||
           producer->Guides()->Get().textures[3].identity!=scope->ticket.source.privateOutput)return {};
        const auto gameMotion=offer.ticket.gameMotion;
        if(!gameMotion || scope->ticket.gameMotion!=gameMotion ||
           !gameMotion->Matches(canonicalOwner,scope->ticket.pictureSerial) ||
           !GenshinGuides::SameOwner(gameMotion->PreviousOwner(),camera.predecessor) ||
           nativeFrame.motion!=gameMotion->Motion() || nativeFrame.depth!=gameMotion->Depth() ||
           nativeFrame.color!=authenticatedFrame.color || nativeFrame.guideEpoch!=authenticatedFrame.guideEpoch ||
           nativeFrame.guideCutSerial!=authenticatedFrame.guideCutSerial)return {};
        if(!Config::Instance()->GenshinControlsCurrent(scope->ticket.source.configEpoch) ||
           scope->FgKind()==PictureKind::Unknown || scope->FgKind()!=CurrentPictureKind())return {};
        ComPtr<ID3D12Device> device;
        if(FAILED(queue->GetDevice(IID_PPV_ARGS(&device))) || !OnDevice(cmd,device.Get()) ||
           !OnDevice(nativeFrame.depth,device.Get()) || !OnDevice(nativeFrame.motion,device.Get()) ||
           queue->GetDesc().Type!=D3D12_COMMAND_LIST_TYPE_DIRECT || FAILED(device->GetDeviceRemovedReason()))return {};
        const auto dd=nativeFrame.depth->GetDesc(),md=nativeFrame.motion->GetDesc();
        const auto shape=[&](const D3D12_RESOURCE_DESC& d){return d.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
            d.Width==scope->ticket.pictureWidth && d.Height==scope->ticket.pictureHeight &&
            d.DepthOrArraySize==1 && d.MipLevels==1 && d.SampleDesc.Count==1 && !d.SampleDesc.Quality;};
        if(!shape(dd) || !shape(md) || dd.Format!=DXGI_FORMAT_R32_FLOAT || md.Format!=DXGI_FORMAT_R16G16B16A16_FLOAT ||
           Identity(nativeFrame.depth)==Identity(nativeFrame.motion) ||
           nativeFrame.depthReversed!=(scope->ticket.depth.convention==GenshinGuides::DepthConvention::DeviceZReversed))return {};
        const uint64_t bytes=dd.Width*dd.Height*12;
        auto f=std::make_unique<Frame>();f->bytes=bytes;
        auto recording=std::unique_ptr<Recording>(new Recording(std::move(f)));auto& s=*recording->frame_;
        {auto& p=Storage();std::lock_guard lock(p.mutex);CollectLocked(p);
            if(p.reservations>=3 || bytes>192ull*1024*1024 || p.bytes>192ull*1024*1024-bytes)return {};
            ++p.reservations;p.bytes+=bytes;s.reserved=true;}
        s.ticket=scope->ticket;s.producer=std::move(producer);s.origin=offer.Transfer();s.device=device;s.producerQueue=queue;s.producerList=cmd;
        s.gameMotion=gameMotion;
        s.sourceDepth=nativeFrame.depth;s.sourceMotion=nativeFrame.motion;s.constants=camera.constants;s.cameraLineage=camera;
        s.pictureKind=scope->FgKind();
        s.guideEpoch=nativeFrame.guideEpoch;s.guideCutSerial=nativeFrame.guideCutSerial;
        s.diagnosticDepthFormat=dd.Format;s.diagnosticDepthWidth=dd.Width;s.diagnosticDepthHeight=dd.Height;
        s.diagnosticMotionFormat=md.Format;s.diagnosticMotionWidth=md.Width;s.diagnosticMotionHeight=md.Height;
        // Certified actual PreparedMotion: native history UV converted to
        // current -> immediate predecessor picture pixels, nearest depth donor.
        s.constants.mvecScale={1.0f/s.ticket.pictureWidth,1.0f/s.ticket.pictureHeight};
        s.constants.jitterOffset={0,0};s.constants.motionVectorsJittered=sl::Boolean::eFalse;
        // The authenticated projection has no observed lens-shift/pinhole
        // offset. Zero the optional SDK field instead of forwarding its
        // INVALID_FLOAT default into the per-frame DLSSG camera constants.
        s.constants.cameraPinholeOffset={0,0};
        s.constants.motionVectors3D=sl::Boolean::eFalse;s.constants.cameraMotionIncluded=sl::Boolean::eTrue;
        // The five-candidate nearest-depth donor selection is already dilated.
        s.constants.motionVectorsDilated=sl::Boolean::eTrue;
        s.constants.motionVectorsInvalidValue=65504.0f;
        s.constants.depthInverted=nativeFrame.depthReversed?sl::Boolean::eTrue:sl::Boolean::eFalse;
        // NR's continuityReset stays pending during an authentic NoWrite. FG
        // has its own actual guide lifetime and accepted Present predecessor;
        // do not fake NR success merely to clear that unrelated NR latch.
        s.constants.reset=camera.constants.reset==sl::Boolean::eTrue || gameMotion->Discontinuity() || scope->reset
            ?sl::Boolean::eTrue:sl::Boolean::eFalse;
        s.diagnosticCameraReset=camera.constants.reset==sl::Boolean::eTrue;
        s.diagnosticScopeReset=scope->reset;
        if(const auto* trace=GenshinPresentNrTrace::Current())s.diagnosticNativeResetReasons=trace->resetReasons;
        // The normalized three-slot ring is already fenced for this exact queue.
        // Retain and consume it directly: no per-frame D3D12 committed pair/copy.
        s.depth=nativeFrame.depth;s.motion=nativeFrame.motion;
        if(!s.depth || !s.motion || !s.lifecycle.Record(MakeKey(s.ticket)))return {};
        // Mint only after all sealed offer/depth/camera/current-consumer checks.
        // GPU certificate binds both actual picture grids, the authentic game
        // motion/depth pair and the exact predecessor. No optical substitution.
        auto guides=std::shared_ptr<DLSSG_Dx12::GenshinNormalizedGuides>(
            new DLSSG_Dx12::GenshinNormalizedGuides(*provider));
        guides->key=MakeKey(s.ticket);guides->swapchain=offer.providerSwapchain;
        guides->guideEpoch=s.guideEpoch;guides->guideCutSerial=s.guideCutSerial;
        guides->device=device;guides->producerQueue=queue;
        guides->depth=s.depth;guides->motion=s.motion;guides->constants=s.constants;
        s.diagnosticProviderOwner=guides->owner; // Already visible to Recording; no provider getter.
        s.normalizedGuides=std::move(guides);
        s.recorded=true; // from this point uncertain submission retains all roots
        return recording;
    }
    // Call synchronously AFTER source.Return on that SAME native Scope. The
    // fence comes from the real producer submission, never a caller ready bool.
    bool Returned() noexcept {
        auto* scope=GenshinPresentNr::Scope::Current();if(!frame_ || !scope)return false;
        auto& s=*frame_;
        if(MakeKey(scope->ticket)!=s.lifecycle.Identity() || !scope->completionFence || !scope->completionValue ||
           !OnDevice(scope->completionFence,s.device.Get()))return false;
        s.produced=scope->completionFence;s.producedValue=scope->completionValue;
        // Join the actual snapshot before Publish or any wrapper read. Failure
        // after this point retains it in the same existing retired-frame pool.
        if(auto lease=scope->ReturnedSnapshotLease()) {
            if(!lease->ClaimFrame())return false;
            s.snapshotLease=std::move(lease);
        }
        const bool written=s.pictureKind==PictureKind::NrWritten && scope->consumerReached && scope->consumerRan && scope->screenCopyOrdered;
        const bool original=GenshinPresentNr::Original(s.pictureKind) && scope->OriginalReady() &&
            !scope->consumerReached && !scope->consumerRan && !scope->screenCopyOrdered;
        if((!written && !original) || s.pictureKind!=scope->FgKind() || s.pictureKind!=CurrentPictureKind() ||
           !s.producer || !s.producer->Live() ||
           !Config::Instance()->GenshinControlsCurrent(scope->ticket.source.configEpoch))return false;
        return s.lifecycle.Advance(MakeKey(scope->ticket),Phase::Recorded,Phase::Returned);
    }
    bool Publish() noexcept {
        if(!frame_ || frame_->lifecycle.State()!=Phase::Returned)return false;
        auto& p=Storage();std::lock_guard lock(p.mutex);
        if(p.ready)return false;p.ready=std::move(frame_);return true;
    }
};

// Lives on the real wrapper's Present stack. It observes the exact native B
// resource copied, then the exact opened shared resource -> provider backbuffer
// copy submission and successful copy-queue -> provider-queue wait.
class PresentScope final {
    inline static thread_local PresentScope* current_=nullptr;
    PresentScope* previous_=nullptr;
    ComPtr<IDXGISwapChain4> swapchain_;
    ComPtr<IDXGISwapChain> nativeSwapchain_;
    ComPtr<ID3D11DeviceContext> nativeContext_;
    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12CommandQueue> copyQueue_,presentQueue_;
    ComPtr<ID3D11Texture2D> native_,shared11_;
    ComPtr<ID3D12Resource> shared12_,color_;
    ComPtr<ID3D12Fence> copied_;
    uint64_t copyValue_=0,stateFrame_=0,copiedOrderValue_=0;
    UINT buffer_=0;uint32_t thread_=0;
    DLSSG_Dx12* provider_=nullptr;
    std::unique_ptr<Frame> frame_;
    std::shared_ptr<DrawUiMask::Frame> uiMask_;
    bool waited_=false,attempted_=false,dispatched_=false,nativeObserved_=false;
    bool prepared_=false,presented_=false,closed_=false,nativeProcessed_=false;
    uint64_t providerFrame_=0;
    Key repeatedPicture_{}; // no resources and no permission to dispatch
    bool MatchesPresentedPicture(const Key& key) const noexcept {
        const auto provider=reinterpret_cast<uintptr_t>(static_cast<IFGFeature_Dx12*>(provider_));
        const auto swapchain=Identity(swapchain_.Get()),queue=Identity(presentQueue_.Get());
        const auto kind=CurrentPictureKind();
        auto& pool=Storage();std::lock_guard lock(pool.mutex);
        const auto& last=pool.presented;
        return last.known && !last.resetRequested && last.key==key && last.kind==kind &&
            last.provider==provider && last.providerSwapchain==swapchain && last.presentQueue==queue && !pool.ready;
    }
    std::optional<DlssNr::FinishedPictureOutcome> finished_;
    GenshinPresentNr::ReturnedPicture nativeReturned_;
    Result result_=Result::NoFrame;
    void CloseProvider(bool sdkPresentSucceeded) noexcept {
        if(closed_)return;closed_=true;
        if(!frame_ || !frame_->touchedProvider)return;
        if(State::Instance().currentFG==provider_)
            // SDK return permits the existing exact input-retirement query.
            // That query still owns every real fence/signal/retirement gate.
            provider_->EndGenshinPresent(frame_->providerRetirement,sdkPresentSucceeded && dispatched_);
        // A different provider cannot sign the receipt. A real later teardown
        // of the original owner can still complete it without this raw pointer.
    }
public:
    PresentScope(IDXGISwapChain* nativeSwapchain,ID3D11DeviceContext* nativeContext,IDXGISwapChain4* sc,
                 IFGFeature_Dx12* provider,ID3D12Device* device,ID3D12CommandQueue* copyQueue)
        :swapchain_(sc),nativeSwapchain_(nativeSwapchain),nativeContext_(nativeContext),device_(device),copyQueue_(copyQueue),
         stateFrame_(State::Instance().frameCount),thread_(GetCurrentThreadId()) {
        previous_=current_;
        if(!Requested() || previous_ || !nativeSwapchain || !nativeContext || !sc || !device || !copyQueue)return;
        provider_=dynamic_cast<DLSSG_Dx12*>(provider);
        if(!provider_ || State::Instance().currentFG!=provider_ || State::Instance().currentFGSwapchain!=sc ||
           !OnDevice(copyQueue,device))return;
        presentQueue_=provider_->GameCommandQueue();
        if(!presentQueue_ || !OnDevice(presentQueue_.Get(),device) ||
           presentQueue_->GetDesc().Type!=D3D12_COMMAND_LIST_TYPE_DIRECT)return;
        current_=this;
        // Snapshot owners may become available before or during native NR. The
        // one-shot frame is removed only when the actual final copy is joined.
        auto& p=Storage();std::lock_guard lock(p.mutex);CollectLocked(p);
    }
    ~PresentScope(){
        try {
        // No provider/COM/fence query: retain the reached stages even when a
        // wrapper fallback never enters Prepare/Dispatch or Presented.
        const uint64_t flags=uint64_t(current_==this)|(uint64_t(bool(frame_))<<1)|(uint64_t(nativeObserved_)<<2)|
            (uint64_t(nativeProcessed_)<<3)|(uint64_t(waited_)<<4)|(uint64_t(prepared_)<<5)|
            (uint64_t(attempted_)<<6)|(uint64_t(dispatched_)<<7)|(uint64_t(presented_)<<8)|
            (uint64_t(Config::Instance()->FGEnabled.value_or_default())<<9)|(uint64_t(result_)<<10);
        static thread_local uint64_t calls=0,lastFlags=UINT64_MAX,lastReport=0,pending=0,seenFailures=0;
        static thread_local const char *lastPrepare=nullptr,*lastDispatch=nullptr,*lastReturn=nullptr;
        static thread_local unsigned transitions=0;
        const auto count=++calls;if(count%256==1)transitions=0;
        if(flags!=lastFlags || prepareStage_!=lastPrepare || dispatchStage_!=lastDispatch || nativeReturnStage_!=lastReturn)++pending;
        lastFlags=flags;lastPrepare=prepareStage_;lastDispatch=dispatchStage_;lastReturn=nativeReturnStage_;
        const auto result=static_cast<unsigned>(result_);
        const bool firstFailure=result_!=Result::Submitted && !(seenFailures&(1ull<<result));
        if(result_!=Result::Submitted)seenFailures|=1ull<<result;
        if(count<=3 || firstFailure || count%1024==0 || (pending && transitions<4)) {
            ++transitions;const auto& t=frame_?frame_->ticket:nativeReturned_.ticket_;
            LOG_INFO("R827 FG late scope: pid={} calls={} flags={:X} result={} prepare={} dispatch={} native_return={} first_failure={} transitions={} since_report={} generation={} picture={} source_frame={} present_epoch={} scope_frame={} owner_epoch={} config_epoch={} provider_frame={} provider={:X} no_authority=true",
                GetCurrentProcessId(),count,flags,result,prepareStage_,dispatchStage_,nativeReturnStage_,firstFailure,pending,count-lastReport,
                t.source.generation,t.pictureSerial,t.source.sourceFrame,t.presentEpoch,stateFrame_,t.source.ownerEpoch,t.source.configEpoch,
                providerFrame_,reinterpret_cast<uintptr_t>(provider_));
            pending=0;lastReport=count;
        }
        }catch(...){} // Cleanup and current-scope restoration stay outside.
        if(!presented_)CloseProvider(false);Retire(std::move(frame_));if(current_==this)current_=previous_;
    }
    PresentScope(const PresentScope&)=delete;PresentScope& operator=(const PresentScope&)=delete;
    static PresentScope* Current() noexcept {return current_;}
    Result LastResult() const noexcept {return result_;}
    void NativeProcessed() noexcept {if(current_==this)nativeProcessed_=true;}
    bool NativeWasProcessed(IDXGISwapChain* sc,ID3D11Device* d) const noexcept {
        ComPtr<ID3D11Device> actual;
        if(nativeContext_)nativeContext_->GetDevice(&actual);
        return current_==this && nativeProcessed_ && GetCurrentThreadId()==thread_ &&
            State::Instance().frameCount==stateFrame_ && Identity(sc)==Identity(nativeSwapchain_.Get()) &&
            d && Identity(d)==Identity(actual.Get());
    }
    bool HasCapturedFrame() const noexcept {
        if(current_!=this || !nativeProcessed_)return false;
        auto& p=Storage();std::lock_guard lock(p.mutex);
        return p.ready && p.ready->lifecycle.State()==Phase::Returned &&
            p.ready->ticket.presentEpoch==stateFrame_ && p.ready->ticket.source.thread==thread_ &&
            p.ready->ticket.swapchain==Identity(nativeSwapchain_.Get());
    }
    bool MatchesNative(IDXGISwapChain* nativeSwapchain,ID3D11Device* device) const noexcept {
        ComPtr<ID3D11Device> actual;
        if(nativeContext_)nativeContext_->GetDevice(&actual);
        return current_==this && Requested() && State::Instance().swapchainInteropApi==SwapchainInteropApi::Dx11wDx12 &&
            GetCurrentThreadId()==thread_ && State::Instance().frameCount==stateFrame_ &&
            State::Instance().currentFG==provider_ && Identity(State::Instance().currentFGSwapchain)==Identity(swapchain_.Get()) &&
            Identity(nativeSwapchain)==Identity(nativeSwapchain_.Get()) && Identity(device)==Identity(actual.Get());
    }
private:
    friend class GenshinLegacy::CapturePipeline;
    void ObserveRepeatedPicture(const Key& key) noexcept {
        ComPtr<ID3D11Device> device;
        if(nativeContext_)nativeContext_->GetDevice(&device);
        if(!key.Known() || !MatchesNative(nativeSwapchain_.Get(),device.Get()) ||
           nativeProcessed_ || nativeObserved_ || prepared_ || attempted_ || frame_ ||
           key.present>=stateFrame_ || key.thread!=thread_ ||
           key.swapchain!=Identity(nativeSwapchain_.Get()) || key.context!=Identity(nativeContext_.Get()) ||
           !Config::Instance()->GenshinControlsCurrent(key.configEpoch))return;
        if(MatchesPresentedPicture(key))repeatedPicture_=key;
    }
public:
    bool RepeatsPresentedPicture() const noexcept {
        return current_==this && repeatedPicture_.Known() && !frame_ && !attempted_ &&
            GetCurrentThreadId()==thread_ && State::Instance().frameCount==stateFrame_ &&
            State::Instance().currentFG==provider_ &&
            Identity(State::Instance().currentFGSwapchain)==Identity(swapchain_.Get()) && Requested() &&
            Config::Instance()->GenshinControlsCurrent(repeatedPicture_.configEpoch) &&
            MatchesPresentedPicture(repeatedPicture_);
    }
    const char* nativeReturnStage_="not-called";
    const char* NativeReturnStage() const noexcept {return nativeReturnStage_;}
    static void NativeReturned(const SenderInput& sender,ID3D12CommandQueue* actualQueue) noexcept {
        auto* p=current_;auto* s=GenshinPresentNr::Scope::Current();
        ComPtr<ID3D11Device> device;
        if(p && p->nativeContext_)p->nativeContext_->GetDevice(&device);
        if(!p)return;
        p->nativeReturnStage_="sender-or-native";
        if(!s || !sender.Known() || sender.pictureKind!=s->FgKind() || sender.pictureKind!=CurrentPictureKind() ||
           !p->MatchesNative(p->nativeSwapchain_.Get(),device.Get()) ||
           MakeKey(sender.ticket)!=MakeKey(s->ticket))return;
        p->nativeReturnStage_="consumer";
        if((sender.pictureKind==PictureKind::NrWritten
               ? (!s->consumerReached || !s->consumerRan || !s->screenCopyOrdered)
               : (!GenshinPresentNr::Original(sender.pictureKind) || !s->OriginalReady() || s->consumerReached || s->consumerRan || s->screenCopyOrdered)))return;
        p->nativeReturnStage_="completion";
        if(!s->completionFence || !s->completionValue)return;
        p->nativeReturnStage_="fence-device";
        if(!OnDevice(s->completionFence,p->device_.Get()))return;
        p->nativeReturnStage_="queue-device";
        if(!OnDevice(actualQueue,p->device_.Get()))return;
        p->nativeReturnStage_="queue-identity";
        if(Identity(actualQueue)!=Identity(p->copyQueue_.Get()))return;
        // Offers and ReturnedPicture use the State IFGFeature_Dx12 interface
        // identity. DLSSG has a virtual base; its complete-object address differs.
        p->nativeReturnStage_="provider";
        if(sender.provider!=reinterpret_cast<uintptr_t>(static_cast<IFGFeature_Dx12*>(p->provider_)) ||
           sender.providerSwapchain!=Identity(p->swapchain_.Get()))return;
        // Only actual Capture -> Return -> Publish can sign the historical
        // depth identity after the temporary NR reader has retired. A sealed
        // offer alone cannot certify that the private FG copies were recorded.
        p->nativeReturnStage_="returned-frame";
        {
            auto& pool=Storage();std::lock_guard lock(pool.mutex);
            const auto* frame=pool.ready.get();
            if(!frame || frame->lifecycle.State()!=Phase::Returned || MakeKey(frame->ticket)!=MakeKey(sender.ticket) ||
               frame->producer!=sender.producer || frame->origin!=sender.Transfer() ||
               frame->snapshotLease!=s->ReturnedSnapshotLease() ||
               Identity(frame->produced.Get())!=Identity(s->completionFence) || frame->producedValue!=s->completionValue ||
               Identity(frame->producerQueue.Get())!=Identity(actualQueue) || !frame->depth || !frame->motion)return;
        }
        auto& r=p->nativeReturned_;r.ticket_=s->ticket;r.producer_=sender.producer;r.depthCaptured_=true;
        r.producerQueue_=actualQueue;r.produced_=s->completionFence;r.producedValue_=s->completionValue;
        r.snapshot11_=s->ReturnedSnapshot();r.snapshotLease_=s->ReturnedSnapshotLease();
        r.provider_=sender.provider;r.swapchain_=sender.providerSwapchain;r.pictureKind_=sender.pictureKind;
        p->nativeReturnStage_="authenticated";
    }
    ID3D11Texture2D* ReturnedSnapshot() const noexcept {
        return current_==this && nativeProcessed_ && nativeReturned_.producer_ &&
            nativeReturned_.ticket_.presentEpoch==stateFrame_ &&
            nativeReturned_.ticket_.source.thread==GetCurrentThreadId()
            ? nativeReturned_.Snapshot11() : nullptr;
    }
    std::shared_ptr<GenshinPresentNr::SnapshotLease> ReturnedSnapshotLease() const noexcept {
        return ReturnedSnapshot()?nativeReturned_.snapshotLease_:nullptr;
    }
    const GenshinPresentNr::ReturnedPicture* NativeReturn() noexcept {
        auto& r=nativeReturned_;
        if(current_!=this || !waited_ || !nativeObserved_ || !r.producer_ || r.Kind()!=CurrentPictureKind() ||
           r.ticket_.presentEpoch!=stateFrame_ || r.ticket_.swapchain!=Identity(nativeSwapchain_.Get()) ||
           r.ticket_.source.context!=Identity(nativeContext_.Get()) ||
           Identity(r.ticket_.backbuffer.Get())!=Identity(native_.Get()) || !color_ || !copied_ || !copyValue_)return nullptr;
        D3D11_TEXTURE2D_DESC n{},s{};native_->GetDesc(&n);shared11_->GetDesc(&s);const auto d=color_->GetDesc();
        if(n.Width!=r.ticket_.pictureWidth || n.Height!=r.ticket_.pictureHeight ||
           !SdrCopy11(n,s) || !SdrCopy11To12(s,d))return nullptr;
        r.presentQueue_=presentQueue_;r.color_=color_;r.copied_=copied_;r.copiedValue_=copyValue_;r.copiedAndWaited_=true;
        return &r;
    }
    // Called by FGPresent BEFORE either FG Mutex or execution/continuity guard.
    // This clock advances once per authenticated returned frame, including provider-paused frames.
    const char* prepareStage_="not-called", *dispatchStage_="not-called";
    const char* PrepareStage() const noexcept {return prepareStage_;}
    const char* DispatchStage() const noexcept {return dispatchStage_;}
    uint64_t ObservedPresentEpoch() const noexcept {return stateFrame_;}
    static void Prepare(IDXGISwapChain* sc) {
        auto* p=current_;if(!p)return;
        p->prepareStage_="scope";
        if(p->prepared_ || Identity(sc)!=Identity(p->swapchain_.Get()) ||
            GetCurrentThreadId()!=p->thread_ || State::Instance().frameCount!=p->stateFrame_ ||
            State::Instance().currentFG!=p->provider_)return;
        // Early/cold calls cannot freeze flags before NativeCopied binds the
        // actual current returned ticket and its authenticated camera constants.
        p->prepareStage_="frame";
        if(!p->frame_ || !p->nativeObserved_ || !p->nativeProcessed_)return;
        const auto& f=*p->frame_;const auto& t=f.ticket;
        p->prepareStage_="ticket";
        if(f.lifecycle.State()!=Phase::Returned || t.presentEpoch!=p->stateFrame_ ||
           t.source.thread!=p->thread_ || t.swapchain!=Identity(p->nativeSwapchain_.Get()) ||
           t.source.context!=Identity(p->nativeContext_.Get()) || Identity(t.backbuffer.Get())!=Identity(p->native_.Get()) ||
           !f.cameraLineage.Matches(t) ||
           (f.constants.depthInverted==sl::Boolean::eTrue)!=(t.depth.convention==GenshinGuides::DepthConvention::DeviceZReversed) ||
           f.constants.motionVectorsJittered!=sl::Boolean::eFalse || State::Instance().hdrOutputActive)return;
        p->prepared_=true;
        FG_Constants fg{};
        DXGI_SWAP_CHAIN_DESC desc{};
        p->prepareStage_="descriptor";
        if(FAILED(sc->GetDesc(&desc)))return;
        fg.displayWidth=desc.BufferDesc.Width;fg.displayHeight=desc.BufferDesc.Height;
        fg.flags|=FG_Flags::DisplayResolutionMVs;
        if(f.constants.depthInverted==sl::Boolean::eTrue)
            fg.flags|=FG_Flags::InvertedDepth;
        p->prepareStage_="begin-provider";
        p->providerFrame_=p->provider_->BeginGenshinPresent(p->device_.Get(),p->swapchain_.Get(),
            p->presentQueue_.Get(),p->stateFrame_,fg);
        p->prepareStage_=p->providerFrame_?"ready":"begin-provider-zero";
        Collect();
    }
    static void Skip() {
        const bool suspend=current_ && State::Instance().currentFG==current_->provider_;
        const unsigned stage=!current_?0u:!suspend?1u:current_->attempted_?2u:3u;
        static thread_local uint64_t calls=0,lastReport=0,pending=0,seen=0;
        static thread_local unsigned lastStage=~0u,transitions=0;
        const auto count=++calls;if(count%256==1)transitions=0;
        if(stage!=lastStage)++pending;lastStage=stage;
        const bool firstFailure=!(seen&(1ull<<stage));seen|=1ull<<stage;
        if(count<=3 || firstFailure || count%1024==0 || (pending && transitions<4)) {
            ++transitions;
            LOG_INFO("R827 FG late skip: pid={} calls={} stage={} first_failure={} transitions={} since_report={} prepare={} dispatch={} scope_frame={} provider_frame={} no_authority=true",
                GetCurrentProcessId(),count,stage,firstFailure,pending,count-lastReport,
                current_?current_->prepareStage_:"no-scope",current_?current_->dispatchStage_:"no-scope",
                current_?current_->stateFrame_:0,current_?current_->providerFrame_:0);
            pending=0;lastReport=count;
        }
        if(current_ && State::Instance().currentFG==current_->provider_)
            current_->provider_->SuspendGenshinPresent();
    }

    void NativeCopied(ID3D11Texture2D* source,ID3D11Texture2D* shared) noexcept {
        if(current_!=this || !source || !shared || nativeObserved_)return;
        nativeObserved_=true;native_=source;shared11_=shared;
        // The NR-returned snapshot must predate this very native B copy.
        // A publication made later in _ApplyNativeInput is not retroactively
        // authoritative over a raw color copy recorded before NR ran.
        {auto& p=Storage();std::lock_guard lock(p.mutex);frame_=std::move(p.ready);}
        uiMask_=DrawUiMask::Acquire(nativeContext_.Get(),source,device_.Get());
        if(frame_ && (frame_->ticket.presentEpoch!=stateFrame_ ||
           frame_->ticket.swapchain!=Identity(nativeSwapchain_.Get()) ||
           Identity(frame_->ticket.backbuffer.Get())!=Identity(source))) {
            frame_->lifecycle.Revoke();Retire(std::move(frame_));result_=Result::WrongTicket;
        }
    }
    // Existing wrapper list, after final color copy and before its Close.
    void BuildUiHudless(ID3D12GraphicsCommandList* list,ID3D12Resource* color) noexcept {
        if(current_!=this || !frame_ || !uiMask_ || !list || !color)return;
        frame_->uiMask=uiMask_;
        auto* result=DrawUiMask::BuildHudless(uiMask_,list,color,D3D12_RESOURCE_STATE_PRESENT);
        if(result){frame_->uiHudless=result;frame_->uiRecorded=true;}
    }
    void Copied(const std::shared_ptr<const ColorImport>& imported,ID3D12Resource* opened,ID3D12Resource* color,
                UINT buffer,ID3D12Fence* fence,uint64_t value) noexcept {
        if(current_!=this || !native_ || !shared11_ || !opened || !color || !fence || !value ||
           !imported || !imported->Matches(shared11_.Get(),opened) ||
           !OnDevice(opened,device_.Get()) || !OnDevice(color,device_.Get()) || !OnDevice(fence,device_.Get()))return;
        copiedOrderValue_=0;
        shared12_=opened;color_=color;buffer_=buffer;copied_=fence;copyValue_=value;
        if(frame_ && frame_->uiRecorded){frame_->interopCopied=fence;frame_->uiCopyValue=value;}
    }
    void Waited(ID3D12CommandQueue* queue,ID3D12Fence* fence,uint64_t value) noexcept {
        copiedOrderValue_=0;
        waited_=current_==this && Identity(queue)==Identity(presentQueue_.Get()) &&
            Identity(fence)==Identity(copied_.Get()) && value==copyValue_ && value;
    }
    // Native wrapper only: the actual color Execute and successful copied-fence
    // Signal precede this call on the exact presenting queue. This is order,
    // not GPU completion and not a claim that a copied-fence Wait ran.
    void CopiedOrderedOnPresentQueue(ID3D12CommandQueue* queue,ID3D12Fence* fence,uint64_t value) noexcept {
        Waited(queue,fence,value);
        if(waited_)copiedOrderValue_=value;
    }
    bool CopiedOrderCovers(ID3D12CommandQueue* queue,ID3D12Fence* fence,uint64_t value) const noexcept {
        if(current_!=this || !waited_ || GetCurrentThreadId()!=thread_ ||
           State::Instance().frameCount!=stateFrame_ || State::Instance().currentFG!=provider_ ||
           !copiedOrderValue_ || copiedOrderValue_==UINT64_MAX || copiedOrderValue_!=copyValue_ ||
           !value || value==UINT64_MAX || value>copiedOrderValue_)return false;
        const auto q=Identity(queue),f=Identity(fence);
        return q && f && q==Identity(presentQueue_.Get()) && f==Identity(copied_.Get());
    }
    // The real Bridge invocation returns its own bounded result. A later generic
    // non-bridge call must not overwrite it, including a generic NoWrite.
    static bool Finished(const DlssNr::FinishedPictureOutcome& outcome) noexcept {
        auto* p=current_;if(!p || p->finished_ || !outcome.bridgeEndpoint || !outcome.endpointSerial ||
            outcome.thread!=p->thread_ || outcome.owner!=Identity(p->swapchain_.Get()) ||
            outcome.device!=Identity(p->device_.Get()) || outcome.presentEpoch!=p->stateFrame_ ||
            outcome.swapchain!=Identity(p->swapchain_.Get()) || outcome.color!=Identity(p->color_.Get()) ||
            outcome.queue!=Identity(p->presentQueue_.Get()))return false;
        p->finished_=outcome;return true;
    }
    bool TryPresent(const DlssNr::FinishedPictureOutcome& outcome) {
        dispatchStage_="entry";
        if(current_!=this || attempted_)return false;attempted_=true;
        dispatchStage_="scope-clock-color-wait";
        if(!Requested() || GetCurrentThreadId()!=thread_ || State::Instance().frameCount!=stateFrame_ || !waited_ || !color_)
            {result_=Result::WrongTicket;return false;}
        dispatchStage_="frame";
        if(!frame_)return false;
        auto& f=*frame_;const auto& t=f.ticket;
        dispatchStage_="ticket-ownership";
        if(!f.producer || !f.origin || !GenshinGuides::SameOwner(f.origin->Get().owner,f.producer->Owner()) ||
           f.lifecycle.State()!=Phase::Returned || f.pictureKind!=CurrentPictureKind() || t.presentEpoch!=stateFrame_ || t.source.thread!=thread_ ||
           !f.producer || !f.producer->Live() ||
           t.swapchain!=Identity(nativeSwapchain_.Get()) || t.source.context!=Identity(nativeContext_.Get()) ||
           Identity(t.backbuffer.Get())!=Identity(native_.Get()) || !t.DepthIdentityMatches() ||
           !Config::Instance()->GenshinControlsCurrent(t.source.configEpoch) ||
           Identity(f.device.Get())!=Identity(device_.Get()) || swapchain_->GetCurrentBackBufferIndex()!=buffer_)
            {result_=Result::WrongTicket;return false;}
        D3D11_TEXTURE2D_DESC nd{},sd{};native_->GetDesc(&nd);shared11_->GetDesc(&sd);const auto cd=color_->GetDesc();
        ComPtr<ID3D11Device> nativeDevice;native_->GetDevice(&nativeDevice);
        ComPtr<ID3D12Resource> currentColor;
        dispatchStage_="resource";
        if(Identity(nativeDevice.Get())!=t.source.device ||
           FAILED(swapchain_->GetBuffer(buffer_,IID_PPV_ARGS(&currentColor))) || Identity(currentColor.Get())!=Identity(color_.Get()))return false;
        // This candidate only admits exact whole-resource copies. MSAA resolve,
        // crop/flip/format conversion require their own real mapping receipt.
        dispatchStage_="mapping";
        if(nd.Width!=t.pictureWidth || nd.Height!=t.pictureHeight ||
           !SdrCopy11(nd,sd) || !SdrCopy11To12(sd,cd))
            {result_=Result::MappingMismatch;return false;}
        dispatchStage_="outcome-key";
        if(!finished_ || !outcome.bridgeEndpoint || !outcome.endpointSerial || outcome.thread!=thread_ ||
           outcome.endpointSerial!=finished_->endpointSerial || outcome.producerQueue!=Identity(copyQueue_.Get()) ||
           outcome.owner!=Identity(swapchain_.Get()) || outcome.device!=Identity(device_.Get()) ||
           outcome.presentEpoch!=stateFrame_ || outcome.swapchain!=Identity(swapchain_.Get()) ||
           outcome.color!=Identity(color_.Get()) || outcome.queue!=Identity(presentQueue_.Get()))
            {result_=Result::UnknownOutcome;return false;}
        const bool written=f.pictureKind==PictureKind::NrWritten && !outcome.original && outcome.Written() &&
            outcome.fence && OnDevice(outcome.fence.Get(),device_.Get()) && outcome.submissionSerial && outcome.fenceValue;
        // Generic disabled/NoWrite and any uncertain/failed NR call have no
        // original receipt. NR completion fields stay empty on this branch.
        const auto* original=outcome.original.get();
        const bool unchanged=GenshinPresentNr::Original(f.pictureKind) && original &&
            outcome.result==DlssNr::FinishedPictureResult::NoWrite && !outcome.fence && !outcome.fenceValue && !outcome.submissionSerial &&
            outcome.reason==(f.pictureKind==PictureKind::OriginalNrDisabled
                ?DlssNr::FinishedPictureReason::Disabled:DlssNr::FinishedPictureReason::ModelDisabled) &&
            original->Kind()==f.pictureKind && MakeKey(original->PictureTicket())==MakeKey(t) &&
            original->Matches(swapchain_.Get(),presentQueue_.Get(),copyQueue_.Get(),color_.Get(),device_.Get(),
                stateFrame_,reinterpret_cast<uintptr_t>(static_cast<IFGFeature_Dx12*>(provider_))) &&
            Identity(original->ProducerFence())==Identity(f.produced.Get()) && original->ProducerValue()==f.producedValue &&
            Identity(original->Fence())==Identity(copied_.Get()) && original->Value()==copyValue_;
        dispatchStage_="outcome-kind";
        if(!written && !unchanged){result_=Result::UnknownOutcome;return false;}
        // Only a real NR write owns an NR completion fence. The original path
        // below still waits the actual guide producer and actual color copy.
        if(written)f.nrFinished=outcome.fence;
        dispatchStage_="fence-reserve";
        if(!OnDevice(f.produced.Get(),device_.Get()))return false;
        f.finalColor=color_;f.providerQueue=presentQueue_;f.importedColor=shared12_;f.sharedColor11=shared11_;f.interopCopied=copied_;
        // The producer Wait and retirement Signal stay serialized on the
        // provider owner. The native scope may already prove exact copied order.
        // Even partial failure publishes its receipt before submission and
        // retains these exact resources and fence owner.
        dispatchStage_="queue-wait-signal";
        f.waitFenced=provider_->WaitGenshinInputs(device_.Get(),swapchain_.Get(),presentQueue_.Get(),
            f.produced.Get(),f.producedValue,copied_.Get(),copyValue_,f.waitRetirement);
        f.waitIssued=bool(f.waitRetirement);
        if(!f.waitFenced)return false;
        dispatchStage_="lifecycle-copy";
        if(!f.lifecycle.Advance(MakeKey(t),Phase::Returned,Phase::Copied))return false;
        auto guard=provider_->GuardExecution(FGExecutionScope::Inputs);
        dispatchStage_="provider-unprepared";
        if(!prepared_)return false;
        dispatchStage_="provider-frame-zero";
        if(!providerFrame_)return false;
        dispatchStage_="provider-frame-mismatch";
        if(provider_->FrameCount()!=providerFrame_)return false;
        dispatchStage_="provider-submit-blocked";
        if(!provider_->CanSubmitInputs())return false;
        dispatchStage_="provider-inactive";
        if(!provider_->IsActive())return false;
        dispatchStage_="provider-paused";
        if(provider_->IsPaused())return false;
        const auto index=provider_->GetIndex();
        // This changes the actual constants consumed by DLSSG. Submission,
        // frame labels and NR reset acknowledgement cannot sign a predecessor.
        auto constants=f.constants;
        {auto& pool=Storage();std::lock_guard lock(pool.mutex);
            Key observedCurrent{};const bool continues=pool.presented.Continues(f,&observedCurrent);
            ObserveContinuity(pool.presented,f,observedCurrent,continues,providerFrame_);
            const auto action=DecideContinuity(pool.presented.known,continues,pool.presented.resetRequested);
            if(action==ContinuityAction::Reject) {
                // A continuing result without submitted history is internally
                // inconsistent. An ordinary stale predecessor instead takes the
                // current-frame Reset path above; neither case reuses old input.
                dispatchStage_="GAME_MOTION_PREDECESSOR_GAP";result_=Result::Revoked;return false;
            }
            if(action==ContinuityAction::Seed || action==ContinuityAction::Reset) {
                constants.reset=sl::Boolean::eTrue;
                if(action==ContinuityAction::Reset)dispatchStage_="discontinuity-reset";
            }}
        dispatchStage_="constants";
        f.providerRetirement=provider_->SetGenshinConstants(f.normalizedGuides,MakeKey(t),providerFrame_,
            constants.reset==sl::Boolean::eTrue);
        if(!f.providerRetirement)return false;
        f.touchedProvider=true; // receipt armed before the first partial tag
        provider_->SetInterpolationRect(t.pictureWidth,t.pictureHeight);
        const auto set=[&](FG_ResourceType type,ID3D12Resource* resource,D3D12_RESOURCE_STATES state) {
            const auto d=resource->GetDesc();Dx12Resource input{};input.type=type;input.resource=resource;
            input.width=d.Width;input.height=d.Height;input.state=state;input.frameIndex=index;
            input.validity=FG_ResourceValidity::UntilPresent;return provider_->SetGenshinResource(f.providerRetirement,&input);
        };
        dispatchStage_="tag";
        // Lease::EndRead restores these shared ring guides to COMMON on the
        // submitted native producer list. Streamline must receive that actual
        // handoff state, not the temporary state used by the native reader.
        if(!set(FG_ResourceType::Depth,f.depth.Get(),D3D12_RESOURCE_STATE_COMMON) ||
           !set(FG_ResourceType::Velocity,f.motion.Get(),D3D12_RESOURCE_STATE_COMMON) ||
           !set(FG_ResourceType::PresentColor,color_.Get(),D3D12_RESOURCE_STATE_PRESENT))
            {provider_->Deactivate();result_=Result::ProviderRefused;return false;}
        // No replacement of depth/velocity/PresentColor or selected FG route.
        // Optional exact-mask input; unsupported/disabled HUDless policy keeps
        // the existing depth/velocity/PresentColor path and selected FG route.
        if(f.uiHudless)provider_->SetGenshinUiMask(f.providerRetirement,f.uiMask);
        static unsigned stateReports=0;if(stateReports++<8)
            LOG_INFO("R842 FG guide handoff: pid={} generation={} provider_frame={} depth_ptr={} depth_format={} depth_extent={}x{} motion_ptr={} motion_format={} motion_extent={}x{} resource_distinct={} source_frame={} picture={} owner_epoch={} config_epoch={} history_epoch={} present_epoch={} guide_epoch={} guide_cut={} provider_owner={} provider_input_epoch=UNKNOWN depth_state=0 motion_state=0 native_reader_restored_common=true tags_accepted=true gpu_read=UNKNOWN pixel_correct=UNKNOWN generated_frame=UNKNOWN visual=UNKNOWN",
                GetCurrentProcessId(),t.source.generation,providerFrame_,
                static_cast<const void*>(f.depth.Get()),static_cast<unsigned>(f.diagnosticDepthFormat),
                f.diagnosticDepthWidth,f.diagnosticDepthHeight,
                static_cast<const void*>(f.motion.Get()),static_cast<unsigned>(f.diagnosticMotionFormat),
                f.diagnosticMotionWidth,f.diagnosticMotionHeight,f.depth.Get()!=f.motion.Get(),
                t.source.sourceFrame,t.pictureSerial,t.source.ownerEpoch,t.source.configEpoch,t.source.historyEpoch,
                t.presentEpoch,f.guideEpoch,f.guideCutSerial,f.diagnosticProviderOwner);
        // Observe the exact two accepted tags, with the same queue ordering.
        // A private readback restores COMMON before any provider consumption.
        const bool preview=CaptureGuidePreview(device_.Get(),presentQueue_.Get(),f.depth.Get(),f.motion.Get(),f.gameMotion,
            {t.source.generation,t.source.sourceFrame,t.pictureSerial,providerFrame_,0,static_cast<unsigned>(f.pictureKind),false});
        static unsigned previewReports=0;if(preview && previewReports++<8)
            LOG_INFO("R843 FG preview copy: pid={} generation={} source_frame={} picture={} provider_frame={} accepted_tags=true state=COMMON async=true visual=UNKNOWN",
                GetCurrentProcessId(),t.source.generation,t.source.sourceFrame,t.pictureSerial,providerFrame_);
        dispatchStage_="lifecycle-tag";
        if(!f.lifecycle.Advance(MakeKey(t),Phase::Copied,Phase::Tagged)){provider_->Deactivate();return false;}
        dispatchStage_="provider-present";
        dispatched_=provider_->Present();
        if(preview)RecordPreviewDispatchResult(t.source.generation,providerFrame_,dispatched_);
        static unsigned submitReports=0;static unsigned previousKind=0;static uint64_t submitSamples=0;
        const auto kind=static_cast<unsigned>(f.pictureKind);++submitSamples;
        if(submitReports<8 || previousKind!=kind || submitSamples%300==0) {
            ++submitReports;previousKind=kind;
            LOG_INFO("R843 FG independent submit: pid={} generation={} source_frame={} picture={} provider_frame={} picture_kind={} original={} submitted={} nr_written={} original_receipt={} visual=UNKNOWN",
                GetCurrentProcessId(),t.source.generation,t.source.sourceFrame,t.pictureSerial,providerFrame_,kind,
                GenshinPresentNr::Original(f.pictureKind),dispatched_,written,unchanged);
        }
        if(!dispatched_){provider_->Deactivate();result_=Result::ProviderRefused;return false;}
        f.lifecycle.Advance(MakeKey(t),Phase::Tagged,Phase::Dispatched);result_=Result::Submitted;dispatchStage_="submitted";return true;
    }
    static bool Dispatch(const DlssNr::FinishedPictureOutcome& outcome) {
        const bool submitted=current_ && current_->TryPresent(current_->finished_?*current_->finished_:outcome);
        if(!submitted)Skip(); // no old On/options may consume a missing current frame
        return submitted;
    }
    void Presented(HRESULT result,bool continuityValid=true,bool sdkReturned=false) noexcept {
        if(presented_)return;
        // sdkReturned belongs to this scope's same-thread saved SDK call, not
        // an outer wrapper HRESULT, physical tail, or provider submission alone.
        const bool sdkSucceeded=sdkReturned && current_==this && GetCurrentThreadId()==thread_ &&
            result==S_OK && frame_ && dispatched_ && frame_->lifecycle.State()==Phase::Dispatched;
        // HWND Show acknowledgement is overlay state, not provider history.
        // Only this dispatch, the real child S_OK and post-present continuity
        // may commit the predecessor consumed by the next engine frame.
        const bool commit=frame_ && dispatched_ && result==S_OK && continuityValid &&
            frame_->lifecycle.State()==Phase::Dispatched;
        const auto committedProvider=reinterpret_cast<uintptr_t>(static_cast<IFGFeature_Dx12*>(provider_));
        const auto committedSwapchain=commit?Identity(swapchain_.Get()):0;
        const auto committedQueue=commit?Identity(presentQueue_.Get()):0;
        {auto& pool=Storage();std::lock_guard lock(pool.mutex);
            const auto before=pool.presented;
            if(commit) {
                pool.presented.Commit(*frame_);pool.presented.resetRequested=false;
                pool.presented.provider=committedProvider;
                pool.presented.providerSwapchain=committedSwapchain;
                pool.presented.presentQueue=committedQueue;
            }
            else if(dispatched_) {pool.presented.known=false;pool.presented.resetRequested=false;}
            // No current dispatch cannot invalidate an earlier real submission.
            // Terminal device/epoch/queue paths explicitly Revoke beforehand.
            ObserveCommit(before,pool.presented,frame_.get(),result,continuityValid,dispatched_,commit,"Presented");}
        if(commit)
            frame_->lifecycle.Advance(MakeKey(frame_->ticket),Phase::Dispatched,Phase::Presented);
        else if(frame_){
            frame_->lifecycle.Revoke(); // no physical receipt: no displayed predecessor
            if(!sdkSucceeded)result_=Result::PresentFailed;
        }
        // A returned SDK transaction can still have no synchronous physical
        // output. Query its exact input receipt without committing display or
        // treating CPU return as completion. The default keeps legacy callers'
        // commit-only closure; the visible wrapper supplies its observed bracket.
        const bool retirementQuery=commit || sdkSucceeded;
        if(sdkReturned && dispatched_){
            static thread_local uint64_t lastTick=0,calls=0;
            static thread_local unsigned transitions=0;
            static thread_local bool lastCommit=false,lastQuery=false;
            const auto now=GetTickCount64();const bool initial=++calls<=4;
            if(now-lastTick>=1000)transitions=0;
            const bool changed=commit!=lastCommit || retirementQuery!=lastQuery;
            if(initial || now-lastTick>=5000 || (changed && transitions<4)){
                if(!transitions)lastTick=now;
                ++transitions;lastCommit=commit;lastQuery=retirementQuery;
                const auto& t=frame_->ticket;
                LOG_INFO("R1069 sdk-closure: pid={} thread={} provider_frame={} present_epoch={} source_generation={} source_frame={} picture={} sdk_returned={} sdk_hr={:X} retirement_requested={} physical_continuity={} predecessor_commit={} completion_authority=false visual=UNKNOWN",
                    GetCurrentProcessId(),GetCurrentThreadId(),providerFrame_,stateFrame_,t.source.generation,
                    t.source.sourceFrame,t.pictureSerial,sdkReturned,static_cast<unsigned>(result),retirementQuery,
                    continuityValid,commit);
            }
        }
        presented_=true;CloseProvider(retirementQuery);
    }
};
}

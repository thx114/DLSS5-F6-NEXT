// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "GenshinFsrSubstageProgress.h"
#include "GenshinPresentNrTrace.h"
#include <native/FrameContract.h>
#include <native/NativeProducer.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <memory>
#include <new>
#include <mutex>
#include <array>
#include <Config.h>
#include <State.h>
#include <integration/HoYoShade.h>
#include <genshin/GenshinProducerTransport.h>

class Dx11wDx12SC;
namespace GenshinBridgeFg { class PresentScope; class ColorImport; class Recording; struct Frame; }
namespace native { class NativeDriver; class Dx11FrameSource; }

// An authorization to reuse the existing final-picture native driver. Raw FSR
// depth/MV and NativeBoundedIdentity never acquire present-picture authority.
namespace GenshinPresentNr
{
struct TemporalGuideFacts {
    uint64_t guideEpoch=0,guideCutSerial=0;
    bool reset=false;
};
inline uintptr_t Identity(IUnknown* object) noexcept {
    Microsoft::WRL::ComPtr<IUnknown> identity;
    return object && SUCCEEDED(object->QueryInterface(IID_PPV_ARGS(&identity)))
        ? reinterpret_cast<uintptr_t>(identity.Get()):0;
}
struct Source {
    enum class Transfer : uint8_t { Copy, NativeOverlay };
    uintptr_t device=0,context=0,privateOutput=0,selectedOutput=0;
    uint64_t deviceEpoch=0,ownerEpoch=0,configEpoch=0,instance=0,generation=0,sourceFrame=0;
    uint32_t thread=0,width=0,height=0,selectedSubresource=0;
    GenshinSubstage::Key progress{};
    // Submission/copy are command-order receipts. GPU readiness is retirement
    // evidence only; its later observation must not invalidate a Scene witness.
    uint64_t submitSerial=0,copyOrderedSerial=0,gpuReadySerial=0;
    // Optional depth epoch; do not change existing SR/cadence/reset admission.
    uint64_t historyEpoch=0;
    // Observation-only tail: preserve all existing aggregate/order and route
    // gates. Missing telemetry cannot reject stable SR or motion-only NR.
    uint64_t attempt=0;
    // Source identity and display-grid geometry survive a reviewed pointwise
    // colour operation; this does not claim byte equality with privateOutput.
    Transfer transfer=Transfer::Copy;
    bool NoticeKeyMatches() const noexcept {
        return progress.Known() && attempt && progress.ownerEpoch==ownerEpoch && progress.context==context &&
            progress.session.deviceEpoch==deviceEpoch && progress.session.controlsEpoch==configEpoch &&
            progress.session.processId==GetCurrentProcessId() &&
            progress.session.processStart==GenshinLegacy::Ui::ProcessStartIdentity();
    }
    bool Known() const noexcept {
        return device && context && privateOutput && selectedOutput && deviceEpoch && ownerEpoch && configEpoch &&
            instance && generation && thread && width && height && submitSerial && copyOrderedSerial &&
            (transfer==Transfer::Copy || transfer==Transfer::NativeOverlay);
    }
};
// The private successful SR receipt can survive native draws for guarded paired
// repair. Its identity alone does not prove the selected game's current pixels.
// This separate scalar receipt owns no GPU resource and never retires a lease.
class SelectedContentAuthority final {
    Source copied_{};
    uint64_t nativeDraw_=0;
public:
    void Revoke() noexcept {copied_={};}
    void NativeAccumulate(uint64_t draw) noexcept {Revoke();nativeDraw_=draw;}
    // Call only after an actual guarded SR/paired copy or reviewed colour transfer into the exact selected
    // subresource. Poll/readiness, CPU params and Scene observation cannot renew.
    void Copied(const Source& source) noexcept {copied_=source.Known()?source:Source{};}
    bool Matches(const Source& source) const noexcept {
        const auto& a=copied_;const auto& b=source;
        return a.Known() && b.Known() && a.device==b.device && a.context==b.context &&
            a.privateOutput==b.privateOutput && a.selectedOutput==b.selectedOutput && a.transfer==b.transfer &&
            a.deviceEpoch==b.deviceEpoch && a.ownerEpoch==b.ownerEpoch && a.configEpoch==b.configEpoch &&
            a.instance==b.instance && a.generation==b.generation && a.sourceFrame==b.sourceFrame &&
            a.thread==b.thread && a.width==b.width && a.height==b.height &&
            a.selectedSubresource==b.selectedSubresource && a.progress==b.progress &&
            a.submitSerial==b.submitSerial && a.copyOrderedSerial==b.copyOrderedSerial;
    }
    uint64_t NativeDraw() const noexcept {return nativeDraw_;}
    uint64_t CopySerial() const noexcept {return copied_.copyOrderedSerial;}
};
// The hook supplies countKnown only for direct or single-instance draws.
// Auto/indirect/multi-instance draws have no proved shape and remain refused.
inline bool PictureDrawShape(unsigned kind,uint32_t count,bool known,bool scene) noexcept {
    return known && kind>=1 && kind<=4 && count==(scene?3u:6u);
}
struct RepairWindow {
    bool nativePending=false,nativeCopyOrdered=false,nativeEnded=false,sameOwner=false;
    bool transportPending=true,capturePending=false,repairOccupied=false,queryFailed=false;
    bool Allows() const noexcept {
        return nativePending && nativeCopyOrdered && nativeEnded && sameOwner &&
            !transportPending && !capturePending && !repairOccupied && !queryFailed;
    }
};
// A single bounded extra lease for a paired copy. Native completion/rawLease
// remain untouched. Allocation failure occurs before commands; failed polling
// retains every owner until successful retirement or confirmed device removal.
class OrderedCopyLease final {
    Microsoft::WRL::ComPtr<ID3D11Query> query_;
    Microsoft::WRL::ComPtr<ID3D11Resource> source_,destination_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    std::shared_ptr<void> bindings_;
    bool ended_=false,failed_=false;
public:
    enum class State { Empty, Pending, Ready, Failed };
    bool Occupied() const noexcept {return query_!=nullptr || bool(bindings_);}
    bool Arm(ID3D11Device* device,ID3D11DeviceContext* context,ID3D11Resource* source,
             ID3D11Resource* destination,std::shared_ptr<void> bindings) noexcept {
        if(Occupied() || !device || !context || context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE ||
           !source || !destination || !bindings)return false;
        Microsoft::WRL::ComPtr<ID3D11Query> next;const D3D11_QUERY_DESC desc{D3D11_QUERY_EVENT,0};
        // Arm before the native draw. Allocation failure has issued no work
        // under this lease and leaves the original native lease/query intact.
        if(FAILED(device->CreateQuery(&desc,&next)))return false;
        query_=std::move(next);source_=source;destination_=destination;context_=context;
        bindings_=std::move(bindings);ended_=failed_=false;return true;
    }
    void End() noexcept {
        if(query_ && !ended_){context_->End(query_.Get());ended_=true;}
    }
    State Poll() noexcept {
        if(failed_)return State::Failed;
        if(!query_)return State::Empty;
        if(!ended_)return State::Pending;
        BOOL done=FALSE;const auto hr=context_->GetData(query_.Get(),&done,sizeof(done),D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if(FAILED(hr)){failed_=true;return State::Failed;}
        if(hr==S_FALSE || !done)return State::Pending;
        query_.Reset();source_.Reset();destination_.Reset();context_.Reset();bindings_.reset();return State::Ready;
    }
    void DeviceRemoved() noexcept {
        // Caller must prove device removal; resize/revoke never calls this.
        query_.Reset();source_.Reset();destination_.Reset();context_.Reset();bindings_.reset();ended_=failed_=false;
    }
};
// A freshness receipt for actual returned native picture draws. This owns no
// old NR result: every admitted Present recaptures its current backbuffer.
// The CPU Render/SR token clock may advance less often than this picture clock.
class PictureCadence final {
    Source source_{};
    Microsoft::WRL::ComPtr<ID3D11Texture2D> intermediate_,picture_;
    uint32_t intermediateSubresource_=0,pictureSubresource_=0;
    uint64_t sceneDraw_=0,pictureDraw_=0,consumedDraw_=0,presentEpoch_=0;
    uint64_t disjointScenes_=0;
    bool pictureAfterScene_=false;
    uint64_t lastSceneDraw_=0,lastPictureDraw_=0;
    uintptr_t lastSceneInput_=0,lastSceneOutput_=0,lastPictureInput_=0,lastPictureOutput_=0;
    const char* lastSceneReason_="not-observed";
    const char* lastPictureReason_="not-observed";
    mutable uint64_t lastTracedDisjointScenes_=0;
    static bool Same(const Source& a,const Source& b) noexcept {
        return a.Known() && b.Known() && a.device==b.device && a.context==b.context &&
            a.deviceEpoch==b.deviceEpoch && a.ownerEpoch==b.ownerEpoch && a.configEpoch==b.configEpoch &&
            a.instance==b.instance && a.generation==b.generation && a.privateOutput==b.privateOutput && a.transfer==b.transfer &&
            a.selectedOutput==b.selectedOutput && a.selectedSubresource==b.selectedSubresource && a.thread==b.thread &&
            a.submitSerial==b.submitSerial && a.copyOrderedSerial==b.copyOrderedSerial;
    }
public:
    // Revocation may arrive from a controls thread. Keep COM owners retained;
    // only the owning native draw callback replaces them, or Storage retires.
    void Revoke() noexcept {source_={};sceneDraw_=pictureDraw_=0;}
    // Call only on the owning draw/Present thread. Retained identity witnesses
    // are CPU references, not asynchronous readbacks; release before resize.
    void ClearOwner() noexcept {Revoke();intermediate_.Reset();picture_.Reset();}
    void RejectUnfresh(const Source& source) noexcept {
        // A duplicate/menu-only Present consumes no new native picture. Keep
        // only the same owner's S -> A witness; B is never retained/replayed.
        if(!Same(source_,source) || source.thread!=GetCurrentThreadId() || !sceneDraw_ || !intermediate_)
            {ClearOwner();return;}
        picture_.Reset();pictureDraw_=0;
    }
    void Scene(const Source& source,ID3D11Texture2D* input,uint32_t inputSubresource,
               ID3D11Texture2D* output,uint32_t outputSubresource,uint64_t draw) noexcept {
        // Another draw can use the same scene shader without belonging to
        // S -> A -> B. Its different input must not erase an already completed
        // current picture if its actual output cannot write any retained edge.
        // Compare canonical texture identities, never SRV/RTV addresses. Treat
        // all subresources of S/A/B as aliases here: uncertainty still revokes.
        const auto inputId=Identity(input),outputId=Identity(output);
        lastSceneDraw_=draw;lastSceneInput_=inputId;lastSceneOutput_=outputId;
        lastSceneReason_="rejected";
        Microsoft::WRL::ComPtr<ID3D11Device> inputDevice,outputDevice;
        if(input)input->GetDevice(&inputDevice);
        if(output)output->GetDevice(&outputDevice);
        D3D11_TEXTURE2D_DESC outputDesc{};if(output)output->GetDesc(&outputDesc);
        const bool ordinaryOutput=outputDesc.Width && outputDesc.Height &&
            !(outputDesc.MiscFlags&(D3D11_RESOURCE_MISC_SHARED|D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX|
                D3D11_RESOURCE_MISC_SHARED_NTHANDLE|D3D11_RESOURCE_MISC_TILED));
        if(Same(source_,source) && sceneDraw_ && intermediate_ &&
           source.thread==GetCurrentThreadId() && draw>sceneDraw_ && draw>pictureDraw_ && draw>consumedDraw_ && inputId && outputId && inputId!=outputId &&
           ordinaryOutput &&
           inputId!=source.selectedOutput && outputId!=source.selectedOutput && outputId!=source.privateOutput &&
           outputId!=Identity(intermediate_.Get()) && outputId!=Identity(picture_.Get()) &&
           Identity(inputDevice.Get())==source.device && Identity(outputDevice.Get())==source.device) {
            lastSceneReason_="preserved-disjoint";++disjointScenes_;return;
        }
        Revoke();
        if(!source.Known()){lastSceneReason_="source-unknown";return;}
        if(source.thread!=GetCurrentThreadId()){lastSceneReason_="owning-thread";return;}
        if(!input || !output || !inputId || !outputId){lastSceneReason_="resource-unknown";return;}
        if(!draw || draw<=consumedDraw_){lastSceneReason_="draw-consumed";return;}
        if(inputId!=source.selectedOutput){lastSceneReason_="selected-input-mismatch";return;}
        if(inputSubresource!=source.selectedSubresource){lastSceneReason_="selected-subresource";return;}
        if(inputId==outputId){lastSceneReason_="input-output-alias";return;}
        source_=source;intermediate_=output;intermediateSubresource_=outputSubresource;sceneDraw_=draw;
        pictureAfterScene_=false;lastSceneReason_="accepted";
    }
    void Picture(const Source& source,ID3D11Texture2D* input,uint32_t inputSubresource,
                 ID3D11Texture2D* output,uint32_t outputSubresource,uint64_t draw) noexcept {
        picture_.Reset();pictureDraw_=0;pictureAfterScene_=true;
        lastPictureDraw_=draw;lastPictureInput_=Identity(input);lastPictureOutput_=Identity(output);
        lastPictureReason_="rejected";
        if(!Same(source_,source)){lastPictureReason_="source-changed-or-no-scene";return;}
        if(source.thread!=GetCurrentThreadId()){lastPictureReason_="owning-thread";Revoke();return;}
        if(!intermediate_ || !input || !output){lastPictureReason_="resource-unknown";return;}
        if(draw<=sceneDraw_ || draw<=consumedDraw_){lastPictureReason_="draw-not-fresh";return;}
        if(lastPictureInput_!=Identity(intermediate_.Get())){lastPictureReason_="intermediate-input-mismatch";return;}
        if(inputSubresource!=intermediateSubresource_){lastPictureReason_="intermediate-subresource";return;}
        if(lastPictureInput_==lastPictureOutput_){lastPictureReason_="input-output-alias";Revoke();return;}
        Microsoft::WRL::ComPtr<ID3D11Device> outputDevice;output->GetDevice(&outputDevice);
        if(Identity(outputDevice.Get())!=source.device){lastPictureReason_="output-device";Revoke();return;}
        picture_=output;pictureSubresource_=outputSubresource;pictureDraw_=draw;
        lastPictureReason_="accepted";
    }
    uint64_t DisjointScenes() const noexcept {return disjointScenes_;}
    void Trace() const noexcept {if(auto* r=GenshinPresentNrTrace::Current()) {
        r->sceneDraw=sceneDraw_;r->pictureDraw=pictureDraw_;r->consumedDraw=consumedDraw_;
        r->disjointScenes=disjointScenes_;r->disjointScenesSinceTrace=disjointScenes_-lastTracedDisjointScenes_;
        lastTracedDisjointScenes_=disjointScenes_;
        r->intermediate=Identity(intermediate_.Get());r->observedPicture=Identity(picture_.Get());
        r->lastSceneDraw=lastSceneDraw_;r->lastSceneInput=lastSceneInput_;r->lastSceneOutput=lastSceneOutput_;
        r->lastPictureDraw=lastPictureDraw_;r->lastPictureInput=lastPictureInput_;r->lastPictureOutput=lastPictureOutput_;
        r->lastSceneReason=lastSceneReason_;r->lastPictureReason=lastPictureReason_;
    }}
    uint64_t Take(const Source& source,ID3D11Texture2D* current,uint64_t presentEpoch) noexcept {
        if(!Same(source_,source) || !current || source.thread!=GetCurrentThreadId() ||
           !presentEpoch || presentEpoch<=presentEpoch_)return 0;
        if(pictureDraw_) {
            if(!picture_ || pictureDraw_<=consumedDraw_ || pictureSubresource_ ||
               Identity(current)!=Identity(picture_.Get()))return 0;
            consumedDraw_=pictureDraw_;presentEpoch_=presentEpoch;
            // Keep the exact offscreen S -> A witness for a later fresh A -> B.
            // Release this consumed B before resize; never replay its pixels.
            picture_.Reset();pictureDraw_=0;
            if(auto* r=GenshinPresentNrTrace::Current())r->picturePath=1;
        } else {
            // The exact known returned Scene draw can already target B. This
            // is proved by canonical current-backbuffer identity, not extent or
            // absence of ToPicture observations. Scene validated selected S and
            // its subresource; the caller validates current B's layout/device.
            // Any later observed Picture edge, including a rejection, forbids
            // falling back to an earlier Scene. A fresh Scene must replace it.
            if(!intermediate_ || !sceneDraw_ || sceneDraw_<=consumedDraw_ || pictureAfterScene_ ||
               intermediateSubresource_ || Identity(current)!=Identity(intermediate_.Get()))return 0;
            consumedDraw_=sceneDraw_;presentEpoch_=presentEpoch;
            // Here A is B itself: retaining A would pin a swapchain buffer.
            // Retire the whole direct witness; only a new native draw can rearm.
            ClearOwner();
            if(auto* r=GenshinPresentNrTrace::Current())r->picturePath=2;
        }
        return consumedDraw_;
    }
};
struct Ticket {
    Source source{};
    Microsoft::WRL::ComPtr<ID3D11Texture2D> backbuffer;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    uintptr_t swapchain=0;
    uint64_t presentEpoch=0,pictureSerial=0;
    DXGI_FORMAT format=DXGI_FORMAT_UNKNOWN;
    // Final backbuffer extent is independent of the internal SR output extent.
    uint32_t pictureWidth=0,pictureHeight=0;
    GenshinTransport::BridgeDepthReceipt depth{};
    // Optional FG-only receipt; ordinary NR/depth admission does not consult it.
    std::shared_ptr<GenshinGameMotion::Lease> gameMotion;
    GenshinGameMotion::Reason gameMotionStatus=GenshinGameMotion::Reason::MissingSource;
    // Historical identity only; never authorizes a new depth reader.
    bool DepthIdentityMatches() const noexcept {
        const bool common=Known() && source.historyEpoch &&
            depth.device==source.device && depth.context==source.context &&
            depth.deviceEpoch==source.deviceEpoch && depth.producerEpoch==source.ownerEpoch && depth.configEpoch==source.configEpoch &&
            depth.instance==source.instance && depth.generation==source.generation && depth.sourceFrame==source.sourceFrame &&
            depth.pictureSerial==pictureSerial && depth.epoch==source.historyEpoch &&
            depth.width==source.width && depth.height==source.height &&
            depth.width==pictureWidth && depth.height==pictureHeight;
        if(!common)return false;
        if(depth.IdentityKnown())return depth.mappingOwner && depth.mappingOwner->selected==source.selectedOutput &&
            depth.mappingOwner->target==Identity(backbuffer.Get());
        return depth.NativeIdentityKnown() && depth.nativeGuide->pictureIdentity==source.privateOutput &&
            depth.nativeGuide->owner.sourceFrame==source.sourceFrame && depth.nativeGuide->owner.generation==source.generation;
    }
    bool DepthMatches() const noexcept {return DepthIdentityMatches() && (depth.Known() || depth.NativeIdentityKnown());}
    bool Known() const noexcept {
        return source.Known() && pictureSerial && presentEpoch && backbuffer && context && swapchain && pictureWidth && pictureHeight && format!=DXGI_FORMAT_UNKNOWN;
    }
};
inline bool Route(bool enabled,bool finished,bool before,bool deferred,bool opticalFg,bool interop,bool gameFg) noexcept {
    return enabled && finished && !before && !deferred && !opticalFg && !interop && !gameFg;
}
struct History {
    Source previous{};
    uintptr_t swapchain=0;
    uint64_t presentEpoch=0,pictureSerial=0;
    DXGI_FORMAT format=DXGI_FORMAT_UNKNOWN;
    uint32_t pictureWidth=0,pictureHeight=0;
    // Bitwise reasons are observations of existing reset gates, not new gates.
    enum Reset : uint32_t { First=1,Swapchain=2,Device=4,Context=8,DeviceEpoch=16,OwnerEpoch=32,ConfigEpoch=64,
        Instance=128,SourceSize=256,PictureLayout=512,GenerationReversed=1024,PictureOrder=2048,PresentOrder=4096,PresentGap=8192 };
    uint32_t resetReasons=0;
    bool known=false,flowReset=true;
    bool Begin(const Ticket& ticket) noexcept {
        const auto& a=previous;const auto& b=ticket.source;
        // Flow compares the actual captured final pictures. CPU Render's game
        // frame index is not this picture clock; gaps must not erase both images
        // before every comparison. Real owner/layout changes or ordering reversal
        // still discard flow. A skipped Present separately resets the NR model.
        resetReasons=(!known?First:0u) | (swapchain!=ticket.swapchain?Swapchain:0u) |
            (a.device!=b.device?Device:0u) | (a.context!=b.context?Context:0u) |
            (a.deviceEpoch!=b.deviceEpoch?DeviceEpoch:0u) | (a.ownerEpoch!=b.ownerEpoch?OwnerEpoch:0u) |
            (a.configEpoch!=b.configEpoch?ConfigEpoch:0u) | (a.instance!=b.instance?Instance:0u) |
            (a.width!=b.width || a.height!=b.height?SourceSize:0u) |
            (format!=ticket.format || pictureWidth!=ticket.pictureWidth || pictureHeight!=ticket.pictureHeight?PictureLayout:0u) |
            (b.generation<a.generation?GenerationReversed:0u) | (ticket.pictureSerial<=pictureSerial?PictureOrder:0u) |
            (ticket.presentEpoch<=presentEpoch?PresentOrder:0u);
        flowReset=resetReasons!=0;
        if(ticket.presentEpoch-presentEpoch!=1)resetReasons|=PresentGap;
        const bool reset=resetReasons!=0;
        if(auto* r=GenshinPresentNrTrace::Current()) {
            r->historySeq=GenshinPresentNrTrace::Sequence();r->previousPresent=presentEpoch;r->previousPicture=pictureSerial;
            r->resetReasons=resetReasons;r->historyReset=reset;r->flowReset=flowReset;
        }
        previous=b;swapchain=ticket.swapchain;presentEpoch=ticket.presentEpoch;pictureSerial=ticket.pictureSerial;format=ticket.format;
        pictureWidth=ticket.pictureWidth;pictureHeight=ticket.pictureHeight;known=true;return reset;
    }
};
// Original admission is explicit policy, never the fallback for a failed NR
// call. Unknown placements retain refusal; ordinary NR Route is unchanged.
enum class FgPictureKind : unsigned char { Unknown, NrWritten, OriginalNrDisabled, OriginalModelDisabled };
inline bool Original(FgPictureKind kind) noexcept {
    return kind==FgPictureKind::OriginalNrDisabled || kind==FgPictureKind::OriginalModelDisabled;
}
inline FgPictureKind SelectFgPicture(bool enabled,bool finished,bool applyModel,bool before,bool deferred) noexcept {
    if(before || deferred)return FgPictureKind::Unknown;
    if(!enabled)return FgPictureKind::OriginalNrDisabled;
    if(!finished)return FgPictureKind::Unknown;
    return applyModel?FgPictureKind::NrWritten:FgPictureKind::OriginalModelDisabled;
}
// One selected-route predicate at allocation AND the actual wrapper copy
// boundary. Stage's ticket/producer/outcome checks remain independent of this.
inline bool SharedSnapshotSelected() noexcept {
    const auto& cfg=*Config::Instance();const auto& state=State::Instance();
    return GenshinPresentNrTrace::IsGenshinProcess() &&
        state.api!=Vulkan && state.swapchainApi!=Vulkan &&
        state.activeFgInput==FGInput::Upscaler && state.activeFgOutput==FGOutput::DLSSG &&
        cfg.FGEnabled.value_or_default() && cfg.DlssNrEnabled.value_or_default() &&
        cfg.DlssNrApplyModel.value_or_default() && cfg.DlssNrFinishedPicture.value_or_default() &&
        !cfg.DlssNrRunBeforeSr.value_or_default() && !cfg.DlssNrDeferredDlss.value_or_default() &&
        !HoYoShade::Selected(HoYoShade::Stage::BeforeSr) &&
        !HoYoShade::Selected(HoYoShade::Stage::BeforeNr) &&
        !HoYoShade::Selected(HoYoShade::Stage::FinalOutput);
}
class Scope;
// A per-picture lease; only retired GPU allocations/associations move to the
// next lease. Neither shared_ptr ownership nor a D3D11 event retires FG.
class SnapshotLease final {
    friend class Scope;
    friend class GenshinBridgeFg::PresentScope;
    friend class GenshinBridgeFg::Recording;
    friend struct GenshinBridgeFg::Frame;
    friend class ::Dx11wDx12SC;
    mutable std::mutex mutex_;
    Source owner_{};
    uintptr_t swapchain_=0;
    D3D11_TEXTURE2D_DESC desc_{};
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture_,writerSource_,reader11Target_;
    Microsoft::WRL::ComPtr<ID3D11Query> query_;
    // This association came from ONE actual handle export/open for texture_.
    // Move it only together with that allocation after explicit retirement.
    std::shared_ptr<const GenshinBridgeFg::ColorImport> imported_;
    Microsoft::WRL::ComPtr<ID3D12Device> importDevice_;
    Microsoft::WRL::ComPtr<ID3D12Resource> reader12_,reader12Target_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> readerQueue12_;
    Microsoft::WRL::ComPtr<ID3D12Fence> read11_,read12_;
    uint64_t value11_=0,value12_=0;
    bool writerDone_=false,writerFailed_=false,claimed_=false,retired_=false,moved_=false;
    bool reading11_=false,reading12_=false,copiesRetired_=false;
    SnapshotLease()=default;
    static bool Done(ID3D12Fence* fence,uint64_t value) noexcept {
        if(!fence || !value || value==UINT64_MAX)return false;
        const auto n=fence->GetCompletedValue();return n!=UINT64_MAX && n>=value;
    }
    bool PollWriterLocked() noexcept {
        if(writerDone_)return true;
        if(writerFailed_ || !query_ || !context_ || owner_.thread!=GetCurrentThreadId())return false;
        BOOL done=FALSE;
        const auto hr=context_->GetData(query_.Get(),&done,sizeof(done),D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if(FAILED(hr)){writerFailed_=true;return false;}
        if(hr!=S_OK || !done)return false;
        writerDone_=true;writerSource_.Reset();return true;
    }
    bool CopiesCompleteLocked() noexcept {
        if(copiesRetired_)return true; // set only after real Frame::Complete
        return PollWriterLocked() && (!reading11_ || Done(read11_.Get(),value11_)) &&
            (!reading12_ || Done(read12_.Get(),value12_));
    }
    bool PollWriter() noexcept {std::lock_guard lock(mutex_);return PollWriterLocked();}
    bool Recyclable() noexcept {
        std::lock_guard lock(mutex_);return !moved_ && retired_ && CopiesCompleteLocked();
    }
    bool RecycleFrom(SnapshotLease& old) noexcept {
        std::lock_guard lock(old.mutex_);
        if(old.moved_ || !old.retired_ || !old.CopiesCompleteLocked())return false;
        // Caller checked exact owner domain and descriptor. Old frame receipts
        // cannot start another read; their retired lease remains a separate object.
        texture_=std::move(old.texture_);query_=std::move(old.query_);
        imported_=std::move(old.imported_);importDevice_=std::move(old.importDevice_);
        old.moved_=true;return true;
    }
    bool ClaimFrame() noexcept {
        std::lock_guard lock(mutex_);
        if(claimed_ || retired_ || moved_)return false;claimed_=true;return true;
    }
    void AbandonUnclaimed() noexcept {
        std::lock_guard lock(mutex_);if(!claimed_)retired_=true;
    }
    bool BeginRead11(ID3D11Texture2D* target) noexcept {
        std::lock_guard lock(mutex_);
        if(!claimed_ || retired_ || moved_ || reading11_ || !target)return false;
        reader11Target_=target;reading11_=true;return true; // armed BEFORE the optional shadow copy
    }
    void SubmittedRead11(ID3D12Fence* fence,uint64_t value) noexcept {
        std::lock_guard lock(mutex_);
        if(reading11_ && !read11_ && fence && value && value!=UINT64_MAX){read11_=fence;value11_=value;}
    }
    bool BeginRead12(ID3D12Resource* resource,ID3D12Resource* target,ID3D12CommandQueue* queue) noexcept {
        std::lock_guard lock(mutex_);
        if(!claimed_ || retired_ || moved_ || reading12_ || !resource || !target || !queue)return false;
        reader12Target_=target;reader12_=resource;readerQueue12_=queue;reading12_=true;return true;
    }
    void SubmittedRead12(ID3D12Fence* fence,uint64_t value) noexcept {
        std::lock_guard lock(mutex_);
        if(reading12_ && !read12_ && fence && value && value!=UINT64_MAX){read12_=fence;value12_=value;}
    }
    std::shared_ptr<const GenshinBridgeFg::ColorImport> CachedImport(ID3D12Device* device) noexcept {
        std::lock_guard lock(mutex_);
        return !moved_ && !retired_ && device && Identity(device)==Identity(importDevice_.Get())?imported_:nullptr;
    }
    void CacheImport(ID3D12Device* device,std::shared_ptr<const GenshinBridgeFg::ColorImport> imported) noexcept {
        std::lock_guard lock(mutex_);
        if(!moved_ && !retired_){importDevice_=device;imported_=std::move(imported);}
    }
public:
    // Called from the existing Frame collect/retire boundary, on the native
    // thread for the D3D11 event. Failed/unsignalled reads stay occupied forever
    // rather than infer completion from CPU Return, pointer identity or timeout.
    bool CopiesComplete() noexcept {std::lock_guard lock(mutex_);return CopiesCompleteLocked();}
    void FrameRetired() noexcept {
        std::lock_guard lock(mutex_);
        if(!claimed_)return;
        // ReleaseLocked calls this only after Frame::Complete including all
        // copies and provider retirement. Idle cache must not pin FG buffers,
        // shadow destinations or old queue timelines across resize/teardown.
        retired_=copiesRetired_=true;
        reader11Target_.Reset();reader12_.Reset();reader12Target_.Reset();readerQueue12_.Reset();
        read11_.Reset();read12_.Reset();
    }
};
// Only the actual wrapper can populate the returned native write. The endpoint
// validates its exact destination after the existing interop copy/wait chain.
class ReturnedPicture final {
    friend class GenshinBridgeFg::PresentScope;
    Ticket ticket_;
    bool depthCaptured_=false; // minted only from a real returned/published private FG recording
    Microsoft::WRL::ComPtr<ID3D11Texture2D> snapshot11_;
    std::shared_ptr<SnapshotLease> snapshotLease_;
    std::shared_ptr<GenshinTransport::ProducerFrame> producer_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> producerQueue_,presentQueue_;
    Microsoft::WRL::ComPtr<ID3D12Fence> produced_,copied_;
    uint64_t producedValue_=0,copiedValue_=0;
    Microsoft::WRL::ComPtr<ID3D12Resource> color_;
    uintptr_t provider_=0,swapchain_=0;
    FgPictureKind pictureKind_=FgPictureKind::Unknown;
    bool copiedAndWaited_=false;
public:
    bool Matches(IDXGISwapChain* swapchain,ID3D12CommandQueue* queue,ID3D12CommandQueue* producerQueue,
                 ID3D12Resource* color,ID3D12Device* device,uint64_t epoch,uintptr_t provider) const noexcept {
        if(pictureKind_==FgPictureKind::Unknown || !copiedAndWaited_ || !depthCaptured_ || !ticket_.DepthIdentityMatches() || !producer_ || !producer_->Live() ||
           !producer_->Guides() || GetCurrentThreadId()!=ticket_.source.thread || ticket_.presentEpoch!=epoch ||
           !provider || provider!=provider_ || swapchain_!=Identity(swapchain) ||
           Identity(queue)!=Identity(presentQueue_.Get()) || Identity(producerQueue)!=Identity(producerQueue_.Get()) ||
           Identity(color)!=Identity(color_.Get()) || !produced_ || !producedValue_ || !copied_ || !copiedValue_)return false;
        GenshinGuides::Owner o{};const auto& s=ticket_.source;
        if(!GenshinGuides::OwnerIdentity::Context(ticket_.context.Get()).ToCanonical(producer_->Owner(),o))return false;
        if(o.device!=s.device || o.context!=s.context || o.deviceEpoch!=s.deviceEpoch || o.producerEpoch!=s.ownerEpoch ||
           o.configEpoch!=s.configEpoch || o.instance!=s.instance || o.generation!=s.generation ||
           o.historyEpoch!=s.historyEpoch || o.sourceFrame!=s.sourceFrame || o.thread!=s.thread ||
           producer_->Guides()->Get().textures[3].identity!=s.privateOutput)return false;
        Microsoft::WRL::ComPtr<ID3D12Device> a,b,c,d;
        return device && SUCCEEDED(produced_->GetDevice(IID_PPV_ARGS(&a))) &&
            SUCCEEDED(copied_->GetDevice(IID_PPV_ARGS(&b))) && SUCCEEDED(color->GetDevice(IID_PPV_ARGS(&c))) &&
            SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&d))) && Identity(a.Get())==Identity(device) &&
            Identity(b.Get())==Identity(device) && Identity(c.Get())==Identity(device) && Identity(d.Get())==Identity(device);
    }
    ID3D11Texture2D* Snapshot11() const noexcept {return snapshot11_.Get();}
    FgPictureKind Kind() const noexcept {return pictureKind_;}
    const Ticket& PictureTicket() const noexcept {return ticket_;}
    ID3D12Fence* ProducerFence() const noexcept {return produced_.Get();}
    uint64_t ProducerValue() const noexcept {return producedValue_;}
    uint64_t ControlsEpoch() const noexcept {return ticket_.source.configEpoch;}
    uint64_t Serial() const noexcept {return ticket_.pictureSerial;}
    ID3D12Fence* Fence() const noexcept {return copied_.Get();}
    uint64_t Value() const noexcept {return copiedValue_;}
};
// Immutable final-picture copy ownership, minted only on the real adapter's
// successful Acquire path. No borrowed Scope/TLS pointer, SR four-resource or
// FG producer lives here. Content reuse/uncertain submission remain governed
// by the existing adapter Return ordering and owned transaction completion.
class Scope;
class NativeColorCapability final {
    friend class Scope;
    // Private, non-aggregate construction key keeps make_shared to one CPU
    // allocation. Neither an endpoint scalar nor {} can construct this key.
    class ConstructionKey { friend class Scope; ConstructionKey() = default; };
    Source source_{};
    uint64_t presentEpoch_=0,pictureSerial_=0,stream_=0,adapterGeneration_=0,adapterFrame_=0;
    Microsoft::WRL::ComPtr<ID3D11Device> sourceDevice_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_,ticketContext_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> backbuffer_,ticketBackbuffer_,shared11_;
    Microsoft::WRL::ComPtr<ID3D12Resource> picture_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue_;
    Microsoft::WRL::ComPtr<ID3D12Device> device_,pictureOwner_;
    Microsoft::WRL::ComPtr<ID3D12Fence> uploadFence_;
    uint64_t uploadValue_=0;
    uintptr_t swapchain_=0;
    // Preserve actual source/ticket format, including typed SRGB(29), separately
    // from the opened shared UNORM(28) picture. Copy-family compatibility grants
    // no ticket-format alias or transfer/SDR authority at MatchesNativeColor.
    D3D11_TEXTURE2D_DESC sourceDesc_{};
    D3D12_RESOURCE_DESC pictureDesc_{};
    native::NativePictureColorObservation observation_{};
public:
    explicit NativeColorCapability(const ConstructionKey&) noexcept {}
    NativeColorCapability(const NativeColorCapability&)=delete;
    NativeColorCapability& operator=(const NativeColorCapability&)=delete;
    const native::NativePictureColorObservation& Observation() const noexcept {return observation_;}
    const D3D11_TEXTURE2D_DESC& SourceDesc() const noexcept {return sourceDesc_;}
    const D3D12_RESOURCE_DESC& PictureDesc() const noexcept {return pictureDesc_;}
    ID3D12Device* PictureOwner() const noexcept {return pictureOwner_.Get();}
    uint64_t ConfigEpoch() const noexcept {return source_.configEpoch;}
};
class Scope final {
    friend class native::Dx11FrameSource;
    friend class NativeParentConsumer;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> privateSnapshot_;
    std::shared_ptr<SnapshotLease> snapshotLease_;
    const char* snapshotStage_="not-called";
    void StageReturnedSnapshot(ID3D11Device* device,ID3D11DeviceContext* context,
                               ID3D11Texture2D* privatePicture) noexcept {
        privateSnapshot_.Reset();snapshotLease_.reset();
        snapshotStage_="admission";
        // Called only by the real adapter after Returned validated its exact
        // input/output, and before its existing copyback signal. Failure is
        // optional FG refusal; it never changes native NR/Original success.
        // The returned color belongs to the authenticated picture consumer.
        // depthBound_ describes optional mapped NR depth, not the separately
        // captured FG game-motion/depth packet. Publication still requires its
        // real Recording::Capture -> Returned -> Publish transaction.
        if(current_!=this || !fgProducer_ || !fgProducer_->Live() || !bound_ ||
           !completionFence || !completionValue ||
           !ticket.DepthMatches() || !device || !context || !privatePicture ||
           Identity(device)!=ticket.source.device || Identity(context)!=ticket.source.context ||
           GetCurrentThreadId()!=ticket.source.thread ||
           !(fgPictureKind_==FgPictureKind::NrWritten ? (consumerReached && consumerRan) :
               (Original(fgPictureKind_) && fgOriginalObserved_ && fgOriginalReturned_)))return;
        snapshotStage_="descriptor";
        D3D11_TEXTURE2D_DESC d{};privatePicture->GetDesc(&d);
        Microsoft::WRL::ComPtr<ID3D11Device> actual;privatePicture->GetDevice(&actual);
        if(Identity(actual.Get())!=ticket.source.device || d.Width!=ticket.pictureWidth || d.Height!=ticket.pictureHeight ||
           !d.Width || !d.Height || d.Width>16384 || d.Height>16384 ||
           uint64_t(d.Width)*d.Height*4>256ull*1024*1024 || d.SampleDesc.Count!=1 || d.SampleDesc.Quality ||
           d.ArraySize!=1 || d.MipLevels!=1 ||
           (d.Format!=DXGI_FORMAT_R8G8B8A8_UNORM && d.Format!=DXGI_FORMAT_B8G8R8A8_UNORM))return;
        d.Usage=D3D11_USAGE_DEFAULT;d.BindFlags=0;d.CPUAccessFlags=0;d.MiscFlags=0;
        // Only this authenticated Genshin native NR snapshot may be opened by
        // the wrapper. Keep its immutable full-picture copy and native B return.
        // Ordinary/original-picture callers retain their previous allocation.
        const bool exportSnapshot=fgPictureKind_==FgPictureKind::NrWritten && SharedSnapshotSelected();
        if(exportSnapshot)d.MiscFlags=D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
        struct SnapshotRing {
            std::mutex mutex;
            // Writer retention keeps the original three-copy/64MiB bound.
            // Pool occupancy is separate: vendor pending cannot suppress FG.
            std::array<std::shared_ptr<SnapshotLease>,3> pending,pool;
        };
        static auto* ring=new(std::nothrow) SnapshotRing; // no unfenced static teardown
        snapshotStage_="pending-allocation";
        if(!ring)return;
        std::lock_guard lock(ring->mutex);
        std::shared_ptr<SnapshotLease>* pending=nullptr;
        uint64_t retainedBytes=0;
        for(auto& slot:ring->pending) {
            if(slot && slot->PollWriter())slot.reset();
            if(slot)retainedBytes+=uint64_t(slot->desc_.Width)*slot->desc_.Height*4;
            else if(!pending)pending=&slot;
        }
        snapshotStage_="snapshot-ring-busy";
        const uint64_t bytes=uint64_t(d.Width)*d.Height*4;
        if(!pending || bytes>64ull*1024*1024 || retainedBytes>64ull*1024*1024-bytes)return;
        std::shared_ptr<SnapshotLease> next;
        try {next=std::shared_ptr<SnapshotLease>(new SnapshotLease);}
        catch(const std::bad_alloc&){return;}
        next->owner_=ticket.source;next->swapchain_=ticket.swapchain;next->desc_=d;
        next->device_=device;next->context_=context;
        std::shared_ptr<SnapshotLease>* cache=nullptr;
        bool reused=false;
        if(exportSnapshot) {
            for(auto& slot:ring->pool) {
                const bool available=!slot || slot->Recyclable();
                if(!cache && available)cache=&slot;
                if(!slot || !available)continue;
                const auto& a=slot->owner_;const auto& b=ticket.source;
                const bool same=a.device==b.device && a.context==b.context && a.thread==b.thread &&
                    a.deviceEpoch==b.deviceEpoch && a.ownerEpoch==b.ownerEpoch && a.configEpoch==b.configEpoch &&
                    a.historyEpoch==b.historyEpoch && a.instance==b.instance && slot->swapchain_==ticket.swapchain &&
                    slot->desc_.Width==d.Width && slot->desc_.Height==d.Height &&
                    slot->desc_.Format==d.Format && slot->desc_.MiscFlags==d.MiscFlags;
                if(same && next->RecycleFrom(*slot)){cache=&slot;reused=true;break;}
            }
            // A new size/device/config gets fresh resources. Keep old in-flight
            // owners; cap only the idle cache, never current-frame allocation.
            if(!reused && cache) {
                uint64_t poolBytes=0;for(const auto& slot:ring->pool)
                    if(slot)poolBytes+=uint64_t(slot->desc_.Width)*slot->desc_.Height*4;
                const auto replaced=*cache?uint64_t((*cache)->desc_.Width)*(*cache)->desc_.Height*4:0;
                if(poolBytes-replaced>64ull*1024*1024-bytes)cache=nullptr;
            }
        }
        snapshotStage_="create-texture";
        if(!next->texture_ && FAILED(device->CreateTexture2D(&d,nullptr,&next->texture_))) {
            if(!exportSnapshot)return;
            d.MiscFlags=0;next->desc_=d;cache=nullptr;
            if(FAILED(device->CreateTexture2D(&d,nullptr,&next->texture_)))return;
        }
        snapshotStage_="create-query";
        const D3D11_QUERY_DESC queryDesc{D3D11_QUERY_EVENT,0};
        if(!next->query_ && FAILED(device->CreateQuery(&queryDesc,&next->query_)))return;
        next->writerSource_=privatePicture;
        *pending=next;if(cache)*cache=next; // retain BEFORE commands, including fallback
        snapshotLease_=next;privateSnapshot_=next->texture_;
        context->CopyResource(next->texture_.Get(),privatePicture);context->End(next->query_.Get());
        snapshotStage_=reused?"staged-reused":"staged";
        // Actual recorded snapshot copies only; no extra sampling/query/thread.
        if(exportSnapshot) {
            static uint64_t staged=0,recycles=0,fallbacks=0;
            ++staged;recycles+=reused;fallbacks+=!cache;
            static unsigned seen=0;const unsigned event=reused?1u:cache?2u:4u;
            const bool first=!(seen&event);seen|=event;
            if(first || staged%512==0)
                LOG_INFO("R352 NR snapshot pool: pid={} present={} picture={} pooled={} pool_slot={} slot_reused={} staged={} recycled={} allocation_fallbacks={} performance=UNKNOWN",
                    GetCurrentProcessId(),ticket.presentEpoch,ticket.pictureSerial,bool(cache),cache?unsigned(cache-ring->pool.data()):UINT_MAX,reused,staged,recycles,fallbacks);
        }
    }

    inline static thread_local Scope* current_=nullptr;
    Scope* previous_=nullptr;
    ID3D12Resource* picture_=nullptr;
    ID3D12CommandQueue* queue_=nullptr;
    ID3D12Device* device_=nullptr;
    bool bound_=false;
    bool depthBound_=false;
    // Allocation facts come from BindInput's existing descriptor/device query.
    // The adapter copies the already-read actual B descriptor after its copy.
    D3D12_RESOURCE_DESC boundPictureDesc_{};
    Microsoft::WRL::ComPtr<ID3D12Device> boundPictureOwner_;
    std::shared_ptr<const NativeColorCapability> nativeColor_;
    void CaptureNativeColor(native::FrameInput& input,native::IFrameSource* source,
        ID3D11Device* device11,ID3D11DeviceContext* context,ID3D11Texture2D* backbuffer,
        ID3D11Texture2D* shared11,const D3D11_TEXTURE2D_DESC& actualSource) noexcept {
        // Only Dx11FrameSource calls this: MatchesPresent has succeeded, the
        // exact shared pair was opened, CopyResource and upload Signal/Flush
        // actually happened, and BindInput accepted THIS picture/queue/device.
        nativeColor_.reset();input.beforeNrNativeColor.reset();
        if(current_!=this || !ticket.Known() || !bound_ || !source || !device11 || !context ||
           !backbuffer || !shared11 || !boundPictureOwner_ || GetCurrentThreadId()!=ticket.source.thread ||
           !ticket.source.historyEpoch || input.api!=native::Api::D3D11 || !input.finalOriginal ||
           input.picture!=picture_ || !input.ready.fence || !input.ready.value || input.ready.value==UINT64_MAX ||
           input.width!=ticket.pictureWidth || input.height!=ticket.pictureHeight ||
           actualSource.Format!=ticket.format || actualSource.Width!=ticket.pictureWidth ||
           actualSource.Height!=ticket.pictureHeight)return;
        try {
            auto owned=std::make_shared<NativeColorCapability>(NativeColorCapability::ConstructionKey{});
            owned->source_=ticket.source;owned->presentEpoch_=ticket.presentEpoch;owned->pictureSerial_=ticket.pictureSerial;
            owned->stream_=reinterpret_cast<uint64_t>(source);owned->adapterGeneration_=1;owned->adapterFrame_=input.frame;
            owned->sourceDevice_=device11;owned->context_=context;owned->ticketContext_=ticket.context;
            owned->backbuffer_=backbuffer;owned->ticketBackbuffer_=ticket.backbuffer;owned->shared11_=shared11;
            owned->picture_=picture_;owned->queue_=queue_;owned->device_=device_;owned->pictureOwner_=boundPictureOwner_;
            owned->uploadFence_=input.ready.fence;owned->uploadValue_=input.ready.value;owned->swapchain_=ticket.swapchain;
            owned->sourceDesc_=actualSource;owned->pictureDesc_=boundPictureDesc_;owned->observation_=input.nativePictureColor;
            nativeColor_=std::move(owned);input.beforeNrNativeColor=nativeColor_;
        } catch (...) {} // Optional color admission never rejects ordinary NR.
    }
    std::shared_ptr<GenshinTransport::ProducerFrame> fgProducer_;
    uintptr_t fgProvider_=0,fgSwapchain_=0;
    FgPictureKind fgPictureKind_=FgPictureKind::Unknown;
    bool fgOriginalObserved_=false,fgOriginalReturned_=false,fgOriginalOrdered_=false;
public:
    Ticket ticket;
    bool reset=false,flowReset=false,consumerReached=false,consumerRan=false,screenCopyOrdered=false;
    ID3D12Fence* completionFence=nullptr;
    uint64_t completionValue=0;
    explicit Scope(Ticket value,bool historyReset,bool pictureReset=true) noexcept
        : previous_(current_),ticket(std::move(value)),reset(historyReset),flowReset(pictureReset) {
        if(ticket.Known())current_=this;
    }
    ~Scope(){
        // A returned native queued reader has the same final-fence obligation
        // as mapped depth, including no-copy/NR-off/warmup returns. Without a
        // successful Returned receipt the retained reader stays quarantined.
        if(depthBound_ && completionFence && completionValue)
            if(auto lease=GenshinTransport::NativeDepthLease(ticket.depth))lease->RetireReader();
        if(snapshotLease_)snapshotLease_->AbandonUnclaimed();
        current_=previous_;
    }
    Scope(const Scope&)=delete;Scope& operator=(const Scope&)=delete;
    static Scope* Current() noexcept{return current_;}
    bool MatchesPresent(IDXGISwapChain* swapchain,ID3D11Device* device,ID3D11DeviceContext* context,
                        ID3D11Texture2D* backbuffer) const noexcept {
        return ticket.Known() && GetCurrentThreadId()==ticket.source.thread &&
            Identity(swapchain)==ticket.swapchain && Identity(device)==ticket.source.device &&
            Identity(context)==ticket.source.context && Identity(backbuffer)==Identity(ticket.backbuffer.Get());
    }
    // Live checks at each color recording boundary; no COM queries or cached
    // ready result. Adapter, ticket and producer-submission clocks are separate.
    bool MatchesNativeColor(const std::shared_ptr<const NativeColorCapability>& capability,
        ID3D12Resource* color,ID3D12CommandQueue* queue,uint64_t stream,uint64_t adapterGeneration,
        uint64_t adapterFrame,uint64_t naturalDevice,uint64_t naturalBackbuffer,
        uint32_t width,uint32_t height,const native::NativePictureColorObservation& observation) const noexcept {
        if(current_!=this || !capability || capability!=nativeColor_ || !bound_ || !ticket.Known() ||
           GetCurrentThreadId()!=ticket.source.thread || consumerReached || consumerRan || completionFence || completionValue)return false;
        const auto& c=*capability;const auto& a=c.source_;const auto& b=ticket.source;
        const bool sameSource=a.Known() && b.Known() && a.device==b.device && a.context==b.context &&
            a.privateOutput==b.privateOutput && a.selectedOutput==b.selectedOutput && a.transfer==b.transfer &&
            a.deviceEpoch==b.deviceEpoch && a.ownerEpoch==b.ownerEpoch && a.configEpoch==b.configEpoch &&
            a.instance==b.instance && a.generation==b.generation && a.sourceFrame==b.sourceFrame && a.thread==b.thread &&
            a.width==b.width && a.height==b.height && a.selectedSubresource==b.selectedSubresource &&
            a.submitSerial==b.submitSerial && a.copyOrderedSerial==b.copyOrderedSerial &&
            a.historyEpoch && a.historyEpoch==b.historyEpoch && a.progress==b.progress && a.attempt==b.attempt;
        return sameSource && c.presentEpoch_==ticket.presentEpoch && c.pictureSerial_==ticket.pictureSerial &&
            c.swapchain_==ticket.swapchain && c.ticketBackbuffer_.Get()==ticket.backbuffer.Get() &&
            c.ticketContext_.Get()==ticket.context.Get() && c.picture_.Get()==picture_ && c.picture_.Get()==color &&
            c.queue_.Get()==queue_ && c.queue_.Get()==queue && c.device_.Get()==device_ &&
            c.pictureOwner_==boundPictureOwner_ && c.stream_==stream && c.adapterGeneration_==adapterGeneration &&
            c.adapterFrame_==adapterFrame && reinterpret_cast<uint64_t>(c.sourceDevice_.Get())==naturalDevice &&
            reinterpret_cast<uint64_t>(c.backbuffer_.Get())==naturalBackbuffer && c.context_ && c.shared11_ &&
            c.uploadFence_ && c.uploadValue_ && c.uploadValue_!=UINT64_MAX &&
            c.sourceDesc_.Format==ticket.format && c.sourceDesc_.Width==width && c.sourceDesc_.Height==height &&
            ticket.pictureWidth==width && ticket.pictureHeight==height && c.observation_==observation;
    }
    const char* SnapshotStage() const noexcept {return snapshotStage_;}
    ID3D11Texture2D* ReturnedSnapshot() const noexcept {
        return current_==this && completionFence && completionValue &&
            (screenCopyOrdered || OriginalReady()) ? privateSnapshot_.Get() : nullptr;
    }
    std::shared_ptr<SnapshotLease> ReturnedSnapshotLease() const noexcept {
        return ReturnedSnapshot()?snapshotLease_:nullptr;
    }
    FgPictureKind FgKind() const noexcept {return fgPictureKind_;}
    bool BindFgSender(std::shared_ptr<GenshinTransport::ProducerFrame> producer,uintptr_t provider,uintptr_t swapchain,
                      FgPictureKind kind) noexcept {
        if(kind==FgPictureKind::Unknown || current_!=this || !producer || !producer->Live() || !provider || !swapchain || !ticket.DepthMatches())return false;
        GenshinGuides::Owner o{};const auto& s=ticket.source;
        if(!GenshinGuides::OwnerIdentity::Context(ticket.context.Get()).ToCanonical(producer->Owner(),o))return false;
        if(o.device!=s.device || o.context!=s.context || o.deviceEpoch!=s.deviceEpoch || o.producerEpoch!=s.ownerEpoch ||
           o.configEpoch!=s.configEpoch || o.instance!=s.instance || o.generation!=s.generation ||
           o.historyEpoch!=s.historyEpoch || o.sourceFrame!=s.sourceFrame || o.thread!=s.thread ||
           !producer->Guides() || producer->Guides()->Get().textures[3].identity!=s.privateOutput)return false;
        fgProducer_=std::move(producer);fgProvider_=provider;fgSwapchain_=swapchain;fgPictureKind_=kind;return true;
    }
    bool MatchesFgInput(uintptr_t provider,uintptr_t swapchain,ID3D12GraphicsCommandList* cmd,ID3D12CommandQueue* queue,
        ID3D12Resource* color,ID3D12Resource* depth,ID3D12Resource* motion,ID3D12Resource* distrust) const noexcept {
        // Optional mapped NR depth is not the normalized FG depth/motion pair.
        // Recording::Capture separately validates and retains that actual pair.
        if(current_!=this || !fgProducer_ || !fgProducer_->Live() || !ticket.DepthMatches() ||
           provider!=fgProvider_ || swapchain!=fgSwapchain_)return false;
        native::NativeFrame frame{};frame.color=color;frame.depth=depth;frame.motion=motion;frame.historyDistrust=distrust;
        return MatchesConsumer(cmd,queue,frame,true);
    }
    // Called only on the real producer callback before any NR evaluation. This
    // records a read-only guide consumer, not consumerRan/model/copyback success.
    bool ObserveOriginal(ID3D12GraphicsCommandList* cmd,ID3D12CommandQueue* queue,const native::NativeFrame& frame) noexcept {
        if(!Original(fgPictureKind_) || fgOriginalObserved_ || consumerReached || consumerRan ||
           !MatchesFgInput(fgProvider_,fgSwapchain_,cmd,queue,frame.color,frame.depth,frame.motion,frame.historyDistrust))return false;
        fgOriginalObserved_=true;return true;
    }
    // The adapter calls this only after the checked queue/context ordering and
    // real D3D11 signal, with no picture copy issued. It signs native B unchanged.
    void OrderOriginal() noexcept {
        fgOriginalOrdered_=Original(fgPictureKind_) && fgOriginalObserved_ && fgOriginalReturned_ &&
            !consumerReached && !consumerRan && !screenCopyOrdered && completionFence && completionValue;
    }
    bool OriginalReady() const noexcept {return fgOriginalOrdered_;}
    bool BindInput(native::FrameInput& input,ID3D12Device* device,ID3D12CommandQueue* queue) noexcept {
        bound_=false;depthBound_=false;picture_=nullptr;queue_=nullptr;device_=nullptr;
        nativeColor_.reset();boundPictureOwner_.Reset();boundPictureDesc_={};
        if(!input.picture || !device || !queue || input.api!=native::Api::D3D11 ||
           input.width!=ticket.pictureWidth || input.height!=ticket.pictureHeight ||
           !input.ready.fence || !input.ready.value || queue->GetDesc().Type!=D3D12_COMMAND_LIST_TYPE_DIRECT)return false;
        Microsoft::WRL::ComPtr<ID3D12Device> pictureDevice,queueDevice,fenceDevice;
        if(FAILED(input.picture->GetDevice(IID_PPV_ARGS(&pictureDevice))) ||
           FAILED(queue->GetDevice(IID_PPV_ARGS(&queueDevice))) ||
           FAILED(input.ready.fence->GetDevice(IID_PPV_ARGS(&fenceDevice))) ||
           Identity(pictureDevice.Get())!=Identity(device) || Identity(queueDevice.Get())!=Identity(device) ||
           Identity(fenceDevice.Get())!=Identity(device))return false;
        const auto d=input.picture->GetDesc();
        if(d.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.DepthOrArraySize!=1 || d.MipLevels!=1 ||
           d.SampleDesc.Count!=1 || d.Width!=input.width || d.Height!=input.height)return false;
        const auto depthLease=GenshinTransport::NativeDepthLease(ticket.depth);
        const auto depthWidth=GenshinTransport::NativeDepthWidth(ticket.depth);
        const auto depthHeight=GenshinTransport::NativeDepthHeight(ticket.depth);
        const bool nativeDepth=!ticket.depth.lease && ticket.depth.nativeLease;
        bool depth= ticket.DepthMatches() && input.depthCount==1 && input.depth[0] && input.depthResource &&
            input.depthLease==depthLease && input.depth[0]==input.depthResource &&
            input.preparedDepthSource==(ticket.depth.preparedPacket?ticket.depth.preparedPacket->Source():nullptr) &&
            input.depthCoordinates==(nativeDepth?native::DepthCoordinates::UnmappedProducer:native::DepthCoordinates::Picture) &&
            input.depthGeneration==ticket.source.generation && input.depthSourceFrame==ticket.source.sourceFrame &&
            input.depthPictureSerial==ticket.pictureSerial && input.depthEpoch==ticket.source.historyEpoch &&
            input.depthWidth==depthWidth && input.depthHeight==depthHeight &&
            input.depthView==(nativeDepth?DXGI_FORMAT_R32_FLOAT:DXGI_FORMAT_R32G32B32A32_FLOAT) &&
            input.depthValidityPaired==!nativeDepth &&
            input.depthReversed==(ticket.depth.convention==GenshinGuides::DepthConvention::DeviceZReversed) &&
            !input.depthOriginX && !input.depthOriginY && !input.depthYFlip && input.depthCoverage &&
            input.depthProducerReady && input.depthLease->Ready() &&
            input.depthLease->MatchesReader(input.depth[0],input.ready.fence,input.ready.value);
        if(depth) {
            const auto dd=input.depth[0]->GetDesc();Microsoft::WRL::ComPtr<ID3D12Device> depthDevice;
            const auto mappedLease=std::dynamic_pointer_cast<GenshinTransport::PreparedDepthLease>(depthLease);
            const bool directMapped=!nativeDepth && dd.Format==DXGI_FORMAT_R32G32B32A32_FLOAT && mappedLease &&
                mappedLease->MatchesDirectMappedReader(input.depth[0],input.ready.fence,input.ready.value);
            depth=dd.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D && dd.DepthOrArraySize==1 && dd.MipLevels==1 &&
                dd.SampleDesc.Count==1 && !dd.SampleDesc.Quality &&
                (dd.Format==(nativeDepth?DXGI_FORMAT_R32_FLOAT:DXGI_FORMAT_R32G32B32A32_TYPELESS) || directMapped) &&
                dd.Width==depthWidth && dd.Height==depthHeight &&
                SUCCEEDED(input.depth[0]->GetDevice(IID_PPV_ARGS(&depthDevice))) && Identity(depthDevice.Get())==Identity(device);
        }
        if(!depth) {
            // Depth is optional: a stale/partial receipt never rejects a valid NR picture.
            for(auto& resource:input.depth)resource=nullptr;
            input.depthCount=0;input.depthView=DXGI_FORMAT_UNKNOWN;input.depthWidth=input.depthHeight=0;
            input.depthReversed=false;input.depthValidityPaired=false;input.depthResource=nullptr;input.depthLease.reset();
            input.preparedDepthSource.reset();
            input.depthGeneration=input.depthSourceFrame=input.depthPictureSerial=input.depthEpoch=0;
            input.depthOriginX=input.depthOriginY=0;input.depthYFlip=false;
            input.depthCoverage=input.depthProducerReady=false;
        }
        depthBound_=depth;
        picture_=input.picture;queue_=queue;device_=device;bound_=true;
        boundPictureDesc_=d;boundPictureOwner_=pictureDevice;return true;
    }
    bool MatchesConsumer(ID3D12GraphicsCommandList* cmd,ID3D12CommandQueue* queue,const native::NativeFrame& frame,
        bool /*normalizedFgDepth*/=false) const noexcept {
        if(!bound_ || !cmd || queue!=queue_ || frame.color!=picture_ || !frame.motion || !frame.historyDistrust ||
           cmd->GetType()!=D3D12_COMMAND_LIST_TYPE_DIRECT || GetCurrentThreadId()!=ticket.source.thread)return false;
        Microsoft::WRL::ComPtr<ID3D12Device> listDevice;
        if(FAILED(cmd->GetDevice(IID_PPV_ARGS(&listDevice))) || Identity(listDevice.Get())!=Identity(device_))return false;
        for(auto* guide:{frame.depth,frame.motion,frame.historyDistrust}) {
            if(!guide)continue;
            Microsoft::WRL::ComPtr<ID3D12Device> guideDevice;
            if(FAILED(guide->GetDevice(IID_PPV_ARGS(&guideDevice))) || Identity(guideDevice.Get())!=Identity(device_))return false;
            const auto d=guide->GetDesc();
            if(d.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.DepthOrArraySize!=1 || d.MipLevels!=1 || d.SampleDesc.Count!=1)return false;
            if(guide!=frame.historyDistrust) {
                // BindInput already validated the optional RAW depth at its
                // producer dimensions. NativeProducer::BuildGuides explicitly
                // returns picture-sized, unjittered depth/motion to this consumer.
                // Comparing that output with the raw low-resolution depth would
                // reject the entire NR callback whenever native fallback is used.
                const auto width=ticket.pictureWidth;
                const auto height=ticket.pictureHeight;
                if(d.Width!=width || d.Height!=height)return false;
            }
        }
        return true;
    }
    bool Returned(const native::FrameInput& input,const native::FrameOutput& output) noexcept {
        if(!bound_ || input.picture!=picture_ || !output.done.fence || !output.done.value)return false;
        Microsoft::WRL::ComPtr<ID3D12Device> fenceDevice;
        if(FAILED(output.done.fence->GetDevice(IID_PPV_ARGS(&fenceDevice))) ||
           Identity(fenceDevice.Get())!=Identity(device_))return false;
        if(depthBound_ && (input.depthLease!=GenshinTransport::NativeDepthLease(ticket.depth) || !input.depthLease ||
           !input.depthLease->BindReaderCompletion(output.done.fence,output.done.value)))return false;
        completionFence=output.done.fence;completionValue=output.done.value;consumerRan=output.processed;
        fgOriginalReturned_=Original(fgPictureKind_) && fgOriginalObserved_ && !output.processed && !consumerReached;
        return true;
    }
    void CopyOrdered() noexcept {
        screenCopyOrdered=consumerReached && consumerRan && completionFence && completionValue;
        if(depthBound_ && completionFence && completionValue)if(auto lease=GenshinTransport::NativeDepthLease(ticket.depth))lease->RetireReader();
    }
};
// Implemented by the real native-parent Present scope, not by an input hint.
bool MatchesNativeParentTicket(const Ticket&) noexcept;
class NativeParentConsumer final {
    friend class native::NativeDriver;
    inline static thread_local const NativeParentConsumer* current_=nullptr;
    const Scope* scope_=nullptr;
    ID3D12GraphicsCommandList* list_=nullptr;
    ID3D12CommandQueue* queue_=nullptr;
    native::NativeFrame frame_{};
    uint64_t epoch_=0;
    NativeParentConsumer(const Scope* s,ID3D12GraphicsCommandList* list,ID3D12CommandQueue* queue,
                         const native::NativeFrame& frame,uint64_t epoch) noexcept
        :scope_(s),list_(list),queue_(queue),frame_(frame),epoch_(epoch) {
        if(!current_ && Valid())current_=this;
    }
    bool Valid() const noexcept {
        return scope_ && scope_==Scope::Current() && scope_->ticket.presentEpoch==epoch_ &&
            MatchesNativeParentTicket(scope_->ticket) && scope_->MatchesConsumer(list_,queue_,frame_);
    }
public:
    NativeParentConsumer(const NativeParentConsumer&)=delete;
    NativeParentConsumer& operator=(const NativeParentConsumer&)=delete;
    ~NativeParentConsumer(){if(current_==this)current_=nullptr;}
    static bool Active() noexcept {return current_ && current_->Valid();}
    static bool Matches(ID3D12GraphicsCommandList* list,ID3D12CommandQueue* queue,
                        ID3D12Resource* color,ID3D12Resource* depth,ID3D12Resource* motion,
                        ID3D12Resource* distrust,uint64_t epoch) noexcept {
        const auto* p=current_;
        return p && p->Valid() && list==p->list_ && queue==p->queue_ && epoch==p->epoch_ &&
            color==p->frame_.color && depth==p->frame_.depth && motion==p->frame_.motion &&
            distrust==p->frame_.historyDistrust;
    }
    static bool PictureFlowGuides(ID3D12GraphicsCommandList* list,ID3D12CommandQueue* queue,
                                  ID3D12Resource* color,ID3D12Resource* depth,ID3D12Resource* motion,
                                  ID3D12Resource* distrust,uint64_t epoch,TemporalGuideFacts& out) noexcept {
        out={};
        // Exact ordinary parent, callback resources, thread, owner and ordered queue.
        // This capability proves measured picture flow/trust only, never scene depth.
        if(!Matches(list,queue,color,depth,motion,distrust,epoch))return false;
        const auto* p=current_;
        const auto& frame=p->frame_;
        if(!motion || !distrust || !frame.pictureFlowGuides_ ||
           !frame.guideEpoch || frame.guideEpoch==UINT64_MAX || frame.guideCutSerial==UINT64_MAX)return false;
        out.guideEpoch=frame.guideEpoch;out.guideCutSerial=frame.guideCutSerial;
        out.reset=frame.continuityReset || frame.reset || frame.guideSceneCut_ ||
            p->scope_->reset || p->scope_->flowReset;
        return true;
    }
    static bool TemporalGuides(ID3D12GraphicsCommandList* list,ID3D12CommandQueue* queue,
                               ID3D12Resource* color,ID3D12Resource* depth,ID3D12Resource* motion,
                               ID3D12Resource* distrust,uint64_t epoch,TemporalGuideFacts& out) noexcept {
        out={};
        // Matches revalidates the existing scope/ticket/thread/list/queue and
        // all resources against the retained actual callback frame. No hint or
        // NrInputObservation TLS can create this capability.
        if(!Matches(list,queue,color,depth,motion,distrust,epoch))return false;
        const auto* p=current_;
        const auto& frame=p->frame_;
        if(!p->scope_->depthBound_ || !p->scope_->ticket.DepthMatches() ||
           !depth || !motion || !distrust || !frame.pictureTemporalGuides_ ||
           frame.depthObservation.guide!=native::NrInputObservation::GuideDepth::Supplied ||
           !frame.guideEpoch || frame.guideEpoch==UINT64_MAX || frame.guideCutSerial==UINT64_MAX)return false;
        out.guideEpoch=frame.guideEpoch;out.guideCutSerial=frame.guideCutSerial;
        // Guide history must drop observed cuts even when the optional model
        // scene-cut policy is Off/Count and therefore requested no model reset.
        out.reset=frame.continuityReset || frame.reset || frame.guideSceneCut_ ||
            p->scope_->reset || p->scope_->flowReset;
        return true;
    }
};

}

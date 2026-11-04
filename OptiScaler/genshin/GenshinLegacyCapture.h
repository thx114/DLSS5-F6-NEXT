// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <inputs/GenshinFsrDx11.h>
#include "GenshinLegacyControls.h"
#include "GenshinFamilySkip.h"
#include "CaptureOnlyBridge.h"
#include "GenshinEngineProducer.h"
#include <memory>
#include "GenshinDx12Transport.h"
#include "GenshinRawObservation.h"
#include "GenshinPresentNr.h"

namespace GenshinBridgeFg { struct CameraObservation; class GenshinFgOffer; using SenderInput=GenshinFgOffer; }
namespace GenshinTransport { class ProducerFrame; }

namespace GenshinLegacy
{
enum class CaptureReason { Uninitialized, WrongGame, EntryNotMatched, EntryInstallFailed, NoRenderToken,
    AmbiguousRenderToken, UnsupportedBindings, InvalidTemporalInput, Pending, ThreadMismatch,
    PreparationFailed, ConsumerFailed, OutputQueued, Draining, CompletionFailed, InstanceCapacity, ReusedOutput, Disabled,
    CaptureQueued, CaptureAdmitted, CaptureRejected, CaptureRetired, ShaderUnavailable, ShaderNotMatched };
// Last capture stage and cumulative counts distinguish exits before shader
// inspection from actual shader, binding and Render-token rejection.
enum class CaptureStage : uint32_t { NotInspected, Disabled, ControlsChanged, DrawCount,
    MissingOwner, ContextState, Thread, Entry, Draining, Pending, CaptureOnly,
    ShaderUnknown, ShaderMismatch, Bindings, Token, Native };
struct Status
{
    CaptureReason reason=CaptureReason::Uninitialized;
    bool entryInstalled=false, pending=false, srAccepted=false, nr=false, fg=false;
    uint64_t observedDraws=0, candidates=0, submitted=0, outputCopies=0, reusedCopies=0, instances=0;
    uint64_t tokenMatches=0,nativeReturns=0,targetShaderDraws=0,bindingRejected=0;
    uint64_t beforeNativeReplacements=0; // Actual SR copyback; no native-return witness.
    uint64_t threeElementDraws=0,preShaderBlocked=0,shaderLookups=0,knownShaderDraws=0,unknownShaderDraws=0;
    CaptureStage captureStage=CaptureStage::NotInspected;
    HRESULT contextStateResult=S_OK;
    Family::ReadObservation shaderObservation{};
    uint32_t bindingRefusal=0;
    uint32_t nativeRefusal=0;
    uint64_t controlsEpoch=0,contextIdentity=0;
    uint32_t controlsVersion=1,owningThread=0;
    bool requestedEnabled=true,effectiveEnabled=false;
    bool requestedFamilySkip=false,effectiveFamilySkip=false,shaderHookInstalled=false;
    Family::Reason familyReason=Family::Reason::Disabled;
    uint64_t familySkipped=0,familyReplacements=0,shaderHash=0,shaderIdentity=0,ownerEpoch=0,deviceEpoch=0;
    bool captureOnlyActive=false;
    uint64_t captureTextureCreates=0,captureCopies=0,captureTextureReuses=0;
};
const char* ReasonText(CaptureReason reason) noexcept;

// The injection point is the real SR consumer, not a capture resolver. Production uses Dx11Owner.
struct Consumer
{
    void* opaque=nullptr;
    GenshinFsr::Result (*submit)(void*,const GenshinFsr::CapturedFrame&)=nullptr;
    GenshinFsr::Reason (*poll)(void*)=nullptr;
    GenshinFsr::Reason (*invalidate)(void*)=nullptr;
};
// A one-use, same-call proof that the game state is held outside the private
// producer's scratch interval. Only CapturePipeline can mint it, after its real
// SwapDeviceContextState and before synchronous internal Submit. It grants no
// resource, parameter, GPU-completion, NR or FG authority.
class InternalSrStateScope final
{
    friend class CapturePipeline;
    inline static thread_local InternalSrStateScope* active_=nullptr;
    InternalSrStateScope* previous_=nullptr;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<ID3DDeviceContextState> scratch_,saved_;
    std::shared_ptr<GenshinTransport::ProducerFrame> producer_;
    GenshinGuides::Owner owner_{};
    std::array<ID3D11Resource*,4> resources_{};
    bool valid_=false,consumed_=false;
    InternalSrStateScope(ID3D11DeviceContext*,ID3DDeviceContextState*,ID3DDeviceContextState*,
        std::shared_ptr<GenshinTransport::ProducerFrame>,const GenshinFsr::CapturedFrame&) noexcept;
public:
    ~InternalSrStateScope();
    InternalSrStateScope(const InternalSrStateScope&)=delete;
    InternalSrStateScope& operator=(const InternalSrStateScope&)=delete;
    static bool Consume(ID3D11DeviceContext*,const GenshinTransport::Scope&,const void* parameters) noexcept;
};
class CapturePipeline
{
    struct Storage;
    std::unique_ptr<Storage> storage_;
    GenshinFsr::NativeDrawAction CompleteNativeReplacement(bool beforeNative=false);
    // Shared by capture and the non-target entry: observers plus mandatory
    // per-draw pending/paired/native cleanup and current-controls revocation.
    bool BeginDrawObservation(UINT count,bool countKnown,unsigned drawKind);
public:
    explicit CapturePipeline(ID3D11DeviceContext* context, Settings settings={}, Consumer consumer={},uint64_t deviceEpoch=0,uint64_t configEpoch=0);
    ~CapturePipeline();
    CapturePipeline(const CapturePipeline&)=delete;
    CapturePipeline& operator=(const CapturePipeline&)=delete;
    GenshinFsr::NativeDrawAction BeforeDraw(UINT elementCount,bool familyAllowed=true,bool countKnown=true,unsigned drawKind=0);
    // Owner-thread positive known non-three draw; keeps all draw observers.
    GenshinFsr::NativeDrawAction BeforeNonTargetDraw(UINT elementCount,unsigned drawKind);
    // Under the native owner lock, construction/owner Present only. Retry a
    // missing scratch state without replacing the pipeline or granting SR.
    void TryInitializeContextState();
    bool Poll();
    bool Invalidate();
    // Serialized lifecycle revocation only; completion and lease release remain owner-thread work.
    void RevokeTransport() noexcept;
    bool NeedsDrain() const noexcept;
    // Under the native owner lock. No polling, snapshot or resource release.
    bool OptionalObservationIdle() const noexcept;
    // CPU Before/After pairing only; async GPU Flights retire through Poll/Present.
    bool DrawPairPending() const noexcept;
    bool ObservationReadersIdle() const noexcept;
    static bool ObservationRetirementsEmpty() noexcept;
    void SubscribeDrawObservation(bool full) noexcept;
    Status GetStatus() const noexcept;
    GenshinPresentNr::Ticket TakePresentNr(IDXGISwapChain*,uint64_t presentEpoch,
        std::shared_ptr<GenshinTransport::ProducerFrame>* acquiredProducer=nullptr) noexcept;
    bool CameraForPresent(const GenshinPresentNr::Ticket&,GenshinBridgeFg::CameraObservation&)const noexcept;
    GenshinTransport::Observation GetTransportObservation() const noexcept;
    NoSrConsumer::Receipt GetCaptureReceipt() const noexcept;
    Engine::Receipt GetEngineProducerReceipt() const noexcept;
    NoSrConsumer::Receipt PresentCapture(const CapturePresentReceipt& present) noexcept;
    std::shared_ptr<const CaptureSnapshot> GetCaptureSnapshot() const noexcept;
    std::shared_ptr<const RawObservation::Snapshot> GetRawObservationSnapshot() const noexcept;
    RawObservation::Receipt GetRawObservationReceipt() const noexcept;
    Presentation::Ticket BeginPresent(std::uint64_t swapchain,UINT flags,bool present1) noexcept;
    Presentation::Receipt CompletePresent(const Presentation::Ticket&,HRESULT result) noexcept;
    Presentation::Receipt GetPresentReceipt() const noexcept;
    void ForgetSwapchain(std::uint64_t swapchain) noexcept;
    void AfterOriginalDraw(UINT count,bool countKnown=true) noexcept;
    void ObservePresentTarget(IDXGISwapChain*,std::uint64_t identity) noexcept;
    static void CollectRetired();
};

// Called by Opti's actual native device/draw/present/resize/unhook paths.
bool InitializeDevice(ID3D11Device* device) noexcept;
bool IsInternalCallActive() noexcept;
void SetShaderHookInstalled(bool installed) noexcept;
void NotePixelShaderCreated(ID3D11Device* device,ID3D11PixelShader* shader,const void* bytes,size_t size) noexcept;
void NoteRawShaderCreated(ID3D11Device*,ID3D11DeviceChild*,const void* bytes,size_t size,unsigned stage) noexcept;
struct OptionalDrawObserver {
    void (*collect)(ID3D11DeviceContext*) noexcept=nullptr;
    bool full=true; // Unknown/refused/exception entries preserve original generic collection.
};
// A hit skips observation, never the game draw. The hook must skip BOTH
// observation Before and After, including indirect scope/return callbacks.
// With no real observer, direct per-instance non-three counts can reject
// capture. Three-element, DrawAuto and indirect draws need a known non-FSR PS.
// This negative classifier never grants SR, replay or decoded GPU arguments.
bool TrySkipNonTargetDrawObservation(ID3D11DeviceContext*,UINT elementCount,bool countKnown,unsigned drawKind) noexcept;
GenshinFsr::NativeDrawAction BeforeNativeDraw(ID3D11DeviceContext* context,UINT elementCount,bool inputsEnabled=true,bool countKnown=true,unsigned drawKind=0,
    const ControlsSnapshot* acquiredControls=nullptr,bool knownNonTargetDraw=false,OptionalDrawObserver* observer=nullptr) noexcept;
// knownNonTargetDraw is a caller hint only. The owner rechecks count, current
// controls, context/thread and drain before omitting target capture getters;
// depth/manual/guide/visible observers and after-draw pairing remain mandatory.
// Lock-free entry/lifecycle demand for common D3D11 hooks, not a request for
// all optional collectors. SR remains active while OptionalDrawObserver may be
// unsubscribed under the real owner lock. False never cancels mandatory drain.
bool UpdateNativeObservationDemand(const ControlsSnapshot& controls) noexcept;
bool NativeObservationActive() noexcept;
bool NativeObservationDrainNeeded() noexcept;
// Borrowed CPU controls for ONE hook invocation only. Each direct draw owns its
// stack-local snapshot; nested draws never overwrite the caller's controls.
// Consumers recheck context/thread and the controls epoch published by all input writers.
struct DrawControlsSnapshot {
    ControlsSnapshot controls{};
    ID3D11DeviceContext* context=nullptr;
    DWORD thread=0;
};
void AfterNativeDraw(ID3D11DeviceContext*,UINT count,bool countKnown=true,const DrawControlsSnapshot* drawControls=nullptr) noexcept;
// CPU-only original-API pairing after current Config/device/thread gates.
void BeforeNativeDepthRaster(ID3D11DeviceContext*,const DrawControlsSnapshot* drawControls=nullptr) noexcept;
// R520: genuine MOD raster observation only. No SR submit/AfterOriginalDraw.
// The token belongs to one physical Before/Draw/After on the native owner thread.
struct ModRasterObservation {
    DrawControlsSnapshot draw;
    uint64_t deviceEpoch=0;
    bool observed=false;
    // Exact existing GenshinDepthMapping::DrawArguments kind/call identity.
    // 1=Draw, 2=DrawIndexed, 4=DrawIndexedInstanced. No kind/count folding.
    UINT drawKind=0;
    uint64_t observationCall=0;
    bool originalBoundary=false; // typed instanced PRE/POST identity only
};
bool BeforeModRaster(ID3D11DeviceContext*,UINT count,UINT first,INT base,ModRasterObservation&) noexcept;
bool AfterModRaster(ID3D11DeviceContext*,ModRasterObservation&) noexcept;
// Tracking-only physical nonindexed Draw. True Before always requires the real
// Draw trampoline followed by AfterModDraw (including a false After result).
// False Before leaves no open observation; neither helper submits/replaces SR.
bool BeforeModDraw(ID3D11DeviceContext*,UINT vertexCount,UINT firstVertex,ModRasterObservation&) noexcept;
bool AfterModDraw(ID3D11DeviceContext*,ModRasterObservation&) noexcept;
// Physical indexed-instanced raster: preserve all five API operands and
// boundary identity. Neither callback submits GPU work or invokes SR.
bool BeforeModIndexedInstanced(ID3D11DeviceContext*,UINT count,UINT instances,UINT first,INT base,
    UINT firstInstance,bool originalBoundary,ModRasterObservation&) noexcept;
bool AfterModIndexedInstanced(ID3D11DeviceContext*,UINT count,UINT instances,UINT first,INT base,
    UINT firstInstance,bool originalBoundary,ModRasterObservation&) noexcept;
void BeforeNativeDepthClear(ID3D11DeviceContext*,ID3D11DepthStencilView*,UINT flags,float depth,UINT8 stencil) noexcept;
void AfterNativeDepthClear(ID3D11DeviceContext*,ID3D11DepthStencilView*,UINT flags,float depth,UINT8 stencil) noexcept;
// Bounded compute metadata/readback; current controls before native owner lock.
void ObserveNativeDepthCompute(ID3D11DeviceContext*,UINT x,UINT y,UINT z) noexcept;
// Native state/deferred/discard boundaries revoke admission without blocking the original API.
void AfterContextBoundary(ID3D11DeviceContext*) noexcept;
void OnPresent(ID3D11Device* device) noexcept;
GenshinPresentNr::Ticket TakePresentNr(IDXGISwapChain*,ID3D11Device*) noexcept;
// Distinct authenticated interop sender; ordinary NR Route is unchanged.
GenshinBridgeFg::SenderInput TakePresentFg(IDXGISwapChain*,ID3D11Device*) noexcept;
HRESULT BeforeResize(IDXGISwapChain* swapchain) noexcept;
void Shutdown() noexcept;
Status GetStatus() noexcept;
GenshinTransport::Observation GetTransportObservation() noexcept;
Engine::Receipt GetEngineProducerReceipt() noexcept;
}

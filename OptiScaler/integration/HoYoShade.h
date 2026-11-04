#pragma once
#include "HoYoShadePolicy.h"
#include "HoYoShadeStatus.h"
#include "HoYoShadeSchedule.h"
#include "HoYoShadeColorSafety.h"
#include "HoYoShadeOwnedRuntime.h"
#include "HoYoShadeDepth.h"
#include <Windows.h>
#include <d3d12.h>
#include <array>
#include <memory>
#include <native/NativeSubmissionReceipt.h>
#include <native/FrameContract.h>

namespace GenshinTransport { class ProducerFrame; class Lease; class Scope; }

namespace DrawUiMask {class Frame;}

namespace HoYoShade
{
// Existing loaded core only. This capability neither registers an addon nor
// creates/enables an effect runtime. Its module is pinned for process lifetime.
struct ExistingFactoryOwner;
using ExistingFactoryOwnerRef=std::shared_ptr<const ExistingFactoryOwner>;
ExistingFactoryOwnerRef AcquireExistingFactoryOwner(const void* terminal,const char* symbol) noexcept;
using FactoryOwnerFreeze=bool(*)(void*,const void*,const void*,size_t,const MEMORY_BASIC_INFORMATION&);
bool ExistingFactoryOwnerIntact(const ExistingFactoryOwnerRef&,const void* terminal,const char* symbol,
                               FactoryOwnerFreeze freeze=nullptr,void* context=nullptr) noexcept;
namespace NaturalAssociation {class Source;}
// Bind the one active acquired source to a unique current callback lifetime /
// epoch before producer work. A missing/ambiguous/consumed source stays denied.
void BindNaturalSource(const NaturalAssociation::Source* source) noexcept;
void Initialize(HMODULE self, bool enabled, unsigned stage) noexcept;
// UI publishes a coherent request only: no SDK/GPU work, blocking mutex,
// preset/config write or DLL loading occurs on this call's thread.
bool Reconfigure(bool enabled, unsigned stage) noexcept;
struct LiveSettingsStatus
{
    bool readable = false, initialized = false, connecting = false, accepted = false;
    bool desiredEnabled = false, appliedEnabled = false, appliedKnown = false;
    unsigned desiredStage = 2, appliedStage = 2;
    uint64_t desiredGeneration = 0, appliedGeneration = 0;
    bool pending = false; // Configuration has not reached a CPU owner boundary.
    bool waitingForEpoch = false; // Latest resolved source only, never all runtimes.
    uint64_t waitingStream = 0, waitingGeneration = 0;
    uint32_t retainedTransactions = 0;
    Reason connectionReason = Reason::Disabled;
};
LiveSettingsStatus ReadLiveSettings() noexcept;
// Narrow observation subscription only. No status snapshot, polling, resource
// export or GPU admission. A zero publication means Full, including contention.
// settings may be zero only for a checked, unstarted/off lifetime. Enabled
// idle additionally requires complete live zero-technique selection at the
// existing claim boundary; selected-but-not-ready never grants this token.
struct ObservationDemand {
    uint64_t settings = 0;
    uint64_t publication = 0; // Revoked before lifecycle/selection/real-owner publication.
    bool enabledIdle = false;
    bool Current() const noexcept;
};
ObservationDemand ReadObservationDemand() noexcept;
// Real Evaluate/Present owner interval, including nested calls. Only an outer
// entry can apply settings; never UI/loader/core callbacks. Old natural epochs
// gate only their resolved source, never another stream or a game/frame counter.
class RenderOwnerScope final
{
    bool entered = false;
public:
    RenderOwnerScope() noexcept;
    ~RenderOwnerScope();
    RenderOwnerScope(const RenderOwnerScope&) = delete;
    RenderOwnerScope& operator=(const RenderOwnerScope&) = delete;
};
#ifdef HOYOSHADE_STAGE_TEST
namespace Testing {
// Standalone host only. Reuses the exact loaded-core identity/API20 handshake
// and callbacks; no fabricated SDK object or alternate product loader.
bool ConnectHost(HMODULE self, const char* config, Stage stage) noexcept;
void EnableHost(bool enabled) noexcept;
}
#endif
bool Selected(Stage stage) noexcept;
// Same-call admission hint for the real native producer, before splitting its
// recording. Uses the accepted core and this bound source's executable
// selection. It grants no submission authority; RenderStage rechecks all facts.
bool NativeBeforeSrAvailable(const NaturalAssociation::Source* source) noexcept;
// Independent of enabled/stage. Disabling HoYo or notifying resize cannot
// make an already uncertain physical color consumable. New chains are isolated.
ConsumerAccess ColorConsumers(IDXGISwapChain* chain) noexcept;
void QuarantineColor(IDXGISwapChain* chain, ID3D12CommandQueue* queue,
                     ID3D12Device* device, ID3D12Resource* color, uint64_t generation) noexcept;
// Recoverable content ambiguity is separate from permanent GPU quarantine.
// This uncopyable capability owns one exact stream/generation/tail denial.
// It can reserve old-buffer discard only after that GPU tail actually completes;
// only a successful real resize commits discard. It never clears quarantine.
class PhysicalContentHold final {
    uint64_t token = 0;
public:
    PhysicalContentHold() = default;
    PhysicalContentHold(const PhysicalContentHold&) = delete;
    PhysicalContentHold& operator=(const PhysicalContentHold&) = delete;
    bool Active() const noexcept { return token != 0; }
    bool Publish(IDXGISwapChain*, ID3D12CommandQueue*, ID3D12Device*, ID3D12Resource*,
                 uint64_t owner, uint64_t generation, ID3D12Fence*, uint64_t tail) noexcept;
    bool ReserveDiscard(IDXGISwapChain*, uint64_t owner, uint64_t generation) noexcept;
    bool ResizeAccess(IDXGISwapChain*, uint64_t owner, uint64_t generation) const noexcept;
    bool CommitDiscard(IDXGISwapChain*, uint64_t owner, uint64_t oldGeneration,
                       uint64_t newGeneration, HRESULT realResizeResult) noexcept;
    void CancelDiscard() noexcept;
};
// Transient admission for an exact physical API12 color writer, independent
// of the driver/menu mutex. The destructor quarantines an abandoned writer;
// FinishAfterReturn closes only this CPU recording interval, never a denial.
class ColorWriteScope {
public:
    ColorWriteScope(bool enabled, ID3D12Device* device, ID3D12CommandQueue* queue,
                    ID3D12Resource* color, IDXGISwapChain* chain = nullptr) noexcept;
    ~ColorWriteScope();
    ColorWriteScope(const ColorWriteScope&) = delete;
    ColorWriteScope& operator=(const ColorWriteScope&) = delete;
    bool Acquired() const noexcept { return token != 0; }
    // Call only after source.Return: uncertain=false means untouched OR the
    // real producer/consumer restored entry state and queued its completion.
    void FinishAfterReturn(bool uncertain) noexcept;
private:
    uint64_t token = 0;
};
// R061: exact wrapper enrollment grants resize coverage, never GPU authority.
// No COM ownership, SDK work, config/UI change or per-frame registration.
void RegisterDirectChain(IDXGISwapChain*, const void* wrapper) noexcept;
void ForgetDirectChain(IDXGISwapChain*, const void* wrapper) noexcept;
class DirectResizeScope final
{
    unsigned chainSlot_ = 32;
    uint64_t lifetime_ = 0;
    bool resourceOnly_ = false;
    OwnedRuntime::Retirement state_ = OwnedRuntime::Retirement::Ready;
    std::array<OwnedRuntime::ResourceCollection, OwnedRuntime::Capacity> collections_;
public:
    DirectResizeScope(IDXGISwapChain*, const void* wrapper) noexcept;
    ~DirectResizeScope();
    DirectResizeScope(const DirectResizeScope&) = delete;
    DirectResizeScope& operator=(const DirectResizeScope&) = delete;
    HRESULT Result() const noexcept;
    void Complete(HRESULT realResult) noexcept;
};
// A real queue Signal after the producer's Execute, or after an already
// submitted cross-API upload + queue Wait. Never mint from a recording game list.
struct ProducerReady
{
    ID3D12CommandQueue* queue = nullptr;
    ID3D12Fence* fence = nullptr;
    uint64_t value = 0;
    native::NativeSubmission submission;
    uint64_t generation = 0, batch = 0;
};
enum class NaturalSourceContract : unsigned {
    TypedBackbuffer, BeforeSrExactDevice, BeforeSrOwnedRuntimeWithSelectionDonor, BeforeSrNativeProducer
};
// BeforeSrExactDevice remains the legacy non-Genshin route. The Genshin
// contract below has three independent authorities: authenticated producer,
// exact owned D3D12 execution domain, and selection-only natural donor.
// Same-call source binding only, not a transfer/SDR metadata override.
// The existing owned transaction retains these full content owners before
// recording. scope is borrowed only during the authenticated invocation.
struct BeforeSrNativeColor
{
    std::shared_ptr<GenshinTransport::ProducerFrame> producer;
    std::shared_ptr<GenshinTransport::Lease> transport;
    const GenshinTransport::Scope* scope = nullptr;
    bool providerHdrKnown = false, providerHdr = false;
};
// Final-picture authority is independent of the BeforeSR four-resource
// invocation. Public observations cannot mint or refresh this capability.
struct BeforeNrNativeColor
{
    std::shared_ptr<const GenshinPresentNr::NativeColorCapability> picture;
    native::NativePictureColorObservation observation;
};
struct ColorEndpoint
{
    std::shared_ptr<DrawUiMask::Frame> uiMask;
    Stage stage = Stage::BeforeNr;
    ID3D12Resource* color = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ProducerReady ready;
    uint64_t stream = 0, generation = 0, frame = 0;
    uint32_t width = 0, height = 0;
    // Active color rectangle in subresource 0, in native texels. Only the
    // legacy BeforeSR exact-device route may crop a larger allocation.
    // All picture/native contracts remain full-resource, with origin (0,0).
    uint32_t colorX = 0, colorY = 0;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    bool stateKnown = false, sdr = false;
    ColorMode colorMode = ColorMode::TransferQualified;
    BeforeSrNativeColor nativeColor;
    BeforeNrNativeColor beforeNrNativeColor;
    // Scheduling device entry and exact source buffer, NOT an SDK native
    // device alias. A synchronous typed proof resolves the resource owner.
    uint64_t naturalDevice = 0, naturalBackbuffer = 0;
    const NaturalAssociation::Source* naturalSource = nullptr; // borrowed active scope
    // For Genshin BeforeSR, naturalDevice is ONLY the D3D11 producer owner
    // observation. It must not match or select a natural runtime GPU device.
    // That low-resolution input is not a typed-backbuffer Source. The owned
    // runtime takes color/queue from the exact D3D12 transport; the unique live
    // natural donor supplies selection plus bounded empty-RTV suppression.
    NaturalSourceContract naturalContract = NaturalSourceContract::TypedBackbuffer;
    OutputKind output = OutputKind::Original;
    Depth::Input depth; // optional real leased input; never modifies SR/NR parameters
    PhysicalSource physicalSource;
    OutputToken outputToken;
};
struct StageResult
{
    Reason reason = Reason::Disabled;
    NativeColorAdmission nativeColor = NativeColorAdmission::NotRequested;
    Attempt attempt;
    OwnedRuntime::Completion completion;
    uint64_t techniques = 0;
    bool depthAvailable = false; // actual owned DEPTH binding reflected by the core
    bool Submitted() const noexcept { return attempt.state == SubmissionState::Submitted; }
    // Execution evidence only. Identity/pass-through techniques need not change pixels.
    bool EffectsExecuted() const noexcept { return Submitted() && techniques != 0; }
    // Legacy native scheduling name; never use as pixel-delta acceptance.
    bool PictureChanged() const noexcept { return EffectsExecuted(); }
    bool Uncertain() const noexcept { return attempt.state == SubmissionState::RecordedOrUncertain; }
};
StageResult RenderStage(const ColorEndpoint& endpoint) noexcept;
// Observe an executable selection on the source's owning thread. This does
// NOT suppress natural effects. Only an admitted physical output may suppress
// its exact natural runtime, after owned techniques and submission succeed.
PhysicalSource PreparePhysicalSource(IDXGISwapChain* source, bool copiedBeforeNatural = false) noexcept;
// Exact scoped snapshot release; stale/destroyed/reused runtime cannot release
// another ticket. No natural effects state or natural frame is modified.
void ReleasePhysicalSource(const PhysicalSource&) noexcept;
// Nonblocking collection before DXGI resize; no backbuffer reference may remain.
void RetirePhysicalOutput(uint64_t stream) noexcept;
// Exact DX12 original backbuffer after optional Present NR, before Opt overlay.
// Physical generated frames require a provider contract; never inferred here.
StageResult RenderPresentOriginal(IDXGISwapChain* chain, ID3D12CommandQueue* queue,
                                  bool sdr, uint64_t frame) noexcept;
void NotifyNativeSubmit(ID3D12CommandQueue* queue, UINT count,
                        ID3D12CommandList* const* lists) noexcept;
}

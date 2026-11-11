// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "GenshinOutputContract.h"
#include "GenshinGpuTransport.h"
#include <d3d11_4.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <array>
#include <memory>

namespace GenshinTransport {
using GenshinGuides::Owner;
enum class Stage : unsigned char { Empty, InputsOrdered, Submitted, CopybackOrdered, Complete, Revoked };
enum class OutputFlushReason : unsigned char { Present, NextSr, ResourceReuse, Resize, Revoke, Consumer };
// Scheduling only: BeforeSR precedes SR on its queue; native BeforeNR and
// FinalOutput run after Acquire's submission. This grants no filter admission.
enum class OutputFilterStage : unsigned char { None, BeforeSr, BeforeNr, FinalOutput, Unknown };
struct OutputRouteContract {
    bool deferOutputFlush=false;
    bool batchAtNativeAcquire=false;
    OutputFilterStage filterStage=OutputFilterStage::None;
    const char* reason="sync-output-flush";
};
struct Observation {
    Owner owner{};
    Stage stage=Stage::Empty;
    std::array<uint64_t,4> source{},shared{},imported{}; // colour, depth, motion, output
    uint64_t queue=0,parameters=0,inputFence=0,submissionFence=0,consumerFence=0;
    bool parametersRestored=false,pipelineRestored=false,copyback=false,revoked=false;
    bool inputsComplete=false,inputWaitComplete=false,consumerComplete=false,copybackComplete=false;
    bool outputFlushPending=false,outputFlushIssued=false;
};
// Three capture-owned slots at most per history. A slot is writable only when
// capture is its sole owner. Producer/Scope/Storage retain it through the real
// GPU quarantine, including failed or unfenced submissions. Exported NR/FG
// packets additionally pin the slot through their existing physical lifetime;
// a retired cache entry keeps only a weak identity, never a writable-slot pin.
class PreparedSrInputs final {
    friend class Lease;
    friend class ProducerFrame; // Exact source/context proof for SR resource admission only.
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<ID3D12Device> importedDevice_;
    std::array<Microsoft::WRL::ComPtr<ID3D11Texture2D>,2> textures_;
    std::array<Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView>,2> uavs_;
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>,2> imported_;
    DWORD thread_=0;
    bool importUnavailable_=false;
    PreparedSrInputs()=default;
    bool Import(ID3D12Device*) noexcept;
public:
    static std::shared_ptr<PreparedSrInputs> Create(ID3D11DeviceContext*,UINT width,UINT height) noexcept;
    ID3D11Texture2D* Texture(size_t i) const noexcept {return i<2?textures_[i].Get():nullptr;}
    ID3D11UnorderedAccessView* Uav(size_t i) const noexcept {return i<2?uavs_[i].Get():nullptr;}
};
// Capture-owned output, minted only from a completed transport's actual work
// domain. Import/UAV support is established before this can become h.output.
// No public constructor or pointer-only allow-shared authority. The original
// private output is retained separately by History as the unsupported fallback.
class PreparedSrOutput final {
    friend class Lease;
    friend class ProducerFrame;
    Owner owner_{};
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture_;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> uav_;
    Microsoft::WRL::ComPtr<ID3D12Resource> imported_;
    GpuWorkDomain domain_;
    D3D11_TEXTURE2D_DESC desc_{};
    PreparedSrOutput()=default;
    bool Authenticate(const Owner&,ID3D11DeviceContext*,ID3D11Resource*,UINT,UINT,DXGI_FORMAT) const noexcept;
public:
    static bool PureSr() noexcept;
    // Capture must independently retire History, diagnostics and paired readers
    // before selecting this resource for another write. This proves identity only.
    ID3D11Texture2D* TextureFor(const Owner&,ID3D11DeviceContext*,UINT,UINT,DXGI_FORMAT) const noexcept;
};
struct Storage;
// This is emitted only by the real completed SR transport. Integer progress
// counters cannot stand in for its retained resources, command list or fences.
// It certifies ordering/identity only, never depth, motion or camera semantics.
class CompletedTransfer final {
    friend class Lease;
    CompletedTransfer()=default;
public:
    struct View {
        Owner owner{};
        std::array<Microsoft::WRL::ComPtr<ID3D11Resource>,4> sources;
        std::array<Microsoft::WRL::ComPtr<ID3D12Resource>,4> imported;
        std::array<D3D11_TEXTURE2D_DESC,4> descriptors{};
        Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
        Microsoft::WRL::ComPtr<ID3D12Fence> inputs,submitted;
        Microsoft::WRL::ComPtr<ID3D11Fence> returned;
        // Immutable per-packet values on a slot-owned monotonic fence group.
        // Never infer completion from a counter, assume value 1, or accept
        // UINT64_MAX. Holding surfaceLifetime excludes reuse by another lease.
        uint64_t inputValue=0,submittedValue=0,returnedValue=0;
        // Type-erased shared-surface/fence-group lifetime. Its deleter requires
        // all transport/consumer fences and the last external reader/packet
        // owner to retire before the slot can be reused. Direct leases do not
        // export this packet.
        std::shared_ptr<const void> surfaceLifetime;
    };
    const View& Get() const noexcept {return view_;}
private:
    View view_;
};
// Successful queue submission and ordered copyback are distinct from CPU-observed
// completion. This packet retains the actual per-invocation shared input copies,
// never the reusable capture scratch content. Consumers must order their reads.
class QueuedTransfer final {
    friend class Lease;
    QueuedTransfer()=default;
    CompletedTransfer::View view_;
    std::array<Microsoft::WRL::ComPtr<ID3D11Texture2D>,4> prepared_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext4> context_;
    Microsoft::WRL::ComPtr<ID3D11Fence> submitted11_;
    // Private ordering receipt for this packet's owner/context/submitted11_.
    // Minted from a successful Copyback Wait, never from fence completion.
    uint64_t copybackWaitValue_=0;
    friend class NativeQueuedDepthLease;
    // Scheduling proof only. The private Acquire caller must still bind and
    // submit its real input-ready signal after this exact context dependency.
    bool CopybackOrdersRead(ID3D11DeviceContext4*)const noexcept;
public:
    const CompletedTransfer::View& Get()const noexcept {return view_;}
    ID3D11Texture2D* Prepared(size_t i)const noexcept {return i<prepared_.size()?prepared_[i].Get():nullptr;}
    // Exact owning context; reuse its successful Copyback dependency when it
    // covers this packet, otherwise enqueue the original producer fence Wait.
    // No CPU completion or event wait is claimed.
    bool OrderRead(ID3D11DeviceContext4*)const noexcept;
};
// Observation of resource transport only. Never grants guide admission or provider success.
// Calls must remain on owner.thread; GPU retention is independent of the caller's lifetime.
class Lease final {
    std::unique_ptr<Storage> storage_;
    explicit Lease(std::unique_ptr<Storage>);
    bool Match(const Owner&) noexcept;
public:
    // Retired cache capacity follows the existing three-flight recording ring.
    // Active/exported/quarantined leases retain their original owners and are
    // never evicted or overwritten to satisfy this cache budget.
    static constexpr size_t SurfaceCacheCapacity=GpuWorkDomain::RecordingCount;
    ~Lease();
    Lease(const Lease&)=delete;
    Lease& operator=(const Lease&)=delete;
    static std::shared_ptr<Lease> Begin(const Owner&,ID3D11DeviceContext*,const GpuWorkDomain&,ID3D12CommandQueue*,
        const std::array<ID3D11Resource*,4>&,const void* parameters,
        std::shared_ptr<PreparedSrInputs> preparedInputs={});
    ID3D12Resource* Resource(const Owner&,size_t index) noexcept;
    struct Submission {
        bool executed=false,returned=false,submitted=false;
        HRESULT hr=S_OK;
        const char* reason="ok";
    };
    // Own preflight, lifetime pinning, Execute and the GPU fence as one operation.
    // A rejected post-execute witness can retire, but can never publish output.
    Submission Execute(const Owner&,ID3D12CommandQueue*,ID3D12GraphicsCommandList*,ID3D12CommandAllocator*) noexcept;
    // Optional monitor timestamps share this exact invocation/list and its
    // existing submission fence. Failure only drops timing, never SR admission.
    void BeginGpuTimeline(const Owner&,ID3D12GraphicsCommandList*,ID3D12CommandAllocator*) noexcept;
    void EndGpuTimeline(const Owner&,ID3D12GraphicsCommandList*) noexcept;
    bool Restored(const Owner&,bool parameters,bool pipeline) noexcept;
    bool Copyback(const Owner&,bool deferFlush=false) noexcept;
    // Submission only. Wait/copy remain at Copyback; completion is still Poll.
    bool FinishOutputFlush(const Owner&,OutputFlushReason) noexcept;
    bool OutputFlushPending() const noexcept;
    bool CanBatchOutputFlush() const noexcept;
    bool ObserveOutputFlush(const Owner&,ID3D11DeviceContext*) noexcept;
    // Only this same queue may consume leased resources; signal after its last consumer submission.
    bool ConsumerSubmitted(const Owner&,ID3D12CommandQueue*,ID3D12GraphicsCommandList*,ID3D12CommandAllocator*);
    void Revoke() noexcept;
    bool Poll() noexcept;
    // Fresh owner-thread query only: never Poll/export/release to infer absence.
    // An escaped packet or retained surface lifetime keeps full observation.
    bool ObservationQuiescent(const Owner&) const noexcept;
    // For the authenticated current producer invocation only: this says nothing
    // about completion. The owner must retain that invocation's normal flight.
    bool ObservationNoEscapedReaders(const Owner&) const noexcept;
    Observation Snapshot() const noexcept;
    // Owner-thread, first-failure observation only. Never Poll/Flush/Revoke or
    // turn device-loss/fence values into completion/admission authority.
    void LogFailureSnapshot(UINT recordingSlot) const noexcept;
    // No work or resource export: a completed, unescaped transport can establish
    // the next capture-owned output against its exact creator/public owner/queue.
    std::shared_ptr<PreparedSrOutput> PrepareSrOutput(const Owner&) noexcept;
    // Owning thread only, after actual input/wait/consumer/copyback completion.
    // No GPU command, wait or signal is issued by this optional FG acquisition.
    std::shared_ptr<const CompletedTransfer> Completed(const Owner&) noexcept;
    // Only after actual SR submit/restoration/fenced copyback ordering. Does not
    // poll or wait for GPU completion and cannot authorize ordinary NR readers.
    std::shared_ptr<const QueuedTransfer> Queued(const Owner&) noexcept;
    // Exact native source/context only. One bounded event wait per lease may
    // bridge Present's early CPU poll to actual completion; timeout grants nothing.
    // The returned receipt has ALL the same physical gates as Completed().
    static constexpr DWORD DepthCompletionWaitMs=16;
    std::shared_ptr<const CompletedTransfer> CompletedDepth(const Owner&,ID3D11DeviceContext*,ID3D11Resource*) noexcept;
};
// Collect only completed retained leases. Failed Signal/ambiguous consumer work
// permanently quarantines its actual owners; a later higher fence value cannot
// turn that failed receipt into retirement. No forced release on device removal.
size_t CollectRetired() noexcept;
size_t RetainedCount() noexcept;
// Includes packet lifetimes after their producing Lease/History has gone away.
bool ObservationRetirementsEmpty() noexcept;
OutputRouteContract CurrentOutputRoute() noexcept;
bool QueueDeferredOutputFlush(const std::shared_ptr<Lease>&,const Owner&) noexcept;
// Owning-thread pending slots only; does not wait for GPU completion.
size_t FinishPendingOutput(ID3D11Device* device,OutputFlushReason) noexcept;
// Called only AFTER the named immediate context actually issued Flush.
void ObserveOutputFlush(ID3D11DeviceContext*) noexcept;
// Ordinary/changed routes finish on entry. The selected full-stack route may
// use Acquire's existing submission; all same-thread early returns still drain.
class PresentOutputScope final {
    bool active_;
public:
    explicit PresentOutputScope(bool active) noexcept;
    ~PresentOutputScope();
    PresentOutputScope(const PresentOutputScope&)=delete;
    PresentOutputScope& operator=(const PresentOutputScope&)=delete;
};

// Private synchronous producer seam. ProducerInvocation and the real SR receipt authenticate
// full Owner and same-call resources/parameters; descriptors and SR booleans grant no authority.
class Scope final {
    Scope* previous_=nullptr;
    bool owns_=false;
public:
    Owner owner;
    std::array<ID3D11Resource*,4> resources;
    const void* parameters;
    std::shared_ptr<Lease> lease;
    std::shared_ptr<PreparedSrInputs> preparedInputs;
    std::shared_ptr<PreparedSrOutput> preparedOutput;
    Scope(const Owner&,const std::array<ID3D11Resource*,4>&,const void*);
    ~Scope();
    Scope(const Scope&)=delete;
    Scope& operator=(const Scope&)=delete;
    static Scope* Current() noexcept;
};
}

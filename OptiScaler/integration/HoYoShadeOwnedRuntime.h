#pragma once
#include "HoYoShadeApi.h"
#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <cstdint>
#include <memory>

namespace HoYoShade::OwnedRuntime
{
// All entry points are outside DllMain. The caller supplies the already accepted,
// process-pinned core and the actual F5 native objects, never registry snapshots.
// Pool storage intentionally has process lifetime, including quarantined entries.
// Optional observation on the existing retained payload. Runs synchronously;
// must not reenter this pool, perform SDK/GPU work, or release the payload.
enum class PayloadEvent { Submitted, Completed, Quarantined };
using PayloadObserver=void (*)(void*,PayloadEvent,uintptr_t,uint64_t,uint64_t) noexcept;
enum class Status
{
    Ready, InvalidBinding, CoreUnavailable, Capacity, StaleGeneration,
    BindingChanged, Busy, GpuPending, Retired, CreateFailed, IdentityMismatch,
    SignalFailed, CompletionUnknown, WrongThread, Quarantined
};

// Diagnostic-only value: no pointers, strings, allocation or completion authority.
// None means no numeric error was returned (including logical/pending/exception
// outcomes); Boolean is an actual bool result, never a fabricated HRESULT.
enum class CodeKind : uint8_t { None, Boolean, Win32, HResult, Count };
// Diagnostic schema v1: these textual field values are the wire contract. Keep
// names stable; additions need mapping tests, not module-specific string copies.
inline constexpr unsigned DiagnosticSchemaVersion = 1;
inline constexpr unsigned DiagnosticStageCount = 3;
inline constexpr uint64_t DiagnosticInitialReports = 3;
inline constexpr uint64_t DiagnosticReportPeriod = 600;
constexpr bool ShouldReportDiagnosticCount(uint64_t count) noexcept
{ return count <= DiagnosticInitialReports || count % DiagnosticReportPeriod == 0; }
enum class DiagnosticOperation : uint8_t
{
    None, Acquire, ValidateBinding, ReadExports, SelectOwner, SelectEntry,
    CreateWindow, CreateSwapchain, CreateFence, SwapchainDevice, CreateRuntime,
    RuntimeIdentity, CompletionDevice, CompletionFence, ReleasePayloads,
    ImmediateList, RetireCheck, Submit, FlushImmediateList, UpdateRuntime,
    FenceValue, SignalDevice, Signal, Count
};
struct Detail
{
    Status status = Status::Ready;
    DiagnosticOperation operation = DiagnosticOperation::None;
    CodeKind codeKind = CodeKind::None;
    uint32_t code = 0;
    bool attempted = false;
};
inline constexpr unsigned StatusCount = static_cast<unsigned>(Status::Quarantined) + 1;
inline constexpr unsigned CodeKindCount = static_cast<unsigned>(CodeKind::Count);
inline constexpr unsigned OperationCount = static_cast<unsigned>(DiagnosticOperation::Count);
constexpr const char* Text(Status s) noexcept
{
    switch (s)
    {
#define F5_OWNED_STATUS(name, text) case Status::name: return text
    F5_OWNED_STATUS(Ready, "Ready"); F5_OWNED_STATUS(InvalidBinding, "InvalidBinding"); F5_OWNED_STATUS(CoreUnavailable, "CoreUnavailable");
    F5_OWNED_STATUS(Capacity, "Capacity"); F5_OWNED_STATUS(StaleGeneration, "StaleGeneration"); F5_OWNED_STATUS(BindingChanged, "BindingChanged");
    F5_OWNED_STATUS(Busy, "Busy"); F5_OWNED_STATUS(GpuPending, "GpuPending"); F5_OWNED_STATUS(Retired, "Retired");
    F5_OWNED_STATUS(CreateFailed, "CreateFailed"); F5_OWNED_STATUS(IdentityMismatch, "IdentityMismatch"); F5_OWNED_STATUS(SignalFailed, "SignalFailed");
    F5_OWNED_STATUS(CompletionUnknown, "CompletionUnknown"); F5_OWNED_STATUS(WrongThread, "WrongThread"); F5_OWNED_STATUS(Quarantined, "Quarantined");
#undef F5_OWNED_STATUS
    }
    return "UnknownStatus";
}
constexpr const char* Text(DiagnosticOperation op) noexcept
{
    switch (op)
    {
#define F5_OWNED_OPERATION(name, text) case DiagnosticOperation::name: return text
    F5_OWNED_OPERATION(None, "None"); F5_OWNED_OPERATION(Acquire, "Acquire"); F5_OWNED_OPERATION(ValidateBinding, "ValidateBinding");
    F5_OWNED_OPERATION(ReadExports, "ReadExports"); F5_OWNED_OPERATION(SelectOwner, "SelectOwner"); F5_OWNED_OPERATION(SelectEntry, "SelectEntry");
    F5_OWNED_OPERATION(CreateWindow, "CreateWindow"); F5_OWNED_OPERATION(CreateSwapchain, "CreateSwapchain"); F5_OWNED_OPERATION(CreateFence, "CreateFence");
    F5_OWNED_OPERATION(SwapchainDevice, "SwapchainDevice"); F5_OWNED_OPERATION(CreateRuntime, "CreateRuntime"); F5_OWNED_OPERATION(RuntimeIdentity, "RuntimeIdentity");
    F5_OWNED_OPERATION(CompletionDevice, "CompletionDevice"); F5_OWNED_OPERATION(CompletionFence, "CompletionFence"); F5_OWNED_OPERATION(ReleasePayloads, "ReleasePayloads");
    F5_OWNED_OPERATION(ImmediateList, "ImmediateList"); F5_OWNED_OPERATION(RetireCheck, "RetireCheck"); F5_OWNED_OPERATION(Submit, "Submit");
    F5_OWNED_OPERATION(FlushImmediateList, "FlushImmediateList"); F5_OWNED_OPERATION(UpdateRuntime, "UpdateRuntime"); F5_OWNED_OPERATION(FenceValue, "FenceValue");
    F5_OWNED_OPERATION(SignalDevice, "SignalDevice"); F5_OWNED_OPERATION(Signal, "Signal"); F5_OWNED_OPERATION(Count, "Count");
#undef F5_OWNED_OPERATION
    }
    return "UnknownOperation";
}
constexpr const char* Text(CodeKind kind) noexcept
{
    switch (kind)
    {
    case CodeKind::None: return "none";
    case CodeKind::Boolean: return "bool";
    case CodeKind::Win32: return "win32";
    case CodeKind::HResult: return "hresult";
    case CodeKind::Count: break;
    }
    return "unknown-code-kind";
}
// A single boundary projection for all module reports. The original Reason
// remains a control-flow result; these independent fields preserve typed cause.
struct DiagnosticFields
{
    bool attempted;
    const char* status;
    const char* operation;
    const char* codeKind;
    uint32_t code;
};
constexpr DiagnosticFields Fields(const Detail& detail) noexcept
{
    return {detail.attempted, detail.attempted ? Text(detail.status) : "not-attempted",
        Text(detail.operation), Text(detail.codeKind), detail.code};
}
// Existing first-three/every-600 policy, partitioned by operation/status so
// transient Busy cannot hide an eventual CreateFailed. Caller holds its mutex.
struct DiagnosticReports
{
    uint64_t counts[DiagnosticStageCount][OperationCount][StatusCount]{};
    bool ShouldReport(unsigned stage, const Detail& detail) noexcept
    {
        if (!detail.attempted || stage < 1 || stage > DiagnosticStageCount ||
            static_cast<unsigned>(detail.operation) >= OperationCount ||
            static_cast<unsigned>(detail.status) >= StatusCount) return false;
        auto& count = counts[stage - 1][static_cast<unsigned>(detail.operation)][static_cast<unsigned>(detail.status)];
        ++count;
        return ShouldReportDiagnosticCount(count);
    }
};

struct Binding
{
    uint64_t owner = 0;      // Stable F5 stream owner, not an arbitrary SDK pointer.
    uint64_t generation = 0; // Strictly increasing on device/queue/size/config reset.
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr; // Exact DIRECT queue, not an adapter alias.
    IDXGIFactory2* factory = nullptr;    // Existing F5 DXGI factory.
    uint32_t width = 0, height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    const char* configPathUtf8 = nullptr; // Explicit core configuration; copied once.
    // Optional private config/preset owner; retained through core destruction,
    // not released with per-submit payloads. Quarantined entries keep it.
    std::shared_ptr<void> configLifetime;
};

struct Completion
{
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    uint64_t value = 0;
    // Nonblocking. MAX/device removal is unknown and never authorizes recycling.
    bool IsComplete() const noexcept;
};

// R207: diagnostic scalars only. They never mint a lease, certify completion or
// stand in for an effect submission. A saved entry trace is last-warmup context;
// its token/runtime are rebound to the actual reserved pool entry at each use.
struct WarmupTrace
{
    uint64_t attempt = 0, natural = 0, lifetime = 0, settings = 0, revision = 0, reload = 0;
    uint64_t owner = 0, generation = 0, token = 0, runtime = 0;
    uint64_t stream = 0, sourceGeneration = 0, sourceFrame = 0, skipped = 0;
    uint64_t predecessor = 0, predecessorValue = 0;
    uint64_t device = 0, queue = 0, factory = 0, configLifetime = 0;
    uint32_t width = 0, height = 0, format = 0;
    bool creating = false;
};
enum class WallOperation : uint8_t
{
    Attempt, Acquire, CreateWindow, CreateSwapchain, CreateFence, CreateRuntime,
    ImmediateList, DepthSemantics, ApplySelection, EmptyRender, Flush, UpdateRuntime,
    Signal, DestroyRuntime, DestroyWindow, Count
};
// Process-wide hard cap, NOT a periodic report. Budget exhaustion only stops
// diagnostics; the recoverable warmup scheduler and genuine core calls continue.
inline constexpr unsigned WallReportsPerOperation = 16;
uint64_t NextWarmupAttempt() noexcept;
class WallScope final
{
    WarmupTrace trace_{};
    WallOperation operation_;
    LARGE_INTEGER begin_{};
    int exceptions_ = 0;
    bool reporting_ = false, clockKnown_ = false;
    const char* outcome_ = "returned"; // static text only; no SDK/string ownership
public:
    WallScope(WallOperation operation, const WarmupTrace* trace) noexcept;
    ~WallScope() noexcept;
    WallScope(const WallScope&) = delete;
    WallScope& operator=(const WallScope&) = delete;
    void Context(const WarmupTrace& trace) noexcept { trace_ = trace; }
    void Outcome(const char* outcome) noexcept { outcome_ = outcome; }
};
template<class Call>
decltype(auto) MeasureWarmup(const WarmupTrace* trace, WallOperation operation, Call&& call)
{
    if (!trace) return call(); // normal owned rendering takes this direct branch
    WallScope wall(operation, trace);
    return call();
}

struct Resource
{
    virtual ~Resource() = default; // Must not call SDK/GPU APIs here.
    // Invoked by the pool with the genuine SDK device still alive, no active
    // render call, and the last submitted batch complete. Destroy only owned
    // resources/views; never record or submit additional GPU work.
    virtual void DestroyGpuObjects(reshade::api::device* device) noexcept = 0;
    // Checked bundles retain their native owners on a destruction failure.
    // The pool must keep the object AND runtime, rather than deleting it and
    // mistaking a failed SDK destroy for physical retirement.
    virtual bool GpuObjectsReleased() const noexcept { return true; }
};

// Minted only by a genuine active pool lease. Scalar identity never owns GPU
// objects and cannot turn a reused slot/runtime address into the old entry.
class EntryReceipt final
{
    friend class Lease;
    friend class ResourceCollection;
    friend Status ReserveResources(const EntryReceipt&, class ResourceCollection&) noexcept;
    uint32_t slot_ = 0;
    uint64_t token_ = 0, owner_ = 0, generation_ = 0;
    reshade::api::effect_runtime* runtime_ = nullptr;
public:
    explicit operator bool() const noexcept { return token_ != 0; }
    bool operator==(const EntryReceipt&) const = default;
};
enum class Retirement { Ready, Pending, Unknown };
// Pins exactly one live pool entry through resource collection / real resize.
// No runtime creation, submission, GPU wait or core teardown, including retries
// without another Present/Execute. Ready here means collection ran; the caller
// must ALSO observe its resources' physical destruction receipts.
class ResourceCollection final
{
    friend Status ReserveResources(const EntryReceipt&, ResourceCollection&) noexcept;
    EntryReceipt entry_;
    void Release() noexcept;
public:
    ResourceCollection() = default;
    ~ResourceCollection();
    ResourceCollection(ResourceCollection&&) noexcept;
    ResourceCollection& operator=(ResourceCollection&&) noexcept;
    ResourceCollection(const ResourceCollection&) = delete;
    ResourceCollection& operator=(const ResourceCollection&) = delete;
    explicit operator bool() const noexcept { return bool(entry_); }
    Retirement Poll() noexcept;
};
Status ReserveResources(const EntryReceipt&, ResourceCollection&) noexcept;

class Lease
{
public:
    Lease() noexcept = default;
    ~Lease();
    Lease(Lease&&) noexcept;
    Lease& operator=(Lease&&) noexcept;
    Lease(const Lease&) = delete;
    Lease& operator=(const Lease&) = delete;
    explicit operator bool() const noexcept { return _token != 0; }

    // Valid only on the acquiring thread and until Submit or lease destruction.
    // These are genuinely owned objects protected by an exclusive active call,
    // not borrowed observer registry pointers. Do not cache them beyond the lease.
    reshade::api::effect_runtime* Runtime() const noexcept;
    reshade::api::device* Device() const noexcept;
    reshade::api::command_queue* Queue() const noexcept;
    reshade::api::command_list* List() const noexcept;
    ID3D12GraphicsCommandList* NativeList() const noexcept;
    uint64_t Generation() const noexcept;
    EntryReceipt Receipt() const noexcept;

    // Explicitly transfers a cached SDK resource bundle into pool custody. The
    // returned owner may outlive this render lease and Retire; its last release
    // only queues cleanup. GPU completion AND exclusive collection authorize
    // DestroyGpuObjects, then destruction of the bundle and finally runtime.
    // Eight bundles maximum per runtime. On failure resource stays caller-owned.
    bool AdoptResource(std::unique_ptr<Resource>& resource, std::shared_ptr<Resource>& owner) noexcept;

    // Up to eight strong payload owners per batch. Use adopted resource owners
    // for SDK objects; generic payload destructors MUST NOT use SDK pointers.
    // Generic owners can retain native COM inputs, buffers and CPU metadata.
    bool KeepAlive(const std::shared_ptr<void>& owner, PayloadObserver observer = nullptr) noexcept;

    // Flushes the owned SDK immediate list, advances the runtime frame through
    // its real export, then signals the exact native queue. Render effects on the
    // external RTV before this call; Update suppresses the natural duplicate.
    // The returned fence proves this batch, not that any technique ran.
    Status Submit(Completion& completion, Detail* detail = nullptr,
                  const WarmupTrace* trace = nullptr) noexcept;

private:
    friend Status AcquireForRender(const Binding&, HMODULE, const CoreApi&, Lease&, Detail*, WarmupTrace*) noexcept;
    uint32_t _slot = 0;
    uint64_t _token = 0;
    DWORD _thread = 0;
    bool _submitted = false;
    void Release() noexcept;
};

inline constexpr unsigned Capacity = 4;
// Optional trace is diagnostic in/out: only scalars from the actual reserved
// entry are returned. No receipt/SDK pointer lifetime is granted by this value.
Status AcquireForRender(const Binding& binding, HMODULE loadedCore,
                        const CoreApi& acceptedApi, Lease& lease, Detail* detail = nullptr,
                        WarmupTrace* trace = nullptr) noexcept;
// Stop new calls immediately; an existing active lease may finish/submit safely.
// generation==0 retires every generation of owner, owner==0 retires the pool.
void Retire(uint64_t owner = 0, uint64_t generation = 0) noexcept;
struct Collection
{
    unsigned destroyed = 0, pending = 0, active = 0, quarantined = 0, wrongThread = 0, resourceOwners = 0;
};
// Polls fences without a CPU fence wait. Core teardown itself is an external
// export and may perform internal work. HWNDs are destroyed only
// on their creating thread. Call from the first-acquire thread during teardown;
// otherwise entries remain retained until that thread calls TryCollect.
Collection TryCollect() noexcept;
// Read-only observation claim at an existing owner/Present boundary. No fence
// polling, SDK calls, retirement or resource release. Non-free entries retain
// real runtime/GPU custody; historical Owner keys alone do not block idle.
// HoYoShade revokes its demand token before entering the render/acquire path.
bool ObservationOwnersIdle() noexcept;

#ifdef HOYOSHADE_OWNED_RUNTIME_TEST
namespace Testing
{
struct Hooks
{
    HRESULT (*signal)(ID3D12CommandQueue*, ID3D12Fence*, uint64_t) = nullptr;
    uint64_t (*completed)(ID3D12Fence*) = nullptr;
    HWND (*createWindow)(const Binding&) = nullptr;
    HRESULT (*createSwapchain)(IDXGIFactory2*, ID3D12CommandQueue*, HWND,
        const DXGI_SWAP_CHAIN_DESC1*, IDXGISwapChain1**) = nullptr;
    HRESULT (*createFence)(ID3D12Device*, ID3D12Fence**) = nullptr;
    bool (*createRuntime)(reshade::api::device_api, void*, void*, void*,
        const char*, reshade::api::effect_runtime**) = nullptr;
};
// Standalone helper fault injection, absent from product builds. Set only while
// no module operation runs. Creation hooks inject failure/null only, never fake
// valid core/runtime objects. They are absent from product builds.
void SetHooks(Hooks hooks) noexcept;
}
#endif
}

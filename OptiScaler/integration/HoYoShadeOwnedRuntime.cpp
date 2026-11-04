#ifndef HOYOSHADE_OWNED_RUNTIME_STANDALONE
#include "pch.h"
#elif defined(HOYOSHADE_OWNED_RUNTIME_PRODUCT_LOGGING)
// Keep the product TU free of Config/PCH while using its existing logger.
#include "SysUtils.h"
#endif
#include "HoYoShadeOwnedRuntime.h"
#include "HoYoShadeColorContract.h"
#include "HoYoShadeTypedCreationOwner.h"
#include <array>
#include <atomic>
#include <cstring>
#include <exception>
#include <limits>
#include <mutex>
#include <new>
#include <optional>
#include <utility>

namespace HoYoShade::OwnedRuntime
{
namespace
{
using Microsoft::WRL::ComPtr;
namespace api = reshade::api;
constexpr uint64_t MaxValue = (std::numeric_limits<uint64_t>::max)();
constexpr size_t MaxPath = 2048, MaxPayloads = 8;
using CreateRuntime = bool (*)(api::device_api, void*, void*, void*, const char*, api::effect_runtime**);
using DestroyRuntime = void (*)(api::effect_runtime*);
using UpdateRuntime = void (*)(api::effect_runtime*);
enum class Phase { Free, Creating, Live, Collecting, Quarantined };
struct Entry
{
    Phase phase = Phase::Free;
    bool active = false, retiring = false;
    unsigned resourceReservations = 0; // skips runtime teardown while resize holds an exact pin
    uint64_t token = 0, owner = 0, generation = 0, fenceValue = 0;
    DWORD windowThread = 0;
    uint32_t width = 0, height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    std::array<char, MaxPath> config{};
    std::shared_ptr<void> configLifetime;
    HWND window = nullptr;
    HMODULE core = nullptr;
    DestroyRuntime destroy = nullptr;
    UpdateRuntime update = nullptr;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<IDXGIFactory2> factory;
    ComPtr<IDXGISwapChain1> swapchain;
    ComPtr<ID3D12Fence> fence;
    // Actual factory queue/creator and each typed returned owner stay separate.
    // These receipts authorize only their own swapchain/fence, never a list,
    // resource, query birth or a creator/public-owner alias.
    TypedCreationOwner<IDXGISwapChain1> swapchainOwner;
    TypedCreationOwner<ID3D12Fence> fenceOwner;
    api::effect_runtime* runtime = nullptr;
    api::device* sdkDevice = nullptr;
    api::command_queue* sdkQueue = nullptr;
    api::command_list* sdkList = nullptr;
    ComPtr<ID3D12GraphicsCommandList> nativeList;
    struct Payload {std::shared_ptr<void> owner;PayloadObserver observer=nullptr;};
    std::array<Payload, MaxPayloads> payloads;
    struct OwnedResource
    {
        Resource* object = nullptr; // Exclusive pool custody, not a borrowed registry object.
        uint64_t token = 0;
        // Final owner release has no pool-lock acquisition or SDK work. The
        // shared flag belongs to this adoption, never to a reused array slot.
        std::shared_ptr<std::atomic<bool>> released;
    };
    std::array<OwnedResource, MaxPayloads> resources{};
    uint64_t nextResourceToken = 0;
    WarmupTrace warmupTrace{}; // scalar last-attempt context, no extra owners
};
struct Owner
{
    uint64_t id = 0, generation = 0;
    bool retired = false;
};
struct Pool
{
    std::mutex mutex;
    std::array<Entry, Capacity> entries;
    std::array<Owner, Capacity> owners{};
    uint64_t nextToken = 0;
    HMODULE core = nullptr;
};
Pool& Get() { static Pool* pool = new Pool; return *pool; }
thread_local bool inOperation = false;
struct Operation
{
    bool entered = !inOperation;
    Operation() noexcept { if (entered) inOperation = true; }
    ~Operation() { if (entered) inOperation = false; }
};

std::array<std::atomic<unsigned>, static_cast<size_t>(WallOperation::Count)> wallReports{};
std::atomic<uint64_t> nextWarmupAttempt{0};
struct PreserveLastError
{
    DWORD value = GetLastError();
    ~PreserveLastError() { SetLastError(value); }
};
bool ReserveWallReport(WallOperation operation) noexcept
{
    const auto index = static_cast<size_t>(operation);
    if (index >= wallReports.size()) return false;
    auto& budget = wallReports[index];
    auto count = budget.load(std::memory_order_relaxed);
    // Even diagnostic contention has a fixed retry bound. No pool/new mutex.
    for (unsigned retry = 0; retry < 4 && count < WallReportsPerOperation; ++retry)
        if (budget.compare_exchange_weak(count, count + 1, std::memory_order_relaxed)) return true;
    return false;
}
const char* WallText(WallOperation operation) noexcept
{
    switch (operation) {
#define F5_WARMUP_WALL(name) case WallOperation::name: return #name
    F5_WARMUP_WALL(Attempt); F5_WARMUP_WALL(Acquire); F5_WARMUP_WALL(CreateWindow);
    F5_WARMUP_WALL(CreateSwapchain); F5_WARMUP_WALL(CreateFence); F5_WARMUP_WALL(CreateRuntime);
    F5_WARMUP_WALL(ImmediateList); F5_WARMUP_WALL(DepthSemantics); F5_WARMUP_WALL(ApplySelection);
    F5_WARMUP_WALL(EmptyRender); F5_WARMUP_WALL(Flush); F5_WARMUP_WALL(UpdateRuntime);
    F5_WARMUP_WALL(Signal); F5_WARMUP_WALL(DestroyRuntime); F5_WARMUP_WALL(DestroyWindow);
#undef F5_WARMUP_WALL
    case WallOperation::Count: break;
    }
    return "UNKNOWN";
}
void ReportWall(const WarmupTrace& t, WallOperation operation, const char* boundary,
                int64_t tick, bool wallKnown, double wallMs, const char* outcome) noexcept
{
#if !defined(HOYOSHADE_OWNED_RUNTIME_STANDALONE) || defined(HOYOSHADE_OWNED_RUNTIME_PRODUCT_LOGGING)
    try {
        LOG_INFO("HoYoShade R207 warmup attempt={} operation={} boundary={} qpc={} wall_ms_known={} wall_ms={:.3f} outcome={} thread={} owner=0x{:x} binding_generation={} entry_token={} runtime=0x{:x} creating={} device=0x{:x} queue=0x{:x} factory=0x{:x} config_lifetime=0x{:x} extent={}x{} format={} natural=0x{:x} lifetime={} settings={} revision={} reload={} source_stream=0x{:x} source_generation={} source_frame={} skipped={} predecessor_fence=0x{:x} predecessor_value={} context_scope=last-warmup identity_authority=none effects_submission=not-certified pixels=UNKNOWN",
            t.attempt, WallText(operation), boundary, tick, wallKnown, wallMs, outcome, GetCurrentThreadId(),
            t.owner, t.generation, t.token, t.runtime, t.creating, t.device, t.queue, t.factory, t.configLifetime,
            t.width, t.height, t.format, t.natural, t.lifetime, t.settings, t.revision, t.reload,
            t.stream, t.sourceGeneration, t.sourceFrame, t.skipped, t.predecessor, t.predecessorValue);
    } catch (...) {} // Never change admission/exception handling for diagnostics.
#endif
}
WarmupTrace TraceEntry(const WarmupTrace& trace, const Entry& e) noexcept
{
    auto out = trace;
    out.owner = e.owner; out.generation = e.generation; out.token = e.token;
    out.runtime = reinterpret_cast<uintptr_t>(e.runtime);
    out.device = reinterpret_cast<uintptr_t>(e.device.Get()); out.queue = reinterpret_cast<uintptr_t>(e.queue.Get());
    out.factory = reinterpret_cast<uintptr_t>(e.factory.Get()); out.configLifetime = reinterpret_cast<uintptr_t>(e.configLifetime.get());
    out.width = e.width; out.height = e.height; out.format = static_cast<uint32_t>(e.format);
    return out;
}

#ifdef HOYOSHADE_OWNED_RUNTIME_TEST
Testing::Hooks testHooks;
#endif
HRESULT Signal(Entry& entry, uint64_t value)
{
#ifdef HOYOSHADE_OWNED_RUNTIME_TEST
    if (testHooks.signal) return testHooks.signal(entry.queue.Get(), entry.fence.Get(), value);
#endif
    return entry.queue->Signal(entry.fence.Get(), value);
}
uint64_t Completed(ID3D12Fence* fence)
{
#ifdef HOYOSHADE_OWNED_RUNTIME_TEST
    if (testHooks.completed) return testHooks.completed(fence);
#endif
    return fence->GetCompletedValue();
}
// Stack-only reporting. No logging/IO under the pool lock or SDK callbacks.
struct Diagnostics
{
    Detail value{};
    Detail* output;
    WallScope* wall;
    Diagnostics(Detail* out, DiagnosticOperation op, WallScope* measured = nullptr) noexcept : output(out), wall(measured)
    { value.attempted = true; At(op); }
    void At(DiagnosticOperation op) noexcept { value.operation = op; value.codeKind = CodeKind::None; value.code = 0; }
    void Code(CodeKind kind, uint32_t code) noexcept { value.codeKind = kind; value.code = code; }
    Status Result(Status status) noexcept
    { value.status = status; if (output) *output = value; if (wall) wall->Outcome(Text(status)); return status; }
};
HWND CreateOwnedWindow(const Binding& b)
{
#ifdef HOYOSHADE_OWNED_RUNTIME_TEST
    if (testHooks.createWindow) return testHooks.createWindow(b);
#endif
    return CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, L"STATIC", L"F5 owned effects",
        WS_OVERLAPPED, 0, 0, static_cast<int>(b.width), static_cast<int>(b.height),
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
}
HRESULT CreateOwnedSwapchain(Entry& e, const DXGI_SWAP_CHAIN_DESC1& desc)
{
    const auto hr = e.swapchainOwner.Create(e.device.Get(), [&](IDXGISwapChain1** out) {
#ifdef HOYOSHADE_OWNED_RUNTIME_TEST
        if (testHooks.createSwapchain) return testHooks.createSwapchain(e.factory.Get(), e.queue.Get(), e.window, &desc, out);
#endif
        return e.factory->CreateSwapChainForHwnd(e.queue.Get(), e.window, &desc, nullptr, nullptr, out);
    });
    e.swapchain = e.swapchainOwner.Get();
    return hr;
}
HRESULT CreateOwnedFence(Entry& e)
{
    const auto hr = e.fenceOwner.Create(e.device.Get(), [&](ID3D12Fence** out) {
#ifdef HOYOSHADE_OWNED_RUNTIME_TEST
        if (testHooks.createFence) return testHooks.createFence(e.device.Get(), out);
#endif
        return e.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(out));
    });
    e.fence = e.fenceOwner.Get();
    return hr;
}
bool QueueCurrent(const Entry& e)
{
    if (!e.device || !e.queue || e.queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) return false;
    ComPtr<ID3D12Device> owner;
    return SUCCEEDED(e.queue->GetDevice(IID_PPV_ARGS(&owner))) && owner.Get() == e.device.Get();
}
bool ValidBinding(const Binding& b)
{
    if (!b.owner || !b.generation || !b.device || !b.queue || !b.factory ||
        b.width < 160 || b.height < 120 || b.width > 16384 || b.height > 16384 ||
        !b.configPathUtf8 || !b.configPathUtf8[0] ||
        strnlen_s(b.configPathUtf8, MaxPath) >= MaxPath ||
        !ColorContract::IsRuntimeFormat(b.format)) return false;
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, b.configPathUtf8, -1, nullptr, 0)) return false;
    if (b.queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT || FAILED(b.device->GetDeviceRemovedReason())) return false;
    ComPtr<ID3D12Device> queueDevice;
    if (FAILED(b.queue->GetDevice(IID_PPV_ARGS(&queueDevice))) || queueDevice.Get() != b.device) return false;
    const auto luid = b.device->GetAdapterLuid();
    ComPtr<IDXGIFactory4> factory4;
    if (SUCCEEDED(b.factory->QueryInterface(IID_PPV_ARGS(&factory4))))
    {
        ComPtr<IDXGIAdapter1> exact;
        return SUCCEEDED(factory4->EnumAdapterByLuid(luid, IID_PPV_ARGS(&exact)));
    }
    for (UINT index = 0; index < 128; ++index)
    {
        ComPtr<IDXGIAdapter1> adapter;
        const auto hr = b.factory->EnumAdapters1(index, &adapter);
        if (FAILED(hr)) return false;
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(adapter->GetDesc1(&desc)) && desc.AdapterLuid.LowPart == luid.LowPart &&
            desc.AdapterLuid.HighPart == luid.HighPart) return true;
    }
    return false;
}
bool ReadExports(HMODULE core, const CoreApi& apiBridge, CreateRuntime& create,
                 DestroyRuntime& destroy, UpdateRuntime& update)
{
    if (!core || !apiBridge.accepted ||
        reinterpret_cast<FARPROC>(apiBridge.registerAddon) != GetProcAddress(core, "ReShadeRegisterAddon") ||
        reinterpret_cast<FARPROC>(apiBridge.unregisterAddon) != GetProcAddress(core, "ReShadeUnregisterAddon") ||
        reinterpret_cast<FARPROC>(apiBridge.registerEvent) != GetProcAddress(core, "ReShadeRegisterEvent") ||
        reinterpret_cast<FARPROC>(apiBridge.unregisterEvent) != GetProcAddress(core, "ReShadeUnregisterEvent")) return false;
    create = reinterpret_cast<CreateRuntime>(GetProcAddress(core, "ReShadeCreateEffectRuntime"));
    destroy = reinterpret_cast<DestroyRuntime>(GetProcAddress(core, "ReShadeDestroyEffectRuntime"));
    update = reinterpret_cast<UpdateRuntime>(GetProcAddress(core, "ReShadeUpdateAndPresentEffectRuntime"));
    HMODULE held = nullptr;
    return create && destroy && update &&
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(create), &held) && held == core;
}
bool Matches(const Entry& e, const Binding& b, HMODULE core)
{
    return e.device.Get() == b.device && e.queue.Get() == b.queue && e.factory.Get() == b.factory &&
        e.width == b.width && e.height == b.height && e.format == b.format && e.core == core &&
        std::strcmp(e.config.data(), b.configPathUtf8) == 0;
}
void ObservePayloads(Entry& entry,PayloadEvent event) noexcept
{
    for(auto& payload:entry.payloads)
        if(payload.owner && payload.observer)
            payload.observer(payload.owner.get(),event,reinterpret_cast<uintptr_t>(&entry),entry.token,event==PayloadEvent::Quarantined?0:entry.fenceValue);
}
void Quarantine(Entry& entry) noexcept
{
    std::lock_guard lock(Get().mutex);
    entry.phase = Phase::Quarantined;
    entry.retiring = true;
    ObservePayloads(entry,PayloadEvent::Quarantined);
}
Entry* Active(uint32_t slot, uint64_t token, DWORD thread, bool submitted)
{
    if (!token || submitted || thread != GetCurrentThreadId() || slot >= Capacity) return nullptr;
    std::lock_guard lock(Get().mutex);
    auto& e = Get().entries[slot];
    return e.token == token && e.active && e.phase == Phase::Live ? &e : nullptr;
}
bool RuntimeMatches(Entry& e)
{
    HRESULT ownerHr = E_PENDING;
    if (!e.runtime || !QueueCurrent(e) ||
        !e.swapchainOwner.Current(e.device.Get(), e.swapchain.Get(), ownerHr) ||
        !e.fenceOwner.Current(e.device.Get(), e.fence.Get(), ownerHr)) return false;
    auto* d = e.runtime->get_device();
    auto* q = e.runtime->get_command_queue();
    if (!d || !q || d->get_api() != api::device_api::d3d12 || q->get_device() != d ||
        d->get_native() != reinterpret_cast<uint64_t>(e.device.Get()) ||
        q->get_native() != reinterpret_cast<uint64_t>(e.queue.Get()) ||
        e.runtime->get_hwnd() != e.window ||
        (e.sdkDevice && e.sdkDevice != d) || (e.sdkQueue && e.sdkQueue != q)) return false;
    e.sdkDevice = d;
    e.sdkQueue = q;
    return true;
}
bool GetList(Entry& e)
{
    e.sdkList = e.sdkQueue->get_immediate_command_list();
    if (!e.sdkList || e.sdkList->get_device() != e.sdkDevice) return false;
    auto* native = reinterpret_cast<ID3D12GraphicsCommandList*>(e.sdkList->get_native());
    if (!native || native->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT) return false;
    ComPtr<ID3D12Device> device;
    if (FAILED(native->GetDevice(IID_PPV_ARGS(&device))) || device.Get() != e.device.Get()) return false;
    e.nativeList = native;
    return true;
}
// Entry is reserved/collecting and no active lease exists. User destructors and
// SDK destruction run without the pool mutex, preventing callback deadlocks.
bool ReleasePayloads(Entry& e)
{
    // Entry is reserved exclusively. Custom adopted-owner deleters only store
    // to their own atomic receipt; callbacks cannot acquire/render this entry.
    // Every caller already observed this exact entry's genuine completion.
    ObservePayloads(e,PayloadEvent::Completed);
    for (auto& payload : e.payloads) {payload.owner.reset();payload.observer=nullptr;}
    for (auto& resource : e.resources)
    {
        if (!resource.object || !resource.released ||
            !resource.released->load(std::memory_order_acquire)) continue;
        resource.object->DestroyGpuObjects(e.sdkDevice);
        if (!resource.object->GpuObjectsReleased()) return false;
        delete resource.object;
        resource = {};
    }
    e.sdkList = nullptr;
    e.nativeList.Reset();
    return true;
}

Status CompletionStatus(Entry& e, Diagnostics* report = nullptr)
{
    if (report) report->At(DiagnosticOperation::CompletionDevice);
    HRESULT hr = S_OK;
    if (!QueueCurrent(e) || !e.fenceOwner.Current(e.device.Get(), e.fence.Get(), hr))
    {
        if (report && FAILED(hr)) report->Code(CodeKind::HResult, static_cast<uint32_t>(hr));
        return Status::CompletionUnknown;
    }
    if (!e.fenceValue) return Status::Ready;
    if (report) report->At(DiagnosticOperation::CompletionFence);
    const auto value = Completed(e.fence.Get());
    if (value == MaxValue) return Status::CompletionUnknown;
    return value >= e.fenceValue ? Status::Ready : Status::GpuPending;
}
}

uint64_t NextWarmupAttempt() noexcept
{
    auto value = nextWarmupAttempt.load(std::memory_order_relaxed);
    for (unsigned retry = 0; retry < 4 && value != MaxValue; ++retry)
        if (nextWarmupAttempt.compare_exchange_weak(value, value + 1, std::memory_order_relaxed)) return value + 1;
    return 0; // Missing diagnostic ID cannot fail or disable real effects.
}
WallScope::WallScope(WallOperation operation, const WarmupTrace* trace) noexcept : operation_(operation)
{
    PreserveLastError error;
    if (!trace || !trace->attempt || !ReserveWallReport(operation)) return;
    reporting_ = true; trace_ = *trace;
    exceptions_ = std::uncaught_exceptions();
    clockKnown_ = QueryPerformanceCounter(&begin_) != FALSE;
    ReportWall(trace_, operation_, "begin", begin_.QuadPart, false, 0.0, "entered");
}
WallScope::~WallScope() noexcept
{
    if (!reporting_) return;
    PreserveLastError error;
    LARGE_INTEGER end{}, frequency{};
    const bool known = clockKnown_ && QueryPerformanceCounter(&end) && QueryPerformanceFrequency(&frequency) &&
        frequency.QuadPart > 0 && end.QuadPart >= begin_.QuadPart;
    const double ms = known ? static_cast<double>(end.QuadPart - begin_.QuadPart) * 1000.0 / frequency.QuadPart : 0.0;
    ReportWall(trace_, operation_, "end", end.QuadPart, known, ms,
        std::uncaught_exceptions() > exceptions_ ? "exception-unwinding" : outcome_);
}

bool Completion::IsComplete() const noexcept
{
    if (!fence || !value || value == MaxValue) return false;
    ComPtr<ID3D12Device> device;
    if (FAILED(fence->GetDevice(IID_PPV_ARGS(&device))) || FAILED(device->GetDeviceRemovedReason())) return false;
    const auto completed = fence->GetCompletedValue();
    return completed != MaxValue && completed >= value;
}

Lease::~Lease() { Release(); }
Lease::Lease(Lease&& other) noexcept
{
    _slot = other._slot; _token = std::exchange(other._token, 0);
    _thread = other._thread; _submitted = other._submitted;
}
Lease& Lease::operator=(Lease&& other) noexcept
{
    if (this != &other)
    {
        Release();
        _slot = other._slot; _token = std::exchange(other._token, 0);
        _thread = other._thread; _submitted = other._submitted;
    }
    return *this;
}
void Lease::Release() noexcept
{
    if (!_token) return;
    std::lock_guard lock(Get().mutex);
    auto& e = Get().entries[_slot];
    if (e.token == _token && e.active)
    {
        if (!_submitted) {
            e.phase = Phase::Quarantined; e.retiring = true;
            ObservePayloads(e,PayloadEvent::Quarantined);
        }
        e.active = false;
    }
    _token = 0;
}
reshade::api::effect_runtime* Lease::Runtime() const noexcept
{ auto* e = Active(_slot, _token, _thread, _submitted); return e ? e->runtime : nullptr; }
reshade::api::device* Lease::Device() const noexcept
{ auto* e = Active(_slot, _token, _thread, _submitted); return e ? e->sdkDevice : nullptr; }
reshade::api::command_queue* Lease::Queue() const noexcept
{ auto* e = Active(_slot, _token, _thread, _submitted); return e ? e->sdkQueue : nullptr; }
reshade::api::command_list* Lease::List() const noexcept
{ auto* e = Active(_slot, _token, _thread, _submitted); return e ? e->sdkList : nullptr; }
ID3D12GraphicsCommandList* Lease::NativeList() const noexcept
{ auto* e = Active(_slot, _token, _thread, _submitted); return e ? e->nativeList.Get() : nullptr; }
uint64_t Lease::Generation() const noexcept
{ auto* e = Active(_slot, _token, _thread, _submitted); return e ? e->generation : 0; }
EntryReceipt Lease::Receipt() const noexcept
{
    EntryReceipt receipt;
    if (auto* e = Active(_slot, _token, _thread, _submitted)) {
        receipt.slot_ = _slot; receipt.token_ = _token;
        receipt.owner_ = e->owner; receipt.generation_ = e->generation;
        receipt.runtime_ = e->runtime;
    }
    return receipt;
}
bool Lease::AdoptResource(std::unique_ptr<Resource>& resource, std::shared_ptr<Resource>& owner) noexcept
{
    auto* e = Active(_slot, _token, _thread, _submitted);
    if (!e || !resource || owner) return false;
    std::shared_ptr<std::atomic<bool>> released;
    try { released = std::make_shared<std::atomic<bool>>(false); } catch (...) { return false; }
    size_t index = MaxPayloads;
    uint64_t resourceToken = 0;
    {
        std::lock_guard lock(Get().mutex);
        if (e->nextResourceToken == MaxValue) return false;
        for (size_t n = 0; n < MaxPayloads; ++n)
            if (!e->resources[n].object) { index = n; break; }
        if (index == MaxPayloads) return false;
        resourceToken = ++e->nextResourceToken;
        e->resources[index] = {resource.get(), resourceToken, released};
    }
    try
    {
        // The control block never owns GPU destruction. Its final callback
        // merely publishes the CPU retirement request for this adoption.
        std::shared_ptr<Resource> result(resource.get(), [released](Resource*) noexcept {
            released->store(true, std::memory_order_release);
        });
        (void)resource.release();
        owner = std::move(result);
        return true;
    }
    catch (...)
    {
        // Even failed control-block allocation cannot destroy the caller's SDK
        // bundle. The constructor's deleter only marked this reserved slot.
        std::lock_guard lock(Get().mutex);
        e->resources[index] = {};
        return false;
    }
}
bool Lease::KeepAlive(const std::shared_ptr<void>& owner, PayloadObserver observer) noexcept
{
    auto* e = Active(_slot, _token, _thread, _submitted);
    if (!e || !owner || !owner.use_count()) return false;
    for (auto& payload : e->payloads)
        if (!payload.owner) { payload = {owner,observer}; return true; }
    return false;
}
Status Lease::Submit(Completion& completion, Detail* detail, const WarmupTrace* trace) noexcept
{
    Diagnostics report(detail, DiagnosticOperation::Submit);
    completion = {};
    if (_thread != GetCurrentThreadId()) return report.Result(Status::WrongThread);
    Operation operation;
    if (!operation.entered) return report.Result(Status::Busy);
    auto* e = Active(_slot, _token, _thread, _submitted);
    if (!e) return report.Result(Status::Retired);
    std::optional<WarmupTrace> entryTrace;
    if (trace) entryTrace = TraceEntry(*trace, *e);
    const auto* measured = entryTrace ? &*entryTrace : nullptr;
    // Reentrant callbacks cannot use this lease to add commands while the SDK
    // is flushing/resetting its immediate list. The active lifetime stays held.
    _submitted = true;
    try
    {
        // Active lease survives a concurrent Retire. Destruction cannot start.
        // Validate the exact already-recorded SDK/native list, without replacing
        // it or inferring its owner from the created swapchain/fence owner.
        report.At(DiagnosticOperation::RuntimeIdentity);
        ComPtr<ID3D12Device> listOwner;
        if (!RuntimeMatches(*e) || !e->sdkList || !e->nativeList ||
            e->sdkList->get_device() != e->sdkDevice ||
            e->sdkList->get_native() != reinterpret_cast<uint64_t>(e->nativeList.Get()) ||
            e->nativeList->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT ||
            FAILED(e->nativeList->GetDevice(IID_PPV_ARGS(&listOwner))) || listOwner.Get() != e->device.Get())
        { Quarantine(*e); return report.Result(Status::IdentityMismatch); }
        report.At(DiagnosticOperation::FlushImmediateList);
        MeasureWarmup(measured, WallOperation::Flush, [&] { e->sdkQueue->flush_immediate_command_list(); });
        report.At(DiagnosticOperation::UpdateRuntime);
        MeasureWarmup(measured, WallOperation::UpdateRuntime, [&] { e->update(e->runtime); });
        report.At(DiagnosticOperation::FenceValue);
        if (e->fenceValue >= MaxValue - 1)
        { Quarantine(*e); return report.Result(Status::SignalFailed); }
        report.At(DiagnosticOperation::SignalDevice);
        HRESULT deviceHr = S_OK;
        if (!QueueCurrent(*e) || !e->fenceOwner.Current(e->device.Get(), e->fence.Get(), deviceHr))
        { if (FAILED(deviceHr)) report.Code(CodeKind::HResult, static_cast<uint32_t>(deviceHr));
          Quarantine(*e); return report.Result(Status::SignalFailed); }
        report.At(DiagnosticOperation::Signal);
        const auto signalHr = MeasureWarmup(measured, WallOperation::Signal, [&] { return Signal(*e, e->fenceValue + 1); });
        if (FAILED(signalHr))
        { report.Code(CodeKind::HResult, static_cast<uint32_t>(signalHr)); Quarantine(*e); return report.Result(Status::SignalFailed); }
        ++e->fenceValue;
        ObservePayloads(*e,PayloadEvent::Submitted);
        completion.fence = e->fence;
        completion.value = e->fenceValue;
        return report.Result(Status::Ready);
    }
    catch (...)
    {
        report.Code(CodeKind::None, 0);
        // Exception is an UNKNOWN transaction, never a lifetime synchronization.
        Quarantine(*e);
        return report.Result(Status::CompletionUnknown);
    }
}

Status AcquireForRender(const Binding& b, HMODULE loadedCore, const CoreApi& acceptedApi, Lease& lease,
                        Detail* detail, WarmupTrace* trace) noexcept
{
    std::optional<WallScope> acquireWall;
    if (trace) acquireWall.emplace(WallOperation::Acquire, trace);
    Diagnostics report(detail, DiagnosticOperation::Acquire, acquireWall ? &*acquireWall : nullptr);
    if (lease) return report.Result(Status::Busy);
    Operation operation;
    if (!operation.entered) return report.Result(Status::Busy);
    Entry* selected = nullptr;
    uint32_t index = 0;
    bool createNew = false;
    try
    {
        report.At(DiagnosticOperation::ValidateBinding);
        if (!ValidBinding(b)) return report.Result(Status::InvalidBinding);
        CreateRuntime create = nullptr;
        DestroyRuntime destroy = nullptr;
        UpdateRuntime update = nullptr;
        report.At(DiagnosticOperation::ReadExports);
        if (!ReadExports(loadedCore, acceptedApi, create, destroy, update)) return report.Result(Status::CoreUnavailable);
        report.At(DiagnosticOperation::SelectOwner);
        auto& pool = Get();
        {
            std::lock_guard lock(pool.mutex);
            if (pool.core && pool.core != loadedCore) return report.Result(Status::CoreUnavailable);
            pool.core = loadedCore;
            Owner* owner = nullptr;
            Owner* emptyOwner = nullptr;
            for (auto& o : pool.owners)
            {
                if (o.id == b.owner) owner = &o;
                else if (!o.id && !emptyOwner) emptyOwner = &o;
            }
            if (!owner)
            {
                if (!emptyOwner) return report.Result(Status::Capacity);
                owner = emptyOwner;
                *owner = {b.owner, b.generation, false};
            }
            else if (b.generation < owner->generation || (b.generation == owner->generation && owner->retired))
                return report.Result(Status::StaleGeneration);
            else if (b.generation > owner->generation)
            {
                *owner = {b.owner, b.generation, false};
                for (auto& e : pool.entries) if (e.owner == b.owner) e.retiring = true;
            }
            report.At(DiagnosticOperation::SelectEntry);
            for (uint32_t n = 0; n < Capacity; ++n)
            {
                auto& e = pool.entries[n];
                if (e.phase != Phase::Free && e.owner == b.owner && e.generation == b.generation)
                {
                    if (e.phase == Phase::Quarantined) return report.Result(Status::Quarantined);
                    if (e.retiring) return report.Result(Status::Retired);
                    if (e.phase != Phase::Live || e.active || e.resourceReservations) return report.Result(Status::Busy);
                    if (!Matches(e, b, loadedCore)) return report.Result(Status::BindingChanged);
                    selected = &e; index = n;
                    e.phase = Phase::Collecting;
                    break;
                }
            }
            if (!selected)
            {
                for (uint32_t n = 0; n < Capacity; ++n)
                    if (pool.entries[n].phase == Phase::Free) { selected = &pool.entries[n]; index = n; break; }
                if (!selected || pool.nextToken == MaxValue) return report.Result(Status::Capacity);
                auto& e = *selected;
                e.phase = Phase::Creating;
                e.token = ++pool.nextToken;
                e.owner = b.owner; e.generation = b.generation;
                e.device = b.device; e.queue = b.queue; e.factory = b.factory;
                e.width = b.width; e.height = b.height; e.format = b.format;
                std::memcpy(e.config.data(), b.configPathUtf8, std::strlen(b.configPathUtf8) + 1);
                e.configLifetime = b.configLifetime;
                e.core = loadedCore; e.destroy = destroy; e.update = update;
                e.windowThread = GetCurrentThreadId();
                createNew = true;
            }
        }
        auto& e = *selected;
        if (trace) {
            *trace = TraceEntry(*trace, e); trace->creating = createNew;
            e.warmupTrace = *trace;
        }
        const auto* measured = trace ? &e.warmupTrace : nullptr;
        if (acquireWall) acquireWall->Context(*trace);
        if (createNew)
        {
            report.At(DiagnosticOperation::CreateWindow);
            e.window = MeasureWarmup(measured, WallOperation::CreateWindow, [&] { return CreateOwnedWindow(b); });
            if (!e.window)
            { const auto error = GetLastError(); report.Code(CodeKind::Win32, error); Quarantine(e); return report.Result(Status::CreateFailed); }
            DXGI_SWAP_CHAIN_DESC1 desc{};
            desc.Width = b.width; desc.Height = b.height; desc.Format = b.format;
            desc.SampleDesc.Count = 1; desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            desc.BufferCount = 2; desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
            report.At(DiagnosticOperation::CreateSwapchain);
            const auto swapchainHr = MeasureWarmup(measured, WallOperation::CreateSwapchain, [&] { return CreateOwnedSwapchain(e, desc); });
            if (FAILED(swapchainHr) || !e.swapchain)
            { if (FAILED(swapchainHr)) report.Code(CodeKind::HResult, static_cast<uint32_t>(swapchainHr));
              Quarantine(e); return report.Result(Status::CreateFailed); }
            report.At(DiagnosticOperation::CreateFence);
            const auto fenceHr = MeasureWarmup(measured, WallOperation::CreateFence, [&] { return CreateOwnedFence(e); });
            if (FAILED(fenceHr) || !e.fence)
            { if (FAILED(fenceHr)) report.Code(CodeKind::HResult, static_cast<uint32_t>(fenceHr));
              Quarantine(e); return report.Result(Status::CreateFailed); }
            report.At(DiagnosticOperation::SwapchainDevice);
            HRESULT swapDeviceHr = E_PENDING;
            if (!e.swapchainOwner.Current(e.device.Get(), e.swapchain.Get(), swapDeviceHr))
            {
                if (FAILED(swapDeviceHr)) report.Code(CodeKind::HResult, static_cast<uint32_t>(swapDeviceHr));
                Quarantine(e); return report.Result(Status::IdentityMismatch);
            }
            // Fence GetDevice is captured only after its real successful Create.
            // A different returned owner is local to this exact fence, not an
            // exception to queue/list/resource device ownership.
            HRESULT fenceDeviceHr = E_PENDING;
            if (!e.fenceOwner.Current(e.device.Get(), e.fence.Get(), fenceDeviceHr))
            {
                if (FAILED(fenceDeviceHr)) report.Code(CodeKind::HResult, static_cast<uint32_t>(fenceDeviceHr));
                Quarantine(e); return report.Result(Status::IdentityMismatch);
            }
            report.At(DiagnosticOperation::CreateRuntime);
            const bool created = MeasureWarmup(measured, WallOperation::CreateRuntime, [&] {
#ifdef HOYOSHADE_OWNED_RUNTIME_TEST
                return testHooks.createRuntime ? testHooks.createRuntime(api::device_api::d3d12, e.device.Get(), e.queue.Get(), e.swapchain.Get(), e.config.data(), &e.runtime) :
#else
                return
#endif
                create(api::device_api::d3d12, e.device.Get(), e.queue.Get(), e.swapchain.Get(), e.config.data(), &e.runtime);
            });
            if (trace) { *trace = TraceEntry(*trace, e); e.warmupTrace = *trace; }
            if (acquireWall) acquireWall->Context(*trace);
            if (!created || !e.runtime)
            {
                if (!created) report.Code(CodeKind::Boolean, 0);
                HRESULT ownerHr = E_PENDING;
                if (e.runtime || !QueueCurrent(e) || !e.fenceOwner.Current(e.device.Get(), e.fence.Get(), ownerHr) ||
                    FAILED(Signal(e, 1))) { Quarantine(e); return report.Result(Status::CreateFailed); }
                std::lock_guard lock(pool.mutex);
                e.fenceValue = 1; e.retiring = true; e.phase = Phase::Live;
                for (auto& owner : pool.owners)
                    if (owner.id == e.owner && owner.generation == e.generation) owner.retired = true;
                return report.Result(Status::CreateFailed);
            }
            report.At(DiagnosticOperation::RuntimeIdentity);
            if (!RuntimeMatches(e)) { Quarantine(e); return report.Result(Status::IdentityMismatch); }
        }
        else
        {
            const auto completed = CompletionStatus(e, &report);
            if (completed == Status::CompletionUnknown) { Quarantine(e); return report.Result(completed); }
            if (completed == Status::GpuPending)
            {
                std::lock_guard lock(pool.mutex); e.phase = Phase::Live;
                return report.Result(completed);
            }
            // Validate the owned SDK identity before using it to destroy
            // completed payloads; a same-native wrapper replacement is not a
            // renewed lifetime or permission to call the previous SDK object.
            report.At(DiagnosticOperation::RuntimeIdentity);
            if (!RuntimeMatches(e)) { Quarantine(e); return report.Result(Status::IdentityMismatch); }
            report.At(DiagnosticOperation::ReleasePayloads);
            if (!ReleasePayloads(e)) { Quarantine(e); return report.Result(Status::CompletionUnknown); }
        }
        report.At(DiagnosticOperation::ImmediateList);
        if (!MeasureWarmup(measured, WallOperation::ImmediateList, [&] { return GetList(e); }))
        { Quarantine(e); return report.Result(Status::IdentityMismatch); }
        {
            std::lock_guard lock(pool.mutex);
            // Reentrant/concurrent retirement during creation is respected. The
            // issued list has no retirement receipt: retain, never destroy it.
            report.At(DiagnosticOperation::RetireCheck);
            if (e.retiring) { e.phase = Phase::Quarantined; return report.Result(Status::Retired); }
            e.phase = Phase::Live;
            e.active = true;
            lease._slot = index; lease._token = e.token; lease._thread = GetCurrentThreadId(); lease._submitted = false;
        }
        return report.Result(Status::Ready);
    }
    catch (...)
    {
        report.Code(CodeKind::None, 0);
        if (selected) Quarantine(*selected);
        return report.Result(Status::CompletionUnknown);
    }
}


ResourceCollection::~ResourceCollection() { Release(); }
ResourceCollection::ResourceCollection(ResourceCollection&& other) noexcept
    : entry_(std::exchange(other.entry_, EntryReceipt{})) {}
ResourceCollection& ResourceCollection::operator=(ResourceCollection&& other) noexcept
{
    if (this != &other) { Release(); entry_ = std::exchange(other.entry_, EntryReceipt{}); }
    return *this;
}
void ResourceCollection::Release() noexcept
{
    if (!entry_) return;
    std::lock_guard lock(Get().mutex);
    auto& e = Get().entries[entry_.slot_];
    if (e.token == entry_.token_ && e.resourceReservations) --e.resourceReservations;
    entry_ = {};
}
Status ReserveResources(const EntryReceipt& receipt, ResourceCollection& pin) noexcept
{
    if (!receipt || pin || receipt.slot_ >= Capacity) return Status::InvalidBinding;
    std::unique_lock lock(Get().mutex, std::try_to_lock);
    if (!lock.owns_lock()) return Status::Busy;
    auto& e = Get().entries[receipt.slot_];
    if (e.token != receipt.token_ || e.owner != receipt.owner_ ||
        e.generation != receipt.generation_ || e.runtime != receipt.runtime_ || e.phase == Phase::Free)
        return Status::CompletionUnknown;
    if (e.phase == Phase::Quarantined) return Status::Quarantined;
    if (e.phase != Phase::Live || e.active) return Status::Busy;
    if (e.resourceReservations == (std::numeric_limits<unsigned>::max)()) return Status::Capacity;
    ++e.resourceReservations;
    pin.entry_ = receipt;
    return Status::Ready;
}
Retirement ResourceCollection::Poll() noexcept
{
    if (!entry_ || entry_.slot_ >= Capacity) return Retirement::Unknown;
    Operation operation;
    if (!operation.entered) return Retirement::Pending;
    auto& pool = Get();
    auto& e = pool.entries[entry_.slot_]; // exact receipt; never scan the pool
    {
        std::unique_lock lock(pool.mutex, std::try_to_lock);
        if (!lock.owns_lock()) return Retirement::Pending;
        if (e.token != entry_.token_ || e.owner != entry_.owner_ ||
            e.generation != entry_.generation_ || e.runtime != entry_.runtime_ ||
            !e.resourceReservations || e.phase == Phase::Free) return Retirement::Unknown;
        if (e.phase == Phase::Quarantined) return Retirement::Unknown;
        if (e.phase != Phase::Live || e.active) return Retirement::Pending;
        e.phase = Phase::Collecting;
    }
    try {
        const auto status = CompletionStatus(e);
        if (status == Status::GpuPending) {
            std::lock_guard lock(pool.mutex); e.phase = Phase::Live;
            return Retirement::Pending;
        }
        if (status != Status::Ready || !RuntimeMatches(e) || !ReleasePayloads(e)) {
            Quarantine(e); return Retirement::Unknown;
        }
        // Deliberately no e.destroy, Retire, HWND/native swapchain teardown,
        // flush/update/Signal or GPU wait. The pin keeps this runtime alive.
        std::lock_guard lock(pool.mutex); e.phase = Phase::Live;
        return Retirement::Ready;
    } catch (...) { Quarantine(e); return Retirement::Unknown; }
}

bool ObservationOwnersIdle() noexcept
{
    try {
        if (inOperation) return false;
        auto& pool = Get();
        std::unique_lock lock(pool.mutex, std::try_to_lock);
        if (!lock.owns_lock()) return false;
        for (const auto& entry : pool.entries)
            if (entry.phase != Phase::Free) return false;
        return true; // Owner generation keys contain no resources or executable pointers.
    } catch (...) { return false; }
}
void Retire(uint64_t owner, uint64_t generation) noexcept
{
    auto& pool = Get();
    std::lock_guard lock(pool.mutex);
    for (auto& o : pool.owners)
        if ((!owner || o.id == owner) && (!generation || o.generation == generation)) o.retired = true;
    for (auto& e : pool.entries)
        if (e.phase != Phase::Free && (!owner || e.owner == owner) && (!generation || e.generation == generation)) e.retiring = true;
}
Collection TryCollect() noexcept
{
    Collection result;
    Operation operation;
    if (!operation.entered) { result.active = 1; return result; }
    auto& pool = Get();
    for (auto& e : pool.entries)
    {
        {
            std::lock_guard lock(pool.mutex);
            if (e.phase == Phase::Free) continue;
            if (e.active || e.phase == Phase::Creating || e.phase == Phase::Collecting) { ++result.active; continue; }
            if (e.phase == Phase::Quarantined) { ++result.quarantined; continue; }
            // Even resource-only collection is serialized with all SDK calls.
            e.phase = Phase::Collecting;
        }
        try
        {
            const auto completed = CompletionStatus(e);
            if (completed == Status::CompletionUnknown)
            { Quarantine(e); ++result.quarantined; continue; }
            if (completed == Status::GpuPending)
            {
                std::lock_guard lock(pool.mutex); e.phase = Phase::Live; ++result.pending; continue;
            }
            if (!ReleasePayloads(e)) { Quarantine(e); ++result.quarantined; continue; }
            {
                std::lock_guard lock(pool.mutex);
                // Exact-entry resource collection pins never authorize core
                // teardown, even if another path marked this entry retiring.
                if (!e.retiring || e.resourceReservations) { e.phase = Phase::Live; continue; }
                unsigned remainingOwners = 0;
                for (const auto& resource : e.resources) if (resource.object) ++remainingOwners;
                if (remainingOwners)
                {
                    // A real adopted owner outlives Retire. Do not infer its
                    // final release from use_count or discard its SDK device.
                    e.phase = Phase::Live; ++result.pending; result.resourceOwners += remainingOwners; continue;
                }
                if (GetCurrentThreadId() != e.windowThread)
                { e.phase = Phase::Live; ++result.wrongThread; continue; }
            }
            if (e.runtime) {
                const auto trace = TraceEntry(e.warmupTrace, e);
                MeasureWarmup(trace.attempt ? &trace : nullptr, WallOperation::DestroyRuntime, [&] { e.destroy(e.runtime); });
                e.runtime = nullptr;
            }
            e.sdkDevice = nullptr; e.sdkQueue = nullptr;
            e.swapchain.Reset();
            e.swapchainOwner = {}; // Release retained COM outside the pool mutex.
            const auto trace = TraceEntry(e.warmupTrace, e);
            if (e.window && !MeasureWarmup(trace.attempt ? &trace : nullptr, WallOperation::DestroyWindow,
                                          [&] { return DestroyWindow(e.window); }))
            { Quarantine(e); ++result.quarantined; continue; }
            e.window = nullptr;
            // Native Release may itself dispatch integration callbacks. Keep the
            // collecting reservation, but never hold the pool mutex across it.
            e.fence.Reset();
            e.fenceOwner = {};
            e.factory.Reset(); e.queue.Reset(); e.device.Reset();
            std::lock_guard lock(pool.mutex);
            e = Entry{};
            ++result.destroyed;
        }
        catch (...)
        { Quarantine(e); ++result.quarantined; }
    }
    return result;
}

#ifdef HOYOSHADE_OWNED_RUNTIME_TEST
void Testing::SetHooks(Hooks hooks) noexcept { testHooks = hooks; }
#endif
}

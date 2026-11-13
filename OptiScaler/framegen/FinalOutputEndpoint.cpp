#include "pch.h"
#include "FinalOutputEndpoint.h"
#include <integration/HoYoShade.h>
#include <array>
#include <atomic>
#include <mutex>
#include <wrl/client.h>
#ifdef HOYOSHADE_STAGE_TEST
#define LOG_INFO(...) ((void)0)
#define LOG_WARN(...) ((void)0)
#endif

namespace framegen::final_output
{
using Microsoft::WRL::ComPtr;
namespace {
uint64_t Identity(IUnknown* object) noexcept
{
    ComPtr<IUnknown> identity;
    return object && SUCCEEDED(object->QueryInterface(IID_PPV_ARGS(&identity)))
        ? reinterpret_cast<uint64_t>(identity.Get()) : 0;
}
bool FullPresent(UINT flags, const DXGI_PRESENT_PARAMETERS* p) noexcept
{
    // DO_NOT_SEQUENCE/RESTART can reuse content without a new producer epoch.
    constexpr UINT rejected = DXGI_PRESENT_TEST | DXGI_PRESENT_DO_NOT_WAIT |
        DXGI_PRESENT_DO_NOT_SEQUENCE | DXGI_PRESENT_RESTART;
    return !(flags & rejected) && (!p || (!p->DirtyRectsCount && !p->pScrollRect && !p->pScrollOffset));
}
std::atomic<uint64_t> nextOwner{0};
thread_local std::shared_ptr<Provider> creating;
struct Providers {
    std::mutex mutex;
    std::array<std::weak_ptr<Provider>, 8> entries;
};
Providers& Registry() { static auto* r = new Providers; return *r; }
}
class Provider
{
public:
    std::mutex mutex;
    uint64_t input = 0, owner = ++nextOwner;
    bool live = true, active = false, prepared = false, retiring = false;
    uint64_t retirementEpoch = 0;
    bool retirementCommit = false;
    unsigned inFlight = 0; // Includes default-OFF/raw Present, preparation, capture and full Resize reservations.
    DWORD inputThread = 0;
    HoYoShade::PhysicalSource source;
};
namespace {
std::shared_ptr<Provider> BeginActivity(const std::shared_ptr<Provider>& p) noexcept
{
    if (!p) return {};
    std::unique_lock lock(p->mutex, std::try_to_lock);
    if (!lock.owns_lock() || !p->live || p->retiring || p->inFlight == UINT_MAX) return {};
    ++p->inFlight;
    return p;
}
void EndActivity(std::shared_ptr<Provider>& p) noexcept
{
    if (!p) return;
    { std::lock_guard lock(p->mutex); --p->inFlight; }
    p.reset();
}
struct ProviderActivity
{
    std::shared_ptr<Provider> provider;
    explicit ProviderActivity(const std::shared_ptr<Provider>& p) : provider(BeginActivity(p)) {}
    ~ProviderActivity() { EndActivity(provider); }
    explicit operator bool() const noexcept { return bool(provider); }
};
std::shared_ptr<Provider> Find(IDXGISwapChain* input) noexcept
{
    const auto identity = Identity(input);
    if (!identity) return {};
    std::lock_guard lock(Registry().mutex);
    for (auto& weak : Registry().entries) {
        auto p = weak.lock();
        if (!p) continue;
        std::lock_guard guard(p->mutex);
        if (p->live && !p->retiring && p->input == identity) return p;
    }
    return {};
}
}
std::shared_ptr<Provider> CreateProvider() noexcept
{
    try {
        Endpoint::CollectRetired();
        std::lock_guard lock(Registry().mutex);
        for (auto& entry : Registry().entries) if (entry.expired()) {
            auto p = std::make_shared<Provider>(); entry = p; return p;
        }
    } catch (...) {}
    return {};
}
void BindInput(const std::shared_ptr<Provider>& p, IDXGISwapChain* input) noexcept
{
    if (!p) return;
    const auto identity = Identity(input);
    std::lock_guard lock(p->mutex);
    if (!p->live || p->retiring) return;
    p->input = identity; p->source = {}; p->prepared = p->active = false;
}

void RetireProvider(std::shared_ptr<Provider>& provider) noexcept
{
    auto p = std::move(provider);
    if (!p) return;
    std::lock_guard lock(p->mutex);
    p->live = false; p->source = {}; p->prepared = false;
}
ProviderRetirement TryRetireProvider(const std::shared_ptr<Provider>& p, IDXGISwapChain* input) noexcept
{
    ProviderRetirement r;
    if (!p) return r;
    const auto identity = Identity(input);
    {
        std::unique_lock lock(p->mutex, std::try_to_lock);
        if (!lock.owns_lock()) { r.status = DXGI_ERROR_WAS_STILL_DRAWING; return r; }
        if (!identity || identity != p->input || !p->owner || p->owner == UINT64_MAX || !p->live) {
            r.status = DXGI_ERROR_INVALID_CALL; return r;
        }
        p->retiring = true; // Close new work BEFORE testing existing scopes.
        if (p->inFlight || p->active) { r.status = DXGI_ERROR_WAS_STILL_DRAWING; return r; }
        if (p->retirementCommit || p->retirementEpoch == UINT64_MAX) {
            r.status = DXGI_ERROR_WAS_STILL_DRAWING; return r;
        }
        r.provider = p; r.owner = p->owner; r.epoch = ++p->retirementEpoch;
    }
    r.status = Endpoint::TryRetire(p, false);
    return r;
}
HRESULT ProviderRetirement::Commit() noexcept
{
    if (status != S_OK || !provider) return FAILED(status) ? status : E_UNEXPECTED;
    {
        std::unique_lock lock(provider->mutex, std::try_to_lock);
        if (!lock.owns_lock()) return DXGI_ERROR_WAS_STILL_DRAWING;
        if (provider->owner != owner || provider->retirementEpoch != epoch || provider->retirementCommit ||
            !provider->live || !provider->retiring || provider->inFlight || provider->active)
            return DXGI_ERROR_WAS_STILL_DRAWING;
        provider->retirementCommit = true;
    }
    struct FinishCommit {
        Provider& provider;
        ~FinishCommit() { std::lock_guard lock(provider.mutex); provider.retirementCommit = false; }
    } finish { *provider };
    const auto hr = Endpoint::TryRetire(provider, true);
    if (hr != S_OK) return hr;
    {
        std::lock_guard lock(provider->mutex);
        provider->source = {};
        provider->prepared = provider->live = false;
    }
    // Host/SDK teardown and every captured endpoint have positively drained.
    // This also retires an input scope that never bound any physical raw.
    status = E_UNEXPECTED; // One commit; this token cannot certify a later owner.
    return S_OK;
}
CreationScope::CreationScope(const std::shared_ptr<Provider>& p) noexcept
    : previous(std::move(creating)), activity(BeginActivity(p)) { ++depth; creating = activity; }
CreationScope::~CreationScope() { creating = std::move(previous); EndActivity(activity); --depth; }
void PrepareSource(IDXGISwapChain* input, IDXGISwapChain* source, UINT flags,
                   const DXGI_PRESENT_PARAMETERS* params) noexcept
{
    if (!HoYoShade::Selected(HoYoShade::Stage::FinalOutput)) return;
    auto p = Find(input); if (!p) return;
    ProviderActivity activity(p); if (!activity) return;
    {
        std::lock_guard lock(p->mutex);
        p->source = {}; p->prepared = false;
        if (!p->live || p->active || !FullPresent(flags, params)) return;
    }
    const auto ticket = HoYoShade::PreparePhysicalSource(source);
    {
        std::lock_guard lock(p->mutex);
        if (p->live && !p->retiring && !p->active) {
            p->source = ticket; p->prepared = true; p->inputThread = GetCurrentThreadId(); return;
        }
    }
}
InputScope::InputScope(IDXGISwapChain* input, UINT flags, const DXGI_PRESENT_PARAMETERS* params) noexcept
{
    if (!HoYoShade::Selected(HoYoShade::Stage::FinalOutput)) return;
    auto p = Find(input); if (!p) return;
    bool prepared = false;
    {
        std::lock_guard lock(p->mutex);
        if (!p->live || p->active) return;
        prepared = p->prepared && p->inputThread == GetCurrentThreadId();
        if (!FullPresent(flags, params)) { p->source = {}; p->prepared = false; return; }
    }
    if (!prepared) PrepareSource(input, input, flags, params);
    std::lock_guard lock(p->mutex);
    if (!p->live || p->retiring || p->active || !p->prepared || !p->source || p->inputThread != GetCurrentThreadId()) return;
    p->active = true; p->prepared = false; provider = std::move(p);
}
InputScope::~InputScope()
{
    if (!provider) return;
    std::lock_guard lock(provider->mutex);
    provider->active = false;
    provider->source = {};
}

struct Endpoint::State
{
    std::mutex mutex;
    std::shared_ptr<Provider> provider;
    ComPtr<IDXGISwapChain3> chain;
    ComPtr<ID3D12Device> device;
    struct Queue { ComPtr<ID3D12CommandQueue> queue; UINT node = 0; };
    std::array<Queue, 16> queues;
    UINT count = 0;
    ComPtr<ID3D12Fence> fence;
    uint64_t value = 0, generation = 1, sequence = 0, stream = 0, tail = 0;

    DWORD thread = 0;
    bool quarantined = false, resizing = false, retired = false, retryAmbiguous = false;
    HoYoShade::PhysicalContentHold content;
#ifdef HOYOSHADE_STAGE_TEST
    bool failNextTail = false;
#endif
    DXGI_COLOR_SPACE_TYPE colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709; // DXGI creation default
    std::atomic<bool> writing{false};
    uint64_t techniques = 0, restores = 0;
    bool submitted = false, suppressed = false;
    struct Pending {
        ComPtr<ID3D12Resource> color, original;
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> snapshot, restore;
        uint64_t tail = 0;
#ifdef HOYOSHADE_STAGE_TEST
        uint64_t originalCreates = 0, allocatorCreates = 0, listCreates = 0;
#endif

        static void Barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
                            D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) noexcept {
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
            list->ResourceBarrier(1, &barrier);
        }
        // Both lists belong to this endpoint, never to the game/core. They are
        // pre-recorded before any Submit and retained until the exact tail.
        bool Prepare(ID3D12Device* device, ID3D12Resource* target, UINT node) noexcept {
            if (!original) {
                auto desc = target->GetDesc(); desc.Flags = D3D12_RESOURCE_FLAG_NONE; desc.Alignment = 0;
                D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
                heap.CreationNodeMask = heap.VisibleNodeMask = node ? node : 1;
                if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&original)))) return false;
#ifdef HOYOSHADE_STAGE_TEST
                ++originalCreates;
#endif
            }
            if (!allocator) {
                if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                        IID_PPV_ARGS(&allocator)))) return false;
#ifdef HOYOSHADE_STAGE_TEST
                ++allocatorCreates;
#endif
            }
            for (auto* list : {std::addressof(snapshot), std::addressof(restore)}) if (!*list) {
                if (FAILED(device->CreateCommandList(node, D3D12_COMMAND_LIST_TYPE_DIRECT,
                        allocator.Get(), nullptr, IID_PPV_ARGS(list->ReleaseAndGetAddressOf()))) || FAILED((*list)->Close())) return false;
#ifdef HOYOSHADE_STAGE_TEST
                ++listCreates;
#endif
            }
            if (FAILED(allocator->Reset()) || FAILED(snapshot->Reset(allocator.Get(), nullptr))) return false;
            Barrier(snapshot.Get(), target, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
            snapshot->CopyResource(original.Get(), target);
            Barrier(snapshot.Get(), target, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
            // A command allocator can have only one list recording at a time.
            // Close snapshot before binding the same allocator to restore.
            if (FAILED(snapshot->Close()) || FAILED(restore->Reset(allocator.Get(), nullptr))) return false;
            Barrier(restore.Get(), target, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
            Barrier(restore.Get(), original.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
            restore->CopyResource(target, original.Get());
            Barrier(restore.Get(), original.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
            Barrier(restore.Get(), target, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
            return SUCCEEDED(restore->Close());
        }
    };
    std::array<Pending, 16> pending;

    // Process lifetime bounded custody, including unknown-completion entries.
    static auto& Pool() {
        struct Storage { std::mutex mutex; std::array<std::shared_ptr<State>, 8> entries; };
        static auto* pool = new Storage; return *pool;
    }
    bool Collect() noexcept {
        if (quarantined) return false;
        if (!fence) return true;
        const auto completed = fence->GetCompletedValue();
        if (completed == UINT64_MAX || FAILED(device->GetDeviceRemovedReason())) {
            quarantined = true;
            for (auto& p : pending) if (p.color)
                HoYoShade::QuarantineColor(chain.Get(), nullptr, device.Get(), p.color.Get(), generation);
            return false;
        }
        bool complete = true;
        for (auto& p : pending) {
            if (p.color && p.tail && completed >= p.tail) { p.color.Reset(); p.tail = 0; }
            complete &= !p.color;
        }
        return complete && (!tail || completed >= tail);
    }
    void Quarantine(ID3D12CommandQueue* queue, ID3D12Resource* color) noexcept {
        quarantined = true;
        HoYoShade::QuarantineColor(chain.Get(), queue, device.Get(), color, generation);
    }
};
bool CaptureProviderOutputBinding(IDXGISwapChain* raw, IUnknown* creationQueue) noexcept
{
    if (!creating || !raw || !creationQueue) return false;
    ProviderActivity activity(creating); if (!activity) return false;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Device> device, queueDevice;
    if (FAILED(creationQueue->QueryInterface(IID_PPV_ARGS(&queue))) ||
        queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT ||
        FAILED(raw->GetDevice(IID_PPV_ARGS(&device))) ||
        FAILED(queue->GetDevice(IID_PPV_ARGS(&queueDevice)))) return false;
    const auto owner = Identity(device.Get());
    return owner && owner == Identity(queueDevice.Get());
}
std::unique_ptr<Endpoint> Endpoint::Capture(IDXGISwapChain* raw, IUnknown* creationQueue) noexcept
{
    if (!creating || !raw || !creationQueue) return {};
    ProviderActivity activity(creating); if (!activity) return {};
    try {
        auto s = std::make_shared<State>();
        ComPtr<ID3D12CommandQueue> queue;
        ComPtr<ID3D12Device> queueDevice;
        DXGI_SWAP_CHAIN_DESC desc{};
        if (FAILED(raw->QueryInterface(IID_PPV_ARGS(&s->chain))) ||
            FAILED(creationQueue->QueryInterface(IID_PPV_ARGS(&queue))) ||
            queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT ||
            FAILED(raw->GetDevice(IID_PPV_ARGS(&s->device))) ||
            FAILED(queue->GetDevice(IID_PPV_ARGS(&queueDevice))) || Identity(queueDevice.Get()) != Identity(s->device.Get()) ||
            FAILED(raw->GetDesc(&desc)) || !desc.BufferCount || desc.BufferCount > s->queues.size() ||
            desc.SwapEffect != DXGI_SWAP_EFFECT_FLIP_DISCARD) return {};
        // An F5 creation scope is provider authority; queue/state authority is
        // DXGI's raw Present contract, not a private vendor command-list guess.
        s->provider = creating; s->count = desc.BufferCount; s->stream = ++nextOwner;
        for (UINT i = 0; i < s->count; ++i) s->queues[i] = {queue, queue->GetDesc().NodeMask};
        std::lock_guard lock(State::Pool().mutex);
        for (auto& old : State::Pool().entries) if (old) {
            auto retained = old;
            std::unique_lock guard(retained->mutex, std::try_to_lock);
            if (guard.owns_lock() && retained->retired && retained->Collect()) {
                old.reset();
            }
        }
        for (auto& entry : State::Pool().entries) if (!entry) {
            auto endpoint = std::unique_ptr<Endpoint>(new Endpoint);
            endpoint->state = s; entry = std::move(s); return endpoint;
        }
    } catch (...) {}
    return {};
}
Endpoint::~Endpoint()
{
    if (!state) return;
    std::lock_guard lock(state->mutex);
    state->retired = true;
    HoYoShade::RetirePhysicalOutput(state->stream);
    state->Collect();
    // Pool retains pending/unknown references after wrapper release.
}
HRESULT Endpoint::Present(UINT flags,const DXGI_PRESENT_PARAMETERS* params,
                          const std::function<HRESULT()>& present) noexcept
{
    return PresentImpl(flags,params,present);
}

HRESULT Endpoint::PresentImpl(UINT flags,const DXGI_PRESENT_PARAMETERS* params,
                              const std::function<HRESULT()>& present) noexcept
{
    auto& s = *state;
    ProviderActivity activity(s.provider);
    if (!activity) return DXGI_ERROR_WAS_STILL_DRAWING;
    // Content denial and true quarantine survive OFF and block all consumers.
    if (HoYoShade::ColorConsumers(s.chain.Get()) != HoYoShade::ConsumerAccess::Safe) return E_FAIL;

    std::unique_lock lock(s.mutex,std::defer_lock);
    if (!HoYoShade::Selected(HoYoShade::Stage::FinalOutput)) return present();
    if(!lock.owns_lock() && !lock.try_lock()) return s.writing.load(std::memory_order_acquire)
        ? DXGI_ERROR_WAS_STILL_DRAWING : present();
    s.techniques = 0; s.submitted = s.suppressed = false;
    if (s.quarantined) return E_FAIL;
    s.Collect();
    if (s.quarantined) return E_FAIL;
    const auto untouched = [&]() -> HRESULT { return present(); };
    if (s.resizing || s.retired || s.retryAmbiguous || !FullPresent(flags, params)) return untouched();
    HoYoShade::PhysicalSource source;
    bool bound = false;
    {
        std::lock_guard providerLock(s.provider->mutex);
        // The source remains exact to this synchronous input scope.
        bound = s.provider->live && s.provider->active && s.provider->inputThread == GetCurrentThreadId();
        source = s.provider->source;

    }
    if (!bound || !source || (s.thread && s.thread != GetCurrentThreadId())) return untouched();
    const auto index = s.chain->GetCurrentBackBufferIndex();
    if (index >= s.count || !s.queues[index].queue) return untouched();
    // An admitted snapshot/restore still owns this slot until its actual tail.
    // Do not run natural or reset its allocator on a speculative early retry.
    // This is nonblocking GPU-pending admission, not an unknown-content latch.
    if (s.pending[index].color) return DXGI_ERROR_WAS_STILL_DRAWING;
    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(s.chain->GetDesc(&desc)) || desc.BufferCount != s.count ||
        desc.SwapEffect != DXGI_SWAP_EFFECT_FLIP_DISCARD) return untouched();
    UINT support = 0;
    if (s.colorSpace != DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709 ||
        FAILED(s.chain->CheckColorSpaceSupport(s.colorSpace, &support)) || !(support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT)) return untouched();
    ComPtr<ID3D12Resource> color;
    if (FAILED(s.chain->GetBuffer(index, IID_PPV_ARGS(&color)))) return untouched();
    const auto cd = color->GetDesc();
    if (cd.Format != DXGI_FORMAT_R8G8B8A8_UNORM && cd.Format != DXGI_FORMAT_B8G8R8A8_UNORM) return untouched();
    auto* queue = s.queues[index].queue.Get();
    if (!s.fence && FAILED(s.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&s.fence)))) return untouched();
    if (s.value >= UINT64_MAX - 2 || s.sequence == UINT64_MAX) return untouched();
    auto& pending = s.pending[index];
    if (!pending.Prepare(s.device.Get(), color.Get(), s.queues[index].node)) {
        pending = {}; // no GPU work queued; discard a failed/open recording safely
        return untouched();
    }
    HoYoShade::ColorWriteScope writer(true, s.device.Get(), queue, color.Get(), s.chain.Get());
    if (!writer.Acquired()) return untouched();
    s.writing.store(true, std::memory_order_release);
    struct ActiveWrite {
        std::atomic<bool>& flag;
        ~ActiveWrite() { flag.store(false, std::memory_order_release); }
    } activeWrite{s.writing};
    pending.color = color;
    ID3D12CommandList* snapshotLists[] = {pending.snapshot.Get()};
    queue->ExecuteCommandLists(1, snapshotLists);
    // Raw DXGI PRESENT + creation queue supplies the handoff. This signal
    // includes our original-color snapshot; it is not output completion.
    if (FAILED(queue->Signal(s.fence.Get(), ++s.value))) {
        writer.FinishAfterReturn(true); s.writing.store(false, std::memory_order_release);
        s.Quarantine(queue, color.Get()); return E_FAIL;
    }
    s.thread = GetCurrentThreadId();
    HoYoShade::ColorEndpoint endpoint;
    endpoint.stage = HoYoShade::Stage::FinalOutput; endpoint.output = HoYoShade::OutputKind::UnknownPhysical;
    endpoint.color = color.Get(); endpoint.queue = queue; endpoint.ready = {queue, s.fence.Get(), s.value};
    endpoint.stream = s.stream; endpoint.generation = s.generation; endpoint.frame = source.serial;
    endpoint.outputToken = {s.stream, s.generation, ++s.sequence}; endpoint.physicalSource = source;
    endpoint.width = static_cast<uint32_t>(cd.Width); endpoint.height = cd.Height;
    endpoint.state = D3D12_RESOURCE_STATE_PRESENT; endpoint.stateKnown = true; endpoint.sdr = true;
    const auto result = HoYoShade::RenderStage(endpoint);
    s.techniques = result.techniques; s.submitted = result.Submitted(); s.suppressed = result.attempt.naturalSuppressed;
    writer.FinishAfterReturn(result.Uncertain());
    if (result.Uncertain()) { s.Quarantine(queue, color.Get()); return E_FAIL; }
    // ColorWriteScope ends before overlay and raw consumers. Same exact queue
    // orders effects -> existing overlay -> raw Present -> F5-only tail fence.
    const auto hr = present();
    // WAS_STILL_DRAWING means the operation was neither executed nor scheduled
    // (DXGI_ERROR documentation). Only that HRESULT, unchanged physical index
    // and a healthy exact device authorize restoring the pre-filter snapshot.
    // Occluded/device removal/unknown failures do NOT establish this contract.
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING && s.chain->GetCurrentBackBufferIndex() == index &&
        SUCCEEDED(s.device->GetDeviceRemovedReason())) {
        ID3D12CommandList* restoreLists[] = {pending.restore.Get()};
        queue->ExecuteCommandLists(1, restoreLists);
        ++s.restores;
    } else if (hr != S_OK) {
        s.retryAmbiguous = true; // content only; the tail below decides GPU certainty
    }
    // The same exact queue orders snapshot -> owned/natural -> raw -> optional
    // restore -> tail. A later same-queue producer is after the restore, so no
    // future content-identity guess or persistent natural disable is needed.
    const auto tailValue = ++s.value;
    HRESULT tailResult;
#ifdef HOYOSHADE_STAGE_TEST
    if (s.failNextTail) { s.failNextTail = false; tailResult = E_FAIL; }
    else
#endif
    tailResult = queue->Signal(s.fence.Get(), tailValue);
    if (FAILED(tailResult)) s.Quarantine(queue, color.Get());
    else {
        pending.tail = s.tail = s.value;
        if (s.retryAmbiguous && !s.quarantined && !s.content.Publish(s.chain.Get(), queue, s.device.Get(),
                color.Get(), s.stream, s.generation, s.fence.Get(), s.tail))
            s.Quarantine(queue, color.Get()); // no bounded denial slot/proof: never admit consumers
    }
    if (s.sequence <= 3 || s.sequence % 600 == 0)
        LOG_INFO("HoYoShade physical_output owner={} generation={} output_sequence={} source_token={} kind=unknownphysical raw_hr=0x{:08x} submission={} techniques={} tail={} retry_ambiguous={}",
            s.stream,s.generation,s.sequence,source.serial,static_cast<unsigned>(hr),
            static_cast<unsigned>(result.attempt.state),result.techniques,s.tail,s.retryAmbiguous);

    return hr; // a failed tail is never retried/promoted
}
void Endpoint::SetColorSpace(DXGI_COLOR_SPACE_TYPE space) noexcept
{
    std::lock_guard lock(state->mutex); state->colorSpace = space;
}
#ifdef HOYOSHADE_STAGE_TEST
void Endpoint::FailNextTailSignalForTest() noexcept
{
    std::lock_guard lock(state->mutex); state->failNextTail = true;
}
Endpoint::Observation Endpoint::Observe() const noexcept
{
    std::lock_guard lock(state->mutex);
    Observation result{state->sequence, state->value, state->techniques, state->submitted,
        state->suppressed, state->retryAmbiguous, state->quarantined, state->retired, state->restores};
    for (const auto& slot : state->pending) {
        result.originalCreates += slot.originalCreates;
        result.allocatorCreates += slot.allocatorCreates;
        result.listCreates += slot.listCreates;
    }
    result.contentBlocked = state->content.Active(); result.generation = state->generation;
    return result;
}
#endif
Endpoint::Resize::Resize(Endpoint* e) noexcept : endpoint(e)
{
    if (!e) return;
    auto& s = *e->state;
    activity = BeginActivity(s.provider);
    if (!activity) { admitted = false; endpoint = nullptr; return; }
    std::unique_lock lock(s.mutex, std::try_to_lock);
    if (!lock.owns_lock() || s.resizing || s.quarantined || !s.Collect()) { admitted = false; endpoint = nullptr; return; }
    if (s.generation == UINT64_MAX || (s.content.Active() &&
        !s.content.ReserveDiscard(s.chain.Get(), s.stream, s.generation))) { admitted = false; endpoint = nullptr; return; }
    s.resizing = true;
    HoYoShade::RetirePhysicalOutput(s.stream);
    // Safe retirement only disables filtering. Known-complete snapshot/list
    // references must also release their target before any real DXGI resize.
    for (auto& pending : s.pending) pending = {};
}
Endpoint::Resize::~Resize()
{
    if (endpoint) {
        std::lock_guard lock(endpoint->state->mutex);
        endpoint->state->content.CancelDiscard(); endpoint->state->resizing = false;
    }
    EndActivity(activity);
}
bool Endpoint::Resize::ConsumersSafe(IDXGISwapChain* chain) const noexcept
{
    if (!admitted) return false;
    if (!endpoint) return HoYoShade::ColorConsumers(chain) == HoYoShade::ConsumerAccess::Safe;
    auto& s = *endpoint->state;
    std::lock_guard lock(s.mutex);
    return s.content.Active() ? s.content.ResizeAccess(chain, s.stream, s.generation)
        : HoYoShade::ColorConsumers(chain) == HoYoShade::ConsumerAccess::Safe;
}
void Endpoint::Resize::Complete(HRESULT hr, UINT count, const UINT* nodes,
                                 IUnknown* const* queues, bool resize1) noexcept
{
    if (!endpoint || FAILED(hr)) return;
    auto& s = *endpoint->state;
    std::lock_guard lock(s.mutex);
    if (s.content.Active() && !s.content.CommitDiscard(s.chain.Get(), s.stream, s.generation,
            s.generation + 1, hr)) return; // keep denial if exact proof changed during DXGI call
    const auto retiredGeneration=s.generation++;
    s.retryAmbiguous = false; s.thread = 0;
    if (s.retired) return; // optional adapter stays disabled; DXGI remains usable
    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(s.chain->GetDesc(&desc)) || !desc.BufferCount || desc.BufferCount > s.queues.size()) {
        s.retired = true; return;
    }
    auto updated = s.queues;
    if (resize1) {
        // Preserve per-buffer queues and node masks exactly. Unknown rebinding
        // disables only this endpoint, without changing the game's HRESULT.
        if (!queues || !nodes || (count && count != desc.BufferCount)) { s.retired = true; return; }
        for (UINT i = 0; i < desc.BufferCount; ++i) {
            ComPtr<ID3D12CommandQueue> queue; ComPtr<ID3D12Device> device;
            if (!queues[i] || FAILED(queues[i]->QueryInterface(IID_PPV_ARGS(&queue))) ||
                queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT ||
                FAILED(queue->GetDevice(IID_PPV_ARGS(&device))) || Identity(device.Get()) != Identity(s.device.Get()) ||
                (nodes[i] && queue->GetDesc().NodeMask && nodes[i] != queue->GetDesc().NodeMask)) { s.retired = true; return; }
            updated[i] = {queue, nodes[i]};
            // This first endpoint uses one ordered completion timeline. A
            // split presenting queue is a different contract, never inferred
            // from shared-device identity or a larger Signal value.
            if (i && Identity(updated[i].queue.Get()) != Identity(updated[0].queue.Get())) { s.retired = true; return; }
        }
    } else if (desc.BufferCount != s.count) {
        // ResizeBuffers preserves the creation queue, except after per-buffer
        // ResizeBuffers1: no inferred queue for newly introduced buffers.
        for (UINT i = 1; i < s.count; ++i)
            if (Identity(s.queues[i].queue.Get()) != Identity(s.queues[0].queue.Get())) { s.retired = true; return; }
        for (UINT i = 0; i < desc.BufferCount; ++i) updated[i] = s.queues[0];
    }
    s.queues = std::move(updated); s.count = desc.BufferCount;
}
HRESULT Endpoint::TryRetire(const std::shared_ptr<Provider>& provider, bool commit) noexcept
{
    if (!provider) return DXGI_ERROR_UNSUPPORTED;
    {
        std::unique_lock lock(provider->mutex, std::try_to_lock);
        if (!lock.owns_lock() || !provider->retiring || provider->inFlight || provider->active)
            return DXGI_ERROR_WAS_STILL_DRAWING;
    }
    // No provider lock held: normal Endpoint ordering is state -> provider.
    // Admission is already closed, so zero activity cannot grow behind this snapshot.
    std::unique_lock poolLock(State::Pool().mutex, std::try_to_lock);
    if (!poolLock.owns_lock()) return DXGI_ERROR_WAS_STILL_DRAWING;
    // Move ownership out under the state locks, but drop COM refs only AFTER unlocking.
    // A final Release may synchronously destroy a wrapper/Endpoint and reenter its mutex.
    struct Released {
        std::array<State::Pending, 16> pending;
        std::array<State::Queue, 16> queues;
        ComPtr<ID3D12Fence> fence;
        ComPtr<IDXGISwapChain3> chain;
        uint64_t stream = 0;
    };
    std::array<Released, 8> released;
    std::array<std::shared_ptr<State>, 8> states;
    std::array<std::unique_lock<std::mutex>, 8> locks;
    unsigned count = 0;
    for (const auto& entry : State::Pool().entries) if (entry && entry->provider == provider) {
        states[count] = entry;
        locks[count] = std::unique_lock(entry->mutex, std::try_to_lock);
        if (!locks[count].owns_lock() || entry->resizing || entry->writing.load(std::memory_order_acquire) ||
            entry->quarantined || !entry->device || entry->device->GetDeviceRemovedReason() != S_OK ||
            !entry->Collect() || entry->content.Active()) return DXGI_ERROR_WAS_STILL_DRAWING;
        ++count;
    }
    if (commit) for (unsigned i = 0; i < count; ++i) {
        auto& s = *states[i];
        s.retired = true;
        released[i].stream = s.stream;
        released[i].pending = std::move(s.pending);
        released[i].queues = std::move(s.queues);
        released[i].fence = std::move(s.fence);
        released[i].chain = std::move(s.chain);
    }
    for (auto& lock : locks) if (lock.owns_lock()) lock.unlock();
    poolLock.unlock();
    if (commit) for (unsigned i = 0; i < count; ++i) HoYoShade::RetirePhysicalOutput(released[i].stream);
    return S_OK;
}
void Endpoint::CollectRetired() noexcept
{
    std::lock_guard lock(State::Pool().mutex);
    for (auto& entry : State::Pool().entries) if (entry) {
        auto retained = entry;
        std::unique_lock guard(retained->mutex, std::try_to_lock);
        if (guard.owns_lock() && retained->retired && retained->Collect()) {
            entry.reset();
        }
    }
}
}

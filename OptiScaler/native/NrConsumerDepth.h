#pragma once
#include <integration/HoYoShade.h>
#include <wrl/client.h>
#include <memory>
#include <mutex>
#include <cstdint>
#include <array>
#include <atomic>

namespace native::nrfilter::depth
{
// Collected independently from the motion/jitter metadata already queried at
// the NGX capture seam. An absent parameter is not a known zero origin.
struct Region
{
    uint32_t x = 0, y = 0, width = 0, height = 0;
    bool known = false;
};
// Borrowed ONLY during Late's selected-slot call, after its successful ready
// Wait. The selected Slot still owns these contents. This is not a submission
// receipt and grants no permission to submit or reset the producer's list.
struct Capture
{
    ID3D12Resource* resource = nullptr;
    ID3D12CommandQueue* producerQueue = nullptr;
    ID3D12Fence* readyFence = nullptr;
    uint64_t readyValue = 0, serial = 0, epoch = 0;
    Region region;
    bool directionKnown = false, reversed = false;
};
inline bool Fits(const D3D12_RESOURCE_DESC& d, const Region& r) noexcept
{
    return r.known && r.width && r.height && d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
        d.DepthOrArraySize == 1 && d.MipLevels == 1 && d.SampleDesc.Count == 1 && !d.SampleDesc.Quality &&
        uint64_t(r.x) + r.width <= d.Width && uint64_t(r.y) + r.height <= d.Height &&
        uint64_t(r.x) + r.width <= UINT32_MAX;
}
inline bool ScalarFormat(DXGI_FORMAT f) noexcept
{
    // Capture has already made a typed, shader-readable NGX depth clone.
    // No stencil view, paired validity invention, conversion or neutral depth.
    return f == DXGI_FORMAT_R32_FLOAT || f == DXGI_FORMAT_R16_FLOAT || f == DXGI_FORMAT_R16_UNORM;
}
inline bool ValidValue(uint64_t value, uint64_t completed) noexcept
{
    return value && value != UINT64_MAX && completed != UINT64_MAX;
}
inline bool Same(IUnknown* a, IUnknown* b) noexcept
{
    if (!a || !b) return false;
    Microsoft::WRL::ComPtr<IUnknown> x, y;
    return SUCCEEDED(a->QueryInterface(IID_PPV_ARGS(&x))) &&
        SUCCEEDED(b->QueryInterface(IID_PPV_ARGS(&y))) && x == y;
}

// The actual CPU transaction used below. It separates a source-copy reader
// from a later filter reader, including a sealed but entirely unused offer.
// Fence values are checked by Snapshot; these booleans never mint GPU proof.
class Lifetime final
{
    bool executed_ = false, sealed_ = false, offered_ = false, returned_ = false;
    bool bound_ = false, quarantined_ = false;
public:
    void Executed() noexcept { executed_ = true; }
    void Seal(bool signalled) noexcept { sealed_ = signalled; if (!signalled) quarantined_ = true; }
    bool Offer() noexcept {
        if (!sealed_ || offered_ || quarantined_) return false;
        offered_ = true; return true;
    }
    bool Validate() const noexcept { return sealed_ && offered_ && !returned_ && !quarantined_; }
    bool Bind() noexcept {
        if (!Validate() || bound_) return false;
        bound_ = true; return true;
    }
    void Return(bool uncertain, bool depthAvailable) noexcept {
        returned_ = true;
        if (uncertain || (depthAvailable && !bound_)) quarantined_ = true;
    }
    void Quarantine() noexcept { quarantined_ = true; }
    bool Quarantined() const noexcept { return quarantined_; }
    bool SourceHealthy() const noexcept { return !executed_ || sealed_; }
    bool SourceFinished(bool copyComplete) const noexcept {
        return !executed_ || (sealed_ && copyComplete);
    }
    bool Reclaimable(bool copyComplete, bool readerComplete) const noexcept {
        return !quarantined_ && SourceFinished(copyComplete) &&
            (!offered_ || (returned_ && (!bound_ || readerComplete)));
    }
};

// GPU allocation key: normal per-frame epoch advance is NOT an allocation
// change. Epoch rollback and stream/generation replacement retire the domain.
struct PoolKey
{
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    uint64_t stream = 0, generation = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    Region region;
    bool Matches(const PoolKey& b) const noexcept {
        return device == b.device && queue == b.queue && stream == b.stream && generation == b.generation &&
            format == b.format && region.x == b.region.x && region.y == b.region.y &&
            region.width == b.region.width && region.height == b.region.height;
    }
};
inline bool EpochRolledBack(uint64_t oldCapture, uint64_t oldFrame, uint64_t capture, uint64_t frame) noexcept
{
    return capture < oldCapture || frame < oldFrame;
}
inline bool CanReuse(const Lifetime& life, bool copyComplete, bool readerComplete, bool soleReader) noexcept
{
    return soleReader && life.Reclaimable(copyComplete, readerComplete);
}
struct Availability { bool allocated = false, readerHeld = false, retired = false; };
template<size_t N> size_t SelectEntry(const std::array<Availability, N>& entries) noexcept
{
    for (size_t i = 0; i < N; ++i)
        if (entries[i].allocated && !entries[i].readerHeld && !entries[i].retired) return i;
    for (size_t i = 0; i < N; ++i)
        if (!entries[i].allocated && !entries[i].readerHeld) return i;
    return N;
}

// Two fixed GPU entries per existing Late slot. Every per-use Reader below is
// immutable across generations; a stale Prepared cannot bind a new generation.
struct Storage final
{
    template<class T> using ComPtr = Microsoft::WRL::ComPtr<T>;
    PoolKey key;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Resource> image;
    ComPtr<ID3D12Fence> sealed;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commands;
    uint64_t nextValue = 0;
    bool imageWritten = false;
    std::atomic<bool> retired{false}; // a reader quarantine can also arrive during core retirement

    bool Initialize(const PoolKey& k) noexcept {
        key = k; device = k.device; queue = k.queue;
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = k.region.width; d.Height = k.region.height;
        d.DepthOrArraySize = 1; d.MipLevels = 1; d.Format = k.format;
        d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        heap.CreationNodeMask = heap.VisibleNodeMask = 1;
        return SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&image))) &&
            SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&sealed))) &&
            SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) &&
            SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                IID_PPV_ARGS(&commands))) && SUCCEEDED(commands->Close());
    }
};

class Snapshot final : public HoYoShade::Depth::Reader, public std::enable_shared_from_this<Snapshot>
{
    template<class T> using ComPtr = Microsoft::WRL::ComPtr<T>;
    mutable std::mutex mutex_;
    Lifetime life_;
    Capture capture_;
    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12Resource> source_;
    std::shared_ptr<Storage> storage_;
    ComPtr<ID3D12CommandQueue> producer_, queue_, colorQueue_;
    ComPtr<ID3D12Fence> captureReady_, colorReady_, readerDone_;
    uint64_t sealValue_ = 0;
    uint64_t colorValue_ = 0, readerValue_ = 0;
    uint64_t stream_ = 0, generation_ = 0, frame_ = 0;
    DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN;
    std::shared_ptr<Snapshot> quarantine_; // only unknown execution/reader completion

    static bool Complete(ID3D12Fence* f, uint64_t value) noexcept {
        if (!f) return false;
        const auto completed = f->GetCompletedValue();
        return ValidValue(value, completed) && completed >= value;
    }
    void RetainQuarantine() noexcept {
        life_.Quarantine();
        if (storage_) storage_->retired = true;
        if (!quarantine_) quarantine_ = weak_from_this().lock();
    }
    static void Barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* r,
                        D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) noexcept {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = r; b.Transition.Subresource = 0;
        b.Transition.StateBefore = before; b.Transition.StateAfter = after;
        list->ResourceBarrier(1, &b);
    }
public:
    explicit Snapshot(std::shared_ptr<Storage> storage = {}) : storage_(std::move(storage)) {}
    Snapshot(const Snapshot&) = delete;
    Snapshot& operator=(const Snapshot&) = delete;

    // Called once per Reader (GPU storage may be reused), before publication, on Late's closed-list/queue-owned seam.
    // All fallible CPU setup/Close occurs before Execute. A failed Signal after
    // Execute retains the exact source, snapshot, queue, allocator and list.
    bool Seal(const Capture& c, const HoYoShade::ColorEndpoint& e) noexcept
    {
        if (!c.resource || !c.producerQueue || !c.readyFence || !c.serial || !c.epoch ||
            c.epoch > e.frame || !c.directionKnown || !e.queue || !e.stream || !e.generation ||
            !e.ready.queue || !e.ready.fence || e.queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT ||
            !ValidValue(c.readyValue, c.readyFence->GetCompletedValue()) ||
            !ValidValue(e.ready.value, e.ready.fence->GetCompletedValue())) return false;
        const auto d = c.resource->GetDesc();
        if (!Fits(d, c.region) || !ScalarFormat(d.Format) ||
            (d.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE)) return false;
        ComPtr<ID3D12Device> work, producerDevice, readyDevice, colorQueueDevice, colorFenceDevice;
        if (FAILED(c.resource->GetDevice(IID_PPV_ARGS(&device_))) ||
            FAILED(e.queue->GetDevice(IID_PPV_ARGS(&work))) || !Same(device_.Get(), work.Get()) ||
            FAILED(c.producerQueue->GetDevice(IID_PPV_ARGS(&producerDevice))) || !Same(device_.Get(), producerDevice.Get()) ||
            FAILED(c.readyFence->GetDevice(IID_PPV_ARGS(&readyDevice))) || !Same(device_.Get(), readyDevice.Get()) ||
            FAILED(e.ready.queue->GetDevice(IID_PPV_ARGS(&colorQueueDevice))) || !Same(device_.Get(), colorQueueDevice.Get()) ||
            FAILED(e.ready.fence->GetDevice(IID_PPV_ARGS(&colorFenceDevice))) || !Same(device_.Get(), colorFenceDevice.Get()) ||
            FAILED(device_->GetDeviceRemovedReason())) return false;
        if (!storage_ || storage_->retired || !Same(storage_->device.Get(), device_.Get()) ||
            storage_->queue.Get() != e.queue || storage_->nextValue >= UINT64_MAX - 1) return false;
        capture_ = c; source_ = c.resource; producer_ = c.producerQueue; captureReady_ = c.readyFence;
        queue_ = e.queue; colorQueue_ = e.ready.queue; colorReady_ = e.ready.fence; colorValue_ = e.ready.value;
        stream_ = e.stream; generation_ = e.generation; frame_ = e.frame; format_ = d.Format;
        // Owner selected storage only after BOTH actual completions and all old
        // Reader references retired. Never reset a still-recording game list.
        if (FAILED(storage_->allocator->Reset()) ||
            FAILED(storage_->commands->Reset(storage_->allocator.Get(), nullptr))) {
            storage_->retired = true; return false; // no Execute in this generation
        }
        if (storage_->imageWritten)
            Barrier(storage_->commands.Get(), storage_->image.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_COPY_DEST);
        Barrier(storage_->commands.Get(), source_.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
        dst.pResource = storage_->image.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.pResource = source_.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        const D3D12_BOX box{c.region.x, c.region.y, 0, c.region.x + c.region.width, c.region.y + c.region.height, 1};
        storage_->commands->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
        Barrier(storage_->commands.Get(), source_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        Barrier(storage_->commands.Get(), storage_->image.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (FAILED(storage_->commands->Close())) { storage_->retired = true; return false; } // never executed
        ID3D12CommandList* lists[]{storage_->commands.Get()};
        sealValue_ = ++storage_->nextValue; // monotonic; never reset a shared fence to zero
        life_.Executed();
        queue_->ExecuteCommandLists(1, lists);
        const bool signalled = SUCCEEDED(queue_->Signal(storage_->sealed.Get(), sealValue_));
        life_.Seal(signalled);
        storage_->imageWritten = true;
        if (!signalled) storage_->retired = true;
        if (!signalled) RetainQuarantine();
        return signalled;
    }
    HoYoShade::Depth::Input Offer() noexcept
    {
        std::lock_guard lock(mutex_);
        if (!life_.Offer()) return {};
        HoYoShade::Depth::Input a;
        a.resource = storage_->image.Get(); a.typedFormat = format_;
        a.width = capture_.region.width; a.height = capture_.region.height;
        a.entryState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE; a.stateKnown = true;
        a.directionKnown = capture_.directionKnown; a.reversed = capture_.reversed;
        // Same top-left DirectX NGX image coordinates as the existing NR read;
        // CopyTextureRegion only crops. This is not FG's independent Y convention.
        a.yFlipped = false;
        a.queue = queue_.Get(); a.readyFence = colorReady_.Get(); a.readyValue = colorValue_;
        a.owner = stream_; a.generation = generation_; a.frame = frame_; a.epoch = capture_.epoch;
        a.reader = weak_from_this().lock();
        return a;
    }
    bool Validate(const HoYoShade::Depth::Access& a) const noexcept override
    {
        std::lock_guard lock(mutex_);
        return life_.Validate() && capture_.serial && a.reader.get() == this &&
            a.resource == storage_->image.Get() && a.typedFormat == format_ &&
            a.width == capture_.region.width && a.height == capture_.region.height && !a.subresource &&
            a.entryState == D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE && a.stateKnown &&
            a.directionKnown == capture_.directionKnown && a.reversed == capture_.reversed &&
            !a.yFlipped && !a.validityPaired && a.queue == queue_.Get() &&
            a.readyFence == colorReady_.Get() && a.readyValue == colorValue_ &&
            a.owner == stream_ && a.generation == generation_ && a.frame == frame_ && a.epoch == capture_.epoch &&
            storage_ && storage_->sealed && ValidValue(sealValue_, storage_->sealed->GetCompletedValue()) &&
            ValidValue(colorValue_, colorReady_->GetCompletedValue());
    }
    bool BindCompletion(ID3D12Fence* fence, uint64_t value) noexcept override
    {
        std::lock_guard lock(mutex_);
        ComPtr<ID3D12Device> owner;
        if (!fence || !ValidValue(value, fence->GetCompletedValue()) ||
            FAILED(fence->GetDevice(IID_PPV_ARGS(&owner))) || !Same(owner.Get(), device_.Get()) || !life_.Bind()) {
            RetainQuarantine(); return false;
        }
        // Only Depth::Prepared::Complete after the real owned filter submit
        // calls this. A StageResult or the copy fence is never substituted.
        readerDone_ = fence; readerValue_ = value;
        return true;
    }
    void Quarantine() noexcept override { std::lock_guard lock(mutex_); RetainQuarantine(); }
    void Return(const HoYoShade::StageResult& result) noexcept {
        std::lock_guard lock(mutex_);
        life_.Return(result.Uncertain(), result.depthAvailable);
        if (life_.Quarantined()) RetainQuarantine();
    }
    bool SourceHealthy() const noexcept { std::lock_guard lock(mutex_); return life_.SourceHealthy(); }
    bool SourceFinished() const noexcept {
        std::lock_guard lock(mutex_); return life_.SourceFinished(Complete(storage_ ? storage_->sealed.Get() : nullptr, sealValue_));
    }
    bool Reclaimable() const noexcept {
        std::lock_guard lock(mutex_);
        return life_.Reclaimable(Complete(storage_ ? storage_->sealed.Get() : nullptr, sealValue_), Complete(readerDone_.Get(), readerValue_));
    }
    bool Reusable(bool soleReader) const noexcept {
        std::lock_guard lock(mutex_);
        return CanReuse(life_, Complete(storage_ ? storage_->sealed.Get() : nullptr, sealValue_),
                        Complete(readerDone_.Get(), readerValue_), soleReader);
    }
};

// Fixed-capacity pool. In-flight/retired entries count against the same bound;
// no-entry means optional depth absence, not an allocation or GPU/CPU wait.
class Owner final
{
    struct Entry {
        std::shared_ptr<Storage> storage;
        std::shared_ptr<Snapshot> reader;
    };
    static constexpr size_t Capacity = 2;
    std::array<Entry, Capacity> entries_{};
    Snapshot* offeredThisCall_ = nullptr; // synchronous alias; Entry retains it
    uint64_t lastCapture_ = 0, lastFrame_ = 0;
public:
    Owner() = default;
    Owner(const Owner&) = delete;
    Owner& operator=(const Owner&) = delete;
    ~Owner() {
        for (auto& entry : entries_)
            if (entry.reader && !entry.reader->Reclaimable()) entry.reader->Quarantine();
    }
    void Collect() noexcept {
        for (auto& entry : entries_) {
            // Old Prepared may still carry its per-use Reader after GPU finish.
            // Wait for CPU ownership too, so late destruction cannot affect reuse.
            if (entry.reader && entry.reader->Reusable(entry.reader.use_count() == 1)) entry.reader.reset();
            if (!entry.reader && entry.storage && entry.storage->retired) entry.storage.reset();
        }
    }
    bool SourceHealthy() const noexcept {
        for (const auto& entry : entries_) if (entry.reader && !entry.reader->SourceHealthy()) return false;
        return true;
    }
    bool SourceFinished() const noexcept {
        for (const auto& entry : entries_) if (entry.reader && !entry.reader->SourceFinished()) return false;
        return true;
    }
    bool SafeToRelease() noexcept {
        // Actual resize/shutdown seam: even a same-size new swapchain domain
        // must not inherit this cache just because its numeric generation is 1.
        // Mark first; an in-flight/held Reader keeps its exact retired storage.
        for (auto& entry : entries_) if (entry.storage) entry.storage->retired = true;
        Collect();
        for (const auto& entry : entries_) if (entry.reader) return false;
        return true; // retired idle objects have been released without waits
    }
    HoYoShade::Depth::Input Acquire(const Capture& c, const HoYoShade::ColorEndpoint& endpoint) noexcept {
        offeredThisCall_ = nullptr;
        Collect();
        if (!c.resource || !endpoint.queue) return {};
        const auto desc = c.resource->GetDesc();
        if (!c.directionKnown || !Fits(desc, c.region) || !ScalarFormat(desc.Format)) return {};
        Microsoft::WRL::ComPtr<ID3D12Device> device;
        if (FAILED(c.resource->GetDevice(IID_PPV_ARGS(&device)))) return {};
        const PoolKey key{device.Get(), endpoint.queue, endpoint.stream, endpoint.generation, desc.Format, c.region};
        const bool rollback = EpochRolledBack(lastCapture_, lastFrame_, c.epoch, endpoint.frame);
        lastCapture_ = c.epoch; lastFrame_ = endpoint.frame;
        for (auto& entry : entries_)
            if (entry.storage && (rollback || !entry.storage->key.Matches(key) || entry.storage->nextValue >= UINT64_MAX - 1))
                entry.storage->retired = true;
        Collect(); // retire idle incompatible entries; busy ones retain their exact owners
        std::array<Availability, Capacity> available{};
        for (size_t i = 0; i < Capacity; ++i)
            available[i] = {bool(entries_[i].storage), bool(entries_[i].reader),
                            entries_[i].storage && entries_[i].storage->retired.load()};
        const auto index = SelectEntry(available);
        if (index == Capacity) return {}; // fixed bound exhausted: keep color/NR alive
        auto* selected = &entries_[index];
        try {
            if (!selected->storage) {
                auto storage = std::make_shared<Storage>();
                if (!storage->Initialize(key)) return {}; // no Execute: partial setup retires normally
                selected->storage = std::move(storage);
            }
            selected->reader = std::make_shared<Snapshot>(selected->storage);
        } catch (...) { return {}; }
        if (!selected->reader->Seal(c, endpoint)) { Collect(); return {}; }
        auto input = selected->reader->Offer();
        if (input.reader) offeredThisCall_ = selected->reader.get();
        return input;
    }
    void Return(const HoYoShade::StageResult& result) noexcept {
        if (offeredThisCall_) offeredThisCall_->Return(result);
        offeredThisCall_ = nullptr;
        Collect();
    }
};
}

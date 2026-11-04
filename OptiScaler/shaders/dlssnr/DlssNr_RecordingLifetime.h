#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <memory>
#include <unordered_map>
#include <vector>
#include <limits>
#include <initializer_list>

// NR mutex required. Completion of one Execute is not closure of an executable
// recording. Every parked resource and descriptor slot shares this generation token.
namespace DlssNrGpuLifetime
{
#if defined(DLSSNR_LIFETIME_TEST_SEAM)
struct TestFaults { bool failSignal = false, failHistorySignal = false, deviceRemoved = false; };
#endif
struct Token
{
    static constexpr size_t QueueCapacity = 8, OwnerCapacity = 512;
    struct Submission
    {
        Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
        Microsoft::WRL::ComPtr<ID3D12Fence> fence;
        UINT64 target = 0;
    };
    std::array<Submission, QueueCapacity> executions {};
    std::vector<Microsoft::WRL::ComPtr<IUnknown>> owners;
    Microsoft::WRL::ComPtr<ID3D12CommandList> listOwner;
    uint64_t generation = 0;
    bool closed = false, discarded = false, submitted = false, quarantined = false;
#if defined(DLSSNR_LIFETIME_TEST_SEAM)
    TestFaults* faults = nullptr;
#endif
    bool DeviceLost() const
    {
#if defined(DLSSNR_LIFETIME_TEST_SEAM)
        if (faults && faults->deviceRemoved) return true;
#endif
        for (const auto& use : executions)
            if (use.fence && use.fence->GetCompletedValue() == UINT64_MAX) return true;
        return false;
    }
    bool GpuComplete() const
    {
        if (quarantined || !submitted) return false;
        for (const auto& use : executions)
        {
            if (!use.queue) continue;
            if (!use.fence || !use.target) return false;
            const auto done = use.fence->GetCompletedValue();
#if defined(DLSSNR_LIFETIME_TEST_SEAM)
            if (faults && faults->deviceRemoved) return false;
#endif
            if (done == UINT64_MAX || done < use.target) return false;
        }
        return true;
    }
    bool Complete() const { return !quarantined && closed && (discarded || GpuComplete()); }
    void RetireOwners()
    {
        // Completed metadata may remain referenced by feature-create uses/slots.
        // Do not let it invisibly retain old aux after the retirement budget drops it.
        if (!Complete()) return;
        owners.clear(); listOwner.Reset();
    }
    bool Retain(IUnknown* object)
    {
        if (!object) return true;
        if (quarantined || closed) return false;
        for (const auto& owner : owners) if (owner.Get() == object) return true;
        if (owners.size() >= OwnerCapacity) { quarantined = true; return false; }
        owners.emplace_back(object); return true;
    }
};
using Use = std::shared_ptr<Token>;
using Uses = std::vector<Use>;
inline bool CreationReady(const Use& use) { return use && !use->discarded && use->GpuComplete(); }
inline bool Complete(const Uses& uses)
{
    return std::all_of(uses.begin(), uses.end(), [](const Use& use) { return use && use->Complete(); });
}
inline bool Retain(const Use& use, std::initializer_list<IUnknown*> objects)
{
    if (!use || use->quarantined || use->closed) return false;
    for (auto* object : objects) if (!use->Retain(object)) return false;
    return true;
}
inline bool SameDevice(ID3D12DeviceChild* a, ID3D12DeviceChild* b)
{
    if (!a || !b) return false;
    Microsoft::WRL::ComPtr<ID3D12Device> da, db;
    Microsoft::WRL::ComPtr<IUnknown> ia, ib;
    return SUCCEEDED(a->GetDevice(IID_PPV_ARGS(&da))) && SUCCEEDED(b->GetDevice(IID_PPV_ARGS(&db))) &&
        SUCCEEDED(da.As(&ia)) && SUCCEEDED(db.As(&ib)) && ia.Get() == ib.Get();
}
inline ID3D12CommandQueue* SelectProducerQueue(bool authenticated, ID3D12CommandQueue* current,
                                              ID3D12CommandQueue* observed)
{
    return authenticated ? current : observed ? observed : current;
}
class Registry
{
    std::unordered_map<ID3D12CommandList*, Use> open_;
    Uses outstanding_;
    uint64_t nextGeneration_ = 0;
    bool halted_ = false;
#if defined(DLSSNR_LIFETIME_TEST_SEAM)
    TestFaults* faults_ = nullptr;
#endif
public:
    static constexpr size_t GenerationCapacity = 256;
#if defined(DLSSNR_LIFETIME_TEST_SEAM)
    explicit Registry(TestFaults* faults = nullptr) : faults_(faults) {}
#endif
    bool Halted() const { return halted_; }
    size_t Tracked() const { return outstanding_.size(); }
    bool Watches(ID3D12CommandList* list) const { return open_.find(list) != open_.end(); }
    Use Recording(ID3D12Device* device, ID3D12CommandList* list)
    {
        Collect();
        if (!device || !list || halted_) return {};
        if (auto found = open_.find(list); found != open_.end())
            return found->second->submitted || found->second->quarantined ? Use {} : found->second;
        if (outstanding_.size() >= GenerationCapacity) return {};
        auto token = std::make_shared<Token>();
        token->generation = ++nextGeneration_; token->listOwner = list;
#if defined(DLSSNR_LIFETIME_TEST_SEAM)
        token->faults = faults_;
#endif
        open_[list] = token; outstanding_.push_back(token); return token;
    }
    void Submitted(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists)
    {
        if (!lists) return;
        for (UINT i = 0; i < count; ++i)
        {
            auto found = open_.find(lists[i]); if (found == open_.end()) continue;
            auto& token = *found->second;
            const bool replay = token.submitted; token.submitted = true;
            // Replay is an exceptional safety stop, not a new recording. All resource,
            // heap and constant owners remain pinned, including pre-existing snapshots.
            if (replay || !queue || !SameDevice(lists[i], queue) || queue->GetDesc().Type != lists[i]->GetType())
            { token.quarantined = true; halted_ = true; }
            if (!queue) continue;
            Token::Submission* use = nullptr;
            for (auto& candidate : token.executions) if (candidate.queue.Get() == queue) { use = &candidate; break; }
            if (!use) for (auto& candidate : token.executions) if (!candidate.queue) { use = &candidate; break; }
            if (!use) { token.quarantined = true; halted_ = true; continue; }
            if (!use->queue)
            {
                use->queue = queue;
                Microsoft::WRL::ComPtr<ID3D12Device> device;
                if (FAILED(queue->GetDevice(IID_PPV_ARGS(&device))) || FAILED(device->CreateFence(
                    0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&use->fence))))
                { token.quarantined = true; halted_ = true; continue; }
            }
            if (!use->fence || use->target == UINT64_MAX) { token.quarantined = true; halted_ = true; continue; }
            ++use->target;
            HRESULT signal = E_FAIL;
#if defined(DLSSNR_LIFETIME_TEST_SEAM)
            if (!(faults_ && faults_->failSignal))
#endif
                signal = queue->Signal(use->fence.Get(), use->target);
            if (FAILED(signal)) { token.quarantined = true; halted_ = true; }
        }
        Collect();
    }
    void ResetRecording(ID3D12CommandList* list)
    {
        auto found = open_.find(list); if (found == open_.end()) return;
        found->second->closed = true;
        found->second->discarded = !found->second->submitted;
        open_.erase(found); Collect();
    }
    void Collect()
    {
        for (const auto& token : outstanding_)
        {
            if (token->DeviceLost()) token->quarantined = true;
            if (token->quarantined) halted_ = true;
            token->RetireOwners();
        }
        std::erase_if(outstanding_, [](const Use& token) { return token->Complete(); });
    }
    Uses Snapshot() { Collect(); return outstanding_; }
};
template <size_t Count> inline unsigned AvailableSlots(const Use (&slots)[Count])
{
    unsigned available = 0;
    for (const auto& use : slots) if (!use || use->Complete()) ++available;
    return available;
}
template <size_t Count> inline int AcquireSlot(Use (&slots)[Count], unsigned& next, const Use& recording)
{
    if (!recording || recording->quarantined || recording->closed || recording->submitted) return -1;
    for (unsigned i = 0; i < Count; ++i)
    {
        const unsigned slot = (next + i) % Count;
        if (slots[slot] && !slots[slot]->Complete()) continue;
        if (slots[slot]) slots[slot]->RetireOwners();
        slots[slot] = recording; next = (slot + 1) % Count; return static_cast<int>(slot);
    }
    return -1;
}
}

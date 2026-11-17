// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <array>
#include <memory>
#include <mutex>
#include <vector>

// Private command owners may stay cached after their last Evaluate. GPU
// completion does not close an executable NR generation. Poll those exact
// owners at the next Evaluate, without releasing the feature or waiting on CPU.
namespace OwnedRecordingRetirement
{
template<size_t Count> class Pool
{
    struct Recording
    {
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
        Microsoft::WRL::ComPtr<ID3D12Fence> fence;
        UINT64 target = 0;
    };
    struct Registry
    {
        std::mutex mutex;
        std::vector<std::weak_ptr<Pool>> pools;
    };
    static Registry& Owners()
    {
        static Registry owners;
        return owners;
    }
    std::recursive_mutex mutex;
    unsigned users = 0;
    bool uncertain = false;
    std::array<Recording, Count> recordings;

  public:
    class Use
    {
        Pool& pool;
        std::unique_lock<std::recursive_mutex> lock;
      public:
        explicit Use(Pool& owner) : pool(owner), lock(owner.mutex) { ++pool.users; }
        ~Use() { --pool.users; }
        Use(const Use&) = delete;
        Use& operator=(const Use&) = delete;
    };
    static std::shared_ptr<Pool> Create()
    {
        auto pool = std::make_shared<Pool>();
        auto& registry = Owners();
        std::lock_guard lock(registry.mutex);
        registry.pools.emplace_back(pool);
        return pool;
    }
    // Mutations are made only under the private owner's Use scope, covering
    // recording, Execute/Signal, context recreation and destruction.
    void Uncertain(bool value) noexcept { uncertain = value; }
    void Publish(size_t slot, ID3D12GraphicsCommandList* list,
                 ID3D12CommandAllocator* allocator, ID3D12Fence* fence, UINT64 target)
    {
        recordings.at(slot) = { list, allocator, fence, target };
    }
    void Forget(size_t slot) { recordings.at(slot) = {}; }
    void ForgetAll() { recordings = {}; }

    template<class Close> static unsigned Poll(const Pool* current, Close&& close)
    {
        auto& registry = Owners();
        std::vector<std::shared_ptr<Pool>> owners;
        {
            std::lock_guard registryLock(registry.mutex);
            for (auto it = registry.pools.begin(); it != registry.pools.end();)
            {
                auto pool = it->lock();
                if (!pool) { it = registry.pools.erase(it); continue; }
                ++it;
                if (pool.get() != current) owners.push_back(std::move(pool));
            }
        }
        unsigned retired = 0;
        for (const auto& pool : owners)
        {
            std::unique_lock ownerLock(pool->mutex, std::try_to_lock);
            if (!ownerLock || pool->users || pool->uncertain) continue;
            for (auto& record : pool->recordings)
            {
                if (!record.list || !record.allocator || !record.fence ||
                    !record.target || record.target == UINT64_MAX) continue;
                const auto completed = record.fence->GetCompletedValue();
                if (completed == UINT64_MAX || completed < record.target) continue;
                // The callback also checks every execution retained by NR and
                // performs the real allocator/list Reset, notification and Close.
                if (FAILED(close(record.list.Get(), record.allocator.Get()))) continue;
                record = {};
                ++retired;
            }
        }
        return retired;
    }
};
}

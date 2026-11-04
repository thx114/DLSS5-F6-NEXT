// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "GenshinCpuStageTiming.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <new>
#include <cstdint>
#include <utility>

// Admission accounting for observation-owned buffers/payloads, NOT a bound on
// driver residency, game memory, textures, shader metadata or process commit.
namespace GenshinDepthRetention {
inline constexpr uint64_t BufferBudget=64ull*1024*1024,PayloadBudget=16ull*1024*1024;
// Source images outlive frame subscriptions. Keep their admission independent
// of frame/coverage metadata slots, with 1 MiB unavailable to ordinary source
// caches/scratch. This is an additional bounded 16 MiB CPU source allowance.
inline constexpr uint64_t SourceBudget=16ull*1024*1024,SourceReserve=1024ull*1024;
struct SourceTelemetry {uint64_t bytes,ordinaryBytes,allocations,refused;};
struct RefusalTelemetry {uint64_t contention,capacity,bytes,conflict,allocation;};
struct Telemetry {uint64_t bytes,payloadBytes,resources,payloads,refused,peak,releases;};
namespace detail {
static_assert(std::atomic<uint64_t>::is_always_lock_free,"health sampling must be lock-free");
inline std::atomic<uint64_t> bytes{0},payloadBytes{0},resources{0},payloads{0},refused{0},peak{0},releases{0};
struct Lease;
inline std::atomic<uint64_t> sourceBytes{0},ordinarySourceBytes{0},sourceAllocations{0},sourceRefused{0};
inline std::atomic<uint64_t> contentionRefused{0},capacityRefused{0},bytesRefused{0},conflictRefused{0},allocationRefused{0};
struct Entry {const void* identity=nullptr;uint64_t bytes=0;std::weak_ptr<const Lease> lease;size_t next=0;};
// Exact identity deduplication, fixed capacity, indexed free slots. No complete
// 2048/4096-entry scan on each retained CB/Draw acquisition or final release.
template<size_t N,size_t B> struct Table {
    std::array<Entry,N> entries{};std::array<size_t,B> heads{};size_t free=1;
    Table(){for(size_t i=0;i<N;++i)entries[i].next=i+1<N?i+2:0;}
    static size_t Bucket(const void* key)noexcept {return (reinterpret_cast<uintptr_t>(key)>>4)%B;}
    Entry* Find(const void* key)noexcept {
        for(auto i=heads[Bucket(key)];i;i=entries[i-1].next)if(entries[i-1].identity==key)return &entries[i-1];
        return nullptr;
    }
    Entry* Insert(const void* key)noexcept {
        if(!free)return nullptr;const auto i=free;auto& e=entries[i-1];free=e.next;
        auto& head=heads[Bucket(key)];e.identity=key;e.next=head;head=i;return &e;
    }
    void Erase(Entry* e)noexcept {
        const auto i=static_cast<size_t>(e-entries.data())+1;auto* link=&heads[Bucket(e->identity)];
        while(*link&&*link!=i)link=&entries[*link-1].next;
        if(*link)*link=e->next;*e={};e->next=free;free=i;
    }
};
struct Registry {
    std::mutex mutex;
    Table<2048,4096> buffers;Table<4096,8192> payloads;
};
// Fixed metadata intentionally lives until process exit: tokens may be released
// by other function-static owners during shutdown, in any destruction order.
inline Registry* Store()noexcept {
    static Registry* const p=[]()noexcept->Registry*{try{return new(std::nothrow) Registry;}catch(...){return nullptr;}}();
    return p;
}
struct Lease {
    Microsoft::WRL::ComPtr<IUnknown> object;
    Registry* registry=nullptr;Entry* entry=nullptr;uint64_t size=0;bool payload=false,source=false,ordinary=false;
    ~Lease()noexcept {
        // The retained allocation is released while its charge is still live.
        object.Reset();
        if(source){sourceBytes.fetch_sub(size,std::memory_order_relaxed);
            if(ordinary)ordinarySourceBytes.fetch_sub(size,std::memory_order_relaxed);
            sourceAllocations.fetch_sub(1,std::memory_order_relaxed);releases.fetch_add(1,std::memory_order_relaxed);return;}
        if(!entry)return;
        std::lock_guard<std::mutex> lock(registry->mutex);
        if(payload)registry->payloads.Erase(entry);else registry->buffers.Erase(entry);
        (payload?payloadBytes:bytes).fetch_sub(size,std::memory_order_relaxed);
        (payload?payloads:resources).fetch_sub(1,std::memory_order_relaxed);
        releases.fetch_add(1,std::memory_order_relaxed);
    }
};
inline std::shared_ptr<const Lease> Acquire(const void* identity,uint64_t size,bool payload,
    Microsoft::WRL::ComPtr<IUnknown> object={})noexcept {
    GenshinCpuStageTiming::Scope cpuCost(GenshinCpuStageTiming::Stage::Retention);
    try{
        auto* registry=Store();if(!registry || !identity || !size) {++refused;return {};}
        // Declared before lock: any last candidate release follows unlock.
        std::shared_ptr<Lease> candidate;
        std::unique_lock<std::mutex> lock(registry->mutex,std::try_to_lock);
        if(!lock.owns_lock()){++contentionRefused;++refused;return {};}
        auto* existing=payload?registry->payloads.Find(identity):registry->buffers.Find(identity);
        if(existing){
            if(existing->bytes!=size){++conflictRefused;++refused;return {};}
            if(auto live=existing->lease.lock())return live;
            // Its destructor has begun and still owes uncharge/unlink. Never
            // alias/reuse that slot before the destructor holds this mutex.
            ++contentionRefused;++refused;return {};
        }
        const bool capacity=payload?registry->payloads.free!=0:registry->buffers.free!=0;
        if(!capacity){++capacityRefused;++refused;return {};}
        auto& used=payload?payloadBytes:bytes;const auto budget=payload?PayloadBudget:BufferBudget;
        const auto current=used.load(std::memory_order_relaxed);
        if(size>budget || current>budget-size){++bytesRefused;++refused;return {};}
        candidate=std::make_shared<Lease>();candidate->object=std::move(object);
        candidate->registry=registry;candidate->size=size;candidate->payload=payload;
        auto* free=payload?registry->payloads.Insert(identity):registry->buffers.Insert(identity);
        free->bytes=size;free->lease=candidate;candidate->entry=free;
        used.fetch_add(size,std::memory_order_relaxed);
        (payload?payloads:resources).fetch_add(1,std::memory_order_relaxed);
        const auto total=bytes.load(std::memory_order_relaxed)+payloadBytes.load(std::memory_order_relaxed);
        auto prior=peak.load(std::memory_order_relaxed);
        while(prior<total && !peak.compare_exchange_weak(prior,total,std::memory_order_relaxed)){}
        return candidate;
    }catch(...){++allocationRefused;++refused;return {};}
}
}
using Token=std::shared_ptr<const detail::Lease>;
inline Token Buffer(ID3D11Buffer* buffer)noexcept {
    if(!buffer)return {};
    Microsoft::WRL::ComPtr<IUnknown> identity;D3D11_BUFFER_DESC desc{};buffer->GetDesc(&desc);
    if(FAILED(buffer->QueryInterface(IID_PPV_ARGS(&identity))) || !identity){++detail::refused;return {};}
    const auto* key=identity.Get();return detail::Acquire(key,desc.ByteWidth,false,std::move(identity));
}
inline Token Payload(const void* identity,uint64_t bytes)noexcept {return detail::Acquire(identity,bytes,true);}
// One lease per immutable source allocation, shared by its actual owners.
// Unlike Payload(identity), this API never deduplicates unrelated allocations
// or consumes a frame metadata registry slot. Final source release is lock-free.
inline Token Source(uint64_t bytes,bool priority=false)noexcept {
    using namespace detail;
    try{
        if(!bytes||bytes>SourceBudget){++sourceRefused;return {};}
        auto token=std::make_shared<Lease>();
        auto reserve=[](std::atomic<uint64_t>& used,uint64_t size,uint64_t limit)noexcept {
            auto n=used.load(std::memory_order_relaxed);
            do{if(size>limit||n>limit-size)return false;}
            while(!used.compare_exchange_weak(n,n+size,std::memory_order_relaxed));return true;
        };
        if(!reserve(sourceBytes,bytes,SourceBudget)){++sourceRefused;return {};}
        if(!priority&&!reserve(ordinarySourceBytes,bytes,SourceBudget-SourceReserve)){
            sourceBytes.fetch_sub(bytes,std::memory_order_relaxed);++sourceRefused;return {};}
        token->size=bytes;token->source=true;token->ordinary=!priority;
        sourceAllocations.fetch_add(1,std::memory_order_relaxed);return token;
    }catch(...){++sourceRefused;return {};}
}
inline SourceTelemetry ReadSourceTelemetry()noexcept {
    using namespace detail;return {sourceBytes.load(),ordinarySourceBytes.load(),sourceAllocations.load(),sourceRefused.load()};
}
inline RefusalTelemetry ReadRefusalTelemetry()noexcept {
    using namespace detail;return {contentionRefused.load(),capacityRefused.load(),bytesRefused.load(),conflictRefused.load(),allocationRefused.load()};
}
// Declare sets BEFORE all owners they cover. Owning structs use replacement
// assignment, so both destruction and assignment release owners before tokens.
template<size_t N> struct Buffers {
    std::array<Token,N> tokens{};size_t count=0;
    bool Add(ID3D11Buffer* buffer)noexcept {
        if(!buffer)return true;
        auto token=Buffer(buffer);if(!token)return false;
        for(size_t i=0;i<count;++i)if(tokens[i]==token)return true;
        if(count==N){++detail::refused;return false;}
        tokens[count++]=std::move(token);return true;
    }
};
inline Telemetry ReadTelemetry()noexcept {
    using namespace detail;
    return {bytes.load(std::memory_order_relaxed),payloadBytes.load(std::memory_order_relaxed),
        resources.load(std::memory_order_relaxed),payloads.load(std::memory_order_relaxed),
        refused.load(std::memory_order_relaxed),peak.load(std::memory_order_relaxed),releases.load(std::memory_order_relaxed)};
}
}

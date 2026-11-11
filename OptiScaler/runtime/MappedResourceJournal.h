// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <type_traits>
#include <utility>

namespace FramePipeline {
struct MappedResourceKey {
    uintptr_t context=0,resource=0;
    uint32_t subresource=0;
    bool operator==(const MappedResourceKey&)const=default;
};
enum class MapConflict { Resource, ContextResource };

// Owns CPU writeback transactions, not graphics completion or frame authority.
// Once a replacement pointer reaches the application, owner/config changes must
// not erase its transaction. Take is independent of thread/config eligibility;
// adapters must perform writeback before validating optional snapshot publication.
template<class Transaction,size_t Capacity,MapConflict Conflict=MapConflict::ContextResource>
class MappedResourceJournal final {
    static_assert(Capacity>0);
    static_assert(std::is_nothrow_move_constructible_v<Transaction>);
    static_assert(std::is_nothrow_move_assignable_v<Transaction>);
    struct Entry {MappedResourceKey key;Transaction transaction;};
    std::mutex mutex_;
    std::array<std::optional<Entry>,Capacity> entries_{};
    std::atomic<size_t> count_{0};
public:
    bool HasEntries()const noexcept {return count_.load(std::memory_order_acquire)!=0;}

    // The commit callback runs only with capacity and uniqueness established.
    // It may validate/commit the adapter's exact resource revision under its own
    // lock. A refusal leaves both the application pointer and transaction intact.
    template<class Commit>
    bool TryInsert(MappedResourceKey key,Transaction&& transaction,Commit&& commit) {
        if(!key.context || !key.resource)return false;
        std::unique_lock lock(mutex_,std::try_to_lock);
        if(!lock.owns_lock())return false;
        std::optional<Entry>* free=nullptr;
        for(auto& entry:entries_){
            if(!entry){if(!free)free=&entry;continue;}
            if(entry->key.resource==key.resource &&
               (Conflict==MapConflict::Resource || entry->key.context==key.context))return false;
        }
        if(!free || !std::forward<Commit>(commit)())return false;
        free->emplace(Entry{key,std::move(transaction)});
        count_.fetch_add(1,std::memory_order_release);
        return true;
    }
    bool TryInsert(MappedResourceKey key,Transaction&& transaction) {
        return TryInsert(key,std::move(transaction),[]()noexcept{return true;});
    }
    Transaction Take(MappedResourceKey key) {
        if(!HasEntries())return {};
        std::lock_guard lock(mutex_);
        for(auto& entry:entries_)if(entry && entry->key==key){
            auto transaction=std::move(entry->transaction);
            entry.reset();count_.fetch_sub(1,std::memory_order_release);
            return transaction;
        }
        return {};
    }
};
} // namespace FramePipeline

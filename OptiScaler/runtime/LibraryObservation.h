// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "LoaderModuleState.h"
#include <atomic>
#include <algorithm>
#include <mutex>
#include <new>
#include <vector>
#include <type_traits>

namespace LibraryLoading {
// Producers may run in DllMain: they only enqueue a real module reference.
// The consumer runs after loader callouts, checks completed initialization,
// and pins each process-lifetime hook target exactly once per requested role.
enum class ObservationOwnership { Ignore, Metadata, Hooks };
class Observations final {
    struct Item { HMODULE module; bool role; Item* next; };
    std::atomic<Item*> pending_{nullptr};
    std::atomic<bool> scan_{false};
    std::atomic<bool> retryPending_{false};
    std::recursive_mutex mutex_;
    bool processing_ = false;
    Item* retries_ = nullptr;
    std::vector<std::pair<HMODULE, bool>> applied_;
public:
    void RequestScan() noexcept { scan_.store(true, std::memory_order_release); }
    void Enqueue(HMODULE module, bool role = false) {
        if (!module || (reinterpret_cast<uintptr_t>(module) & 3)) return;
        auto* item = new(std::nothrow) Item{module, role, nullptr};
        if (!item) return;
        HMODULE retained = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(module), &retained)) {
            delete item; return;
        }
        auto* previous = pending_.load(std::memory_order_relaxed);
        do { item->next = previous; }
        while (!pending_.compare_exchange_weak(previous, item, std::memory_order_release, std::memory_order_relaxed));
    }
    bool HasImmediateWork() const noexcept { return pending_.load(std::memory_order_acquire) || scan_.load(std::memory_order_acquire); }
    bool HasWork() const noexcept { return HasImmediateWork() || retryPending_.load(std::memory_order_acquire); }
    // Only called by the consumer while its serialization is already held.
    void Discover(HMODULE module) {
        if (std::ranges::find(applied_,std::pair{module,false}) == applied_.end()) Enqueue(module);
    }
    template<class Scan, class Apply> void Process(bool allowed, Scan&& scan, Apply&& apply) {
        Process(allowed, std::forward<Scan>(scan), std::forward<Apply>(apply),
            [](HMODULE, bool) { return ObservationOwnership::Hooks; });
    }
    template<class Scan, class Apply, class Ownership>
    void Process(bool allowed, Scan&& scan, Apply&& apply, Ownership&& ownership) {
        if (!allowed) return;
        std::unique_lock lock(mutex_, std::try_to_lock);
        if (!lock.owns_lock() || processing_ || (!HasWork() && !retries_)) return;
        processing_ = true;
        struct End { bool& processing; ~End() { processing = false; } } end{processing_};
        auto* retries = retries_; retries_ = nullptr; retryPending_.store(false,std::memory_order_release);
        while (retries) {
            auto* item = retries; retries = item->next;
            auto* previous = pending_.load(std::memory_order_relaxed);
            do { item->next = previous; }
            while (!pending_.compare_exchange_weak(previous,item,std::memory_order_release,std::memory_order_relaxed));
        }
        std::vector<std::pair<HMODULE,bool>> attempted;
        do {
            if (scan_.exchange(false, std::memory_order_acq_rel)) scan();
            while (auto* batch = pending_.exchange(nullptr, std::memory_order_acquire)) {
                Item* ordered = nullptr;
                while (batch) { auto* next = batch->next; batch->next = ordered; ordered = batch; batch = next; }
                while (ordered) {
                    auto* item = ordered; ordered = item->next;
                    const auto key = std::pair{item->module, item->role};
                    bool retry = false;
                    if (std::ranges::find(applied_,key) == applied_.end() &&
                        std::ranges::find(attempted,key) == attempted.end() && Initialized(item->module)) {
                        attempted.push_back(key);
                        const auto role = ownership(item->module,item->role);
                        if (role != ObservationOwnership::Ignore) {
                            HMODULE pinned = nullptr;
                            const bool owned = role == ObservationOwnership::Metadata ||
                                (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                                    reinterpret_cast<LPCWSTR>(item->module),&pinned) && pinned == item->module);
                            if (owned) {
                                bool completed = true;
                                if constexpr (std::is_void_v<decltype(apply(item->module,item->role))>) apply(item->module,item->role);
                                else completed = apply(item->module,item->role);
                                if (completed && role == ObservationOwnership::Hooks) applied_.push_back(key);
                                retry = !completed;
                            } else retry = true;
                        }
                    }
                    if (retry) { item->next = retries_; retries_ = item; retryPending_.store(true,std::memory_order_release); }
                    else { FreeLibrary(item->module); delete item; }
                }
            }
        } while (HasImmediateWork()); // loads made by apply also reconcile their static imports
    }
};
}

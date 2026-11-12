// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <utility>

namespace PluginStartup
{
enum class Status
{
    Disabled, Pending, Initializing, WorkerUnavailable, Missing, InventoryFailed,
    AlreadyLoaded, LoadFailed, EntryMissing, Failed, InitReturned, Completed
};

// A queue receipt only: terminal failures release consumers too. The caller must
// let each SDK determine its own support; completion never certifies capability.
class Gate
{
    std::atomic<Status> status_ { Status::Disabled };
    std::atomic<uint64_t> thread_ { 0 };
    std::mutex mutex_;
    std::condition_variable changed_;

    static bool Pending(Status status) noexcept
    {
        return status == Status::Pending || status == Status::Initializing;
    }

  public:
    Status Current() const noexcept { return status_.load(std::memory_order_acquire); }
    bool Selected() const noexcept { return Current() != Status::Disabled; }

    // Called during attach, before publishing the startup worker.
    bool Arm() noexcept
    {
        Status expected = Status::Disabled;
        return status_.compare_exchange_strong(expected, Status::Pending, std::memory_order_acq_rel);
    }

    void WorkerUnavailable()
    {
        {
            std::lock_guard lock(mutex_);
            Status expected = Status::Pending;
            status_.compare_exchange_strong(expected, Status::WorkerUnavailable, std::memory_order_acq_rel);
        }
        changed_.notify_all();
    }

    template<class Work> void Run(uint64_t thread, Work&& work)
    {
        Status expected = Status::Pending;
        if (!status_.compare_exchange_strong(expected, Status::Initializing, std::memory_order_acq_rel))
            return;
        thread_.store(thread, std::memory_order_release);
        Status result = Status::Failed;
        try
        {
            result = std::forward<Work>(work)();
            if (result == Status::Disabled || Pending(result)) result = Status::Failed;
        }
        catch (...) { result = Status::Failed; }
        {
            std::lock_guard lock(mutex_);
            status_.store(result, std::memory_order_release);
        }
        changed_.notify_all();
    }

    bool Await(bool safeContext, uint64_t thread, unsigned timeoutMilliseconds = 5000)
    {
        auto status = Current();
        if (!Pending(status)) return true;
        // No wait from DllMain, executable TLS, an unknown loader context, or
        // recursive graphics calls made by the plugin's own initializing thread.
        if (!safeContext || thread_.load(std::memory_order_acquire) == thread) return false;
        std::unique_lock lock(mutex_);
        if (!changed_.wait_for(lock, std::chrono::milliseconds(timeoutMilliseconds),
                               [&] { return !Pending(Current()); })) return false;
        return true;
    }
};
}

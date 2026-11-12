#include <runtime/PluginStartupGate.h>
#include <atomic>
#include <cstdio>
#include <stdexcept>
#include <thread>
#include <vector>

namespace
{
unsigned checks = 0;
void Check(bool result, const char* message)
{
    ++checks;
    if (!result) throw std::runtime_error(message);
}
}

int main()
{
    try
    {
        using namespace PluginStartup;
        Gate disabled;
        Check(disabled.Await(false, 1, 0), "unselected game has no startup dependency");
        disabled.WorkerUnavailable();
        disabled.Run(1, []() -> Status { throw std::runtime_error("must not run"); });
        Check(disabled.Current() == Status::Disabled, "disabled remains inert");
        Gate failedWorker;
        Check(failedWorker.Arm() && !failedWorker.Arm(), "selection is once per startup");
        Check(!failedWorker.Await(false, 1, 0), "loader or TLS context never waits");
        Check(!failedWorker.Await(true, 1, 1), "pending worker times out");
        failedWorker.WorkerUnavailable();
        Check(failedWorker.Await(true, 1), "worker failure terminates the queue without claiming SDK support");
        std::atomic<unsigned> calls { 0 };
        failedWorker.Run(2, [&] { ++calls; return Status::InitReturned; });
        Check(calls == 0, "unavailable worker is terminal");
        Gate active;
        active.Arm();
        std::atomic<bool> inside { false }, release { false }, recursiveRejected { false };
        std::jthread worker([&]
        {
            active.Run(2, [&]
            {
                ++calls;
                recursiveRejected = !active.Await(true, 2);
                inside = true;
                while (!release.load()) std::this_thread::yield();
                return Status::InitReturned;
            });
        });
        while (!inside.load()) std::this_thread::yield();
        Check(recursiveRejected, "initializing plugin cannot wait for itself");
        Check(!active.Await(false, 3) && !active.Await(true, 3, 1), "unsafe context and pending timeout reject");
        std::vector<std::jthread> competitors;
        for (unsigned i = 0; i < 12; ++i)
            competitors.emplace_back([&, i] { active.Run(i + 10, [&] { ++calls; return Status::Failed; }); });
        for (auto& competitor : competitors) competitor.join();
        active.WorkerUnavailable();
        Check(active.Current() == Status::Initializing, "late worker failure cannot interrupt active startup");
        release = true;
        Check(active.Await(true, 3), "terminal publication wakes graphics consumer");
        worker.join();
        Check(calls == 1 && active.Current() == Status::InitReturned, "concurrent calls initialize exactly once");
        for (auto terminal : { Status::WorkerUnavailable, Status::Missing, Status::InventoryFailed, Status::AlreadyLoaded,
                               Status::LoadFailed, Status::EntryMissing, Status::Failed, Status::InitReturned,
                               Status::Completed })
        {
            Gate failed;
            failed.Arm();
            failed.Run(1, [=] { return terminal; });
            Check(failed.Await(true, 2), "all terminal receipts release the queue barrier");
            Check(failed.Current() == terminal, "completion does not rewrite the plugin outcome");
        }
        Gate exception;
        exception.Arm();
        exception.Run(1, []() -> Status { throw std::runtime_error("plugin initialization"); });
        Check(exception.Current() == Status::Failed && exception.Await(true, 2), "exception publishes terminal receipt");
        Gate invalid;
        invalid.Arm();
        invalid.Run(1, [] { return Status::Pending; });
        Check(invalid.Current() == Status::Failed, "invalid callback result cannot leave waiters pending");
        Check(invalid.Await(true, 2), "invalid result cannot permanently block consumers");
        std::printf("PluginStartupGate PASS: %u checks\n", checks);
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "PluginStartupGate FAIL: %s\n", error.what());
        return 1;
    }
}

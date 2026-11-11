#include <runtime/SourcePresent.h>
#include <atomic>
#include <cstdio>
#include <stdexcept>
#include <thread>
#include <type_traits>

unsigned checks = 0;
void Check(bool ok, const char* message)
{
    ++checks;
    if (!ok) throw std::runtime_error(message);
}
int main() try
{
    static_assert(!std::is_copy_constructible_v<SourcePresent::Scope>);
    {
        SourcePresent::Scope testOrOutput(false);
        Check(!testOrOutput.Owns(), "TEST/physical output has no source authority");
        SourcePresent::Scope directProxy(true);
        Check(directProxy.Owns(), "direct native FG proxy owns missing source observation");
        SourcePresent::Scope nestedProxy(true);
        Check(!nestedProxy.Owns(), "recursive proxy cannot advance another source epoch");
    }
    {
        SourcePresent::Scope wrapper(true);
        Check(wrapper.Owns(), "ordinary source wrapper owns observation");
        SourcePresent::Scope nestedProxy(true);
        Check(!nestedProxy.Owns(), "nested FG proxy cannot duplicate outer source bookkeeping");
        SourcePresent::Scope physicalCallback(false);
        Check(!physicalCallback.Owns(), "same-thread physical callback has no authority");
        std::atomic<bool> workerOwned{true};
        std::thread worker([&] {
            SourcePresent::Scope physicalWorker(false);
            workerOwned = physicalWorker.Owns();
        });
        worker.join();
        Check(!workerOwned, "cross-thread SDK output has no source authority");
    }
    try {
        SourcePresent::Scope wrapper(true);
        throw 1;
    } catch (int) {}
    SourcePresent::Scope afterUnwind(true);
    Check(afterUnwind.Owns(), "exception unwinding releases exactly its source scope");
    std::atomic<bool> independentSource{false};
    std::thread sourceThread([&] {
        SourcePresent::Scope source(true);
        independentSource = source.Owns();
    });
    sourceThread.join();
    Check(independentSource, "independent source threads do not borrow another thread's scope");
    std::printf("PASS %u source Present scope CPU checks\n", checks);
    return 0;
} catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
}

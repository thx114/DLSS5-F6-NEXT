#include <runtime/PrivateGraphicsScope.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <type_traits>

bool PrivateGraphicsScopeActiveFromOtherTranslationUnit();

namespace
{
unsigned checks = 0;
void Check(bool ok, const char* what)
{
    ++checks;
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); std::exit(1); }
}
}

int main()
{
    using namespace PrivateGraphics;
    static_assert(!std::is_copy_constructible_v<Scope> && !std::is_move_constructible_v<Scope>);
    Check(!Active() && !PrivateGraphicsScopeActiveFromOtherTranslationUnit(), "initial state");
    {
        Scope outer;
        Check(Active() && PrivateGraphicsScopeActiveFromOtherTranslationUnit(), "cross-unit scope");
        { Scope inner; Check(Active(), "nested scope active"); }
        Check(Active(), "outer scope survives nested scope");
        Check(Invoke([] { return Active() ? 37 : 0; }) == 37 && Active(), "invoke returns value");
        try { Invoke([] { throw 1; }); } catch (int) {}
        Check(Active(), "throw restores outer scope");

        std::atomic<bool> workerReady = false, releaseWorker = false;
        std::atomic<bool> workerOk = false;
        std::thread worker([&] {
            bool ok = !Active() && !PrivateGraphicsScopeActiveFromOtherTranslationUnit();
            { Scope nested; ok = ok && Active() && PrivateGraphicsScopeActiveFromOtherTranslationUnit(); }
            ok = ok && !Active();
            workerOk.store(ok, std::memory_order_release);
            workerReady.store(true, std::memory_order_release);
            while (!releaseWorker.load(std::memory_order_acquire)) std::this_thread::yield();
        });
        while (!workerReady.load(std::memory_order_acquire)) std::this_thread::yield();
        Check(workerOk.load(std::memory_order_acquire) && Active(), "other thread isolated");
        releaseWorker.store(true, std::memory_order_release);
        worker.join();
    }
    Check(!Active() && !PrivateGraphicsScopeActiveFromOtherTranslationUnit(), "scope exited");
    try { Scope outer; throw 1; } catch (int) {}
    Check(!Active(), "exception restores public context");
    std::printf("PASS %u private graphics scope checks\n", checks);
}

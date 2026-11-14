#include "../ReflexForwarding.h"
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
    namespace forwarding = reflex::forwarding;
    constexpr auto marker=sl::PCLMarker::ePresentStart;
    static_assert(!std::is_copy_constructible_v<forwarding::Scope>);
    Check(!forwarding::Active(marker) && !forwarding::sleeping,"initial call forwards");
    {
        const auto outer=forwarding::Scope::Marker(marker);
        const auto sleep=forwarding::Scope::Sleep();
        Check(forwarding::Active(marker) && forwarding::sleeping,"same-thread recursive calls bypass SL");
        Check(!forwarding::Active(sl::PCLMarker::eSimulationStart),"different marker remains independent");
        try
        {
            const auto nested=forwarding::Scope::Marker(marker);
            const auto nestedSleep=forwarding::Scope::Sleep();
            throw 7;
        }
        catch(int) {}
        Check(forwarding::Active(marker) && forwarding::sleeping,"nested exception preserves outer recursion flags");
        std::atomic<bool> independent{false}, guarded{false}, restored{false};
        std::thread worker([&] {
            independent=!forwarding::Active(marker) && !forwarding::sleeping;
            {
                const auto sameMarker=forwarding::Scope::Marker(marker);
                const auto sameSleep=forwarding::Scope::Sleep();
                guarded=forwarding::Active(marker) && forwarding::sleeping;
            }
            restored=!forwarding::Active(marker) && !forwarding::sleeping;
        });
        worker.join();
        Check(independent && guarded && restored,"concurrent same-marker caller forwards and unwinds independently");
        Check(forwarding::Active(marker) && forwarding::sleeping,"worker cannot clear calling thread's guard");
    }
    Check(!forwarding::Active(marker) && !forwarding::sleeping,"normal return restores forwarding");
    for (size_t i=0;i<static_cast<size_t>(sl::PCLMarker::eMaximum);++i)
    {
        const auto value=static_cast<sl::PCLMarker>(i);
        const auto scope=forwarding::Scope::Marker(value);
        Check(forwarding::Active(value),"guard covers current SDK's complete marker range");
    }
    Check(forwarding::Active(sl::PCLMarker::eMaximum),"unmapped marker is refused before indexing");
    std::printf("PASS %u Reflex forwarding checks.\n",checks);
    return 0;
}
catch(const std::exception& error)
{
    std::fprintf(stderr,"FAIL: %s\n",error.what());
    return 1;
}

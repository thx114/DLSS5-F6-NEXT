#include <misc/FrameLimitTimer.h>
#include <array>
#include <barrier>
#include <cstdio>
#include <thread>

int main()
{
    constexpr size_t callers=4, rounds=12;
    std::barrier start(static_cast<std::ptrdiff_t>(callers));
    std::array<std::thread,callers> workers;
    std::array<unsigned,callers> failures {};
    for(size_t i=0;i<callers;++i)
        workers[i]=std::thread([&,i] {
            for(size_t round=0;round<rounds;++round)
            {
                start.arrive_and_wait();
                failures[i]+=frame_limit::TimerSleep(40'000+static_cast<int64_t>(i)*2'000)!=0;
            }
        });
    for(auto& worker:workers)worker.join();
    for(const auto failure:failures)
        if(failure)
        {
            std::fprintf(stderr,"FAIL: concurrent frame limit timer lost an independent caller\n");
            return 1;
        }
    std::printf("PASS %zu concurrent frame limit waits; all caller threads exited.\n",callers*rounds);
    return 0;
}

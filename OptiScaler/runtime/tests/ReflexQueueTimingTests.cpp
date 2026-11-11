// SPDX-License-Identifier: GPL-3.0-or-later
#include <runtime/ReflexQueueTiming.h>
#include <cassert>
#include <cmath>
#include <cstdio>
int main()
{
    using namespace ReflexQueueTiming;
    const auto actual=Split(6743,false,1000000,1103896,1050734,1103896);
    assert(actual && std::abs(actual->beforeGpuMs-50.734)<1e-9 &&
        std::abs(actual->gpuSpanMs-53.162)<1e-9 && actual->afterGpuMs==0);
    assert(!Split(0,false,1,20,2,18));
    assert(!Split(1,true,1,20,2,18));
    assert(!Split(1,false,0,20,2,18));
    assert(!Split(1,false,1,0,2,18));
    assert(!Split(1,false,1,20,0,18));
    assert(!Split(1,false,1,20,2,0));
    assert(!Split(1,false,3,20,2,18));
    assert(!Split(1,false,1,20,19,18));
    assert(!Split(1,false,1,17,2,18));
    const auto idle=Split(1,false,1,21,1,1);
    assert(idle && idle->beforeGpuMs==0 && idle->gpuSpanMs==0 && idle->afterGpuMs==.02);
    const auto large=Split(1,false,UINT64_MAX-200,UINT64_MAX,UINT64_MAX-100,UINT64_MAX-50);
    assert(large && large->beforeGpuMs==.1 && large->gpuSpanMs==.05 && large->afterGpuMs==.05);
    std::puts("Reflex queue timing: 12 cases passed; same-report endpoints only");
}

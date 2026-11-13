// SPDX-License-Identifier: GPL-3.0-or-later
#include <shaders/dlssnr/NrGpuExecutionLifetime.h>
#include <cassert>
#include <cstdio>

namespace
{
int featureIdentity, parameterIdentity;
int order=0, releases=0;
void DefaultRelease(void* feature)
{
    assert(feature==&featureIdentity && order==0);
    order=1;
    ++releases;
}
void OverrideRelease(void* feature)
{
    assert(feature==&featureIdentity && order==0);
    order=2;
    ++releases;
}
void Destroy(NVSDK_NGX_Parameter* parameters)
{
    assert(parameters==reinterpret_cast<NVSDK_NGX_Parameter*>(&parameterIdentity));
    assert(order==1 || order==2 || order==4);
    order=3;
}
}

int main()
{
    NrGpuExecutionLifetime owner;
    void* feature=&featureIdentity;
    auto* parameters=reinterpret_cast<NVSDK_NGX_Parameter*>(&parameterIdentity);
    owner.ParkFeature(feature,nullptr,&parameters);
    assert(!feature && !parameters && owner.RetiredCount()==1 && releases==0);
    owner.CollectCompleted(DefaultRelease,Destroy);
    assert(owner.Empty() && order==3 && releases==1);
    owner.CollectCompleted(DefaultRelease,Destroy);
    assert(releases==1);

    order=0;feature=&featureIdentity;
    parameters=reinterpret_cast<NVSDK_NGX_Parameter*>(&parameterIdentity);
    owner.ParkFeature(feature,OverrideRelease,&parameters);
    owner.CollectCompleted(DefaultRelease,Destroy);
    assert(owner.Empty() && order==3 && releases==2);

    order=4;
    parameters=reinterpret_cast<NVSDK_NGX_Parameter*>(&parameterIdentity);
    owner.ParkFeature(feature,nullptr,&parameters);
    assert(!feature && !parameters && owner.RetiredCount()==1);
    owner.CollectCompleted(DefaultRelease,Destroy);
    assert(owner.Empty() && order==3 && releases==2);
    owner.ParkFeature(feature,nullptr,&parameters);
    assert(owner.Empty());
    std::puts("PASS production NR feature/parameter ownership transfer, release order, override, parameter-only and idempotent collection; WARP recording gates tested separately");
}

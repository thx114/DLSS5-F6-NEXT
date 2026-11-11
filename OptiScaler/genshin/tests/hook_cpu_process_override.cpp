#include "../GenshinHookCpuTiming.h"
#include <cstdio>

// A fresh process per environment value. No OnPresent, graphics API or game.
int main(int argc,char** argv)
{
    if(argc!=2 || (argv[1][0]!='0' && argv[1][0]!='1') || argv[1][1])return 2;
    const bool expected=argv[1][0]=='1';
    constexpr DWORD sentinel=0x12345678;
    const auto control=GenshinHookCpuTiming::control.load();
    const auto epoch=GenshinHookCpuTiming::monitorEpoch.load();
    SetLastError(sentinel);
    if(GenshinHookCpuTiming::ProcessTimingRequested()!=expected || GetLastError()!=sentinel)return 3;
    // Initialization is process-scoped, never a per-draw environment lookup.
    if(!SetEnvironmentVariableW(L"OPTISCALER_HOOK_CPU_TIMING",expected?L"0":L"1"))return 4;
    SetLastError(sentinel);
    if(GenshinHookCpuTiming::ProcessTimingRequested()!=expected || GetLastError()!=sentinel)return 5;
    if(GenshinHookCpuTiming::control.load()!=control || GenshinHookCpuTiming::monitorEpoch.load()!=epoch)return 6;
    std::puts("PASS process diagnostic selection/cache/error preservation; no sampler or graphics started");
}

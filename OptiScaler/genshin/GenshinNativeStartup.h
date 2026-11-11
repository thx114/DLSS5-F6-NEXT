// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <Windows.h>
#include <atomic>
#include <cstdint>
#include <intrin.h>

namespace GenshinNativeStartup {
// Scheduling only: no readiness result is a native/resource/birth certificate.
enum class Status { Ready, Refused, LoaderCallout, ProbeUnavailable, Reentrant, Timeout };
inline const char* Name(Status status)noexcept {
    switch(status){
    case Status::Ready:return "ready";case Status::Refused:return "refused";
    case Status::LoaderCallout:return "loader-callout";case Status::ProbeUnavailable:return "loader-probe-unavailable";
    case Status::Reentrant:return "reentrant";default:return "timeout";}
}
struct Result {Status status;uint64_t waitedMs;};
inline Status ProbeLoaderCallout()noexcept {
    // Resolve only from an existing system runtime; never load/inject a DLL.
    // Unknown or callout context forbids sleeping. This grants no call authority.
    using Probe=BOOLEAN(NTAPI*)();
    const auto module=GetModuleHandleW(L"ntdll.dll");
    const auto probe=module?reinterpret_cast<Probe>(GetProcAddress(module,"RtlIsThreadWithinLoaderCallout")):nullptr;
    if(!probe)return Status::ProbeUnavailable;
    if(probe())return Status::LoaderCallout;
    // This export alone is false in a real executable TLS callback on the
    // supported Windows build. Also inspect the x64 PEB loader critical section.
    // Every read is bounded; the section and its debug backlink must both be
    // writable non-executable ntdll image data. An unknown ABI refuses waiting.
#if defined(_M_X64)
    const auto peb=static_cast<uintptr_t>(__readgsqword(0x60));
    CRITICAL_SECTION* lock=nullptr;SIZE_T read=0;
    if(!peb || !ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<const void*>(peb+0x110),&lock,sizeof(lock),&read) || read!=sizeof(lock) || !lock)
        return Status::ProbeUnavailable;
    const auto dataRange=[&](const void* address,size_t size){
        MEMORY_BASIC_INFORMATION info{};
        if(!address || !size || VirtualQuery(address,&info,sizeof(info))!=sizeof(info) ||
           info.State!=MEM_COMMIT || info.Type!=MEM_IMAGE || info.AllocationBase!=module ||
           (info.Protect&(PAGE_GUARD|PAGE_NOACCESS)) ||
           ((info.Protect&0xff)!=PAGE_READWRITE && (info.Protect&0xff)!=PAGE_WRITECOPY))return false;
        const auto start=reinterpret_cast<uintptr_t>(address),base=reinterpret_cast<uintptr_t>(info.BaseAddress);
        return start>=base && start-base<=info.RegionSize && size<=info.RegionSize-(start-base);
    };
    CRITICAL_SECTION current{};RTL_CRITICAL_SECTION_DEBUG debug{};
    if(!dataRange(lock,sizeof(current)) || !ReadProcessMemory(GetCurrentProcess(),lock,&current,sizeof(current),&read) || read!=sizeof(current) ||
       !dataRange(current.DebugInfo,sizeof(debug)) ||
       !ReadProcessMemory(GetCurrentProcess(),current.DebugInfo,&debug,sizeof(debug),&read) || read!=sizeof(debug) ||
       debug.CriticalSection!=lock || current.RecursionCount<0 ||
       (current.RecursionCount==0)!=(current.OwningThread==nullptr))return Status::ProbeUnavailable;
    return current.OwningThread==reinterpret_cast<HANDLE>(static_cast<uintptr_t>(GetCurrentThreadId()))?Status::LoaderCallout:Status::Ready;
#else
    return Status::ProbeUnavailable;
#endif
}
class Barrier final {
    std::atomic<bool> expired_{false};
    inline static thread_local bool waiting_=false;
public:
    // Concurrent first creations may join, but the first timeout closes all
    // later waits. A later Ready still has to pass the actual NativeCall proof.
    // No loader/global/native-ledger lock spans a pause or a factory call.
    template<class Ready,class Refused,class Probe,class Clock,class Pause>
    Result Await(Ready ready,Refused refused,Probe probe,Clock clock,Pause pause,uint64_t budgetMs=60000)noexcept {
        if(refused())return {Status::Refused,0};
        if(ready())return {Status::Ready,0};
        if(waiting_)return {Status::Reentrant,0};
        if(expired_.load())return {Status::Timeout,0};
        const auto context=probe();if(context!=Status::Ready)return {context,0};
        waiting_=true;struct Leave {~Leave(){waiting_=false;}} leave;
        const auto started=clock();
        for(;;){
            const uint64_t elapsed=clock()-started;
            if(refused())return {Status::Refused,elapsed};
            if(ready())return {Status::Ready,elapsed};
            if(expired_.load() || elapsed>=budgetMs){expired_=true;return {Status::Timeout,elapsed};}
            const auto left=budgetMs-elapsed;pause(static_cast<DWORD>(left<10?left:10));
        }
    }
};
}

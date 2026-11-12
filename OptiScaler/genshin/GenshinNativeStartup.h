// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <Windows.h>
#include <atomic>
#include <cstdint>
#include <intrin.h>
#include <runtime/LoaderStartupContext.h>

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
    const auto context=LoaderStartupContext::Probe(module,probe);
    if(context==LoaderStartupContext::Status::Ready)return Status::Ready;
    return context==LoaderStartupContext::Status::LoaderCallout?Status::LoaderCallout:Status::ProbeUnavailable;
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

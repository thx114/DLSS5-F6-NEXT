// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../genshin/GenshinHookCpuTiming.h"
#include <cassert>
#include <cstdio>
#include <stdexcept>
#include <thread>

// Production HookScope and publisher, CPU/Win32 clock only; no Present, SDK,
// D3D device, cache proof mutation or game. Link GenshinHookCpuTiming.cpp.
int main() {
    using namespace GenshinHookCpuTiming;
    using Consumer=GenshinLegacy::Family::ShaderCacheMetrics::Consumer;
    unsigned groups=0;
    control.store(0);const auto epoch=monitorEpoch.load();const auto slotsBefore=nextSlot.load();
    BeginFsrHudOwner();
    {
        HookScope root(Hook::hkDraw);assert(!root.Active());ConfirmFsrHudRoot(true);
        int value=123;int& returned=root.Original([&]() -> int& {return value;});assert(&returned==&value);
        root.Original([]{});
    }
    assert(control.load()==0 && monitorEpoch.load()==epoch && nextSlot.load()==slotsBefore && local.depth==0);
    auto reading=fsrHudLocal.Summarize(10000000,1000);assert(reading.known && reading.calls==1 && reading.samples==1);++groups;
    {
        // RAII excludes original even if the forwarded function throws.
        BeginFsrHudOwner();
        try {HookScope root(Hook::hkDraw);ConfirmFsrHudRoot(true);
            root.Original([]{throw std::runtime_error("test-only");});assert(false);
        }catch(const std::runtime_error&){}
        reading=fsrHudLocal.Summarize(10000000,1000);assert(reading.known && reading.calls==1);++groups;
    }
    {
        // An old hook may unwind after a new owner was admitted on this thread.
        // Its old root/native guards must not erase the new owner's payload.
        BeginFsrHudOwner();
        {HookScope old(Hook::hkDraw);ConfirmFsrHudRoot(true);old.Original([]{
            BeginFsrHudOwner();
            HookScope current(Hook::hkDrawIndexed);ConfirmFsrHudRoot(true);current.Original([]{});
        });}
        reading=fsrHudLocal.Summarize(10000000,1000);assert(reading.known && reading.calls==1);++groups;
    }
    {
        // Another thread does not inherit the capture owner's admission.
        std::thread foreign([]{HookScope root(Hook::hkDraw);ConfirmFsrHudRoot(true);
            root.Original([]{});assert(fsrHudLocal.Generation()==0);});foreign.join();
        reading=fsrHudLocal.Summarize(10000000,1000);assert(reading.calls==1);++groups;
    }
    ShaderCacheSnapshot source;
    const auto now=GetTickCount64();assert(now>5000);
    source.values[ShaderCacheSnapshot::BeginMs]=now-1000;source.values[ShaderCacheSnapshot::EndMs]=now;
    source.values[ShaderCacheSnapshot::ThreadId]=GetCurrentThreadId();source.values[ShaderCacheSnapshot::Frames]=60;
    source.values[ShaderCacheSnapshot::MetadataHits]=70;source.values[ShaderCacheSnapshot::MetadataMisses]=30;
    // Deliberately conflicting aggregate/Late counts: HUD must use Current only.
    source.values[ShaderCacheSnapshot::BindingHits]=9999;source.values[ShaderCacheSnapshot::BindingMisses]=0;
    source.detail.hits[static_cast<unsigned>(Consumer::Current)]=9;
    source.detail.misses[static_cast<unsigned>(Consumer::Current)]=1;
    source.detail.misses[static_cast<unsigned>(Consumer::Late)]=9999;
    constexpr DWORD sentinel=0x12345678;
    SetLastError(sentinel);PublishFsrHudWindow(source);assert(GetLastError()==sentinel);
    auto hud=ReadFsrHudSnapshot();assert(hud.windowKnown && hud.frames==60 && hud.hookHost.known);
    assert(hud.shaderMetadata.Percent()==70 && hud.psBindingCurrent.Percent()==90);
    assert(hud.shaderMetadata.Attempts()==100 && hud.psBindingCurrent.Attempts()==10);
    assert(hud.hookHost.Prefix=='~' && control.load()==0 && frequency.load()==0);++groups;
    {
        // A newly published complete cache window can lack host timing samples.
        // Ratios remain available with heavy Debug off; no invented zero latency.
        PublishFsrHudWindow(source);hud=ReadFsrHudSnapshot();
        assert(hud.windowKnown && !hud.hookHost.known && hud.psBindingCurrent.Known());++groups;
    }
    {
        InvalidateFsrHudOwner();assert(!ReadFsrHudSnapshot().windowKnown);
        BeginFsrHudOwner();assert(!ReadFsrHudSnapshot().windowKnown);
        PublishFsrHudWindow(source);assert(ReadFsrHudSnapshot().windowKnown);
        BeginFsrHudOwner();assert(!ReadFsrHudSnapshot().windowKnown);++groups;
    }
    {
        source.detail.hits={};source.detail.misses={};source.values[ShaderCacheSnapshot::MetadataHits]=0;
        source.values[ShaderCacheSnapshot::MetadataMisses]=0;PublishFsrHudWindow(source);hud=ReadFsrHudSnapshot();
        assert(hud.windowKnown && !hud.shaderMetadata.Known() && !hud.psBindingCurrent.Known());
        source.values[ShaderCacheSnapshot::EndMs]=now-3000;source.values[ShaderCacheSnapshot::BeginMs]=now-4000;
        PublishFsrHudWindow(source);assert(!ReadFsrHudSnapshot().windowKnown);++groups;
    }
    {
        // Existing Debug population, sampling phase and Original counters survive.
        BeginFsrHudOwner();control.store(1);
        const auto id=static_cast<unsigned>(Hook::hkDrawIndexed)*PathCount;
        const auto before=local.values[id];
        {HookScope root(Hook::hkDrawIndexed);assert(root.Active());ConfirmFsrHudRoot(true);
            root.Original([]{});}
        const auto after=local.values[id];
        assert(after[Calls]==before[Calls]+1 && after[Samples]==before[Samples]+1);
        assert(after[OriginalCalls]==before[OriginalCalls]+1 && after[OriginalSamples]==before[OriginalSamples]+1);
        assert(after[Nested]==before[Nested] && local.depth==0);
        reading=fsrHudLocal.Summarize(10000000,1000);assert(reading.known && reading.calls==1);
        control.store(0);++groups;
    }
    {
        // Presentation scopes never turn an inclusive frame into a hook total.
        BeginFsrHudOwner();{HookScope present(Hook::Present);ConfirmFsrHudRoot(true);present.Original([]{});}
        assert(!fsrHudLocal.Summarize(10000000,1000).known);++groups;
    }
    std::printf("GenshinFsrHudWiring: %u CPU groups passed\n",groups);
}

// SPDX-License-Identifier: GPL-3.0-or-later
#include <runtime/HookCpuTiming.h>
#include <cassert>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string_view>

int main()
{
    using namespace HookCpuTiming;
    control.store(0);
    const auto epoch = monitorEpoch.load();
    const auto slotsBefore = nextSlot.load();
    int value = 123;
    {
        HookScope disabled(Hook::hkOMSetRenderTargets);
        assert(!disabled.Active());
        int& reference = disabled.Original([&]() -> int& { return value; });
        assert(&reference == &value);
        disabled.Original([] {});
        try { disabled.Original([] { throw std::runtime_error("forwarded"); }); assert(false); }
        catch (const std::runtime_error&) {}
    }
    assert(control.load() == 0 && monitorEpoch.load() == epoch && nextSlot.load() == slotsBefore && local.depth == 0);
    control.store(1);
    const auto id = static_cast<unsigned>(Hook::hkOMSetRenderTargetsAndUAV) * PathCount;
    const auto before = local.values[id];
    {
        HookScope sampled(Hook::hkOMSetRenderTargetsAndUAV);
        assert(sampled.Active());
        int& reference = sampled.Original([&]() -> int& { return value; });
        assert(&reference == &value && !sampled.InOriginal());
    }
    const auto after = local.values[id];
    assert(after[Calls] == before[Calls] + 1 && after[Samples] == before[Samples] + 1);
    assert(after[OriginalCalls] == before[OriginalCalls] + 1 && after[OriginalSamples] == before[OriginalSamples] + 1);
    assert(after[Nested] == before[Nested] && local.depth == 0);
    try {
        HookScope outer(Hook::hkOMSetRenderTargets);
        outer.Original([&] {
            HookScope inner(Hook::hkClearDepthStencilView);
            inner.Original([] { throw std::runtime_error("unwind"); });
        });
        assert(false);
    } catch (const std::runtime_error&) {}
    assert(local.depth == 0);
    const auto workId = HookCount * PathCount + static_cast<unsigned>(Stage::StateLock);
    const auto workBefore = local.values[workId];
    for (unsigned i = 0; i < 65; ++i) ObserveWork(Stage::StateLock, [] {});
    assert(local.values[workId][Calls] == workBefore[Calls] + 65);
    assert(local.values[workId][Samples] == workBefore[Samples] + 2);
    int& reference = ObserveWork(Stage::ProviderEvaluate, [&]() -> int& { return value; });
    assert(&reference == &value);
    auto moved = ObserveWork(Stage::ProviderEvaluate, [] { return std::make_unique<int>(42); });
    assert(*moved == 42);
    try { ObserveWork(Stage::ProviderEvaluate, [] { throw std::runtime_error("work"); }); assert(false); }
    catch (const std::runtime_error&) {}
    assert(std::string_view(StageName(static_cast<unsigned>(Stage::StateLock))) == "StateLock");
    assert(std::string_view(StageName(static_cast<unsigned>(Stage::ProviderEvaluate))) == "ProviderEvaluate");
    control.store(0);
    std::puts("PASS shared hook timing forwarding, identity, exception unwind, counters and stride sampling");
}

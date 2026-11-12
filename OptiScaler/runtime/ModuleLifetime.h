// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "LoaderStartupContext.h"
#include <atomic>

namespace ModuleLifetime
{
inline constinit std::atomic<bool> processExitRequested {false};
inline constinit std::atomic<bool> detaching {false};
inline constinit std::atomic<HMODULE> contextModule {nullptr};
inline constinit std::atomic<LoaderStartupContext::ProbeFunction> contextProbe {nullptr};

// Resolve once during attach, before publishing callbacks or taking product
// locks. Cleanup checks must never acquire the loader lock to resolve APIs.
inline void InitializeContextProbe() noexcept
{
    const auto module = GetModuleHandleW(L"ntdll.dll");
    const auto probe = module ? reinterpret_cast<LoaderStartupContext::ProbeFunction>(
        GetProcAddress(module, "RtlIsThreadWithinLoaderCallout")) : nullptr;
    contextModule.store(module, std::memory_order_relaxed);
    contextProbe.store(probe, std::memory_order_release);
}

// Hooks, exported proxy functions and detached workers are process-lifetime
// registrations. Keep their code mapped until ExitProcess; the project has no
// complete hot-unload protocol that could revoke all of those callbacks.
inline bool PinCallbacks(HMODULE module) noexcept
{
    HMODULE pinned = nullptr;
    if (!module || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(module), &pinned) || pinned != module) return false;
    InitializeContextProbe();
    return true;
}

inline void RequestProcessExit() noexcept { processExitRequested.store(true, std::memory_order_release); }
inline void NotifyDetach() noexcept { detaching.store(true, std::memory_order_release); }

inline LoaderStartupContext::Status CurrentContext() noexcept
{
    const auto probe = contextProbe.load(std::memory_order_acquire);
    return LoaderStartupContext::Probe(contextModule.load(std::memory_order_relaxed), probe);
}

// A scheduling veto only; ordinary cleanup still needs its own recording,
// queue/fence, SDK and callback-lifetime proofs. Unknown context cannot grant it.
inline bool CleanupAllowed() noexcept
{
    return !processExitRequested.load(std::memory_order_acquire) &&
        !detaching.load(std::memory_order_acquire) && CurrentContext() == LoaderStartupContext::Status::Ready;
}
}

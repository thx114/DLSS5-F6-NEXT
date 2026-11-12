// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <Windows.h>
#include <cstdint>
#include <intrin.h>

namespace LoaderStartupContext
{
enum class Status { Ready, LoaderCallout, ProbeUnavailable };
using ProbeFunction = BOOLEAN (NTAPI*)();

// Scheduling only. Unknown loader context forbids startup waiting.
inline Status Probe(HMODULE module, ProbeFunction probe) noexcept
{
    if (!module || !probe) return Status::ProbeUnavailable;
    if (probe()) return Status::LoaderCallout;
    // The RTL flag can miss executable TLS callbacks. Also check the x64 PEB
    // loader section, with bounded reads and an ntdll data/debug backlink proof.
#if defined(_M_X64)
    const auto peb = static_cast<uintptr_t>(__readgsqword(0x60));
    CRITICAL_SECTION* lock = nullptr;
    SIZE_T read = 0;
    ULONG processFlags = 0;
    if (!peb || !ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(peb + 0x50),
                                  &processFlags, sizeof(processFlags), &read) || read != sizeof(processFlags))
        return Status::ProbeUnavailable;
    // ProcessInitializing can remain set in executable TLS callbacks even when
    // the loader critical section has no owner and the RTL flag is clear.
    if (processFlags & 0x2) return Status::LoaderCallout;
    if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(peb + 0x110),
                                  &lock, sizeof(lock), &read) || read != sizeof(lock) || !lock)
        return Status::ProbeUnavailable;
    const auto dataRange = [module](const void* address, size_t size) {
        MEMORY_BASIC_INFORMATION info {};
        if (!address || !size || VirtualQuery(address, &info, sizeof(info)) != sizeof(info) ||
            info.State != MEM_COMMIT || info.Type != MEM_IMAGE || info.AllocationBase != module ||
            (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
            ((info.Protect & 0xff) != PAGE_READWRITE && (info.Protect & 0xff) != PAGE_WRITECOPY))
            return false;
        const auto start = reinterpret_cast<uintptr_t>(address), base = reinterpret_cast<uintptr_t>(info.BaseAddress);
        return start >= base && start - base <= info.RegionSize && size <= info.RegionSize - (start - base);
    };
    CRITICAL_SECTION current {};
    RTL_CRITICAL_SECTION_DEBUG debug {};
    if (!dataRange(lock, sizeof(current)) ||
        !ReadProcessMemory(GetCurrentProcess(), lock, &current, sizeof(current), &read) || read != sizeof(current) ||
        !dataRange(current.DebugInfo, sizeof(debug)) ||
        !ReadProcessMemory(GetCurrentProcess(), current.DebugInfo, &debug, sizeof(debug), &read) || read != sizeof(debug) ||
        debug.CriticalSection != lock || current.RecursionCount < 0 ||
        (current.RecursionCount == 0) != (current.OwningThread == nullptr))
        return Status::ProbeUnavailable;
    return current.OwningThread == reinterpret_cast<HANDLE>(static_cast<uintptr_t>(GetCurrentThreadId()))
        ? Status::LoaderCallout : Status::Ready;
#else
    return Status::ProbeUnavailable;
#endif
}
}

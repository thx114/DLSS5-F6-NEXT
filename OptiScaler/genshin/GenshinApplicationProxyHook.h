// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "GenshinOwnHookProvenance.h"
#include <runtime/F6InputPolicy.h>

// Application method hooks retain Own transaction/detach ownership without
// granting Windows-native query authority to an exact independent proxy.
namespace GenshinApplicationProxyHook {
// This selects an application proxy role, never native code authority. The
// exact loaded core instance must own both this interface table and its entry.
// In particular a system entry that normalizes into ReShade stays enrolled.
inline bool IndependentReShadeMethod(void** table, PVOID entry) noexcept
{
    if (table == nullptr || entry == nullptr)
        return false;

    HMODULE core = nullptr;
    if (!GetModuleHandleExW(0, L"ReShade64.dll", &core))
        return false;
    struct ReleaseModule
    {
        HMODULE module;
        ~ReleaseModule() { FreeLibrary(module); }
    } release {core};

    const auto ownerIsCore = [core](const void* address) noexcept
    {
        HMODULE owner = nullptr;
        return address != nullptr &&
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(address), &owner) && owner == core;
    };
    if (!ownerIsCore(table) || !ownerIsCore(entry))
        return false;

    // Existing public registration role only. Do not call these exports or
    // mistake their presence for a signature, clean-code or unwrap contract.
    for (const auto* name : {"ReShadeRegisterAddon", "ReShadeUnregisterAddon", "ReShadeRegisterEvent", "ReShadeUnregisterEvent"})
        if (!ownerIsCore(reinterpret_cast<const void*>(GetProcAddress(core, name))))
            return false;
    if (DetourCodeFromPointer(entry, nullptr) != entry)
        return false;

    // The installed trampoline must not outlive its proxy code/table. This is
    // an already loaded core reference, not a load or a second core.
    HMODULE pinned = nullptr;
    return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(entry), &pinned) && pinned == core;
}

inline LONG AddMethod(GenshinNativeQuery::Own::Batch& hooks, GenshinNativeQuery::Own::Slot& slot,
    PVOID* original, void** table, unsigned index, PVOID hook,
    const std::source_location& caller = std::source_location::current()) noexcept
{
    const auto entry = table[index];
    if constexpr (!F6InputPolicy::FsrBridgeEnabled)
        return hooks.Add(slot, original, nullptr, entry, hook, nullptr, nullptr, false, caller);
    const bool independent = IndependentReShadeMethod(table, entry);
    const auto result = hooks.Add(slot, original, nullptr, entry, hook, nullptr, nullptr, !independent, caller);
    // Close the classification/staging window. If Detours actually picked a
    // different entry, no native certificate may survive the mismatch. Keep
    // the installed hook's ordinary commit/rollback ownership intact.
    if (result == NO_ERROR && independent && slot.detachReceipt.entry != entry)
        GenshinNativeQuery::Revoke();
    return result;
}

} // namespace GenshinApplicationProxyHook

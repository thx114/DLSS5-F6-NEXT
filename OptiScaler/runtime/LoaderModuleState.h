// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <Windows.h>
#include <winternl.h>
#include <cstddef>
#include <string_view>

namespace LibraryLoading {
// Windows x64 loader ABI, isolated here rather than inferred from an HMODULE
// or PE entry point. See winsiderss/phnt ntldr.h, LDR_DATA_TABLE_ENTRY prefix.
// The native enumerator holds the loader lock while reading this prefix.
struct LoaderEntry {
    LIST_ENTRY load, memory, initialization;
    PVOID base, entryPoint;
    ULONG imageSize;
    UNICODE_STRING fullName, baseName;
    ULONG flags;
};
static_assert(sizeof(void*) == 8 && offsetof(LoaderEntry, flags) == 0x68);
using ModuleVisitor = void (*)(HMODULE, std::wstring_view, void*);
inline bool VisitInitialized(ModuleVisitor visitor, void* context) {
    using Callback = void (NTAPI*)(LoaderEntry*, PVOID, BOOLEAN*);
    using Enumerate = NTSTATUS (NTAPI*)(BOOLEAN, Callback, PVOID);
    const auto ntdll = GetModuleHandleW(L"ntdll.dll");
    const auto enumerate = ntdll ? reinterpret_cast<Enumerate>(GetProcAddress(ntdll, "LdrEnumerateLoadedModules")) : nullptr;
    if (!enumerate || !visitor) return false;
    struct Visit { ModuleVisitor callback; void* context; } visit{visitor, context};
    const auto status = enumerate(FALSE, [](LoaderEntry* entry, PVOID data, BOOLEAN*) {
        constexpr ULONG attached = 1u << 19, failed = 1u << 20, loading = 1u << 12;
        if (!entry || !(entry->flags & attached) || (entry->flags & (failed | loading))) return;
        const auto& visit = *static_cast<Visit*>(data);
        const auto& name = entry->baseName;
        if (name.Buffer && !(name.Length % sizeof(wchar_t)))
            visit.callback(static_cast<HMODULE>(entry->base), {name.Buffer, name.Length / sizeof(wchar_t)}, visit.context);
    }, &visit);
    return status >= 0;
}
inline bool Initialized(HMODULE module) {
    struct Find { HMODULE module; bool found = false; } find{module};
    VisitInitialized([](HMODULE candidate, std::wstring_view, void* data) {
        auto& find = *static_cast<Find*>(data);
        if (candidate == find.module) find.found = true;
    }, &find);
    return find.found;
}
}

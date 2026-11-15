// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <Windows.h>

namespace HoYoShade::CoreLoad
{
// LoadLibrary also succeeds when another loader won the race. Only a first
// image-load notification on this call's thread establishes F6 ownership.
// The notification does not call other DLLs, allocate, or touch ReShade.
class Witness
{
    struct Name { USHORT length, capacity; const wchar_t* buffer; };
    struct Loaded { ULONG flags; const Name* fullName; const Name* baseName; void* base; ULONG size; };
    using Callback = void (NTAPI*)(ULONG, const Loaded*, void*);
    using Register = LONG (NTAPI*)(ULONG, Callback, void*, void**);
    using Unregister = LONG (NTAPI*)(void*);
    inline static thread_local Witness* active = nullptr;
    void* cookie = nullptr;
    void* observed = nullptr;
    Unregister unregister = nullptr;

    static void NTAPI Notify(ULONG reason, const Loaded* data, void* context) noexcept
    {
        if (reason != 1 || active != context || !data || !data->baseName) return;
        constexpr wchar_t expected[] = L"reshade64.dll";
        const auto& name = *data->baseName;
        if (!name.buffer || name.length != (sizeof(expected) - sizeof(wchar_t))) return;
        for (unsigned i = 0; i < sizeof(expected) / sizeof(wchar_t) - 1; ++i) {
            auto c = name.buffer[i];
            if (c >= L'A' && c <= L'Z') c += L'a' - L'A';
            if (c != expected[i]) return;
        }
        active->observed = data->base;
    }
public:
    Witness() noexcept
    {
        const auto ntdll = GetModuleHandleW(L"ntdll.dll");
        if (!ntdll) return;
        const auto subscribe = reinterpret_cast<Register>(GetProcAddress(ntdll, "LdrRegisterDllNotification"));
        unregister = reinterpret_cast<Unregister>(GetProcAddress(ntdll, "LdrUnregisterDllNotification"));
        if (subscribe && unregister && subscribe(0, Notify, this, &cookie) < 0) cookie = nullptr;
    }
    ~Witness() { if (cookie) unregister(cookie); }
    Witness(const Witness&) = delete;
    Witness& operator=(const Witness&) = delete;
    HMODULE Load(const wchar_t* path) noexcept
    {
        observed = nullptr;
        auto* previous = active;
        active = this;
        const auto module = LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        active = previous;
        return module;
    }
    bool FirstLoad(HMODULE module) const noexcept { return cookie && module && observed == module; }
};
}

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <Windows.h>
#include <string>
#include <string_view>
#include <atomic>

namespace LibraryLoading {
inline bool InLoaderCallout() noexcept {
    using Probe = BOOLEAN (NTAPI*)();
    static const auto probe = reinterpret_cast<Probe>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlIsThreadWithinLoaderCallout"));
    return !probe || probe();
}
enum class Layer { Kernel32, KernelBase };
struct InternalLoad final {
    inline static std::atomic<void (*)()> notify {nullptr};
    inline static thread_local InternalLoad* current = nullptr;
    InternalLoad* previous;
    std::wstring_view name;
    HANDLE file;
    DWORD flags;
    bool entered = false;
    InternalLoad(std::wstring_view path, HANDLE handle, DWORD options) noexcept
        : previous(current), name(path), file(handle), flags(options) { current = this; }
    ~InternalLoad() {
        current = previous;
        if (!current) if (const auto signal = notify.load(std::memory_order_acquire)) signal();
    }
    static bool Claim(std::wstring_view path, HANDLE file, DWORD flags) noexcept {
        if (!current || current->entered || current->file != file || current->flags != flags) return false;
        const bool matches = current->name.size() == path.size() &&
            CompareStringOrdinal(current->name.data(), static_cast<int>(path.size()),
                path.data(), static_cast<int>(path.size()), TRUE) == CSTR_EQUAL;
        if (matches) current->entered = true;
        return matches;
    }
};

// Metadata follows the actual Win32 continuation into KernelBase/ntdll. Only
// the same request is bypassed: an independent DLL load from DllMain still has
// its own policy and observer. Win32 and native flag domains are never mixed.
struct Win32Request final {
    inline static thread_local Win32Request* current = nullptr;
    Win32Request* previous;
    std::wstring_view name;
    HANDLE file;
    DWORD flags;
    Layer layer;
    bool callout = InLoaderCallout();
    bool baseEntered = false, nativeEntered = false;
    Win32Request(std::wstring_view path, HANDLE handle, DWORD options, Layer entry) noexcept
        : previous(current), name(path), file(handle), flags(options), layer(entry) { current = this; }
    ~Win32Request() { current = previous; }
    static Win32Request* Matching(std::wstring_view path) noexcept {
        auto* request = current;
        if (request && request->callout == InLoaderCallout() && request->name.size() == path.size() &&
            CompareStringOrdinal(request->name.data(), static_cast<int>(path.size()),
                path.data(), static_cast<int>(path.size()), TRUE) == CSTR_EQUAL) return request;
        return nullptr;
    }
    static bool ClaimWin32(std::wstring_view path, HANDLE handle, DWORD options, Layer entry) noexcept {
        auto* request = Matching(path);
        if (!request || request->baseEntered || request->layer != Layer::Kernel32 || entry != Layer::KernelBase ||
            request->file != handle || request->flags != options) return false;
        request->baseEntered = true; return true;
    }
    static bool ClaimNative(std::wstring_view path) noexcept {
        auto* request = Matching(path);
        if (!request || request->nativeEntered) return false;
        request->nativeEntered = true; return true;
    }
};
inline bool Executable(DWORD flags) noexcept {
    return !(flags & (DONT_RESOLVE_DLL_REFERENCES | LOAD_LIBRARY_AS_DATAFILE |
        LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE | LOAD_LIBRARY_AS_IMAGE_RESOURCE));
}
inline bool CanRedirect(std::wstring_view name, HANDLE file, DWORD flags) noexcept {
    constexpr DWORD search = LOAD_WITH_ALTERED_SEARCH_PATH | LOAD_LIBRARY_SEARCH_APPLICATION_DIR |
        LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_USER_DIRS | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS |
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR;
    if (file || (flags & ~search)) return false;
    constexpr DWORD directorySearch = search & ~LOAD_WITH_ALTERED_SEARCH_PATH;
    if ((flags & LOAD_WITH_ALTERED_SEARCH_PATH) && (flags & directorySearch)) return false;
    const bool absolute = (name.size() >= 3 && name[1] == L':' && (name[2] == L'\\' || name[2] == L'/')) ||
        (name.size() >= 2 && name[0] == L'\\' && name[1] == L'\\');
    return !(flags & (LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_WITH_ALTERED_SEARCH_PATH)) || absolute;
}
inline std::wstring AnsiName(LPCSTR name) {
    if (!name) return {};
    const auto count = MultiByteToWideChar(CP_ACP, 0, name, -1, nullptr, 0);
    if (!count) return {};
    std::wstring result(static_cast<size_t>(count), L'\0');
    if (!MultiByteToWideChar(CP_ACP, 0, name, -1, result.data(), count)) return {};
    result.pop_back();
    return result;
}
}

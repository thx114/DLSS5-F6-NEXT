#pragma once
#include "SysUtils.h"
#include <runtime/LibraryLoadRequest.h>
#include <climits>
#include <optional>

class LibraryLoadHooks
{
  private:
    static void CheckModulesInMemory();
    static bool StartsWithInsensitive(std::wstring_view str, std::wstring_view prefix);
    static bool ObserveNow(HMODULE module, bool nvapiRole);

  public:
    enum class LoadAction { Forward, RedirectPath, AliasModule, Reject };
    struct LoadPlan {
        LoadAction action = LoadAction::Forward;
        std::wstring replacement;
        HMODULE alias = nullptr;
        bool fallbackToOriginal = false;
        HMODULE fallbackAlias = nullptr;
    };
    static LoadPlan Decide(std::wstring_view requested);
    static HMODULE AcquireAlias(std::wstring_view requested, HMODULE module);
    static void ObserveExecutable(HMODULE module, std::wstring_view requested = {});
    static void ProcessPending();
    static void SchedulePending() noexcept;

    template<class Forward>
    static HMODULE LoadWin32(std::wstring_view name, HANDLE file, DWORD flags, Forward&& forward,
                             LibraryLoading::Layer layer = LibraryLoading::Layer::Kernel32) {
        const auto incomingError = GetLastError();
        if (name.empty() || LibraryLoading::Win32Request::ClaimWin32(name, file, flags, layer)) {
            SetLastError(incomingError);
            return forward(nullptr);
        }
        const auto plan = !LibraryLoading::InternalLoad::Claim(name, file, flags) && LibraryLoading::CanRedirect(name, file, flags) ? Decide(name) : LoadPlan{};
        if (plan.action == LoadAction::Reject) { SetLastError(ERROR_MOD_NOT_FOUND); return nullptr; }
        if (plan.action == LoadAction::AliasModule) {
            const auto module = AcquireAlias(name, plan.alias);
            if (module) { ObserveExecutable(module, name); SetLastError(incomingError); }
            return module;
        }
        const auto load = [&](LPCWSTR replacement) {
            LibraryLoading::Win32Request request(replacement ? std::wstring_view(replacement) : name, file, flags, layer);
            SetLastError(incomingError);
            return forward(replacement);
        };
        auto module = load(plan.action == LoadAction::RedirectPath ? plan.replacement.c_str() : nullptr);
        if (!module && plan.fallbackToOriginal) module = load(nullptr);
        const auto error = GetLastError();
        if (!module && plan.fallbackAlias) module = AcquireAlias(name, plan.fallbackAlias);
        if (module && LibraryLoading::Executable(flags)) ObserveExecutable(module, name);
        SetLastError(error);
        return module;
    }

    template<class Forward>
    static NTSTATUS LoadNative(PULONG flags, PUNICODE_STRING name, PHANDLE output, Forward&& forward) {
        if (!output || !name || !name->Buffer || !name->Length || name->Length % sizeof(wchar_t))
            return forward(nullptr);
        const std::wstring_view requested(name->Buffer, name->Length / sizeof(wchar_t));
        if (LibraryLoading::Win32Request::ClaimNative(requested))
            return forward(nullptr);
        const auto incomingError = GetLastError();
        // LdrLoadDll flags are not Win32 search flags. Unknown native modes are
        // forwarded unchanged and cannot grant execution/alias authority.
        const bool executable = !flags || !*flags;
        const auto plan = executable && !LibraryLoading::InternalLoad::Claim(requested, nullptr, 0) ? Decide(requested) : LoadPlan{};
        if (plan.action == LoadAction::Reject) return STATUS_DLL_NOT_FOUND;
        if (plan.action == LoadAction::AliasModule) {
            *output = AcquireAlias(requested, plan.alias);
            if (*output) ObserveExecutable(static_cast<HMODULE>(*output), requested);
            const auto status = *output ? NTSTATUS{0} : STATUS_DLL_NOT_FOUND;
            SetLastError(incomingError); return status;
        }
        UNICODE_STRING replacement{};
        if (plan.action == LoadAction::RedirectPath) {
            if (plan.replacement.size() > (USHRT_MAX / sizeof(wchar_t)) - 1)
                return static_cast<NTSTATUS>(0xC0000106L); // STATUS_NAME_TOO_LONG
            replacement.Buffer = const_cast<PWSTR>(plan.replacement.c_str());
            replacement.Length = static_cast<USHORT>(plan.replacement.size() * sizeof(wchar_t));
            replacement.MaximumLength = replacement.Length + sizeof(wchar_t);
        }
        SetLastError(incomingError);
        auto status = forward(plan.action == LoadAction::RedirectPath ? &replacement : nullptr);
        if (!NT_SUCCESS(status) && plan.fallbackToOriginal) status = forward(nullptr);
        const auto error = GetLastError();
        if (!NT_SUCCESS(status) && plan.fallbackAlias) {
            *output = AcquireAlias(requested, plan.fallbackAlias);
            if (*output) status = 0;
        }
        if (NT_SUCCESS(status) && *output && executable) ObserveExecutable(static_cast<HMODULE>(*output), requested);
        SetLastError(error);
        return status;
    }
    static std::optional<NTSTATUS> FreeLibrary(PVOID library);

    static HMODULE LoadNvApi();
    // static HMODULE LoadFfxapiVk(std::wstring originalPath);
    // static HMODULE LoadFfxapiDx12(std::wstring originalPath);
    // static HMODULE LoadLibxessDx11(std::wstring originalPath);
    // static HMODULE LoadLibxess(std::wstring originalPath);
    static HMODULE LoadNvngxDlss(std::wstring originalPath);

    static bool IsApiSetName(const std::wstring_view& n);
    static bool EndsWithInsensitive(std::wstring_view text, std::wstring_view suffix);
    static bool EndsWithInsensitive(const UNICODE_STRING& text, std::wstring_view suffix);
};

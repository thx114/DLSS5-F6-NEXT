#pragma once
#include "SysUtils.h"
#include <Util.h>
#include <State.h>
#include <Config.h>
#include <DllNames.h>

#include "LibraryLoad_Hooks.h"
#include "Hook_Utils.h"

#include <cwctype>

#pragma intrinsic(_ReturnAddress)

class NtdllHooks
{
  private:
    inline static std::mutex hookMutex;

    inline static NtdllProxy::PFN_RtlGetVersion& o_RtlGetVersion = NtdllProxy::Original_RtlGetVersion();
    inline static NtdllProxy::PFN_NtLoadDll& o_NtLoadDll = NtdllProxy::Original_NtLoadDll();
    inline static NtdllProxy::PFN_LdrLoadDll& o_LdrLoadDll = NtdllProxy::Original_LdrLoadDll();
    inline static NtdllProxy::PFN_LdrUnloadDll& o_LdrUnloadDll = NtdllProxy::Original_LdrUnloadDll();

    static NTSTATUS NTAPI hkRtlGetVersion(PRTL_OSVERSIONINFOW lpVersionInformation)
    {
        if (lpVersionInformation == nullptr)
            return STATUS_INVALID_PARAMETER;

        auto result = o_RtlGetVersion(lpVersionInformation);

        if (lpVersionInformation->dwMajorVersion <= 10 && lpVersionInformation->dwBuildNumber < 21996 &&
            State::Instance().activeFgOutput == FGOutput::FSRFG && State::Instance().isRunningOnLinux &&
            Util::GetCallerModule(_ReturnAddress()) ==
                KernelBaseProxy::GetModuleHandleW_()(L"amd_fidelityfx_framegeneration_dx12.dll"))
        {
            lpVersionInformation->dwMajorVersion = 10;
            lpVersionInformation->dwBuildNumber = 21996;
        }

        return result;
    }

    static NTSTATUS NTAPI hkLdrLoadDll(PWSTR PathToFile, PULONG Flags, PUNICODE_STRING ModuleFileName,
                                       PHANDLE ModuleHandle)
    {
        return LibraryLoadHooks::LoadNative(Flags, ModuleFileName, ModuleHandle, [&](PUNICODE_STRING replacement) {
            return o_LdrLoadDll(PathToFile, Flags, replacement ? replacement : ModuleFileName, ModuleHandle);
        });
    }

    static NTSTATUS NTAPI hkNtLoadDll(PUNICODE_STRING PathToFile, PULONG Flags, PUNICODE_STRING ModuleFileName,
                                       PHANDLE ModuleHandle)
    {
        return LibraryLoadHooks::LoadNative(Flags, ModuleFileName, ModuleHandle, [&](PUNICODE_STRING replacement) {
            return o_NtLoadDll(PathToFile, Flags, replacement ? replacement : ModuleFileName, ModuleHandle);
        });
    }

    static NTSTATUS NTAPI hkLdrUnloadDll(PVOID lpLibrary)
    {
        if (lpLibrary == nullptr)
            return STATUS_INVALID_PARAMETER;

#ifdef _DEBUG
        // LOG_TRACE("{:X}", (size_t) lpLibrary);
#endif

        if (!State::Instance().isShuttingDown)
        {
            auto result = LibraryLoadHooks::FreeLibrary(lpLibrary);

            if (result.has_value())
                return result.value();
        }

        return o_LdrUnloadDll(lpLibrary);
    }

    VALIDATE_MEMBER_HOOK(hkRtlGetVersion, NtdllProxy::PFN_RtlGetVersion)
    VALIDATE_MEMBER_HOOK(hkNtLoadDll, NtdllProxy::PFN_NtLoadDll)
    VALIDATE_MEMBER_HOOK(hkLdrLoadDll, NtdllProxy::PFN_LdrLoadDll)
    VALIDATE_MEMBER_HOOK(hkLdrUnloadDll, NtdllProxy::PFN_LdrUnloadDll)

  public:
    static void Hook()
    {
        std::lock_guard lock(hookMutex);

        LOG_FUNC();

        if (!Config::Instance()->UseNtdllHooks.value_or_default())
            return;

        if (NtdllProxy::Module() == nullptr)
            return;

        if (o_RtlGetVersion == nullptr)
            NtdllProxy::Hook_RtlGetVersion(hkRtlGetVersion);

        // if (o_NtLoadDll == nullptr)
        //     NtdllProxy::Hook_NtLoadDll(hkNtLoadDll);

        if (o_LdrLoadDll == nullptr)
            NtdllProxy::Hook_LdrLoadDll(hkLdrLoadDll);

        if (o_LdrUnloadDll == nullptr)
            NtdllProxy::Hook_LdrUnloadDll(hkLdrUnloadDll);
    }
};

// SPDX-License-Identifier: GPL-3.0-or-later
#include "pch.h"
#include "MfgAsiStartup.h"
#include "ModuleLifetime.h"
#include "PluginStartupGate.h"
#include <Logger.h>
#include <proxies/Ntdll_Proxy.h>
#include <proxies/KernelBase_Proxy.h>
#include <TlHelp32.h>

namespace MfgAsiStartup
{
namespace
{
PluginStartup::Gate gate;
std::filesystem::path selectedPath;
// A plugin can retain hooks even if its optional entry is missing. Never unload it.
HMODULE retainedModule = nullptr;

struct Handle
{
    HANDLE value = INVALID_HANDLE_VALUE;
    ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};

PluginStartup::Status LoadSelected()
{
    using PluginStartup::Status;
    Handle file { CreateFileW(selectedPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr) };
    if (file.value == INVALID_HANDLE_VALUE) return Status::Missing;
    // An existing copy, including renamed copies with the exported owner marker,
    // may already own provider hooks. Do not initialize a second owner.
    Handle snapshot { CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId()) };
    if (snapshot.value == INVALID_HANDLE_VALUE) return Status::InventoryFailed;
    MODULEENTRY32W entry { sizeof(entry) };
    if (!Module32FirstW(snapshot.value, &entry)) return Status::InventoryFailed;
    do
    {
        if (KernelBaseProxy::GetProcAddress_()(entry.hModule, "MfgUnlockCoreLoaded") ||
            _wcsnicmp(entry.szModule, L"mfgunlocker", 11) == 0) return Status::AlreadyLoaded;
    } while (Module32NextW(snapshot.value, &entry));
    if (GetLastError() != ERROR_NO_MORE_FILES) return Status::InventoryFailed;

    retainedModule = NtdllProxy::LoadLibraryExW_Ldr(selectedPath.c_str(), nullptr, 0);
    if (!retainedModule) return Status::LoadFailed;
    const auto initialize = reinterpret_cast<void (*)()>(
        KernelBaseProxy::GetProcAddress_()(retainedModule, "InitializeASI"));
    if (!initialize) return Status::EntryMissing;
    initialize();
    return Status::InitReturned;
}
}

bool OwnsName(std::wstring_view name) noexcept
{
    return name.starts_with(L"mfgunlocker") && name.ends_with(L".asi");
}

void Select(const std::filesystem::path& path)
{
    if (gate.Selected()) return;
    selectedPath = std::filesystem::absolute(path);
    gate.Arm();
}

void Initialize()
{
    gate.Run(GetCurrentThreadId(), [] { return LoadSelected(); });
    if (gate.Selected())
        LOG_INFO("MFG ASI startup: status={} path={} (initialization receipt only; SDK/output unverified)",
                 static_cast<unsigned>(gate.Current()), selectedPath.string());
}

void WorkerUnavailable() { gate.WorkerUnavailable(); }

bool AwaitGraphicsStartup(unsigned timeoutMilliseconds)
{
    if (!gate.Selected()) return true;
    return gate.Await(ModuleLifetime::CurrentContext() == LoaderStartupContext::Status::Ready,
                      GetCurrentThreadId(), timeoutMilliseconds);
}
}

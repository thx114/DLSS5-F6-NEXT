// SPDX-License-Identifier: GPL-3.0-or-later
#include "pch.h"
#include "AsiPluginStartup.h"
#include "ModuleLifetime.h"
#include "PluginStartupGate.h"
#include <Logger.h>
#include <proxies/Ntdll_Proxy.h>
#include <proxies/KernelBase_Proxy.h>
#include <TlHelp32.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <cwctype>
#include <memory>
#include <thread>
#include <vector>
#pragma comment(lib, "bcrypt.lib")

namespace AsiPluginStartup
{
namespace
{
using PluginStartup::Status;
PluginStartup::Gate gate;
std::filesystem::path pluginDirectory;
int lateDelay = 0;
void (*patchSucceeded)() = nullptr;

struct Handle
{
    HANDLE value = INVALID_HANDLE_VALUE;
    ~Handle() { if (value != INVALID_HANDLE_VALUE && value != nullptr) CloseHandle(value); }
};

struct Identity
{
    FILE_ID_INFO file {};
    LONGLONG size = 0;
    std::array<UCHAR, 32> digest {};
};

// Only the ordinary worker and, subsequently, its one late batch touch this
// list. Modules are deliberately retained: optional entry failure cannot undo
// hooks installed by DllMain or by a partially executed InitializeASI.
std::vector<Identity> attempted;

bool ReadIdentity(HANDLE file, Identity& identity)
{
    LARGE_INTEGER size {};
    return GetFileInformationByHandleEx(file, FileIdInfo, &identity.file, sizeof(identity.file)) &&
           GetFileSizeEx(file, &size) && (identity.size = size.QuadPart) >= 0;
}

bool Hash(HANDLE file, std::array<UCHAR, 32>& digest)
{
    struct Algorithm
    {
        BCRYPT_ALG_HANDLE value = nullptr;
        ~Algorithm() { if (value) BCryptCloseAlgorithmProvider(value, 0); }
    } algorithm;
    std::vector<UCHAR> object;
    struct Hasher
    {
        BCRYPT_HASH_HANDLE value = nullptr;
        ~Hasher() { if (value) BCryptDestroyHash(value); }
    } hasher;
    if (BCryptOpenAlgorithmProvider(&algorithm.value, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return false;
    DWORD length = 0, read = 0;
    if (BCryptGetProperty(algorithm.value, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&length),
                          sizeof(length), &read, 0) < 0) return false;
    object.resize(length);
    if (BCryptCreateHash(algorithm.value, &hasher.value, object.data(), length, nullptr, 0, 0) < 0) return false;
    std::array<UCHAR, 64 * 1024> buffer {};
    LARGE_INTEGER start {};
    if (!SetFilePointerEx(file, start, nullptr, FILE_BEGIN)) return false;
    for (;;)
    {
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) return false;
        if (!read) break;
        if (BCryptHashData(hasher.value, buffer.data(), read, 0) < 0) return false;
    }
    return BCryptFinishHash(hasher.value, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0;
}

bool SameFile(const Identity& a, const Identity& b)
{
    return a.file.VolumeSerialNumber == b.file.VolumeSerialNumber &&
           std::equal(std::begin(a.file.FileId.Identifier), std::end(a.file.FileId.Identifier),
                      std::begin(b.file.FileId.Identifier));
}

enum class Inventory { Absent, Present, Failed };
Inventory AlreadyLoaded(const Identity& identity)
{
    Handle snapshot { CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId()) };
    if (snapshot.value == INVALID_HANDLE_VALUE) return Inventory::Failed;
    MODULEENTRY32W entry { sizeof(entry) };
    if (!Module32FirstW(snapshot.value, &entry)) return Inventory::Failed;
    do
    {
        // Compare file identity before hashing, and hash only equal-sized images.
        // No module basename or proprietary ownership export is involved.
        Handle file { CreateFileW(entry.szExePath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                                  nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr) };
        Identity loaded;
        if (file.value == INVALID_HANDLE_VALUE || !ReadIdentity(file.value, loaded)) continue;
        if (SameFile(identity, loaded)) return Inventory::Present;
        if (loaded.size == identity.size && Hash(file.value, loaded.digest) && loaded.digest == identity.digest)
            return Inventory::Present;
    } while (Module32NextW(snapshot.value, &entry));
    return GetLastError() == ERROR_NO_MORE_FILES ? Inventory::Absent : Inventory::Failed;
}

// Keep SEH handling in a frame without C++ destructors. C++ exceptions continue
// to the per-plugin catch below; hardware faults terminate only this attempt.
bool Invoke(HMODULE module, bool& patched)
{
    __try
    {
        const auto initialize = reinterpret_cast<void (*)()>(
            KernelBaseProxy::GetProcAddress_()(module, "InitializeASI"));
        const auto result = reinterpret_cast<bool (*)()>(
            KernelBaseProxy::GetProcAddress_()(module, "PatchResult"));
        if (initialize) initialize();
        if (result) patched = result();
        return true;
    }
    __except (GetExceptionCode() == 0xe06d7363 ? EXCEPTION_CONTINUE_SEARCH : EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

Status LoadPlugin(const std::filesystem::path& path)
{
    // Keep the candidate stable through identity comparison, load and entry call.
    Handle file { CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr) };
    if (file.value == INVALID_HANDLE_VALUE) return Status::Missing;
    Identity identity;
    if (!ReadIdentity(file.value, identity) || !Hash(file.value, identity.digest)) return Status::InventoryFailed;
    for (const auto& previous : attempted)
        if (SameFile(identity, previous) || identity.digest == previous.digest) return Status::AlreadyLoaded;
    const auto existing = AlreadyLoaded(identity);
    if (existing == Inventory::Failed) return Status::InventoryFailed;
    // Reserve before calling third-party code, including reentrant entry calls.
    attempted.push_back(identity);
    if (existing == Inventory::Present) return Status::AlreadyLoaded;
    const auto module = NtdllProxy::LoadLibraryExW_Ldr(path.c_str(), nullptr, 0);
    if (!module) return Status::LoadFailed;
    bool patched = false;
    if (!Invoke(module, patched)) return Status::Failed;
    if (patched && patchSucceeded) patchSucceeded();
    return KernelBaseProxy::GetProcAddress_()(module, "InitializeASI") ? Status::InitReturned : Status::EntryMissing;
}

void LoadBatch(const std::vector<std::filesystem::path>& paths)
{
    for (const auto& path : paths)
    {
        if (ModuleLifetime::processExitRequested.load(std::memory_order_acquire) ||
            ModuleLifetime::detaching.load(std::memory_order_acquire)) break;
        Status status = Status::Failed;
        try { status = LoadPlugin(path); }
        catch (...) { /* Optional plugin failures do not prevent later plugins. */ }
        LOG_INFO(L"ASI startup: status={} path={} (initialization receipt only)",
                 static_cast<unsigned>(status), path.wstring());
    }
}

struct LateBatch
{
    std::vector<std::filesystem::path> paths;
    int delay;
};

DWORD WINAPI LoadLate(LPVOID argument)
{
    std::unique_ptr<LateBatch> batch(static_cast<LateBatch*>(argument));
    try
    {
        std::this_thread::sleep_for(std::chrono::seconds(batch->delay));
        if (ModuleLifetime::CurrentContext() == LoaderStartupContext::Status::Ready) LoadBatch(batch->paths);
    }
    catch (...) { LOG_ERROR("ASI delayed batch failed"); }
    return 0;
}

Status LoadDirectory()
{
    std::vector<std::filesystem::path> ordinary, late;
    std::error_code error;
    std::filesystem::directory_iterator entries(pluginDirectory, error), end;
    if (error) return Status::Missing;
    for (; entries != end; entries.increment(error))
    {
        if (error) break;
        const auto& entry = *entries;
        std::error_code itemError;
        if (!entry.is_regular_file(itemError)) continue;
        auto name = entry.path().filename().wstring();
        std::transform(name.begin(), name.end(), name.begin(), [](wchar_t c) { return std::towlower(c); });
        if (!name.ends_with(L".asi")) continue;
        (name.find(L"-loadlate") == std::wstring::npos ? ordinary : late).push_back(entry.path());
    }
    std::sort(ordinary.begin(), ordinary.end());
    std::sort(late.begin(), late.end());
    LoadBatch(ordinary);
    if (!late.empty())
    {
        // One immutable batch and one timer for the entire scan, never one
        // detached thread per directory entry. Only explicitly late plugins use it.
        auto batch = std::make_unique<LateBatch>(LateBatch { std::move(late), lateDelay });
        const auto thread = CreateThread(nullptr, 0, LoadLate, batch.get(), 0, nullptr);
        if (thread)
        {
            batch.release();
            CloseHandle(thread);
        }
        else LOG_ERROR("ASI delayed worker creation failed: {}", GetLastError());
    }
    return error ? Status::InventoryFailed : Status::Completed;
}
}

void Configure(const std::filesystem::path& directory, int lateDelaySeconds, void (*onPatchSucceeded)())
{
    if (gate.Selected()) return;
    try
    {
        pluginDirectory = std::filesystem::absolute(directory);
        lateDelay = (std::max)(0, lateDelaySeconds);
        patchSucceeded = onPatchSucceeded;
        gate.Arm();
    }
    catch (...)
    {
        gate.Arm();
        gate.WorkerUnavailable();
    }
}

void Initialize()
{
    // Do not consume the pending queue from loader callouts or unknown context.
    if (ModuleLifetime::CurrentContext() != LoaderStartupContext::Status::Ready) return;
    gate.Run(GetCurrentThreadId(), [] { return LoadDirectory(); });
}

void WorkerUnavailable() { gate.WorkerUnavailable(); }

bool AwaitGraphicsStartup(unsigned timeoutMilliseconds)
{
    return gate.Await(ModuleLifetime::CurrentContext() == LoaderStartupContext::Status::Ready,
                      GetCurrentThreadId(), timeoutMilliseconds);
}
}

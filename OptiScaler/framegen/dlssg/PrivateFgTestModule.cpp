#include "pch.h"
#include "PrivateFgTestModule.h"

#ifndef F6_PRIVATE_FG_TEST_MODULE
#define F6_PRIVATE_FG_TEST_MODULE 0
#endif

#if F6_PRIVATE_FG_TEST_MODULE
#include <proxies/Ntdll_Proxy.h>
#include <proxies/KernelBase_Proxy.h>
#include <runtime/LoaderStartupContext.h>
#include <bcrypt.h>
#include <tlhelp32.h>
#include <array>
#include <atomic>
#include <cwctype>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <filesystem>
#include <vector>
#pragma comment(lib, "bcrypt.lib")
#endif

namespace framegen::PrivateFgTestModule
{
#if F6_PRIVATE_FG_TEST_MODULE
namespace
{
std::atomic<Status> status { Status::Pending };
std::atomic<DWORD> initializingThread { 0 };
using LoaderCallout = BOOLEAN (NTAPI*)();
std::atomic<LoaderCallout> loaderCallout { nullptr };
std::atomic<HMODULE> loaderModule { nullptr };
std::mutex startupMutex;
std::condition_variable startupChanged;
constexpr std::array<unsigned char, 32> expectedDigest {
    0x17,0x1A,0xC1,0x8D,0x4D,0x63,0x2D,0x12,0xDE,0xE2,0x2D,0x5C,0xAF,0x20,0x85,0xF3,
    0x01,0xBA,0xCF,0xDB,0x9A,0x46,0x25,0x61,0xD9,0x3D,0xBC,0xBB,0x61,0xD6,0xA4,0xF8
};

struct FileHandle
{
    HANDLE value = INVALID_HANDLE_VALUE;
    ~FileHandle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};

struct Sha256Handle
{
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::vector<unsigned char> object;
    ~Sha256Handle()
    {
        if (hash) BCryptDestroyHash(hash);
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    }
};

bool ValidateFile(HANDLE file)
{
    Sha256Handle sha;
    DWORD objectBytes = 0, returned = 0;
    std::array<unsigned char, 32> digest {};
    bool valid = false;
    if (BCryptOpenAlgorithmProvider(&sha.algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return false;
    if (BCryptGetProperty(sha.algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectBytes),
                          sizeof(objectBytes), &returned, 0) >= 0)
    {
        sha.object.resize(objectBytes);
        if (BCryptCreateHash(sha.algorithm, &sha.hash, sha.object.data(), objectBytes, nullptr, 0, 0) >= 0)
        {
            std::array<unsigned char, 65536> buffer;
            DWORD read = 0;
            for (;;)
            {
                if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) break;
                if (!read)
                {
                    valid = BCryptFinishHash(sha.hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0 &&
                            digest == expectedDigest;
                    break;
                }
                if (BCryptHashData(sha.hash, buffer.data(), read, 0) < 0) break;
            }
        }
    }
    return valid;
}

enum class Inventory { Clear, Conflicting, Failed };

Inventory ExistingModule()
{
    FileHandle snapshot { CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId()) };
    if (snapshot.value == INVALID_HANDLE_VALUE) return Inventory::Failed;
    MODULEENTRY32W entry { sizeof(entry) };
    if (!Module32FirstW(snapshot.value, &entry)) return Inventory::Failed;
    do
    {
        if (OwnsAsiName(entry.szModule) ||
            KernelBaseProxy::GetProcAddress_()(entry.hModule, "MfgUnlockCoreLoaded")) return Inventory::Conflicting;
    } while (Module32NextW(snapshot.value, &entry));
    return GetLastError() == ERROR_NO_MORE_FILES ? Inventory::Clear : Inventory::Failed;
}
}

bool Enabled() { return true; }
Status CurrentStatus() { return status.load(std::memory_order_acquire); }
void Prepare()
{
    const auto module = NtdllProxy::Module();
    loaderModule.store(module, std::memory_order_release);
    loaderCallout.store(reinterpret_cast<LoaderCallout>(KernelBaseProxy::GetProcAddress_()(
        module, "RtlIsThreadWithinLoaderCallout")), std::memory_order_release);
}

bool AwaitGraphicsStartup(unsigned timeoutMilliseconds)
{
    if (!ProviderPending()) return true;
    const auto withinLoader = loaderCallout.load(std::memory_order_acquire);
    if (LoaderStartupContext::Probe(loaderModule.load(std::memory_order_acquire), withinLoader) !=
            LoaderStartupContext::Status::Ready ||
        initializingThread.load(std::memory_order_acquire) == GetCurrentThreadId()) return false;
    std::unique_lock lock(startupMutex);
    return startupChanged.wait_for(lock, std::chrono::milliseconds(timeoutMilliseconds), [] {
        return !ProviderPending();
    });
}
void StartupWorkerUnavailable()
{
    {
        std::lock_guard lock(startupMutex);
        Status expected = Status::Pending;
        if (!status.compare_exchange_strong(expected, Status::WorkerUnavailable, std::memory_order_acq_rel)) return;
    }
    startupChanged.notify_all();
}
bool OwnsAsiName(std::wstring_view name)
{
    constexpr std::wstring_view prefix = L"mfgunlocker";
    if (name.size() < prefix.size() || name.size() < 4) return false;
    for (size_t i = 0; i < prefix.size(); ++i)
        if (std::towlower(name[i]) != prefix[i]) return false;
    return _wcsnicmp(name.data() + name.size() - 4, L".asi", 4) == 0;
}

void Initialize()
{
    Status expected = Status::Pending;
    if (!status.compare_exchange_strong(expected, Status::Initializing, std::memory_order_acq_rel)) return;
    initializingThread.store(GetCurrentThreadId(), std::memory_order_release);
    Status result = Status::ValidationFailed;
    try
    {
        wchar_t modulePath[32768] {};
        HMODULE owner = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              reinterpret_cast<LPCWSTR>(&Initialize), &owner))
        {
            const auto length = GetModuleFileNameW(owner, modulePath, _countof(modulePath));
            if (length && length < _countof(modulePath))
            {
                const auto path = std::filesystem::path(modulePath).parent_path() / L"plugins" / L"MFGUnlocker.asi";
                // Hold the same file against writes/deletion through validation and mapping.
                FileHandle file { CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                              OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr) };
                if (file.value == INVALID_HANDLE_VALUE) result = Status::Missing;
                else if (!ValidateFile(file.value)) result = Status::ValidationFailed;
                else if (const auto inventory = ExistingModule(); inventory != Inventory::Clear)
                    result = inventory == Inventory::Failed ? Status::InventoryFailed : Status::AlreadyLoaded;
                else
                {
                    const auto module = NtdllProxy::LoadLibraryExW_Ldr(path.c_str(), nullptr, 0);
                    if (!module) result = Status::LoadFailed;
                    else
                    {
                        const auto initialize = reinterpret_cast<void (*)()>(
                            KernelBaseProxy::GetProcAddress_()(module, "InitializeASI"));
                        if (!initialize) result = Status::EntryMissing;
                        else
                        {
                            initialize();
                            result = Status::InitReturned;
                        }
                        // A mapped module may own hooks even if its optional entry is absent.
                        // Retain the loader reference for process lifetime.
                    }
                }
            }
        }
    }
    catch (...) { result = Status::ValidationFailed; }
    {
        std::lock_guard lock(startupMutex);
        status.store(result, std::memory_order_release);
    }
    startupChanged.notify_all();
    LOG_INFO("Private FG test module: {}; SDK capability and submission remain authoritative", StatusText());
}
#else
bool Enabled() { return false; }
Status CurrentStatus() { return Status::Disabled; }
bool OwnsAsiName(std::wstring_view) { return false; }
void Prepare() {}
void Initialize() {}
void StartupWorkerUnavailable() {}
bool AwaitGraphicsStartup(unsigned) { return true; }
#endif

bool ProviderPending()
{
    const auto current = CurrentStatus();
    return current == Status::Pending || current == Status::Initializing;
}

const char* StatusText()
{
    switch (CurrentStatus())
    {
    case Status::Disabled: return "disabled in this build";
    case Status::Pending: return "waiting for startup worker";
    case Status::Initializing: return "initializing on startup worker";
    case Status::WorkerUnavailable: return "startup worker unavailable";
    case Status::Missing: return "test file unavailable";
    case Status::ValidationFailed: return "test file validation failed";
    case Status::InventoryFailed: return "module inventory incomplete";
    case Status::AlreadyLoaded: return "another module instance is already loaded";
    case Status::LoadFailed: return "module mapping failed";
    case Status::EntryMissing: return "initialization entry absent";
    case Status::InitReturned: return "initialization call returned; runtime output unverified";
    }
    return "unknown";
}
}

#pragma once
#include "stubs/pch.h"
#include <bcrypt.h>
#include <tlhelp32.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <intrin.h>
#include <string>
#include <vector>

namespace PrivateFgFixture
{
// Synthetic metadata supplied to the real LoaderStartupContext::Probe. The
// intrinsic still reads this thread's PEB pointer; its fields are never
// dereferenced, and every subsequent query/read is intercepted and bounded.
enum class LoaderProof
{
    Ready, WriteCopy, OtherThreadOwner, CurrentThreadOwner,
    OtherProcessFlags, ProcessInitializing, ProcessFlagsReadFailure, ProcessFlagsShortRead,
    PebReadFailure, PebShortRead, NullLock,
    LockQueryFailure, LockUncommitted, LockPrivate, LockWrongModule,
    LockReadOnly, LockExecutable, LockGuard, LockNoAccess, LockShortRange, LockOutsideRange,
    LockReadFailure, LockShortRead,
    DebugQueryFailure, DebugWrongModule, DebugShortRange, DebugReadFailure, DebugShortRead,
    BadBacklink, NegativeRecursion, OwnerWithoutRecursion, RecursionWithoutOwner
};

extern thread_local LoaderProof loaderProof;

struct State
{
    std::filesystem::path ownerPath;
    std::vector<std::wstring> modules { L"fixture.exe", L"kernel32.dll" };
    size_t nextModule = 0;
    bool forceDigest = false, failHash = false, failLoad = false, missingEntry = false;
    bool marker = false, failSnapshot = false, failFirst = false, failNext = false, blockEntry = false;
    bool rtlAvailable = true, awaitInsideEntry = false;
    HANDLE entryEntered = nullptr, releaseEntry = nullptr;
    HANDLE loaderProbed = nullptr;
    std::atomic_uint fileNameCalls { 0 }, snapshotCalls { 0 }, finishHashCalls { 0 };
    std::atomic_uint fileOpenCalls { 0 }, fileReadCalls { 0 };
    std::atomic_uint firstModuleCalls { 0 }, nextModuleCalls { 0 };
    std::atomic_uint loadCalls { 0 }, resolveCalls { 0 }, entryLookups { 0 }, initCalls { 0 };
    std::atomic_uint ntdllModuleCalls { 0 }, rtlLookups { 0 }, rtlCalls { 0 };
    std::atomic_uint memoryReadCalls { 0 }, memoryQueryCalls { 0 };
    std::atomic_uint processFlagsReadCalls { 0 };
    std::atomic_uint pebReadCalls { 0 }, lockReadCalls { 0 }, debugReadCalls { 0 };
    std::atomic_bool badMemoryArguments { false };
    std::atomic_bool workerAwaitCalled { false }, workerAwaitResult { true };
    std::atomic_bool workerAwaitPending { false };
    std::atomic<ULONGLONG> workerAwaitElapsed { 0 };
    std::atomic_bool badLoaderArguments { false }, entryWaitFailed { false };
    std::atomic_bool writeWasBlocked { false }, deleteWasBlocked { false };
    std::array<unsigned char, 32> realDigest {};
};

extern State state;
void Check(bool condition, const char* message);
DWORD WINAPI ModuleFileName(HMODULE module, LPWSTR output, DWORD capacity);
HANDLE WINAPI OpenFile(LPCWSTR path, DWORD access, DWORD sharing, LPSECURITY_ATTRIBUTES security,
                      DWORD creation, DWORD flags, HANDLE templateFile);
BOOL WINAPI ReadFile(HANDLE file, LPVOID buffer, DWORD bytes, LPDWORD read, LPOVERLAPPED overlapped);
HANDLE WINAPI Snapshot(DWORD flags, DWORD process);
BOOL WINAPI FirstModule(HANDLE snapshot, LPMODULEENTRY32W entry);
BOOL WINAPI NextModule(HANDLE snapshot, LPMODULEENTRY32W entry);
NTSTATUS WINAPI FinishHash(BCRYPT_HASH_HANDLE hash, PUCHAR output, ULONG length, ULONG flags);
BOOL WINAPI ReadMemory(HANDLE process, LPCVOID address, LPVOID output, SIZE_T size, SIZE_T* read);
SIZE_T WINAPI QueryMemory(LPCVOID address, PMEMORY_BASIC_INFORMATION output, SIZE_T size);
int Run(int argc, wchar_t** argv);
}

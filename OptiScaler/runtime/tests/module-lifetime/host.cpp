#include <runtime/ModuleLifetime.h>
#include "Shared.h"
#include <cstdio>
#include <string>

namespace
{
unsigned checks = 0, failures = 0;
void Check(bool okay, const char* text)
{
    ++checks;
    if (!okay) { ++failures; std::printf("FAIL %s (%lu)\n", text, GetLastError()); }
}
using SetSharedFn = void (*)(Shared*);
using CleanupFn = bool (*)();
void Bind(HMODULE module, Shared* shared)
{
    const auto set = reinterpret_cast<SetSharedFn>(GetProcAddress(module, "SetShared"));
    if (set) set(shared);
    Check(set != nullptr, "test library exports its shared result binding");
}
}
int wmain(int argc, wchar_t** argv)
{
    Check(!ModuleLifetime::CleanupAllowed(), "uninitialized context never grants cleanup");
    ModuleLifetime::InitializeContextProbe();
    if (argc == 5 && std::wstring(argv[1]) == L"--exit-child")
    {
        const auto mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, argv[3]);
        auto* shared = mapping ? static_cast<Shared*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared))) : nullptr;
        const auto module = LoadLibraryW(argv[2]);
        if (!module || !shared) return 90;
        Bind(module, shared);
        const auto cleanup = reinterpret_cast<CleanupFn>(GetProcAddress(module, "Cleanup"));
        if (!cleanup || !cleanup()) return 91;
        ExitProcess(0); // Actual loader termination callback, not a simulated flag.
    }
    if (argc != 3) return 92;
    Check(ModuleLifetime::CleanupAllowed(), "ordinary host cleanup is admitted");
    {
        Shared shared;
        const auto module = LoadLibraryW(argv[2]); // Unpinned control really detaches on FreeLibrary.
        Check(module != nullptr, "load explicit-unload context control");
        if (!module) return 93;
        Bind(module, &shared);
        const auto cleanup = reinterpret_cast<CleanupFn>(GetProcAddress(module, "Cleanup"));
        Check(cleanup && cleanup(), "ordinary cleanup body runs once");
        Check(FreeLibrary(module) != FALSE, "control explicit FreeLibrary completes");
        Check(shared.detaches == 1 && shared.cleanupCalls == 1 && !shared.loaderCleanupCalls,
              "real explicit detach cannot enter cleanup body");
    }
    // Keep shared state mapped through the pinned DLL's process detach.
    auto* pinnedShared = static_cast<Shared*>(VirtualAlloc(nullptr, sizeof(Shared), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    const auto module = LoadLibraryW(argv[1]);
    Check(module && pinnedShared, "load process-lifetime callback library");
    if (!module || !pinnedShared) return 94;
    Bind(module, pinnedShared);
    const auto worker = reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(module, "Worker"));
    const auto event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    const auto thread = worker && event ? CreateThread(nullptr, 0, worker, event, 0, nullptr) : nullptr;
    Check(thread != nullptr, "start callback before releasing host reference");
    Check(FreeLibrary(module) != FALSE, "release host reference to pinned callback DLL");
    HMODULE observed = nullptr;
    Check(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
        reinterpret_cast<LPCWSTR>(worker), &observed) && observed == module && pinnedShared->detaches == 0,
        "code remains mapped with callback in flight");
    if (event) SetEvent(event);
    if (thread)
    {
        const auto wait = WaitForSingleObject(thread, 3000);
        DWORD result = 0;
        Check(wait == WAIT_OBJECT_0 && GetExitCodeThread(thread, &result) && result == 17 && pinnedShared->workerCalls == 1,
              "in-flight callback completes after FreeLibrary");
        CloseHandle(thread);
    }
    if (event) CloseHandle(event);
    const auto cleanup = reinterpret_cast<CleanupFn>(GetProcAddress(module, "Cleanup"));
    const auto beginExit = reinterpret_cast<void (*)()>(GetProcAddress(module, "BeginExit"));
    Check(cleanup && cleanup(), "cleanup outside loader remains usable after host release");
    if (beginExit) beginExit();
    Check(beginExit && cleanup && !cleanup(), "process-exit request closes cleanup admission before detach");

    for (int variant = 1; variant <= 2; ++variant)
    {
        const auto name = L"Local\\F6ModuleLifetime-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(variant);
        const auto mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Shared), name.c_str());
        auto* shared = mapping ? static_cast<Shared*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared))) : nullptr;
        if (!shared) return 95;
        std::wstring command = L"\"" + std::wstring(argv[0]) + L"\" --exit-child \"" + argv[variant] + L"\" \"" + name + L"\" end";
        STARTUPINFOW startup {}; startup.cb = sizeof(startup);
        PROCESS_INFORMATION process {};
        const bool launched = CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                                             nullptr, nullptr, &startup, &process) != FALSE;
        Check(launched, "launch isolated process-exit fixture");
        if (launched)
        {
            const auto wait = WaitForSingleObject(process.hProcess, 5000);
            DWORD result = 0;
            Check(wait == WAIT_OBJECT_0 && GetExitCodeProcess(process.hProcess, &result) && result == 0,
                  "isolated process exits without loader wait");
            if (wait != WAIT_OBJECT_0) TerminateProcess(process.hProcess, 96); // Only our own disposable fixture.
            Check(shared->detaches == 1 && shared->cleanupCalls == 1 && !shared->loaderCleanupCalls,
                  "real process detach invokes zero cleanup bodies, pinned and control DLLs");
            CloseHandle(process.hThread); CloseHandle(process.hProcess);
        }
        UnmapViewOfFile(shared); CloseHandle(mapping);
    }
    std::printf("Module lifetime: %u checks, %u failures; actual Windows FreeLibrary/callback/process exit, no SDK/game\n", checks, failures);
    return failures ? 1 : 0;
}

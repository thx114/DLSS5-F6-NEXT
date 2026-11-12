#include <runtime/ModuleLifetime.h>
#include "Shared.h"

static Shared* shared = nullptr;
extern "C" __declspec(dllexport) void SetShared(Shared* value) { shared = value; }
extern "C" __declspec(dllexport) bool Cleanup()
{
    if (!ModuleLifetime::CleanupAllowed()) return false;
    if (shared) InterlockedIncrement(&shared->cleanupCalls);
    return true;
}
extern "C" __declspec(dllexport) void BeginExit() { ModuleLifetime::RequestProcessExit(); }
extern "C" __declspec(dllexport) DWORD WINAPI Worker(void* event)
{
    WaitForSingleObject(static_cast<HANDLE>(event), 2000);
    if (shared) InterlockedIncrement(&shared->workerCalls);
    return 17;
}
BOOL APIENTRY DllMain(HMODULE module, DWORD reason, void*)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
#if !defined(UNPINNED_CONTROL)
        if (!ModuleLifetime::PinCallbacks(module)) return FALSE;
#else
        (void)module;
        ModuleLifetime::InitializeContextProbe();
#endif
    }
    if (reason == DLL_PROCESS_DETACH)
    {
        // Test the actual Windows callback before publishing our own detach
        // flag: the runtime context probe must independently veto cleanup.
        if (Cleanup() && shared) InterlockedIncrement(&shared->loaderCleanupCalls);
        ModuleLifetime::NotifyDetach();
        if (shared) InterlockedIncrement(&shared->detaches);
    }
    return TRUE;
}

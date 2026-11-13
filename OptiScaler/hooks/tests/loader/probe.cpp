#include <Windows.h>
extern "C" __declspec(dllimport) int DependencyIdentity();
extern "C" __declspec(dllexport) int Identity() { return IDENTITY + DependencyIdentity(); }
BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        SetEnvironmentVariableW(L"F6_LOADER_PROBE_EXECUTED", L"1");
        const auto callback = reinterpret_cast<void (*)(HMODULE)>(GetProcAddress(GetModuleHandleW(nullptr), "OnLoaderCallout"));
        if (callback) callback(module);
    }
    return TRUE;
}

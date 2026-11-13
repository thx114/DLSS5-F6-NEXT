#include <Windows.h>
extern "C" __declspec(dllexport) int DependencyIdentity() { return IDENTITY; }
BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }

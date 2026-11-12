#pragma once
#include <windows.h>

namespace PrivateFgFixture
{
HMODULE MockLoadLibrary(LPCWSTR path, HANDLE file, DWORD flags);
HMODULE MockNtdllModule();
}

class NtdllProxy
{
  public:
    static HMODULE Module() { return PrivateFgFixture::MockNtdllModule(); }
    static HMODULE LoadLibraryExW_Ldr(LPCWSTR path, HANDLE file, DWORD flags)
    {
        return PrivateFgFixture::MockLoadLibrary(path, file, flags);
    }
};

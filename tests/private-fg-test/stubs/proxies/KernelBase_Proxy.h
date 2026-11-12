#pragma once
#include <windows.h>

namespace PrivateFgFixture
{
FARPROC WINAPI MockGetProcAddress(HMODULE module, LPCSTR name);
}

class KernelBaseProxy
{
  public:
    using PFN_GetProcAddress = FARPROC (WINAPI*)(HMODULE, LPCSTR);
    static PFN_GetProcAddress GetProcAddress_() { return &PrivateFgFixture::MockGetProcAddress; }
};

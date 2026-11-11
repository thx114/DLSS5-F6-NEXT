#pragma once
#include "SysUtils.h"

class D3D11Hooks
{
  public:
    static void Hook(HMODULE dx11Module);
    static void HookToDevice(ID3D11Device* InDevice);
    // Existing initializer/Present owners, under GenericDepth's install serialization.
    // No device/native setup; returns the existing creation batch's actual result.
    static LONG InstallGimiCreationHooks(ID3D11Device* device) noexcept;
    static void Unhook();
};

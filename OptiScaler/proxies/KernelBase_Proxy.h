#pragma once

#include "SysUtils.h"

#include <detours/detours.h>
#include <genshin/GenshinOwnHookProvenance.h>

class KernelBaseProxy
{
  public:
    typedef decltype(&FreeLibrary) PFN_FreeLibrary;
    typedef decltype(&LoadLibraryA) PFN_LoadLibraryA;
    typedef decltype(&LoadLibraryW) PFN_LoadLibraryW;
    typedef decltype(&LoadLibraryExA) PFN_LoadLibraryExA;
    typedef decltype(&LoadLibraryExW) PFN_LoadLibraryExW;
    typedef decltype(&GetProcAddress) PFN_GetProcAddress;
    typedef decltype(&GetModuleHandleA) PFN_GetModuleHandleA;
    typedef decltype(&GetModuleHandleW) PFN_GetModuleHandleW;
    typedef decltype(&GetModuleHandleExA) PFN_GetModuleHandleExA;
    typedef decltype(&GetModuleHandleExW) PFN_GetModuleHandleExW;

    static void Init()
    {
        if (_dll != nullptr)
            return;

        _dll = GetModuleHandle(L"kernelbase.dll");

        if (_dll == nullptr)
            _dll = LoadLibrary(L"kernelbase.dll");

        if (_dll == nullptr)
            return;

        _FreeLibrary = (PFN_FreeLibrary) (GetProcAddress(_dll, "FreeLibrary"));
        _LoadLibraryA = (PFN_LoadLibraryA) (GetProcAddress(_dll, "LoadLibraryA"));
        _LoadLibraryW = (PFN_LoadLibraryW) (GetProcAddress(_dll, "LoadLibraryW"));
        _LoadLibraryExA = (PFN_LoadLibraryExA) (GetProcAddress(_dll, "LoadLibraryExA"));
        _LoadLibraryExW = (PFN_LoadLibraryExW) (GetProcAddress(_dll, "LoadLibraryExW"));
        _GetModuleHandleA = (PFN_GetModuleHandleA) (GetProcAddress(_dll, "GetModuleHandleA"));
        _GetModuleHandleW = (PFN_GetModuleHandleW) (GetProcAddress(_dll, "GetModuleHandleW"));
        _GetModuleHandleExA = (PFN_GetModuleHandleExA) (GetProcAddress(_dll, "GetModuleHandleExA"));
        _GetModuleHandleExW = (PFN_GetModuleHandleExW) (GetProcAddress(_dll, "GetModuleHandleExW"));
        _GetProcAddress = (PFN_GetProcAddress) (GetProcAddress(_dll, "GetProcAddress"));
    }

    static HMODULE Module() { return _dll; }

    static PFN_FreeLibrary FreeLibrary_() { return _FreeLibrary; }
    static PFN_LoadLibraryA LoadLibraryA_() { return _LoadLibraryA; }
    static PFN_LoadLibraryW LoadLibraryW_() { return _LoadLibraryW; }
    static PFN_LoadLibraryExA LoadLibraryExA_() { return _LoadLibraryExA; }
    static PFN_LoadLibraryExW LoadLibraryExW_() { return _LoadLibraryExW; }
    static PFN_GetProcAddress GetProcAddress_() { return _GetProcAddress; }
    static PFN_GetModuleHandleA GetModuleHandleA_() { return _GetModuleHandleA; }
    static PFN_GetModuleHandleW GetModuleHandleW_() { return _GetModuleHandleW; }
    static PFN_GetModuleHandleExA GetModuleHandleExA_() { return _GetModuleHandleExA; }
    static PFN_GetModuleHandleExW GetModuleHandleExW_() { return _GetModuleHandleExW; }

    static PFN_FreeLibrary FreeLibrary_Hooked() { return (PFN_FreeLibrary) GetProcAddress_()(_dll, "FreeLibrary"); }
    static PFN_LoadLibraryA LoadLibraryA_Hooked() { return (PFN_LoadLibraryA) _GetProcAddress(_dll, "LoadLibraryA"); }
    static PFN_LoadLibraryW LoadLibraryW_Hooked() { return (PFN_LoadLibraryW) _GetProcAddress(_dll, "LoadLibraryW"); }
    static PFN_LoadLibraryExA LoadLibraryExA_Hooked()
    {
        return (PFN_LoadLibraryExA) _GetProcAddress(_dll, "LoadLibraryExA");
    }
    static PFN_LoadLibraryExW LoadLibraryExW_Hooked()
    {
        return (PFN_LoadLibraryExW) _GetProcAddress(_dll, "LoadLibraryExW");
    }
    static PFN_GetProcAddress GetProcAddress_Hooked()
    {
        return (PFN_GetProcAddress) _GetProcAddress(_dll, "GetProcAddress");
    }
    static PFN_GetModuleHandleA GetModuleHandleA_Hooked()
    {
        return (PFN_GetModuleHandleA) KernelBaseProxy::GetProcAddress_()(_dll, "GetModuleHandleA");
    }
    static PFN_GetModuleHandleW GetModuleHandleW_Hooked()
    {
        return (PFN_GetModuleHandleW) KernelBaseProxy::GetProcAddress_()(_dll, "GetModuleHandleW");
    }
    static PFN_GetModuleHandleExA GetModuleHandleExA_Hooked()
    {
        return (PFN_GetModuleHandleExA) KernelBaseProxy::GetProcAddress_()(_dll, "GetModuleHandleExA");
    }
    static PFN_GetModuleHandleExW GetModuleHandleExW_Hooked()
    {
        return (PFN_GetModuleHandleExW) KernelBaseProxy::GetProcAddress_()(_dll, "GetModuleHandleExW");
    }

    static PFN_FreeLibrary& Original_FreeLibrary() { return GenshinNativeQuery::Own::Original<PFN_FreeLibrary>(_own_FreeLibrary); }
    static PFN_FreeLibrary Hook_FreeLibrary(PVOID method)
    {
        if (_own_FreeLibrary.live) return Original_FreeLibrary();
        const auto result = GenshinNativeQuery::Own::Install(_own_FreeLibrary, &(PVOID&) _FreeLibrary,
            (PVOID) FreeLibrary_Hooked(), method, _dll, "FreeLibrary");
        if (result != NO_ERROR) { LOG_ERROR("Failed to hook: {:X}", result); return nullptr; }
        return Original_FreeLibrary();
    }

    static PFN_LoadLibraryA& Original_LoadLibraryA() { return GenshinNativeQuery::Own::Original<PFN_LoadLibraryA>(_own_LoadLibraryA); }
    static PFN_LoadLibraryA Hook_LoadLibraryA(PVOID method)
    {
        if (_own_LoadLibraryA.live) return Original_LoadLibraryA();
        const auto result = GenshinNativeQuery::Own::Install(_own_LoadLibraryA, &(PVOID&) _LoadLibraryA,
            (PVOID) LoadLibraryA_Hooked(), method, _dll, "LoadLibraryA");
        if (result != NO_ERROR) { LOG_ERROR("Failed to hook: {:X}", result); return nullptr; }
        return Original_LoadLibraryA();
    }

    static PFN_LoadLibraryW& Original_LoadLibraryW() { return GenshinNativeQuery::Own::Original<PFN_LoadLibraryW>(_own_LoadLibraryW); }
    static PFN_LoadLibraryW Hook_LoadLibraryW(PVOID method)
    {
        if (_own_LoadLibraryW.live) return Original_LoadLibraryW();
        const auto result = GenshinNativeQuery::Own::Install(_own_LoadLibraryW, &(PVOID&) _LoadLibraryW,
            (PVOID) LoadLibraryW_Hooked(), method, _dll, "LoadLibraryW");
        if (result != NO_ERROR) { LOG_ERROR("Failed to hook: {:X}", result); return nullptr; }
        return Original_LoadLibraryW();
    }

    static PFN_LoadLibraryExA& Original_LoadLibraryExA() { return GenshinNativeQuery::Own::Original<PFN_LoadLibraryExA>(_own_LoadLibraryExA); }
    static PFN_LoadLibraryExA Hook_LoadLibraryExA(PVOID method)
    {
        if (_own_LoadLibraryExA.live) return Original_LoadLibraryExA();
        const auto result = GenshinNativeQuery::Own::Install(_own_LoadLibraryExA, &(PVOID&) _LoadLibraryExA,
            (PVOID) LoadLibraryExA_Hooked(), method, _dll, "LoadLibraryExA");
        if (result != NO_ERROR) { LOG_ERROR("Failed to hook: {:X}", result); return nullptr; }
        return Original_LoadLibraryExA();
    }

    static PFN_LoadLibraryExW& Original_LoadLibraryExW() { return GenshinNativeQuery::Own::Original<PFN_LoadLibraryExW>(_own_LoadLibraryExW); }
    static PFN_LoadLibraryExW Hook_LoadLibraryExW(PVOID method)
    {
        if (_own_LoadLibraryExW.live) return Original_LoadLibraryExW();
        const auto result = GenshinNativeQuery::Own::Install(_own_LoadLibraryExW, &(PVOID&) _LoadLibraryExW,
            (PVOID) LoadLibraryExW_Hooked(), method, _dll, "LoadLibraryExW");
        if (result != NO_ERROR) { LOG_ERROR("Failed to hook: {:X}", result); return nullptr; }
        return Original_LoadLibraryExW();
    }

    static PFN_GetProcAddress& Original_GetProcAddress() { return GenshinNativeQuery::Own::Original<PFN_GetProcAddress>(_own_GetProcAddress); }
    static PFN_GetProcAddress Hook_GetProcAddress(PVOID method)
    {
        if (_own_GetProcAddress.live) return Original_GetProcAddress();
        const auto result = GenshinNativeQuery::Own::Install(_own_GetProcAddress, &(PVOID&) _GetProcAddress,
            (PVOID) GetProcAddress_Hooked(), method, _dll, "GetProcAddress");
        if (result != NO_ERROR) { LOG_ERROR("Failed to hook: {:X}", result); return nullptr; }
        return Original_GetProcAddress();
    }

    static PFN_GetModuleHandleA& Original_GetModuleHandleA() { return GenshinNativeQuery::Own::Original<PFN_GetModuleHandleA>(_own_GetModuleHandleA); }
    static PFN_GetModuleHandleA Hook_GetModuleHandleA(PVOID method)
    {
        if (_own_GetModuleHandleA.live) return Original_GetModuleHandleA();
        const auto result = GenshinNativeQuery::Own::Install(_own_GetModuleHandleA, &(PVOID&) _GetModuleHandleA,
            (PVOID) GetModuleHandleA_Hooked(), method, _dll, "GetModuleHandleA");
        if (result != NO_ERROR) { LOG_ERROR("Failed to hook: {:X}", result); return nullptr; }
        return Original_GetModuleHandleA();
    }

    static PFN_GetModuleHandleW& Original_GetModuleHandleW() { return GenshinNativeQuery::Own::Original<PFN_GetModuleHandleW>(_own_GetModuleHandleW); }
    static PFN_GetModuleHandleW Hook_GetModuleHandleW(PVOID method)
    {
        if (_own_GetModuleHandleW.live) return Original_GetModuleHandleW();
        const auto result = GenshinNativeQuery::Own::Install(_own_GetModuleHandleW, &(PVOID&) _GetModuleHandleW,
            (PVOID) GetModuleHandleW_Hooked(), method, _dll, "GetModuleHandleW");
        if (result != NO_ERROR) { LOG_ERROR("Failed to hook: {:X}", result); return nullptr; }
        return Original_GetModuleHandleW();
    }

    static PFN_GetModuleHandleExA& Original_GetModuleHandleExA() { return GenshinNativeQuery::Own::Original<PFN_GetModuleHandleExA>(_own_GetModuleHandleExA); }
    static PFN_GetModuleHandleExA Hook_GetModuleHandleExA(PVOID method)
    {
        if (_own_GetModuleHandleExA.live) return Original_GetModuleHandleExA();
        const auto result = GenshinNativeQuery::Own::Install(_own_GetModuleHandleExA, &(PVOID&) _GetModuleHandleExA,
            (PVOID) GetModuleHandleExA_Hooked(), method, _dll, "GetModuleHandleExA");
        if (result != NO_ERROR) { LOG_ERROR("Failed to hook: {:X}", result); return nullptr; }
        return Original_GetModuleHandleExA();
    }

    static PFN_GetModuleHandleExW& Original_GetModuleHandleExW() { return GenshinNativeQuery::Own::Original<PFN_GetModuleHandleExW>(_own_GetModuleHandleExW); }
    static PFN_GetModuleHandleExW Hook_GetModuleHandleExW(PVOID method)
    {
        if (_own_GetModuleHandleExW.live) return Original_GetModuleHandleExW();
        const auto result = GenshinNativeQuery::Own::Install(_own_GetModuleHandleExW, &(PVOID&) _GetModuleHandleExW,
            (PVOID) GetModuleHandleExW_Hooked(), method, _dll, "GetModuleHandleExW");
        if (result != NO_ERROR) { LOG_ERROR("Failed to hook: {:X}", result); return nullptr; }
        return Original_GetModuleHandleExW();
    }

  private:
    inline static GenshinNativeQuery::Own::Slot _own_FreeLibrary;
    inline static GenshinNativeQuery::Own::Slot _own_LoadLibraryA;
    inline static GenshinNativeQuery::Own::Slot _own_LoadLibraryW;
    inline static GenshinNativeQuery::Own::Slot _own_LoadLibraryExA;
    inline static GenshinNativeQuery::Own::Slot _own_LoadLibraryExW;
    inline static GenshinNativeQuery::Own::Slot _own_GetProcAddress;
    inline static GenshinNativeQuery::Own::Slot _own_GetModuleHandleA;
    inline static GenshinNativeQuery::Own::Slot _own_GetModuleHandleW;
    inline static GenshinNativeQuery::Own::Slot _own_GetModuleHandleExA;
    inline static GenshinNativeQuery::Own::Slot _own_GetModuleHandleExW;
    inline static HMODULE _dll = nullptr;

    inline static PFN_FreeLibrary _FreeLibrary = nullptr;
    inline static PFN_LoadLibraryA _LoadLibraryA = nullptr;
    inline static PFN_LoadLibraryW _LoadLibraryW = nullptr;
    inline static PFN_LoadLibraryExA _LoadLibraryExA = nullptr;
    inline static PFN_LoadLibraryExW _LoadLibraryExW = nullptr;
    inline static PFN_GetProcAddress _GetProcAddress = nullptr;
    inline static PFN_GetModuleHandleA _GetModuleHandleA = nullptr;
    inline static PFN_GetModuleHandleW _GetModuleHandleW = nullptr;
    inline static PFN_GetModuleHandleExA _GetModuleHandleExA = nullptr;
    inline static PFN_GetModuleHandleExW _GetModuleHandleExW = nullptr;
};

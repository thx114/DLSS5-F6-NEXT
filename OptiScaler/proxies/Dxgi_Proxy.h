#pragma once

#include "SysUtils.h"

#include <State.h>

#include <proxies/Ntdll_Proxy.h>
#include <proxies/KernelBase_Proxy.h>

#include <detours/detours.h>
#include <genshin/GenshinOwnHookProvenance.h>

#include <dxgi1_6.h>

class DxgiProxy
{
  public:
    typedef HRESULT (*PFN_CreateDxgiFactory)(REFIID riid, IDXGIFactory** ppFactory);
    typedef HRESULT (*PFN_CreateDxgiFactory1)(REFIID riid, IDXGIFactory1** ppFactory);
    typedef HRESULT (*PFN_CreateDxgiFactory2)(UINT Flags, REFIID riid, IDXGIFactory2** ppFactory);
    typedef HRESULT (*PFN_DeclareAdepterRemovalSupport)();
    typedef HRESULT (*PFN_GetDebugInterface)(UINT Flags, REFIID riid, void** ppDebug);

    static void Init(HMODULE module = nullptr)
    {
        if (_dll != nullptr)
            return;

        if (module == nullptr)
        {
            _dll = GetModuleHandle(L"dxgi.dll");

            if (_dll == dllModule)
                _dll = originalModule;

            if (_dll == nullptr)
            {
                _dll = NtdllProxy::LoadLibraryExW_Ldr(L"dxgi.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
            }
        }
        else
        {
            _dll = module;
        }

        if (_dll == nullptr)
            return;

        LOG_DEBUG("dxgi.dll loaded at {:X}", (uintptr_t) _dll);

        _CreateDxgiFactory = (PFN_CreateDxgiFactory) KernelBaseProxy::GetProcAddress_()(_dll, "CreateDXGIFactory");
        _CreateDxgiFactory1 = (PFN_CreateDxgiFactory1) KernelBaseProxy::GetProcAddress_()(_dll, "CreateDXGIFactory1");
        _CreateDxgiFactory2 = (PFN_CreateDxgiFactory2) KernelBaseProxy::GetProcAddress_()(_dll, "CreateDXGIFactory2");
        _DeclareAdepterRemovalSupport = (PFN_DeclareAdepterRemovalSupport) KernelBaseProxy::GetProcAddress_()(
            _dll, "DXGIDeclareAdapterRemovalSupport");
        _GetDebugInterface = (PFN_GetDebugInterface) KernelBaseProxy::GetProcAddress_()(_dll, "DXGIGetDebugInterface1");
    }

    static HMODULE Module() { return _dll; }

    // Marks this thread as inside a DXGI factory creation OptiScaler made or hooked. A DXGI that is a layer on Vulkan
    // (dxvk) creates its Vulkan instance inside CreateDXGIFactory, holding a lock it cannot take twice, and the Vulkan
    // loader can load an overlay's Vulkan layer right there. That layer's DLL loads reach LibraryLoadHooks' overlay
    // branch, which must then not call CreateDXGIFactory again on this thread: Batman: Arkham Knight on dxvk with the
    // Steam overlay hung at startup on exactly that (IdentifyGpu -> dxvk -> vkCreateInstance -> Steam overlay layer ->
    // overlay DLL load -> CreateDXGIFactory -> the same dxvk lock).
    struct ScopedFactoryCreation
    {
        ScopedFactoryCreation() { ++_factoryCreationDepth; }
        ~ScopedFactoryCreation() { --_factoryCreationDepth; }
        ScopedFactoryCreation(const ScopedFactoryCreation&) = delete;
        ScopedFactoryCreation& operator=(const ScopedFactoryCreation&) = delete;
    };

    static bool InsideFactoryCreation() { return _factoryCreationDepth > 0; }

    // Windows' own DXGI factory, for a swapchain on a real D3D12 queue when the loaded dxgi.dll is a layer that cannot
    // make one: dxvk's returns DXGI_ERROR_UNSUPPORTED, so frame generation's D3D12 swapchain for a dxvk game
    // (native/VkPresentBridge.h, NativeDxvkVulkan) comes from here. Loaded from System32 the way OptiScaler loads it for
    // itself as dxgi.dll. Not hooked: its methods are called directly. The caller owns the reference; null when the
    // system DLL cannot be told apart from the loaded one or the factory fails.
    static IDXGIFactory2* CreateSystemFactory2()
    {
        static const PFN_CreateDxgiFactory2 create = []() -> PFN_CreateDxgiFactory2
        {
            auto module = NtdllProxy::LoadLibraryExW_Ldr(L"dxgi.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);

            if (module == nullptr || module == _dll)
                return nullptr;

            return (PFN_CreateDxgiFactory2) KernelBaseProxy::GetProcAddress_()(module, "CreateDXGIFactory2");
        }();

        if (create == nullptr)
            return nullptr;

        IDXGIFactory2* factory = nullptr;
        ScopedFactoryCreation factoryCreation {};

        if (FAILED(create(0, __uuidof(IDXGIFactory2), &factory)))
            return nullptr;

        return factory;
    }

    static PFN_CreateDxgiFactory CreateDxgiFactory_() { return _CreateDxgiFactory; }
    static PFN_CreateDxgiFactory1 CreateDxgiFactory1_() { return _CreateDxgiFactory1; }
    static PFN_CreateDxgiFactory2 CreateDxgiFactory2_() { return _CreateDxgiFactory2; }
    static PFN_DeclareAdepterRemovalSupport DeclareAdepterRemovalSupport_() { return _DeclareAdepterRemovalSupport; }
    static PFN_GetDebugInterface GetDebugInterface_() { return _GetDebugInterface; }

    static PFN_CreateDxgiFactory CreateDxgiFactory_Hooked()
    {
        return (PFN_CreateDxgiFactory) KernelBaseProxy::GetProcAddress_()(_dll, "CreateDXGIFactory");
    }
    static PFN_CreateDxgiFactory1 CreateDxgiFactory1_Hooked()
    {
        return (PFN_CreateDxgiFactory1) KernelBaseProxy::GetProcAddress_()(_dll, "CreateDXGIFactory1");
    }
    static PFN_CreateDxgiFactory2 CreateDxgiFactory2_Hooked()
    {
        return (PFN_CreateDxgiFactory2) KernelBaseProxy::GetProcAddress_()(_dll, "CreateDXGIFactory2");
    }
    static PFN_DeclareAdepterRemovalSupport DeclareAdepterRemovalSupport_Hooked()
    {
        return (PFN_DeclareAdepterRemovalSupport) KernelBaseProxy::GetProcAddress_()(
            _dll, "DXGIDeclareAdepterRemovalSupport");
    }
    static PFN_GetDebugInterface GetDebugInterface_Hooked()
    {
        return (PFN_GetDebugInterface) KernelBaseProxy::GetProcAddress_()(_dll, "DXGIGetDebugInterface1");
    }

    static PFN_CreateDxgiFactory& Original_CreateDxgiFactory() { return GenshinNativeQuery::Own::Original<PFN_CreateDxgiFactory>(_own_CreateDxgiFactory); }
    static PFN_CreateDxgiFactory Hook_CreateDxgiFactory(PVOID method)
    {
        if (_own_CreateDxgiFactory.live) return Original_CreateDxgiFactory();
        const auto result = GenshinNativeQuery::Own::Install(_own_CreateDxgiFactory, nullptr,
            (PVOID) CreateDxgiFactory_ForHook(), method, HookModule(), "CreateDXGIFactory", HookModule() != dllModule);
        if (result != NO_ERROR) { LOG_ERROR("Failed to hook: {:X}", result); return nullptr; }
        return Original_CreateDxgiFactory();
    }

    static PFN_CreateDxgiFactory1& Original_CreateDxgiFactory1() { return GenshinNativeQuery::Own::Original<PFN_CreateDxgiFactory1>(_own_CreateDxgiFactory1); }
    static PFN_CreateDxgiFactory1 Hook_CreateDxgiFactory1(PVOID method)
    {
        if (_own_CreateDxgiFactory1.live) return Original_CreateDxgiFactory1();
        const auto result = GenshinNativeQuery::Own::Install(_own_CreateDxgiFactory1, nullptr,
            (PVOID) CreateDxgiFactory1_ForHook(), method, HookModule(), "CreateDXGIFactory1", HookModule() != dllModule);
        if (result != NO_ERROR) { LOG_ERROR("Failed to hook: {:X}", result); return nullptr; }
        return Original_CreateDxgiFactory1();
    }

    static PFN_CreateDxgiFactory2& Original_CreateDxgiFactory2() { return GenshinNativeQuery::Own::Original<PFN_CreateDxgiFactory2>(_own_CreateDxgiFactory2); }
    static PFN_CreateDxgiFactory2 Hook_CreateDxgiFactory2(PVOID method)
    {
        if (_own_CreateDxgiFactory2.live) return Original_CreateDxgiFactory2();
        const auto result = GenshinNativeQuery::Own::Install(_own_CreateDxgiFactory2, nullptr,
            (PVOID) CreateDxgiFactory2_ForHook(), method, HookModule(), "CreateDXGIFactory2", HookModule() != dllModule);
        if (result != NO_ERROR) { LOG_ERROR("Failed to hook: {:X}", result); return nullptr; }
        return Original_CreateDxgiFactory2();
    }

    static PFN_DeclareAdepterRemovalSupport& Original_DeclareAdepterRemovalSupport() { return GenshinNativeQuery::Own::Original<PFN_DeclareAdepterRemovalSupport>(_own_DeclareAdepterRemovalSupport); }
    static PFN_DeclareAdepterRemovalSupport Hook_DeclareAdepterRemovalSupport(PVOID method)
    {
        if (_own_DeclareAdepterRemovalSupport.live) return Original_DeclareAdepterRemovalSupport();
        const auto result = GenshinNativeQuery::Own::Install(_own_DeclareAdepterRemovalSupport, nullptr,
            (PVOID) DeclareAdepterRemovalSupport_ForHook(), method, HookModule(), "DXGIDeclareAdapterRemovalSupport", HookModule() != dllModule);
        if (result != NO_ERROR) { LOG_ERROR("Failed to hook: {:X}", result); return nullptr; }
        return Original_DeclareAdepterRemovalSupport();
    }

    static PFN_GetDebugInterface& Original_GetDebugInterface() { return GenshinNativeQuery::Own::Original<PFN_GetDebugInterface>(_own_GetDebugInterface); }
    static PFN_GetDebugInterface Hook_GetDebugInterface(PVOID method)
    {
        if (_own_GetDebugInterface.live) return Original_GetDebugInterface();
        const auto result = GenshinNativeQuery::Own::Install(_own_GetDebugInterface, nullptr,
            (PVOID) GetDebugInterface_ForHook(), method, HookModule(), "DXGIGetDebugInterface1", HookModule() != dllModule);
        if (result != NO_ERROR) { LOG_ERROR("Failed to hook: {:X}", result); return nullptr; }
        return Original_GetDebugInterface();
    }

    static void Unhook(PVOID addr, PVOID method)
    {
        for (auto* slot : {&_own_CreateDxgiFactory, &_own_CreateDxgiFactory1, &_own_CreateDxgiFactory2, &_own_DeclareAdepterRemovalSupport, &_own_GetDebugInterface})
            if (slot->live && slot->hook == method && *slot->executed == addr)
            {
                const auto result = GenshinNativeQuery::Own::Detach(*slot);
                if (result != NO_ERROR) LOG_ERROR("Failed to unhook: {:X}", result);
                return;
            }
        LOG_ERROR("Refusing unowned DXGI detach");
    }

  private:
    inline static GenshinNativeQuery::Own::Slot _own_CreateDxgiFactory;
    inline static GenshinNativeQuery::Own::Slot _own_CreateDxgiFactory1;
    inline static GenshinNativeQuery::Own::Slot _own_CreateDxgiFactory2;
    inline static GenshinNativeQuery::Own::Slot _own_DeclareAdepterRemovalSupport;
    inline static GenshinNativeQuery::Own::Slot _own_GetDebugInterface;
    inline static HMODULE _dll = nullptr;
    inline static thread_local int _factoryCreationDepth = 0;

    inline static PFN_CreateDxgiFactory _CreateDxgiFactory = nullptr;
    inline static PFN_CreateDxgiFactory1 _CreateDxgiFactory1 = nullptr;
    inline static PFN_CreateDxgiFactory2 _CreateDxgiFactory2 = nullptr;
    inline static PFN_DeclareAdepterRemovalSupport _DeclareAdepterRemovalSupport = nullptr;
    inline static PFN_GetDebugInterface _GetDebugInterface = nullptr;

    inline static HMODULE HookModule()
    {
        if (State::Instance().workingMode == WorkingMode::Dxgi)
            return dllModule;

        return _dll;
    }

    static PFN_CreateDxgiFactory CreateDxgiFactory_ForHook()
    {
        return (PFN_CreateDxgiFactory) KernelBaseProxy::GetProcAddress_()(HookModule(), "CreateDXGIFactory");
    }
    static PFN_CreateDxgiFactory1 CreateDxgiFactory1_ForHook()
    {
        return (PFN_CreateDxgiFactory1) KernelBaseProxy::GetProcAddress_()(HookModule(), "CreateDXGIFactory1");
    }
    static PFN_CreateDxgiFactory2 CreateDxgiFactory2_ForHook()
    {
        return (PFN_CreateDxgiFactory2) KernelBaseProxy::GetProcAddress_()(HookModule(), "CreateDXGIFactory2");
    }
    static PFN_DeclareAdepterRemovalSupport DeclareAdepterRemovalSupport_ForHook()
    {
        return (PFN_DeclareAdepterRemovalSupport) KernelBaseProxy::GetProcAddress_()(
            HookModule(), "DXGIDeclareAdepterRemovalSupport");
    }
    static PFN_GetDebugInterface GetDebugInterface_ForHook()
    {
        return (PFN_GetDebugInterface) KernelBaseProxy::GetProcAddress_()(HookModule(), "DXGIGetDebugInterface1");
    }
};

#pragma once
#include "SysUtils.h"
#include "FG_Hooks.h"
#include "RoutedHook.h"
#include <dxgi1_6.h>

#include "Hook_Utils.h"

class DxgiFactoryHooks
{
  public:
    static void HookToFactory(IDXGIFactory* pFactory);
    static void HookToDLSSGFactory(IDXGIFactory* pFactory);

    // Correct status-only fullscreen changes for this exact presentation owner, never a hidden peer.
    static void NoteCreateStatus(HRESULT result, IDXGISwapChain* swapChain, IDXGISwapChain* presentationOwner,
                                 DXGI_SWAP_CHAIN_DESC* creationDesc = nullptr);

    // The D3D12 swapchain a bridged game presents through (the Vulkan present bridge, native/VkPresentBridge.cpp), made as a
    // D3D11 game's is: frame generation's own swapchain, else a plain D3D12 one. `desc` is a flip-model description.
    // *realFG: the swapchain is frame generation's own (its present is hooked); false: a plain one. systemFactory: `factory`
    // is Windows' own (DxgiProxy::CreateSystemFactory2, a dxvk game), which is not hooked and is called directly.
    static HRESULT CreateDx12BridgeSwapChain(IDXGIFactory* factory, ID3D12CommandQueue* queue, DXGI_SWAP_CHAIN_DESC* desc,
                                             IDXGISwapChain4** out, bool* realFG, bool systemFactory = false);

  private:
    using PFN_EnumAdapterByGpuPreference =
        rewrite_signature<decltype(&IDXGIFactory6::EnumAdapterByGpuPreference)>::type;
    using PFN_EnumAdapterByLuid = rewrite_signature<decltype(&IDXGIFactory4::EnumAdapterByLuid)>::type;
    using PFN_EnumAdapters1 = rewrite_signature<decltype(&IDXGIFactory1::EnumAdapters1)>::type;
    using PFN_EnumAdapters = rewrite_signature<decltype(&IDXGIFactory::EnumAdapters)>::type;
    using PFN_CreateSwapChain = rewrite_signature<decltype(&IDXGIFactory::CreateSwapChain)>::type;
    using PFN_CreateSwapChainForHwnd = rewrite_signature<decltype(&IDXGIFactory2::CreateSwapChainForHwnd)>::type;
    using PFN_CreateSwapChainForCoreWindow =
        rewrite_signature<decltype(&IDXGIFactory2::CreateSwapChainForCoreWindow)>::type;
    using PFN_CreateSwapChainForComposition =
        rewrite_signature<decltype(&IDXGIFactory2::CreateSwapChainForComposition)>::type;



    static HRESULT CreateSwapChain(IDXGIFactory* realFactory, IUnknown* pDevice, DXGI_SWAP_CHAIN_DESC* pDesc,
                                   IDXGISwapChain** ppSwapChain);

    static HRESULT CreateSwapChainForHwnd(IDXGIFactory2* realFactory, IUnknown* pDevice, HWND hWnd,
                                          const DXGI_SWAP_CHAIN_DESC1* pDesc,
                                          const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc,
                                          IDXGIOutput* pRestrictToOutput, IDXGISwapChain1** ppSwapChain);

    static HRESULT CreateSwapChainForCoreWindow(IDXGIFactory2* realFactory, IUnknown* pDevice, IUnknown* pWindow,
                                                const DXGI_SWAP_CHAIN_DESC1* pDesc, IDXGIOutput* pRestrictToOutput,
                                                IDXGISwapChain1** ppSwapChain);

    static HRESULT CreateSwapChainForComposition(IDXGIFactory2* realFactory, IUnknown* pDevice,
                                                 const DXGI_SWAP_CHAIN_DESC1* pDesc, IDXGIOutput* pRestrictToOutput,
                                                 IDXGISwapChain1** ppSwapChain);



    static HRESULT EnumAdapters(IDXGIFactory* realFactory, UINT Adapter, IDXGIAdapter** ppAdapter);
    static HRESULT EnumAdapters1(IDXGIFactory1* realFactory, UINT Adapter, IDXGIAdapter1** ppAdapter);
    static HRESULT EnumAdapterByLuid(IDXGIFactory4* realFactory, LUID AdapterLuid, REFIID riid, void** ppvAdapter);
    static HRESULT EnumAdapterByGpuPreference(IDXGIFactory6* realFactory, UINT Adapter,
                                              DXGI_GPU_PREFERENCE GpuPreference, REFIID riid, void** ppvAdapter);

    VALIDATE_MEMBER_HOOK(CreateSwapChain, PFN_CreateSwapChain)
    VALIDATE_MEMBER_HOOK(CreateSwapChainForHwnd, PFN_CreateSwapChainForHwnd)
    VALIDATE_MEMBER_HOOK(CreateSwapChainForCoreWindow, PFN_CreateSwapChainForCoreWindow)
    VALIDATE_MEMBER_HOOK(CreateSwapChainForComposition, PFN_CreateSwapChainForComposition)
    VALIDATE_MEMBER_HOOK(EnumAdapters, PFN_EnumAdapters)
    VALIDATE_MEMBER_HOOK(EnumAdapters1, PFN_EnumAdapters1)
    VALIDATE_MEMBER_HOOK(EnumAdapterByLuid, PFN_EnumAdapterByLuid)
    VALIDATE_MEMBER_HOOK(EnumAdapterByGpuPreference, PFN_EnumAdapterByGpuPreference)

    struct FactoryCreateDomain {};
    struct FactoryEnumDomain {};
    inline static HookOwnership::RoutedHook<&EnumAdapterByGpuPreference, 29, FactoryEnumDomain> o_EnumAdapterByGpuPreference;
    inline static HookOwnership::RoutedHook<&EnumAdapterByLuid, 26, FactoryEnumDomain> o_EnumAdapterByLuid;
    inline static HookOwnership::RoutedHook<&EnumAdapters1, 12, FactoryEnumDomain> o_EnumAdapters1;
    inline static HookOwnership::RoutedHook<&EnumAdapters, 7, FactoryEnumDomain> o_EnumAdapters;
    inline static HookOwnership::RoutedHook<&CreateSwapChain, 10, FactoryCreateDomain> o_CreateSwapChain;
    inline static HookOwnership::RoutedHook<&CreateSwapChainForHwnd, 15, FactoryCreateDomain> o_CreateSwapChainForHwnd;
    inline static HookOwnership::RoutedHook<&CreateSwapChainForCoreWindow, 16, FactoryCreateDomain> o_CreateSwapChainForCoreWindow;
    inline static HookOwnership::RoutedHook<&CreateSwapChainForComposition, 24, FactoryCreateDomain> o_CreateSwapChainForComposition;

    // To prevent recursive FG swapchain creation
    inline static thread_local bool _skipFGSwapChainCreation = false;

    class ScopedSkipFGSCCreation
    {
      private:
        bool previousState;

      public:
        ScopedSkipFGSCCreation()
        {
            previousState = DxgiFactoryHooks::_skipFGSwapChainCreation;
            DxgiFactoryHooks::_skipFGSwapChainCreation = true;
        }
        ~ScopedSkipFGSCCreation() { DxgiFactoryHooks::_skipFGSwapChainCreation = previousState; }
    };
};

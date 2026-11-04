#include "pch.h"
#include "DxgiFactory_Hooks.h"
#include "SwapchainCreateRule.h"
#include <genshin/GenshinPresentNr.h>
#include <framegen/dlssg/DLSSG_Dx12.h>
#include "DxgiSwapchainSizing.h"

#include "D3D11_Hooks.h"
#include "D3D12_Hooks.h"

#include <Config.h>

#include <misc/IdentifyGpu.h>
#include <spoofing/Dxgi_Spoofing.h>

#include <misc/HiddenWindow.h>
#include <native/VkPresentBridge.h>
#include <inputs/GenshinFsrContract.h>
#include <with_dx12/with_dx12.h>
#include <wrapped/wrapped_swapchain.h>
#include <with_dx12/dx11_with_dx12_sc.h>

#include <d3d11.h>
#include <magic_enum.hpp>
#include <detours/detours.h>
#include <genshin/GenshinApplicationProxyHook.h>

// #define DETAILED_SC_LOGS

#ifdef DETAILED_SC_LOGS
#include <magic_enum.hpp>
#endif

// NativeDxvkVulkan: a dxvk game's frame generation is the Vulkan present bridge (native/VkPresentBridge.h) on dxvk's own
// Vulkan swapchain, not the D3D11 bridge (Dx11wDx12SC). That one cannot work on dxvk: dxvk's shared fence and texture
// handles do not open on a native D3D12 device (OpenSharedHandle E_HANDLE, then a crash in Batman: Arkham Knight).
static bool DxvkFrameGenerationViaVulkan() { return VkPresentBridge::DxvkThroughVulkan(); }

// R612: the bounded Genshin branch keeps the real native parent visible and
// owns only a hidden FG child. Missing camera refuses that frame's child work;
// it does not replace the selected DLSSG backend or hide the native game chain.
// A plain (wrapped) game swapchain has no interop. Except a dxvk game's D3D11 one while the Vulkan present bridge carries
// its frames: the bridge's VkwDx12 stays (a dxvk that makes its Vulkan swapchain inside CreateSwapChain sets it first).
static void SetPlainSwapchainInterop()
{
    if (VkPresentBridge::DxvkThroughVulkan() && VkPresentBridge::IsUp())
        return;

    State::Instance().swapchainInteropApi = SwapchainInteropApi::None;
}

static bool IsTearingSupported(IDXGIFactory* factory)
{
    if (factory == nullptr)
        return false;

    IDXGIFactory5* factory5 = nullptr;

    if (FAILED(factory->QueryInterface(IID_PPV_ARGS(&factory5))))
        return false;

    BOOL supported = FALSE;

    const HRESULT hr = factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &supported, sizeof(supported));

    factory5->Release();

    return SUCCEEDED(hr) && supported == TRUE;
}

static bool PrepareDx12FlipFormat(DXGI_FORMAT& format)
{
    switch (format)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        LOG_WARN("Dx11wDx12 converting R8G8B8A8_UNORM_SRGB to "
                 "R8G8B8A8_UNORM for DX12 flip swapchain");
        format = DXGI_FORMAT_R8G8B8A8_UNORM;
        return true;

    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        LOG_WARN("Dx11wDx12 converting B8G8R8A8_UNORM_SRGB to "
                 "B8G8R8A8_UNORM for DX12 flip swapchain");
        format = DXGI_FORMAT_B8G8R8A8_UNORM;
        return true;

    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        return true;

    default:
        LOG_ERROR("Unsupported texture format for DX12 flip swapchain: {}", (UINT) format);
        return false;
    }
}

static bool PrepareDx12InteropDesc(DXGI_SWAP_CHAIN_DESC& desc, bool tearingSupported)
{
    // D3D12 swapchain backbuffers cannot be multisampled: this desc is the D3D12 side's, forced to one sample below. The
    // game's own D3D11 swap chain keeps the samples it asked for, and the bridge resolves them into single-sample copies.
    if (desc.SampleDesc.Count > 1)
    {
        LOG_INFO("Dx11wDx12 interop: the game's swapchain is multisampled ({} samples); the D3D12 side is single-sample",
                 desc.SampleDesc.Count);
    }

    // Flip-model swapchains support a limited set of formats.
    if (!PrepareDx12FlipFormat(desc.BufferDesc.Format))
    {
        LOG_WARN("Dx11wDx12 interop unsupported flip-model format: {}", (UINT) desc.BufferDesc.Format);
        return false;
    }

    // Flip-model requires 2-16 buffers.
    if (desc.BufferCount < 2)
        desc.BufferCount = 2;

    if (desc.BufferCount > 16)
    {
        LOG_WARN("Dx11wDx12 interop invalid BufferCount: {}", desc.BufferCount);
        return false;
    }

    // D3D12 supports flip-model swap effects only.
    switch (desc.SwapEffect)
    {
    case DXGI_SWAP_EFFECT_DISCARD:
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        break;

    case DXGI_SWAP_EFFECT_SEQUENTIAL:
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        break;

    case DXGI_SWAP_EFFECT_FLIP_DISCARD:
    case DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL:
        break;

    default:
        LOG_WARN("Dx11wDx12 interop unsupported SwapEffect: {}", (UINT) desc.SwapEffect);
        return false;
    }

    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;

    // D3D12 swapchain backbuffers cannot expose UAV usage.
    if (desc.BufferUsage & DXGI_USAGE_UNORDERED_ACCESS)
    {
        LOG_DEBUG("Dx11wDx12 removing DXGI_USAGE_UNORDERED_ACCESS from DX12 swapchain");
        desc.BufferUsage &= ~DXGI_USAGE_UNORDERED_ACCESS;
    }

    // GDI-compatible swapchains are not applicable to the D3D12 interop path.
    if (desc.Flags & DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE)
    {
        LOG_DEBUG("Dx11wDx12 removing DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE");
        desc.Flags &= ~DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE;
    }

    // Keep the game's tearing intent when the system supports it.
    if (!tearingSupported && (desc.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING))
    {
        LOG_DEBUG("Dx11wDx12 removing unsupported DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING");
        desc.Flags &= ~DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    }

    LOG_DEBUG("Dx11wDx12 DX12 desc: {}x{}, Format: {}, Count: {}, "
              "Sample: {}/{}, Usage: {:X}, SwapEffect: {}, Flags: {:X}, "
              "Windowed: {}, Refresh: {}/{}, Scaling: {}, Scanline: {}",
              desc.BufferDesc.Width, desc.BufferDesc.Height, (UINT) desc.BufferDesc.Format, desc.BufferCount,
              desc.SampleDesc.Count, desc.SampleDesc.Quality, desc.BufferUsage, (UINT) desc.SwapEffect, desc.Flags,
              desc.Windowed, desc.BufferDesc.RefreshRate.Numerator, desc.BufferDesc.RefreshRate.Denominator,
              (UINT) desc.BufferDesc.Scaling, (UINT) desc.BufferDesc.ScanlineOrdering);

    return true;
}

static bool PrepareDx12InteropDesc1(DXGI_SWAP_CHAIN_DESC1& desc, bool tearingSupported)
{
    // D3D12 swapchain backbuffers cannot be multisampled: this desc is the D3D12 side's, forced to one sample below (see the
    // other overload).
    if (desc.SampleDesc.Count > 1)
    {
        LOG_INFO("Dx11wDx12 interop: the game's swapchain is multisampled ({} samples); the D3D12 side is single-sample",
                 desc.SampleDesc.Count);
    }

    if (!PrepareDx12FlipFormat(desc.Format))
    {
        LOG_WARN("Dx11wDx12 interop unsupported flip-model format: {}", (UINT) desc.Format);
        return false;
    }

    if (desc.BufferCount < 2)
        desc.BufferCount = 2;

    if (desc.BufferCount > 16)
    {
        LOG_WARN("Dx11wDx12 interop invalid BufferCount: {}", desc.BufferCount);
        return false;
    }

    // Current interop wrapper does not explicitly handle stereo swapchains.
    if (desc.Stereo)
    {
        LOG_WARN("Dx11wDx12 interop does not support stereo swapchains!");
        return false;
    }

    switch (desc.SwapEffect)
    {
    case DXGI_SWAP_EFFECT_DISCARD:
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        break;

    case DXGI_SWAP_EFFECT_SEQUENTIAL:
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        break;

    case DXGI_SWAP_EFFECT_FLIP_DISCARD:
    case DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL:
        break;

    default:
        LOG_WARN("Dx11wDx12 interop unsupported SwapEffect: {}", (UINT) desc.SwapEffect);
        return false;
    }

    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;

    if (desc.BufferUsage & DXGI_USAGE_UNORDERED_ACCESS)
    {
        LOG_DEBUG("Dx11wDx12 removing DXGI_USAGE_UNORDERED_ACCESS from DX12 swapchain");
        desc.BufferUsage &= ~DXGI_USAGE_UNORDERED_ACCESS;
    }

    if (desc.Flags & DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE)
    {
        LOG_DEBUG("Dx11wDx12 removing DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE");
        desc.Flags &= ~DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE;
    }

    if (!tearingSupported && (desc.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING))
    {
        LOG_DEBUG("Dx11wDx12 removing unsupported DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING");
        desc.Flags &= ~DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    }

    LOG_DEBUG("Dx11wDx12 DX12 desc1: {}x{}, Format: {}, Count: {}, "
              "Sample: {}/{}, Usage: {:X}, SwapEffect: {}, Flags: {:X}, "
              "Scaling: {}, AlphaMode: {}, Stereo: {}",
              desc.Width, desc.Height, (UINT) desc.Format, desc.BufferCount, desc.SampleDesc.Count,
              desc.SampleDesc.Quality, desc.BufferUsage, (UINT) desc.SwapEffect, desc.Flags, (UINT) desc.Scaling,
              (UINT) desc.AlphaMode, desc.Stereo);

    return true;
}

void DxgiFactoryHooks::HookToFactory(IDXGIFactory* pFactory)
{
    if (pFactory == nullptr || o_EnumAdapters != nullptr)
        return;

    LOG_FUNC();

    IDXGIFactory* real = nullptr;
    if (!Util::CheckForRealObject(__FUNCTION__, pFactory, (IUnknown**) &real))
        real = pFactory;

    void** pFactoryVTable = *reinterpret_cast<void***>(real);

    GenshinNativeQuery::Own::Batch hooks;

    if (o_EnumAdapters == nullptr)
    {
        static GenshinNativeQuery::Own::Slot own_o_EnumAdapters;
        if (pFactoryVTable[7] != nullptr)
            GenshinApplicationProxyHook::AddMethod(hooks, own_o_EnumAdapters, &(PVOID&) o_EnumAdapters, pFactoryVTable, 7,
                (PVOID) DxgiFactoryHooks::EnumAdapters);
    }

    if (o_CreateSwapChain == nullptr)
    {
        static GenshinNativeQuery::Own::Slot own_o_CreateSwapChain;
        if (pFactoryVTable[10] != nullptr)
            GenshinApplicationProxyHook::AddMethod(hooks, own_o_CreateSwapChain, &(PVOID&) o_CreateSwapChain, pFactoryVTable, 10,
                (PVOID) DxgiFactoryHooks::CreateSwapChain);
    }

    IDXGIFactory1* factory1 = nullptr;
    if (real->QueryInterface(IID_PPV_ARGS(&factory1)) == S_OK)
    {
        void** factory1VTable = *reinterpret_cast<void***>(factory1);

        if (o_EnumAdapters1 == nullptr)
        {
            static GenshinNativeQuery::Own::Slot own_o_EnumAdapters1;
            if (factory1VTable[12] != nullptr)
                GenshinApplicationProxyHook::AddMethod(hooks, own_o_EnumAdapters1, &(PVOID&) o_EnumAdapters1, factory1VTable, 12,
                    (PVOID) DxgiFactoryHooks::EnumAdapters1);
        }
        factory1->Release();
    }

    IDXGIFactory2* factory2 = nullptr;
    if (real->QueryInterface(IID_PPV_ARGS(&factory2)) == S_OK)
    {
        void** factory2VTable = *reinterpret_cast<void***>(factory2);

        if (o_CreateSwapChainForHwnd == nullptr)
        {
            static GenshinNativeQuery::Own::Slot own_o_CreateSwapChainForHwnd;
            if (factory2VTable[15] != nullptr)
                GenshinApplicationProxyHook::AddMethod(hooks, own_o_CreateSwapChainForHwnd, &(PVOID&) o_CreateSwapChainForHwnd, factory2VTable, 15,
                    (PVOID) DxgiFactoryHooks::CreateSwapChainForHwnd);
        }

        if (o_CreateSwapChainForCoreWindow == nullptr)
        {
            static GenshinNativeQuery::Own::Slot own_o_CreateSwapChainForCoreWindow;
            if (factory2VTable[16] != nullptr)
                GenshinApplicationProxyHook::AddMethod(hooks, own_o_CreateSwapChainForCoreWindow, &(PVOID&) o_CreateSwapChainForCoreWindow, factory2VTable, 16,
                    (PVOID) DxgiFactoryHooks::CreateSwapChainForCoreWindow);
        }

        if (o_CreateSwapChainForComposition == nullptr)
        {
            static GenshinNativeQuery::Own::Slot own_o_CreateSwapChainForComposition;
            if (factory2VTable[24] != nullptr)
                GenshinApplicationProxyHook::AddMethod(hooks, own_o_CreateSwapChainForComposition, &(PVOID&) o_CreateSwapChainForComposition, factory2VTable, 24,
                    (PVOID) DxgiFactoryHooks::CreateSwapChainForComposition);
        }
        factory2->Release();
    }

    IDXGIFactory4* factory4 = nullptr;
    if (real->QueryInterface(IID_PPV_ARGS(&factory4)) == S_OK)
    {
        void** factory4VTable = *reinterpret_cast<void***>(factory4);

        if (o_EnumAdapterByLuid == nullptr)
        {
            static GenshinNativeQuery::Own::Slot own_o_EnumAdapterByLuid;
            if (factory4VTable[26] != nullptr)
                GenshinApplicationProxyHook::AddMethod(hooks, own_o_EnumAdapterByLuid, &(PVOID&) o_EnumAdapterByLuid, factory4VTable, 26,
                    (PVOID) DxgiFactoryHooks::EnumAdapterByLuid);
        }
        factory4->Release();
    }

    IDXGIFactory6* factory6 = nullptr;
    if (real->QueryInterface(IID_PPV_ARGS(&factory6)) == S_OK)
    {
        void** factory6VTable = *reinterpret_cast<void***>(factory6);

        if (o_EnumAdapterByGpuPreference == nullptr)
        {
            static GenshinNativeQuery::Own::Slot own_o_EnumAdapterByGpuPreference;
            if (factory6VTable[29] != nullptr)
                GenshinApplicationProxyHook::AddMethod(hooks, own_o_EnumAdapterByGpuPreference, &(PVOID&) o_EnumAdapterByGpuPreference, factory6VTable, 29,
                    (PVOID) DxgiFactoryHooks::EnumAdapterByGpuPreference);
        }
        factory6->Release();
    }

    const auto detourResult = hooks.Commit();
    if (detourResult != NO_ERROR) LOG_ERROR("Failed to hook IDXGIFactory: {:X}", detourResult);
}

void DxgiFactoryHooks::NoteCreateStatus(HRESULT result, IDXGISwapChain* swapChain,
                                        IDXGISwapChain* presentationOwner, DXGI_SWAP_CHAIN_DESC* creationDesc)
{
    if (result == S_OK || FAILED(result) || swapChain == nullptr || presentationOwner == nullptr)
        return;

    // The caller names its actual presentation owner. In particular, an inner hidden D3D11
    // chain is not the visible FG chain, even if both belong to the same device or window.
    Microsoft::WRL::ComPtr<IUnknown> createdIdentity, ownerIdentity;
    if (FAILED(swapChain->QueryInterface(IID_PPV_ARGS(&createdIdentity))) ||
        FAILED(presentationOwner->QueryInterface(IID_PPV_ARGS(&ownerIdentity))) ||
        createdIdentity.Get() == nullptr || ownerIdentity.Get() == nullptr ||
        createdIdentity.Get() != ownerIdentity.Get())
        return;

    auto& state = State::Instance();
    DXGI_SWAP_CHAIN_DESC actualDesc {};
    BOOL fullscreen = FALSE;
    const HWND expectedWindow = creationDesc ? creationDesc->OutputWindow : state.currentSwapchainDesc.OutputWindow;
    if (FAILED(swapChain->GetDesc(&actualDesc)) || actualDesc.OutputWindow != expectedWindow ||
        FAILED(swapChain->GetFullscreenState(&fullscreen, nullptr)))
        return; // No successful observation: keep the existing state.

    state.realExclusiveFullscreen = fullscreen != FALSE;
    if (actualDesc.OutputWindow == state.currentSwapchainDesc.OutputWindow)
        state.currentSwapchainDesc.Windowed = fullscreen == FALSE;
    // The classic FG caller copies this descriptor back after the provider returns.
    // Carry the observation with it so that copyback cannot restore the requested mode.
    if (creationDesc != nullptr)
        creationDesc->Windowed = fullscreen == FALSE;
    LOG_INFO("Swapchain made with status {:X}, exclusive full screen: {}", (UINT) result, fullscreen != FALSE);
}

HRESULT DxgiFactoryHooks::CreateDx12BridgeSwapChain(IDXGIFactory* factory, ID3D12CommandQueue* queue,
                                                    DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain4** out, bool* realFG,
                                                    bool systemFactory)
{
    *out = nullptr;
    *realFG = false;

    if (factory == nullptr || queue == nullptr || desc == nullptr)
        return E_INVALIDARG;

    // A Vulkan game never made a DXGI factory of its own: frame generation's swapchain is made through this hooked one.
    // Not Windows' own beside dxvk's: the hooks' originals (o_*) are dxvk's methods, which must not be called on it.
    if (!systemFactory)
        HookToFactory(factory);

    if (!PrepareDx12InteropDesc(*desc, IsTearingSupported(factory)))
        return E_INVALIDARG;

    IDXGISwapChain* swapChain = nullptr;
    HRESULT result = E_FAIL;

    {
        ScopedSkipFGSCCreation skipFGSCCreation {};
        result = FGHooks::CreateSwapChain(factory, queue, desc, &swapChain);
        *realFG = SUCCEEDED(result) && swapChain != nullptr;
    }

    if (FAILED(result) || swapChain == nullptr)
    {
        *realFG = false;
        LOG_WARN("Vulkan bridge FG swapchain creation failed: {:X}; creating plain DX12 swapchain", (UINT) result);

        ScopedSkipParentWrapping skipParentWrapping {};
        result = systemFactory ? factory->CreateSwapChain(queue, desc, &swapChain)
                               : o_CreateSwapChain(factory, queue, desc, &swapChain);
    }

    if (SUCCEEDED(result) && swapChain != nullptr)
    {
        result = swapChain->QueryInterface(IID_PPV_ARGS(out));
        swapChain->Release();
    }

    // The game owns full screen (Vulkan, or dxvk's own DXGI): DXGI's Alt+Enter on the window must not put this swapchain
    // into exclusive full screen behind its back. The bridge lets the swapchain go on every remake, and releasing one in
    // exclusive full screen raises DXGI_ERROR_INVALID_CALL (Batman: Arkham Knight on dxvk, Alt+Enter).
    if (SUCCEEDED(result) && *out != nullptr && desc->OutputWindow != nullptr)
        factory->MakeWindowAssociation(desc->OutputWindow, DXGI_MWA_NO_ALT_ENTER);

    return result;
}

void DxgiFactoryHooks::HookToDLSSGFactory(IDXGIFactory* pFactory)
{
    if (pFactory == nullptr || o_DLSSGCreateSwapChain != nullptr)
        return;

    IDXGIFactory* real = nullptr;
    if (!Util::CheckForRealObject(__FUNCTION__, pFactory, (IUnknown**) &real))
        return;

    real->Release();

    LOG_FUNC();

    void** pFactoryVTable = *reinterpret_cast<void***>(pFactory);

    // These are Streamline's upgraded factory methods, used only for FG forwarding.
    // Keep exact commit/mirror/rollback ownership, but do not enroll vendor code as
    // Windows-native query authority. If any native dependency reaches that code,
    // the unchanged signed-image/import/whole-code gates still refuse it.
    GenshinNativeQuery::Own::Batch hooks;

    if (o_DLSSGCreateSwapChain == nullptr)
    {
        static GenshinNativeQuery::Own::Slot own_o_DLSSGCreateSwapChain;
        const auto entry_o_DLSSGCreateSwapChain = (PVOID) pFactoryVTable[10];
        if (entry_o_DLSSGCreateSwapChain != nullptr)
            hooks.Add(own_o_DLSSGCreateSwapChain, &(PVOID&) o_DLSSGCreateSwapChain, nullptr, entry_o_DLSSGCreateSwapChain,
                (PVOID) DxgiFactoryHooks::DLSSGCreateSwapChain, nullptr, nullptr, false);
    }

    IDXGIFactory2* factory2 = nullptr;
    if (pFactory->QueryInterface(IID_PPV_ARGS(&factory2)) == S_OK)
    {
        factory2->Release();

        if (o_DLSSGCreateSwapChainForHwnd == nullptr)
        {
            static GenshinNativeQuery::Own::Slot own_o_DLSSGCreateSwapChainForHwnd;
            const auto entry_o_DLSSGCreateSwapChainForHwnd = (PVOID) pFactoryVTable[15];
            if (entry_o_DLSSGCreateSwapChainForHwnd != nullptr)
                hooks.Add(own_o_DLSSGCreateSwapChainForHwnd, &(PVOID&) o_DLSSGCreateSwapChainForHwnd, nullptr, entry_o_DLSSGCreateSwapChainForHwnd,
                    (PVOID) DxgiFactoryHooks::DLSSGCreateSwapChainForHwnd, nullptr, nullptr, false);
        }

        if (o_DLSSGCreateSwapChainForCoreWindow == nullptr)
        {
            static GenshinNativeQuery::Own::Slot own_o_DLSSGCreateSwapChainForCoreWindow;
            const auto entry_o_DLSSGCreateSwapChainForCoreWindow = (PVOID) pFactoryVTable[16];
            if (entry_o_DLSSGCreateSwapChainForCoreWindow != nullptr)
                hooks.Add(own_o_DLSSGCreateSwapChainForCoreWindow, &(PVOID&) o_DLSSGCreateSwapChainForCoreWindow, nullptr, entry_o_DLSSGCreateSwapChainForCoreWindow,
                    (PVOID) DxgiFactoryHooks::DLSSGCreateSwapChainForCoreWindow, nullptr, nullptr, false);
        }
    }

    const auto result = hooks.Commit();
    if (result != NO_ERROR) LOG_ERROR("Failed to hook DLSSG factory: {:X}", result);
}

HRESULT DxgiFactoryHooks::CreateSwapChain(IDXGIFactory* realFactory, IUnknown* pDevice, DXGI_SWAP_CHAIN_DESC* pDesc,
                                          IDXGISwapChain** ppSwapChain)
{
    *ppSwapChain = nullptr;

    if (State::Instance().vulkanCreatingSC)
    {
        LOG_WARN("Vulkan is creating swapchain!");

        if (pDesc != nullptr)
            LOG_DEBUG("Width: {}, Height: {}, Format: {}, Count: {}, Hwnd: {:X}, Windowed: {}, SkipWrapping: {}",
                      pDesc->BufferDesc.Width, pDesc->BufferDesc.Height, (UINT) pDesc->BufferDesc.Format,
                      pDesc->BufferCount, (SIZE_T) pDesc->OutputWindow, pDesc->Windowed, _skipFGSwapChainCreation);

        ScopedSkipDxgiLoadChecks skipDxgiLoadChecks {};
        ScopedSkipParentWrapping skipParentWrapping {};

        auto res = o_CreateSwapChain(realFactory, pDevice, pDesc, ppSwapChain);
        return res;
    }

    if (pDevice == nullptr || pDesc == nullptr)
    {
        LOG_WARN("pDevice or pDesc is nullptr!");

        ScopedSkipDxgiLoadChecks skipDxgiLoadChecks {};
        ScopedSkipParentWrapping skipParentWrapping {};

        auto res = o_CreateSwapChain(realFactory, pDevice, pDesc, ppSwapChain);
        return res;
    }

    DXGI_SWAP_CHAIN_DESC localDesc = *pDesc;
    const bool sizeToWindow = ResolveWindowSizedSwapchain(localDesc);

    if (localDesc.BufferDesc.Height < 100 || localDesc.BufferDesc.Width < 100)
    {
        LOG_WARN("Overlay call! Width: {}, Height: {}, Format: {}, Count: {}, Hwnd: {:X}, Windowed: {}",
                 pDesc->BufferDesc.Width, pDesc->BufferDesc.Height, (UINT) pDesc->BufferDesc.Format,
                 pDesc->BufferCount, (SIZE_T) pDesc->OutputWindow, pDesc->Windowed);

        ScopedSkipDxgiLoadChecks skipDxgiLoadChecks {};
        ScopedSkipParentWrapping skipParentWrapping {};

        auto res = o_CreateSwapChain(realFactory, pDevice, pDesc, ppSwapChain);
        return res;
    }

    if (sizeToWindow)
        LOG_INFO("CreateSwapChain: 0x0 size-to-window descriptor, Hwnd: {:X}, Count: {}, wrapping normally",
                 (SIZE_T) pDesc->OutputWindow, pDesc->BufferCount);

    LOG_DEBUG("Width: {}, Height: {}, Format: {}, Count: {}, Flags: {:X}, Hwnd: {:X}, Windowed: {}, SkipWrapping: {}",
              localDesc.BufferDesc.Width, localDesc.BufferDesc.Height, (UINT) localDesc.BufferDesc.Format,
              localDesc.BufferCount, localDesc.Flags, (SIZE_T) localDesc.OutputWindow, localDesc.Windowed,
              _skipFGSwapChainCreation);

    if (State::Instance().activeFgOutput == FGOutput::XeFG &&
        Config::Instance()->FGXeFGForceBorderless.value_or_default())
    {
        if (!localDesc.Windowed)
        {
            State::Instance().SCExclusiveFullscreen = true;
            localDesc.Windowed = true;
        }

        localDesc.Flags &= ~DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
        localDesc.BufferDesc.Scaling = DXGI_MODE_SCALING_STRETCHED;
    }

    // For vsync override
    if (!localDesc.Windowed)
    {
        LOG_INFO("Game is creating fullscreen swapchain, disabled V-Sync overrides");
        Config::Instance()->OverrideVsync.set_volatile_value(false);
    }

    if (Config::Instance()->OverrideVsync.value_or_default())
    {
        localDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        localDesc.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

        if (localDesc.BufferCount < 2)
            localDesc.BufferCount = 2;
    }

    State::Instance().SCAllowTearing = (localDesc.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) > 0;
    State::Instance().SCLastFlags = localDesc.Flags;
    State::Instance().realExclusiveFullscreen = !localDesc.Windowed;

#ifdef DETAILED_SC_LOGS
    LOG_TRACE("localDesc.BufferCount: {}", localDesc.BufferCount);
    LOG_TRACE("localDesc.BufferDesc.Format: {}", magic_enum::enum_name(localDesc.BufferDesc.Format));
    LOG_TRACE("localDesc.BufferDesc.Height: {}", localDesc.BufferDesc.Height);
    LOG_TRACE("localDesc.BufferDesc.RefreshRate.Denominator: {}", localDesc.BufferDesc.RefreshRate.Denominator);
    LOG_TRACE("localDesc.BufferDesc.RefreshRate.Numerator: {}", localDesc.BufferDesc.RefreshRate.Numerator);
    LOG_TRACE("localDesc.BufferDesc.Scaling: {}", magic_enum::enum_name(localDesc.BufferDesc.Scaling));
    LOG_TRACE("localDesc.BufferDesc.ScanlineOrdering: {}",
              magic_enum::enum_name(localDesc.BufferDesc.ScanlineOrdering));
    LOG_TRACE("localDesc.BufferDesc.Width: {}", localDesc.BufferDesc.Width);
    LOG_TRACE("localDesc.BufferUsage: {}", localDesc.BufferUsage);
    LOG_TRACE("localDesc.Flags: {}", localDesc.Flags);
    LOG_TRACE("localDesc.OutputWindow: {}", (UINT64) localDesc.OutputWindow);
    LOG_TRACE("localDesc.SampleDesc.Count: {}", localDesc.SampleDesc.Count);
    LOG_TRACE("localDesc.SampleDesc.Quality: {}", localDesc.SampleDesc.Quality);
    LOG_TRACE("localDesc.SwapEffect: {}", magic_enum::enum_name(localDesc.SwapEffect));
    LOG_TRACE("localDesc.Windowed: {}", localDesc.Windowed);
#endif //

    // Check for SL proxy, get real queue
    ID3D12CommandQueue* cq = nullptr;
    IUnknown* real = nullptr;
    HRESULT FGSCResult = E_NOTIMPL;

    if (pDevice->QueryInterface(IID_PPV_ARGS(&cq)) == S_OK)
    {
        if (State::Instance().currentD3D12Device == nullptr)
        {
            ID3D12Device* device = nullptr;
            if (cq->GetDevice(IID_PPV_ARGS(&device)) == S_OK)
            {
                if (device != nullptr)
                {
                    // Update current D3D12 device
                    if (State::Instance().currentD3D12Device != device)
                    {
                        State::Instance().currentD3D12Device = device;
                    }

                    LOG_INFO("Captured D3D12 device from command queue: {:X}", (UINT64) device);
                    D3D12Hooks::HookDevice(State::Instance().currentD3D12Device);
                }
            }
        }

        if (!Util::CheckForRealObject(__FUNCTION__, cq, &real))
            real = cq;

        State::Instance().currentCommandQueue = (ID3D12CommandQueue*) real;

        if (State::Instance().currentD3D12Device != nullptr)
        {
            WithDx12::SetD3D12Objects(State::Instance().currentD3D12Device, State::Instance().currentCommandQueue,
                                      D3D12_COMMAND_LIST_TYPE_DIRECT);
        }

        // Create FG SwapChain
        if (!_skipFGSwapChainCreation)
        {
            ScopedSkipFGSCCreation skipFGSCCreation {};
            FGSCResult = FGHooks::CreateSwapChain(realFactory, real, &localDesc, ppSwapChain);

            if (FGSCResult == S_OK)
            {
                State::Instance().currentSwapchainDesc = localDesc;
                return FGSCResult;
            }
        }
    }
    else
    {
        LOG_INFO("Failed to get ID3D12CommandQueue from pDevice, creating Dx11 swapchain!");

        ID3D11Device* device = nullptr;

        if (pDevice->QueryInterface(IID_PPV_ARGS(&device)) == S_OK)
        {
            D3D11Hooks::HookToDevice(device);
            State::Instance().currentD3D11Device = device;

            if (!_skipFGSwapChainCreation && State::Instance().activeFgInput == FGInput::Upscaler &&
                State::Instance().activeFgOutput != FGOutput::NoFG &&
                State::Instance().activeFgInput != FGInput::NvngxFG && !DxvkFrameGenerationViaVulkan())
            {
                const bool genshinChild = State::Instance().activeFgOutput == FGOutput::DLSSG &&
                    GenshinFsr::IsGenshinExecutable(Util::ExePath().wstring());
                auto visibleChild = genshinChild && localDesc.Windowed
                    ? GenshinVisibleChild::Create(localDesc.OutputWindow) : nullptr;
                const HWND fgHwnd = genshinChild ? (visibleChild ? visibleChild->Child() : nullptr) : localDesc.OutputWindow;
                auto hiddenHwnd = genshinChild ? localDesc.OutputWindow : CreateHiddenSwapchainWindow();

                ID3D12Device* dx12Device = nullptr;
                ID3D12CommandQueue* dx12Queue = nullptr;

                if (WithDx12::PrepareD3D12ForD3D11(device, D3D_FEATURE_LEVEL_11_0))
                {
                    dx12Device = WithDx12::GetD3D12Device();
                    dx12Queue = WithDx12::GetD3D12CommandQueue();
                }

                if (hiddenHwnd != nullptr && (genshinChild || (dx12Device != nullptr && dx12Queue != nullptr)))
                {
                    DXGI_SWAP_CHAIN_DESC realDesc = localDesc;
                    realDesc.OutputWindow = hiddenHwnd;
                    if (!genshinChild) realDesc.Windowed = TRUE;

                    IDXGISwapChain* realDx11SwapChain = nullptr;
                    HRESULT realScResult = E_FAIL;
                    {
                        ScopedSkipParentWrapping skipParentWrapping {};
                        realScResult = o_CreateSwapChain(realFactory, pDevice, &realDesc, &realDx11SwapChain);
                    }

                    DXGI_SWAP_CHAIN_DESC nativeActual{};
                    ComPtr<ID3D11Device> nativeDevice;
                    const bool nativeReady=realDx11SwapChain && SUCCEEDED(realScResult) &&
                        SUCCEEDED(realDx11SwapChain->GetDesc(&nativeActual)) && nativeActual.OutputWindow==hiddenHwnd &&
                        SUCCEEDED(realDx11SwapChain->GetDevice(IID_PPV_ARGS(&nativeDevice))) &&
                        GenshinPresentNr::Identity(nativeDevice.Get())==GenshinPresentNr::Identity(device);
                    DXGI_SWAP_CHAIN_DESC fgDesc = localDesc;
                    if (genshinChild)
                    {
                        DXGI_SWAP_CHAIN_DESC actual{};
                        if (realDx11SwapChain && SUCCEEDED(realDx11SwapChain->GetDesc(&actual)))
                        {
                            fgDesc.BufferDesc.Width = actual.BufferDesc.Width;
                            fgDesc.BufferDesc.Height = actual.BufferDesc.Height;
                        }
                        fgDesc.OutputWindow = fgHwnd;
                        fgDesc.Windowed = TRUE;
                        fgDesc.Flags &= ~DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
                    }
                    HRESULT fgScResult = E_FAIL;
                    IDXGISwapChain* fgSwapChain = nullptr;
                    IDXGISwapChain4* fgSwapChain4 = nullptr;
                    bool fgSwapChainIsRealFG = false;
                    bool sourceFrameLatencyOwner = false;
                    const bool tearingSupported = IsTearingSupported(realFactory);

                    if (SUCCEEDED(realScResult) && (!genshinChild || nativeReady) && dx12Device && dx12Queue && fgHwnd &&
                        PrepareDx12InteropDesc(fgDesc, tearingSupported))
                    {
                        {
                            ScopedSkipFGSCCreation skipFGSCCreation {};
                            fgScResult = FGHooks::CreateSwapChain(realFactory, dx12Queue, &fgDesc, &fgSwapChain,
                                                                        &sourceFrameLatencyOwner);
                            fgSwapChainIsRealFG = SUCCEEDED(fgScResult) && fgSwapChain != nullptr;
                        }

                        if (!genshinChild && (FAILED(fgScResult) || fgSwapChain == nullptr))
                        {
                            fgSwapChainIsRealFG = false;

                            LOG_WARN("Dx11wDx12 FG swapchain creation failed: {:X}; creating plain DX12 swapchain",
                                     (UINT) fgScResult);

                            ScopedSkipParentWrapping skipParentWrapping {};
                            fgScResult = o_CreateSwapChain(realFactory, dx12Queue, &fgDesc, &fgSwapChain);
                        }

                        if (SUCCEEDED(fgScResult) && fgSwapChain != nullptr)
                            fgSwapChain->QueryInterface(IID_PPV_ARGS(&fgSwapChain4));
                    }

                    // The game-facing success is based on the actual native chain.
                    // Child admission additionally needs current provider/route + real QI.
                    const bool childReady = fgScResult==S_OK && fgSwapChain4 && fgSwapChainIsRealFG &&
                        State::Instance().activeFgInput==FGInput::Upscaler && State::Instance().activeFgOutput==FGOutput::DLSSG &&
                        dynamic_cast<DLSSG_Dx12*>(State::Instance().currentFG) &&
                        GenshinPresentNr::Identity(State::Instance().currentFGSwapchain)==GenshinPresentNr::Identity(fgSwapChain4);
                    if (genshinChild && !childReady && fgSwapChain4)
                    { fgSwapChain4->Release(); fgSwapChain4=nullptr; }
                    if (genshinChild ? nativeReady : (SUCCEEDED(realScResult) && realDx11SwapChain && fgSwapChain4))
                    {
                        State::Instance().currentSwapchainDesc = genshinChild ? nativeActual : fgDesc;
                        State::Instance().currentRealSwapchain = realDx11SwapChain;
                        State::Instance().currentFGSwapchain = fgSwapChain4;
                        State::Instance().currentD3D11Device = device;
                        State::Instance().currentD3D12Device = WithDx12::GetD3D12Device();
                        State::Instance().currentCommandQueue = WithDx12::GetD3D12CommandQueue();
                        State::Instance().swapchainInteropApi = SwapchainInteropApi::Dx11wDx12;

                        if (!fgSwapChainIsRealFG && fgSwapChain4 != nullptr)
                            FGHooks::SetDx12InteropPresentSC(fgSwapChain4, localDesc.OutputWindow);

                        *ppSwapChain = new Dx11wDx12SC(realDx11SwapChain, fgSwapChain4, device, localDesc.OutputWindow,
                                                       localDesc.Flags, std::move(visibleChild), genshinChild,
                                                       fgSwapChainIsRealFG && sourceFrameLatencyOwner);

                        State::Instance().currentSwapchain = *ppSwapChain;
                        State::Instance().currentWrappedSwapchain = *ppSwapChain;

                        LOG_INFO("Created Dx11wDx12SC: wrapper {:X}, real11 {:X}, fg12 {:X}", (size_t) *ppSwapChain,
                                 (size_t) realDx11SwapChain, (size_t) fgSwapChain4);

                        realDx11SwapChain->Release();
                        if (fgSwapChain4) fgSwapChain4->Release();
                        if (fgSwapChain != nullptr)
                            fgSwapChain->Release();
                        device->Release();
                        return S_OK;
                    }

                    LOG_WARN("Dx11wDx12 swapchain creation failed: real {:X}, fg {:X}", (UINT) realScResult,
                             (UINT) fgScResult);

                    if (realDx11SwapChain != nullptr)
                        realDx11SwapChain->Release();
                    if (fgSwapChain4 != nullptr)
                        fgSwapChain4->Release();
                    if (fgSwapChain != nullptr)
                        fgSwapChain->Release();
                }
            }

            device->Release();
        }
    }

    HRESULT result = E_FAIL;

    // If FG is disabled or call is coming from FG library
    // Create the DXGI SwapChain and wrap it
    if (_skipFGSwapChainCreation || FGSCResult != S_OK)
    {
        // !_skipFGSwapChainCreation for preventing early enablement flags
        if (!_skipFGSwapChainCreation)
        {
            State::Instance().skipDxgiLoadChecks = true;

            if (Config::Instance()->FGDontUseSwapchainBuffers.value_or_default())
                State::Instance().skipHeapCapture = true;
        }

        {
            ScopedSkipParentWrapping skipParentWrapping {};
            result = o_CreateSwapChain(realFactory, pDevice, &localDesc, ppSwapChain);
        }

        if (!_skipFGSwapChainCreation)
        {
            if (Config::Instance()->FGDontUseSwapchainBuffers.value_or_default())
                State::Instance().skipHeapCapture = false;

            State::Instance().skipDxgiLoadChecks = false;
        }

        if (SwapchainCreate::Created(result, ppSwapChain))
        {
            if (sizeToWindow)
            {
                // Keep the dimensions actually created by DXGI authoritative for menu sizing.
                DXGI_SWAP_CHAIN_DESC resolvedDesc {};
                if ((*ppSwapChain)->GetDesc(&resolvedDesc) == S_OK)
                {
                    LOG_INFO("CreateSwapChain: resolved size-to-window swapchain to {}x{}",
                             resolvedDesc.BufferDesc.Width, resolvedDesc.BufferDesc.Height);
                    localDesc.BufferDesc.Width = resolvedDesc.BufferDesc.Width;
                    localDesc.BufferDesc.Height = resolvedDesc.BufferDesc.Height;
                }
            }

            State::Instance().currentSwapchainDesc = localDesc;
            SetPlainSwapchainInterop();

            // Check for SL proxy
            IDXGISwapChain* realSC = nullptr;
            if (!Util::CheckForRealObject(__FUNCTION__, *ppSwapChain, (IUnknown**) &realSC))
                realSC = *ppSwapChain;

            State::Instance().currentRealSwapchain = realSC;

            IUnknown* realDevice = nullptr;
            if (!Util::CheckForRealObject(__FUNCTION__, pDevice, (IUnknown**) &realDevice))
                realDevice = pDevice;

            if (Util::GetProcessWindow() == localDesc.OutputWindow)
            {
                State::Instance().screenWidth = static_cast<float>(localDesc.BufferDesc.Width);
                State::Instance().screenHeight = static_cast<float>(localDesc.BufferDesc.Height);
            }

            NoteCreateStatus(result, *ppSwapChain, State::Instance().currentFGSwapchain != nullptr
                ? State::Instance().currentFGSwapchain : (!_skipFGSwapChainCreation ? realSC : nullptr));

            LOG_DEBUG("Created new swapchain: {0:X}, hWnd: {1:X}", (UINT64) *ppSwapChain,
                      (UINT64) localDesc.OutputWindow);

            WrappedIDXGISwapChain4* wrapped;
            if ((*ppSwapChain)->QueryInterface(IID_PPV_ARGS(&wrapped)) != S_OK)
            {
                *ppSwapChain =
                    new WrappedIDXGISwapChain4(realSC, realDevice, localDesc.OutputWindow, localDesc.Flags, false);

                // Set as currentSwapchain is FG is disabled
                if (!_skipFGSwapChainCreation)
                    State::Instance().currentSwapchain = *ppSwapChain;

                State::Instance().currentWrappedSwapchain = *ppSwapChain;

                LOG_DEBUG("Created new WrappedIDXGISwapChain4: {:X}, pDevice: {:X}", (size_t) *ppSwapChain,
                          (size_t) pDevice);
            }
            else
            {
                wrapped->Release();
            }
        }
    }
    else
    {
        LOG_ERROR("CreateSwapChain failed: {:X}", (UINT) result);
    }

    return result;
}

HRESULT DxgiFactoryHooks::CreateSwapChainForHwnd(IDXGIFactory2* realFactory, IUnknown* pDevice, HWND hWnd,
                                                 const DXGI_SWAP_CHAIN_DESC1* pDesc,
                                                 const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc,
                                                 IDXGIOutput* pRestrictToOutput, IDXGISwapChain1** ppSwapChain)
{
    *ppSwapChain = nullptr;

    static bool firstCall = static_cast<bool>(State::Instance().gameQuirks & GameQuirk::NoFSRFGFirstSwapchain);
    if (firstCall)
    {
        LOG_DEBUG("Skipping FG swapchain creation");
        _skipFGSwapChainCreation = true;
    }

    if (State::Instance().vulkanCreatingSC)
    {
        LOG_WARN("Vulkan is creating swapchain!");
        HRESULT result;

        {
            ScopedSkipDxgiLoadChecks skipDxgiLoadChecks {};
            ScopedSkipParentWrapping skipParentWrapping {};

            result = o_CreateSwapChainForHwnd(realFactory, pDevice, hWnd, pDesc, pFullscreenDesc, pRestrictToOutput,
                                              ppSwapChain);
        }

        if (firstCall)
            _skipFGSwapChainCreation = false;

        return result;
    }

    if (pDevice == nullptr || pDesc == nullptr)
    {
        LOG_WARN("pDevice or pDesc is nullptr!");
        HRESULT result;

        {
            ScopedSkipDxgiLoadChecks skipDxgiLoadChecks {};
            ScopedSkipParentWrapping skipParentWrapping {};
            result = o_CreateSwapChainForHwnd(realFactory, pDevice, hWnd, pDesc, pFullscreenDesc, pRestrictToOutput,
                                              ppSwapChain);
        }

        if (firstCall)
            _skipFGSwapChainCreation = false;

        return result;
    }

    if (pDesc->Height < 100 || pDesc->Width < 100)
    {
        LOG_WARN("Overlay call! Width: {}, Height: {}", pDesc->Width, pDesc->Height);
        HRESULT result;

        {
            ScopedSkipDxgiLoadChecks skipDxgiLoadChecks {};
            ScopedSkipParentWrapping skipParentWrapping {};
            result = o_CreateSwapChainForHwnd(realFactory, pDevice, hWnd, pDesc, pFullscreenDesc, pRestrictToOutput,
                                              ppSwapChain);
        }

        if (firstCall)
            _skipFGSwapChainCreation = false;

        return result;
    }

    DXGI_SWAP_CHAIN_DESC1 localDesc {};
    memcpy(&localDesc, pDesc, sizeof(DXGI_SWAP_CHAIN_DESC1));

    LOG_DEBUG("Width: {}, Height: {}, Format: {}, Count: {}, Flags: {:X}, Hwnd: {:X}, SkipWrapping: {}",
              localDesc.Width, localDesc.Height, (UINT) localDesc.Format, localDesc.BufferCount, localDesc.Flags,
              (size_t) hWnd, _skipFGSwapChainCreation);

    if (pFullscreenDesc != nullptr)
        State::Instance().realExclusiveFullscreen = !pFullscreenDesc->Windowed;

    DXGI_SWAP_CHAIN_FULLSCREEN_DESC localFullscreenDesc {};

    if (pFullscreenDesc != nullptr)
        memcpy(&localFullscreenDesc, pFullscreenDesc, sizeof(DXGI_SWAP_CHAIN_FULLSCREEN_DESC));

    if (State::Instance().activeFgOutput == FGOutput::XeFG &&
        Config::Instance()->FGXeFGForceBorderless.value_or_default())
    {
        if (pFullscreenDesc != nullptr && !localFullscreenDesc.Windowed)
        {

            State::Instance().SCExclusiveFullscreen = true;
            localFullscreenDesc.Windowed = true;
        }

        localDesc.Flags &= ~DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
        localDesc.Scaling = DXGI_SCALING_STRETCH;
    }

    // For vsync override
    if (pFullscreenDesc != nullptr && !localFullscreenDesc.Windowed)
    {
        LOG_INFO("Game is creating fullscreen swapchain, disabled V-Sync overrides");
        Config::Instance()->OverrideVsync.set_volatile_value(false);
    }

    if (Config::Instance()->OverrideVsync.value_or_default())
    {
        localDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        localDesc.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

        if (localDesc.BufferCount < 2)
            localDesc.BufferCount = 2;
    }

    State::Instance().SCAllowTearing = (localDesc.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) > 0;
    State::Instance().SCLastFlags = localDesc.Flags;
    State::Instance().realExclusiveFullscreen = pFullscreenDesc != nullptr && !localFullscreenDesc.Windowed;

#ifdef VER_PRE_RELEASE
    LOG_TRACE("localDesc.AlphaMode : {}", magic_enum::enum_name(localDesc.AlphaMode));
    LOG_TRACE("localDesc.BufferCount : {}", localDesc.BufferCount);
    LOG_TRACE("localDesc.BufferUsage : {}", localDesc.BufferUsage);
    LOG_TRACE("localDesc.Flags : {}", localDesc.Flags);
    LOG_TRACE("localDesc.Format : {}", magic_enum::enum_name(localDesc.Format));
    LOG_TRACE("localDesc.Height : {}", localDesc.Height);
    LOG_TRACE("localDesc.SampleDesc.Count : {}", localDesc.SampleDesc.Count);
    LOG_TRACE("localDesc.SampleDesc.Quality : {}", localDesc.SampleDesc.Quality);
    LOG_TRACE("localDesc.Scaling : {}", magic_enum::enum_name(localDesc.Scaling));
    LOG_TRACE("localDesc.Stereo : {}", localDesc.Stereo);

    if (pFullscreenDesc != nullptr)
    {
        LOG_TRACE("localFullscreenDesc.RefreshRate.Denominator : {}", localFullscreenDesc.RefreshRate.Denominator);
        LOG_TRACE("localFullscreenDesc.RefreshRate.Numerator : {}", localFullscreenDesc.RefreshRate.Numerator);
        LOG_TRACE("localFullscreenDesc.Scaling : {}", magic_enum::enum_name(localFullscreenDesc.Scaling));
        LOG_TRACE("localFullscreenDesc.ScanlineOrdering : {}",
                  magic_enum::enum_name(localFullscreenDesc.ScanlineOrdering));
        LOG_TRACE("localFullscreenDesc.Windowed : {}", localFullscreenDesc.Windowed);
    }
#endif

    // Check for SL proxy, get real queue
    ID3D12CommandQueue* cq = nullptr;
    IUnknown* real = nullptr;
    HRESULT FGSCResult = E_NOTIMPL;

    if (pDevice->QueryInterface(IID_PPV_ARGS(&cq)) == S_OK)
    {
        if (State::Instance().currentD3D12Device == nullptr)
        {
            ID3D12Device* device = nullptr;
            if (cq->GetDevice(IID_PPV_ARGS(&device)) == S_OK)
            {
                if (device != nullptr)
                {
                    // Update current D3D12 device
                    if (State::Instance().currentD3D12Device != device)
                    {
                        State::Instance().currentD3D12Device = device;
                    }

                    LOG_INFO("Captured D3D12 device from command queue: {:X}", (UINT64) device);
                    D3D12Hooks::HookDevice(State::Instance().currentD3D12Device);
                }
            }
        }

        if (!Util::CheckForRealObject(__FUNCTION__, cq, &real))
            real = cq;

        State::Instance().currentCommandQueue = (ID3D12CommandQueue*) real;

        if (State::Instance().currentD3D12Device != nullptr)
        {
            WithDx12::SetD3D12Objects(State::Instance().currentD3D12Device, State::Instance().currentCommandQueue,
                                      D3D12_COMMAND_LIST_TYPE_DIRECT);
        }

        // Create FG SwapChain
        if (!_skipFGSwapChainCreation)
        {
            ScopedSkipFGSCCreation skipFGSCCreation {};
            FGSCResult = FGHooks::CreateSwapChainForHwnd(realFactory, real, hWnd, &localDesc,
                                                         pFullscreenDesc != nullptr ? &localFullscreenDesc : nullptr,
                                                         pRestrictToOutput, ppSwapChain);

            if (FGSCResult == S_OK)
            {
                ((IDXGISwapChain*) *ppSwapChain)->GetDesc(&State::Instance().currentSwapchainDesc);
                return FGSCResult;
            }
        }
    }
    else
    {
        LOG_INFO("Failed to get ID3D12CommandQueue from pDevice, creating Dx11 swapchain!");

        ID3D11Device* device = nullptr;

        if (pDevice->QueryInterface(IID_PPV_ARGS(&device)) == S_OK)
        {
            D3D11Hooks::HookToDevice(device);
            State::Instance().currentD3D11Device = device;

            if (!_skipFGSwapChainCreation && State::Instance().activeFgInput == FGInput::Upscaler &&
                State::Instance().activeFgOutput != FGOutput::NoFG &&
                State::Instance().activeFgInput != FGInput::NvngxFG && !DxvkFrameGenerationViaVulkan())
            {
                // For dx11 swapchain
                const bool genshinChild = State::Instance().activeFgOutput == FGOutput::DLSSG &&
                    GenshinFsr::IsGenshinExecutable(Util::ExePath().wstring());
                const bool windowed = !pFullscreenDesc || localFullscreenDesc.Windowed;
                auto visibleChild = genshinChild && windowed ? GenshinVisibleChild::Create(hWnd) : nullptr;
                const HWND fgHwnd = genshinChild ? (visibleChild ? visibleChild->Child() : nullptr) : hWnd;
                auto hiddenHwnd = genshinChild ? hWnd : CreateHiddenSwapchainWindow();

                ID3D12Device* dx12Device = nullptr;
                ID3D12CommandQueue* dx12Queue = nullptr;

                if (WithDx12::PrepareD3D12ForD3D11(device, D3D_FEATURE_LEVEL_11_0))
                {
                    dx12Device = WithDx12::GetD3D12Device();
                    dx12Queue = WithDx12::GetD3D12CommandQueue();
                }

                if (hiddenHwnd != nullptr && (genshinChild || (dx12Device != nullptr && dx12Queue != nullptr)))
                {
                    DXGI_SWAP_CHAIN_DESC1 realDesc = localDesc;
                    IDXGISwapChain1* realDx11SwapChain1 = nullptr;
                    HRESULT realScResult = E_FAIL;
                    {
                        ScopedSkipParentWrapping skipParentWrapping {};
                        realScResult = o_CreateSwapChainForHwnd(realFactory, pDevice, hiddenHwnd, &realDesc,
                            genshinChild && pFullscreenDesc ? &localFullscreenDesc : nullptr,
                                                                pRestrictToOutput, &realDx11SwapChain1);
                    }

                    DXGI_SWAP_CHAIN_DESC nativeActual{};
                    ComPtr<ID3D11Device> nativeDevice;
                    const bool nativeReady=realDx11SwapChain1 && realScResult == S_OK &&
                        SUCCEEDED(realDx11SwapChain1->GetDesc(&nativeActual)) && nativeActual.OutputWindow==hiddenHwnd &&
                        SUCCEEDED(realDx11SwapChain1->GetDevice(IID_PPV_ARGS(&nativeDevice))) &&
                        GenshinPresentNr::Identity(nativeDevice.Get())==GenshinPresentNr::Identity(device);
                    DXGI_SWAP_CHAIN_DESC1 fgDesc = localDesc;
                    if (genshinChild)
                    {
                        DXGI_SWAP_CHAIN_DESC1 actual{};
                        if (realDx11SwapChain1 && SUCCEEDED(realDx11SwapChain1->GetDesc1(&actual)))
                        { fgDesc.Width = actual.Width; fgDesc.Height = actual.Height; }
                        fgDesc.Flags &= ~DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
                    }
                    HRESULT fgScResult = E_FAIL;
                    IDXGISwapChain1* fgSwapChain1 = nullptr;
                    IDXGISwapChain4* fgSwapChain4 = nullptr;
                    bool fgSwapChainIsRealFG = false;
                    bool sourceFrameLatencyOwner = false;
                    const bool tearingSupported = IsTearingSupported(realFactory);

                    if (realScResult == S_OK && (!genshinChild || nativeReady) && dx12Device && dx12Queue && fgHwnd &&
                        PrepareDx12InteropDesc1(fgDesc, tearingSupported))
                    {
                        {
                            ScopedSkipFGSCCreation skipFGSCCreation {};
                            fgScResult = FGHooks::CreateSwapChainForHwnd(
                                realFactory, dx12Queue, fgHwnd, &fgDesc,
                                !genshinChild && pFullscreenDesc != nullptr ? &localFullscreenDesc : nullptr, pRestrictToOutput,
                                &fgSwapChain1, &sourceFrameLatencyOwner);

                            fgSwapChainIsRealFG = fgScResult == S_OK && fgSwapChain1 != nullptr;
                        }

                        if (!genshinChild && (fgScResult != S_OK || fgSwapChain1 == nullptr))
                        {
                            fgSwapChainIsRealFG = false;

                            LOG_WARN("Dx11wDx12 FG swapchain creation failed: {:X}; creating plain DX12 swapchain",
                                     (UINT) fgScResult);

                            ScopedSkipParentWrapping skipParentWrapping {};
                            fgScResult =
                                o_CreateSwapChainForHwnd(realFactory, dx12Queue, hWnd, &fgDesc,
                                                         pFullscreenDesc != nullptr ? &localFullscreenDesc : nullptr,
                                                         pRestrictToOutput, &fgSwapChain1);
                        }

                        if (fgScResult == S_OK && fgSwapChain1 != nullptr)
                            fgSwapChain1->QueryInterface(IID_PPV_ARGS(&fgSwapChain4));
                    }

                    // The game-facing success is based on the actual native chain.
                    // Child admission additionally needs current provider/route + real QI.
                    const bool childReady = fgScResult==S_OK && fgSwapChain4 && fgSwapChainIsRealFG &&
                        State::Instance().activeFgInput==FGInput::Upscaler && State::Instance().activeFgOutput==FGOutput::DLSSG &&
                        dynamic_cast<DLSSG_Dx12*>(State::Instance().currentFG) &&
                        GenshinPresentNr::Identity(State::Instance().currentFGSwapchain)==GenshinPresentNr::Identity(fgSwapChain4);
                    if (genshinChild && !childReady && fgSwapChain4)
                    { fgSwapChain4->Release(); fgSwapChain4=nullptr; }
                    if (genshinChild ? nativeReady : (realScResult == S_OK && realDx11SwapChain1 && fgSwapChain4))
                    {
                        if (genshinChild) realDx11SwapChain1->GetDesc(&State::Instance().currentSwapchainDesc);
                        else ((IDXGISwapChain*) fgSwapChain4)->GetDesc(&State::Instance().currentSwapchainDesc);
                        State::Instance().currentSwapchainDesc.OutputWindow = hWnd;
                        State::Instance().currentRealSwapchain = realDx11SwapChain1;
                        State::Instance().currentFGSwapchain = fgSwapChain4;
                        State::Instance().currentD3D11Device = device;
                        State::Instance().currentD3D12Device = WithDx12::GetD3D12Device();
                        State::Instance().currentCommandQueue = WithDx12::GetD3D12CommandQueue();
                        State::Instance().swapchainInteropApi = SwapchainInteropApi::Dx11wDx12;

                        if (!fgSwapChainIsRealFG && fgSwapChain4 != nullptr)
                            FGHooks::SetDx12InteropPresentSC((IDXGISwapChain*) fgSwapChain4, hWnd);

                        *ppSwapChain = (IDXGISwapChain1*) new Dx11wDx12SC(realDx11SwapChain1, fgSwapChain4, device,
                                                                          hWnd, localDesc.Flags, std::move(visibleChild), genshinChild,
                                                                          fgSwapChainIsRealFG && sourceFrameLatencyOwner);

                        State::Instance().currentSwapchain = *ppSwapChain;
                        State::Instance().currentWrappedSwapchain = *ppSwapChain;

                        LOG_INFO("Created Dx11wDx12SC HWND: wrapper {:X}, real11 {:X}, fg12 {:X}",
                                 (size_t) *ppSwapChain, (size_t) realDx11SwapChain1, (size_t) fgSwapChain4);

                        realDx11SwapChain1->Release();
                        if (fgSwapChain4) fgSwapChain4->Release();

                        if (fgSwapChain1 != nullptr)
                            fgSwapChain1->Release();

                        device->Release();
                        return S_OK;
                    }

                    LOG_WARN("Dx11wDx12 HWND swapchain creation failed: real {:X}, fg {:X}", (UINT) realScResult,
                             (UINT) fgScResult);

                    if (realDx11SwapChain1 != nullptr)
                        realDx11SwapChain1->Release();
                    if (fgSwapChain4 != nullptr)
                        fgSwapChain4->Release();
                    if (fgSwapChain1 != nullptr)
                        fgSwapChain1->Release();
                }
            }

            // Legacy DX11 FG path intentionally removed.
            // DX11 FG must now go through Dx11wDx12SC; if interop creation failed, fall back to the normal wrapper path
            // below.

            device->Release();
        }
    }

    HRESULT result = E_FAIL;

    // If FG is disabled or call is coming from FG library
    // Create the DXGI SwapChain and wrap it
    if (_skipFGSwapChainCreation || FGSCResult != S_OK)
    {

        // !_skipFGSwapChainCreation for preventing early enablement flags
        if (!_skipFGSwapChainCreation)
        {
            State::Instance().skipDxgiLoadChecks = true;

            if (Config::Instance()->FGDontUseSwapchainBuffers.value_or_default())
                State::Instance().skipHeapCapture = true;
        }

        {
            ScopedSkipParentWrapping skipParentWrapping {};
            result = o_CreateSwapChainForHwnd(realFactory, pDevice, hWnd, &localDesc,
                                              pFullscreenDesc != nullptr ? &localFullscreenDesc : nullptr,
                                              pRestrictToOutput, ppSwapChain);
        }

        if (!_skipFGSwapChainCreation)
        {
            if (Config::Instance()->FGDontUseSwapchainBuffers.value_or_default())
                State::Instance().skipHeapCapture = false;

            State::Instance().skipDxgiLoadChecks = false;
        }

        if (SwapchainCreate::Created(result, ppSwapChain))
        {
            SetPlainSwapchainInterop();

            // check for SL proxy
            IDXGISwapChain1* realSC = nullptr;
            if (!Util::CheckForRealObject(__FUNCTION__, *ppSwapChain, (IUnknown**) &realSC))
                realSC = *ppSwapChain;

            State::Instance().currentRealSwapchain = realSC;

            IUnknown* readDevice = nullptr;
            if (!Util::CheckForRealObject(__FUNCTION__, pDevice, (IUnknown**) &readDevice))
                readDevice = pDevice;

            if (Util::GetProcessWindow() == hWnd)
            {
                State::Instance().screenWidth = static_cast<float>(localDesc.Width);
                State::Instance().screenHeight = static_cast<float>(localDesc.Height);
            }

            realSC->GetDesc(&State::Instance().currentSwapchainDesc);

            NoteCreateStatus(result, *ppSwapChain, State::Instance().currentFGSwapchain != nullptr
                ? State::Instance().currentFGSwapchain : (!_skipFGSwapChainCreation ? realSC : nullptr));

            LOG_DEBUG("Created new swapchain: {0:X}, hWnd: {1:X}", (uintptr_t) *ppSwapChain, (uintptr_t) hWnd);

            WrappedIDXGISwapChain4* wrapped;
            if ((*ppSwapChain)->QueryInterface(IID_PPV_ARGS(&wrapped)) != S_OK)
            {
                *ppSwapChain = new WrappedIDXGISwapChain4(realSC, readDevice, hWnd, localDesc.Flags, false);

                LOG_DEBUG("Created new WrappedIDXGISwapChain4: {0:X}, pDevice: {1:X}", (uintptr_t) *ppSwapChain,
                          (uintptr_t) pDevice);

                if (!_skipFGSwapChainCreation)
                    State::Instance().currentSwapchain = *ppSwapChain;

                State::Instance().currentWrappedSwapchain = *ppSwapChain;
            }
            else
            {
                wrapped->Release();
            }
        }
        else
        {
            LOG_ERROR("CreateSwapChainForHwnd failed: {:X}", (UINT) result);
        }
    }

    if (firstCall)
    {
        LOG_DEBUG("Unsetting skip FG swapchain creation");
        _skipFGSwapChainCreation = false;
        firstCall = false;
    }

    return result;
}

HRESULT DxgiFactoryHooks::CreateSwapChainForCoreWindow(IDXGIFactory2* realFactory, IUnknown* pDevice, IUnknown* pWindow,
                                                       const DXGI_SWAP_CHAIN_DESC1* pDesc,
                                                       IDXGIOutput* pRestrictToOutput, IDXGISwapChain1** ppSwapChain)
{
    if (State::Instance().vulkanCreatingSC)
    {
        LOG_WARN("Vulkan is creating swapchain!");

        if (pDesc != nullptr)
            LOG_DEBUG("Width: {}, Height: {}, Format: {}, Flags: {:X}, Count: {}, SkipWrapping: {}", pDesc->Width,
                      pDesc->Height, (UINT) pDesc->Format, pDesc->Flags, pDesc->BufferCount, _skipFGSwapChainCreation);

        ScopedSkipDxgiLoadChecks skipDxgiLoadChecks {};
        return realFactory->CreateSwapChainForCoreWindow(pDevice, pWindow, pDesc, pRestrictToOutput, ppSwapChain);
    }

    if (pDevice == nullptr || pDesc == nullptr)
    {
        LOG_WARN("pDevice or pDesc is nullptr!");
        ScopedSkipDxgiLoadChecks skipDxgiLoadChecks {};
        return realFactory->CreateSwapChainForCoreWindow(pDevice, pWindow, pDesc, pRestrictToOutput, ppSwapChain);
    }

    if (pDesc->Height < 100 || pDesc->Width < 100)
    {
        LOG_WARN("Overlay call! Width: {}, Height: {}", pDesc->Width, pDesc->Height);

        ScopedSkipDxgiLoadChecks skipDxgiLoadChecks {};
        return realFactory->CreateSwapChainForCoreWindow(pDevice, pWindow, pDesc, pRestrictToOutput, ppSwapChain);
    }

    DXGI_SWAP_CHAIN_DESC1 localDesc {};
    memcpy(&localDesc, pDesc, sizeof(DXGI_SWAP_CHAIN_DESC1));

    LOG_DEBUG("Width: {}, Height: {}, Format: {}, Count: {}, SkipWrapping: {}", localDesc.Width, localDesc.Height,
              (UINT) localDesc.Format, localDesc.BufferCount, _skipFGSwapChainCreation);

    // For vsync override
    if (Config::Instance()->OverrideVsync.value_or_default())
    {
        localDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        localDesc.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

        if (localDesc.BufferCount < 2)
            localDesc.BufferCount = 2;
    }

    State::Instance().SCAllowTearing = (localDesc.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) > 0;
    State::Instance().SCLastFlags = localDesc.Flags;
    State::Instance().realExclusiveFullscreen = false;

    ID3D12CommandQueue* cq = nullptr;
    IUnknown* real = nullptr;
    if (pDevice->QueryInterface(IID_PPV_ARGS(&cq)) == S_OK)
    {
        if (!Util::CheckForRealObject(__FUNCTION__, cq, &real))
            real = cq;

        State::Instance().currentCommandQueue = (ID3D12CommandQueue*) real;

        if (State::Instance().currentD3D12Device != nullptr)
            WithDx12::SetD3D12Objects(State::Instance().currentD3D12Device, State::Instance().currentCommandQueue,
                                      D3D12_COMMAND_LIST_TYPE_DIRECT);
    }

    HRESULT result = E_FAIL;
    {
        ScopedSkipDxgiLoadChecks skipDxgiLoadChecks {};
        result =
            o_CreateSwapChainForCoreWindow(realFactory, pDevice, pWindow, &localDesc, pRestrictToOutput, ppSwapChain);
    }

    if (SwapchainCreate::Created(result, ppSwapChain))
    {
        // check for SL proxy
        IDXGISwapChain* realSC = nullptr;
        if (!Util::CheckForRealObject(__FUNCTION__, *ppSwapChain, (IUnknown**) &realSC))
            realSC = *ppSwapChain;

        State::Instance().currentRealSwapchain = realSC;

        IUnknown* readDevice = nullptr;
        if (!Util::CheckForRealObject(__FUNCTION__, pDevice, (IUnknown**) &readDevice))
            readDevice = pDevice;

        realSC->GetDesc(&State::Instance().currentSwapchainDesc);

        State::Instance().screenWidth = static_cast<float>(localDesc.Width);
        State::Instance().screenHeight = static_cast<float>(localDesc.Height);

        NoteCreateStatus(result, *ppSwapChain, State::Instance().currentFGSwapchain != nullptr
            ? State::Instance().currentFGSwapchain : (!_skipFGSwapChainCreation ? realSC : nullptr));

        LOG_DEBUG("Created new swapchain: {0:X}, hWnd: {1:X}", (UINT64) *ppSwapChain, (UINT64) pWindow);

        WrappedIDXGISwapChain4* wrapped;
        if ((*ppSwapChain)->QueryInterface(IID_PPV_ARGS(&wrapped)) != S_OK)
        {
            *ppSwapChain = new WrappedIDXGISwapChain4(realSC, readDevice, (HWND) pWindow, localDesc.Flags, true);

            if (!_skipFGSwapChainCreation)
                State::Instance().currentSwapchain = *ppSwapChain;

            State::Instance().currentWrappedSwapchain = *ppSwapChain;

            LOG_DEBUG("Created new WrappedIDXGISwapChain4: {0:X}, pDevice: {1:X}", (UINT64) *ppSwapChain,
                      (UINT64) pDevice);
        }
        else
        {
            wrapped->Release();
        }
    }

    return result;
}


HRESULT DxgiFactoryHooks::CreateSwapChainForComposition(IDXGIFactory2* realFactory, IUnknown* pDevice,
                                                        const DXGI_SWAP_CHAIN_DESC1* pDesc,
                                                        IDXGIOutput* pRestrictToOutput,
                                                        IDXGISwapChain1** ppSwapChain)
{
    // Always call the trampoline, including pass-through/error cases. Calling the detoured virtual
    // method here re-enters this hook. Keep the composition descriptor intact: notably, a desktop
    // VSync override must not turn its FLIP_SEQUENTIAL swap effect into FLIP_DISCARD.
    const bool passThrough = State::Instance().vulkanCreatingSC || _skipFGSwapChainCreation ||
                             pDevice == nullptr || pDesc == nullptr || ppSwapChain == nullptr ||
                             pDesc->Width < 100 || pDesc->Height < 100;
    if (pDesc != nullptr && (pDesc->Width < 100 || pDesc->Height < 100))
        LOG_WARN("Composition overlay/helper call! Width: {}, Height: {}", pDesc->Width, pDesc->Height);

    HRESULT result;
    {
        ScopedSkipDxgiLoadChecks skipDxgiLoadChecks {};
        ScopedSkipParentWrapping skipParentWrapping {};
        result = o_CreateSwapChainForComposition(realFactory, pDevice, pDesc, pRestrictToOutput, ppSwapChain);
    }
    if (passThrough || FAILED(result) || *ppSwapChain == nullptr)
        return result;

    WrappedIDXGISwapChain4* existing = nullptr;
    if ((*ppSwapChain)->QueryInterface(IID_PPV_ARGS(&existing)) == S_OK)
    {
        existing->Release();
        return result;
    }

    DXGI_SWAP_CHAIN_DESC1 resolved {};
    if (FAILED((*ppSwapChain)->GetDesc1(&resolved)))
        return result; // Do not install a wrapper with unknown dimensions/flags.

    // A composition chain has no native HWND. Use only an eligible window in this process, and
    // defer overlay initialization to Present if no unambiguous window exists yet. Do not borrow
    // another application's foreground window. HWND association remains a best-effort fallback.
    const HWND window = FindCompositionWindow();
    IDXGISwapChain1* const created = *ppSwapChain;
    auto* wrapped = new WrappedIDXGISwapChain4(created, pDevice, window, resolved.Flags, false, true);
    *ppSwapChain = wrapped;

    // Preserve the returned COM/proxy chain: the wrapper owns its original reference. Unwrapping it
    // here can leak an outer proxy and bypass another provider's presentation path.
    State::Instance().currentRealSwapchain = created;
    State::Instance().currentSwapchain = wrapped;
    State::Instance().currentWrappedSwapchain = wrapped;
    SetPlainSwapchainInterop();
    State::Instance().SCAllowTearing = (resolved.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) != 0;
    State::Instance().SCLastFlags = resolved.Flags;
    State::Instance().realExclusiveFullscreen = false;
    State::Instance().screenWidth = (float) resolved.Width;
    State::Instance().screenHeight = (float) resolved.Height;
    DXGI_SWAP_CHAIN_DESC legacy {};
    if (SUCCEEDED(created->GetDesc(&legacy)))
        State::Instance().currentSwapchainDesc = legacy;
    LOG_INFO("Wrapped composition swapchain {}x{}, window {:X}; plain presentation, no FG swapchain replacement",
             resolved.Width, resolved.Height, (SIZE_T) window);
    return result;
}

HRESULT DxgiFactoryHooks::DLSSGCreateSwapChain(IDXGIFactory* realFactory, IUnknown* pDevice,
                                               DXGI_SWAP_CHAIN_DESC* pDesc, IDXGISwapChain** ppSwapChain)
{
    *ppSwapChain = nullptr;

    if (State::Instance().vulkanCreatingSC)
    {
        LOG_WARN("Vulkan is creating swapchain!");

        if (pDesc != nullptr)
            LOG_DEBUG("Width: {}, Height: {}, Format: {}, Count: {}, Hwnd: {:X}, Windowed: {}, SkipWrapping: {}",
                      pDesc->BufferDesc.Width, pDesc->BufferDesc.Height, (UINT) pDesc->BufferDesc.Format,
                      pDesc->BufferCount, (SIZE_T) pDesc->OutputWindow, pDesc->Windowed, _skipFGSwapChainCreation);

        ScopedSkipDxgiLoadChecks skipDxgiLoadChecks {};
        ScopedSkipParentWrapping skipParentWrapping {};

        auto res = o_DLSSGCreateSwapChain(realFactory, pDevice, pDesc, ppSwapChain);
        return res;
    }

    if (pDevice == nullptr || pDesc == nullptr)
    {
        LOG_WARN("pDevice or pDesc is nullptr!");

        ScopedSkipDxgiLoadChecks skipDxgiLoadChecks {};
        ScopedSkipParentWrapping skipParentWrapping {};

        auto res = o_DLSSGCreateSwapChain(realFactory, pDevice, pDesc, ppSwapChain);
        return res;
    }

    DXGI_SWAP_CHAIN_DESC localDesc = *pDesc;
    ResolveWindowSizedSwapchain(localDesc);

    if (localDesc.BufferDesc.Height < 100 || localDesc.BufferDesc.Width < 100)
    {
        LOG_WARN("Overlay call! Width: {}, Height: {}", pDesc->BufferDesc.Width, pDesc->BufferDesc.Height);

        ScopedSkipDxgiLoadChecks skipDxgiLoadChecks {};
        ScopedSkipParentWrapping skipParentWrapping {};

        auto res = o_DLSSGCreateSwapChain(realFactory, pDevice, pDesc, ppSwapChain);
        return res;
    }

    LOG_DEBUG("Width: {}, Height: {}, Format: {}, Count: {}, Flags: {:X}, Hwnd: {:X}, Windowed: {}, SkipWrapping: {}",
              localDesc.BufferDesc.Width, localDesc.BufferDesc.Height, (UINT) localDesc.BufferDesc.Format,
              localDesc.BufferCount, localDesc.Flags, (SIZE_T) localDesc.OutputWindow, localDesc.Windowed,
              _skipFGSwapChainCreation);

    if (State::Instance().activeFgOutput == FGOutput::XeFG &&
        Config::Instance()->FGXeFGForceBorderless.value_or_default())
    {
        if (!localDesc.Windowed)
        {
            State::Instance().SCExclusiveFullscreen = true;
            localDesc.Windowed = true;
        }

        localDesc.Flags &= ~DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
        localDesc.BufferDesc.Scaling = DXGI_MODE_SCALING_STRETCHED;
    }

    // For vsync override
    if (!localDesc.Windowed)
    {
        LOG_INFO("Game is creating fullscreen swapchain, disabled V-Sync overrides");
        Config::Instance()->OverrideVsync.set_volatile_value(false);
    }

    if (Config::Instance()->OverrideVsync.value_or_default())
    {
        localDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        localDesc.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

        if (localDesc.BufferCount < 2)
            localDesc.BufferCount = 2;
    }

    State::Instance().SCAllowTearing = (localDesc.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) > 0;
    State::Instance().SCLastFlags = localDesc.Flags;
    State::Instance().realExclusiveFullscreen = !localDesc.Windowed;

#ifdef DETAILED_SC_LOGS
    LOG_TRACE("localDesc.BufferCount: {}", localDesc.BufferCount);
    LOG_TRACE("localDesc.BufferDesc.Format: {}", magic_enum::enum_name(localDesc.BufferDesc.Format));
    LOG_TRACE("localDesc.BufferDesc.Height: {}", localDesc.BufferDesc.Height);
    LOG_TRACE("localDesc.BufferDesc.RefreshRate.Denominator: {}", localDesc.BufferDesc.RefreshRate.Denominator);
    LOG_TRACE("localDesc.BufferDesc.RefreshRate.Numerator: {}", localDesc.BufferDesc.RefreshRate.Numerator);
    LOG_TRACE("localDesc.BufferDesc.Scaling: {}", magic_enum::enum_name(localDesc.BufferDesc.Scaling));
    LOG_TRACE("localDesc.BufferDesc.ScanlineOrdering: {}",
              magic_enum::enum_name(localDesc.BufferDesc.ScanlineOrdering));
    LOG_TRACE("localDesc.BufferDesc.Width: {}", localDesc.BufferDesc.Width);
    LOG_TRACE("localDesc.BufferUsage: {}", localDesc.BufferUsage);
    LOG_TRACE("localDesc.Flags: {}", localDesc.Flags);
    LOG_TRACE("localDesc.OutputWindow: {}", (UINT64) localDesc.OutputWindow);
    LOG_TRACE("localDesc.SampleDesc.Count: {}", localDesc.SampleDesc.Count);
    LOG_TRACE("localDesc.SampleDesc.Quality: {}", localDesc.SampleDesc.Quality);
    LOG_TRACE("localDesc.SwapEffect: {}", magic_enum::enum_name(localDesc.SwapEffect));
    LOG_TRACE("localDesc.Windowed: {}", localDesc.Windowed);
#endif //

    // Check for SL proxy, get real queue
    ID3D12CommandQueue* cq = nullptr;
    IUnknown* real = nullptr;
    HRESULT FGSCResult = E_NOTIMPL;

    if (pDevice->QueryInterface(IID_PPV_ARGS(&cq)) == S_OK)
    {
        if (State::Instance().currentD3D12Device == nullptr)
        {
            ID3D12Device* device = nullptr;
            if (cq->GetDevice(IID_PPV_ARGS(&device)) == S_OK)
            {
                if (device != nullptr)
                {
                    // Update current D3D12 device and adapter
                    if (State::Instance().currentD3D12Device != device)
                    {
                        State::Instance().currentD3D12Device = device;

                        IDXGIDevice* dxgiDevice = nullptr;
                        if (device->QueryInterface(IID_PPV_ARGS(&dxgiDevice)) == S_OK)
                        {
                            IDXGIAdapter* adapter = nullptr;
                            if (dxgiDevice->GetAdapter(&adapter) == S_OK)
                                adapter->Release();

                            dxgiDevice->Release();
                        }
                    }

                    LOG_INFO("Captured D3D12 device from command queue: {:X}", (UINT64) device);
                    D3D12Hooks::HookDevice(State::Instance().currentD3D12Device);
                }
            }
        }

        if (!Util::CheckForRealObject(__FUNCTION__, cq, &real))
            real = cq;

        State::Instance().currentCommandQueue = (ID3D12CommandQueue*) real;

        if (State::Instance().currentD3D12Device != nullptr)
            WithDx12::SetD3D12Objects(State::Instance().currentD3D12Device, State::Instance().currentCommandQueue,
                                      D3D12_COMMAND_LIST_TYPE_DIRECT);

        // Create FG SwapChain
        if (!_skipFGSwapChainCreation)
        {
            ScopedSkipFGSCCreation skipFGSCCreation {};
            FGSCResult = FGHooks::CreateSwapChain(realFactory, real, &localDesc, ppSwapChain);

            if (FGSCResult == S_OK)
            {
                State::Instance().currentSwapchainDesc = localDesc;
                return FGSCResult;
            }
        }
    }
    else
    {
        LOG_INFO("Failed to get ID3D12CommandQueue from pDevice, creating Dx11 swapchain!");

        ID3D11Device* device = nullptr;

        if (pDevice->QueryInterface(IID_PPV_ARGS(&device)) == S_OK)
        {
            D3D11Hooks::HookToDevice(device);
            device->Release();

            // Update current D3D11 device and adapter
            if (State::Instance().currentD3D11Device != device)
            {
                State::Instance().currentD3D11Device = device;

                IDXGIDevice* dxgiDevice = nullptr;
                if (device->QueryInterface(IID_PPV_ARGS(&dxgiDevice)) == S_OK)
                {
                    IDXGIAdapter* adapter = nullptr;
                    if (dxgiDevice->GetAdapter(&adapter) == S_OK)
                        adapter->Release();

                    dxgiDevice->Release();
                }
            }

            // Legacy DX11 FG path intentionally removed.
            // DX11 FG must now go through Dx11wDx12SC; if interop creation failed, fall back to the normal wrapper path
            // below.
        }
    }

    HRESULT result = E_FAIL;

    // If FG is disabled or call is coming from FG library
    // Create the DXGI SwapChain and wrap it
    if (_skipFGSwapChainCreation || FGSCResult != S_OK)
    {
        // !_skipFGSwapChainCreation for preventing early enablement flags
        if (!_skipFGSwapChainCreation)
        {
            State::Instance().skipDxgiLoadChecks = true;

            if (Config::Instance()->FGDontUseSwapchainBuffers.value_or_default())
                State::Instance().skipHeapCapture = true;
        }

        {
            ScopedSkipParentWrapping skipParentWrapping {};
            result = o_DLSSGCreateSwapChain(realFactory, pDevice, &localDesc, ppSwapChain);
        }

        if (!_skipFGSwapChainCreation)
        {
            if (Config::Instance()->FGDontUseSwapchainBuffers.value_or_default())
                State::Instance().skipHeapCapture = false;

            State::Instance().skipDxgiLoadChecks = false;
        }

        if (SwapchainCreate::Created(result, ppSwapChain))
        {
            State::Instance().currentSwapchainDesc = localDesc;

            // Check for SL proxy
            IDXGISwapChain* realSC = nullptr;
            if (!Util::CheckForRealObject(__FUNCTION__, *ppSwapChain, (IUnknown**) &realSC))
                realSC = *ppSwapChain;

            State::Instance().currentRealSwapchain = realSC;

            IUnknown* realDevice = nullptr;
            if (!Util::CheckForRealObject(__FUNCTION__, pDevice, (IUnknown**) &realDevice))
                realDevice = pDevice;

            if (Util::GetProcessWindow() == localDesc.OutputWindow)
            {
                State::Instance().screenWidth = static_cast<float>(localDesc.BufferDesc.Width);
                State::Instance().screenHeight = static_cast<float>(localDesc.BufferDesc.Height);
            }

            NoteCreateStatus(result, *ppSwapChain, State::Instance().currentFGSwapchain != nullptr
                ? State::Instance().currentFGSwapchain : (!_skipFGSwapChainCreation ? realSC : nullptr));

            LOG_DEBUG("Created new swapchain: {0:X}, hWnd: {1:X}", (UINT64) *ppSwapChain,
                      (UINT64) localDesc.OutputWindow);

            WrappedIDXGISwapChain4* wrapped;
            if ((*ppSwapChain)->QueryInterface(IID_PPV_ARGS(&wrapped)) != S_OK)
            {
                *ppSwapChain =
                    new WrappedIDXGISwapChain4(realSC, realDevice, localDesc.OutputWindow, localDesc.Flags, false);

                // Set as currentSwapchain is FG is disabled
                if (!_skipFGSwapChainCreation)
                    State::Instance().currentSwapchain = *ppSwapChain;

                State::Instance().currentWrappedSwapchain = *ppSwapChain;

                LOG_DEBUG("Created new WrappedIDXGISwapChain4: {:X}, pDevice: {:X}", (size_t) *ppSwapChain,
                          (size_t) pDevice);
            }
            else
            {
                wrapped->Release();
            }
        }
    }
    else
    {
        LOG_ERROR("CreateSwapChain failed: {:X}", (UINT) result);
    }

    return result;
}

HRESULT DxgiFactoryHooks::DLSSGCreateSwapChainForHwnd(IDXGIFactory2* realFactory, IUnknown* pDevice, HWND hWnd,
                                                      const DXGI_SWAP_CHAIN_DESC1* pDesc,
                                                      const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc,
                                                      IDXGIOutput* pRestrictToOutput, IDXGISwapChain1** ppSwapChain)
{
    *ppSwapChain = nullptr;

    static bool firstCall = static_cast<bool>(State::Instance().gameQuirks & GameQuirk::NoFSRFGFirstSwapchain);
    if (firstCall)
    {
        LOG_DEBUG("Skipping FG swapchain creation");
        _skipFGSwapChainCreation = true;
    }

    if (State::Instance().vulkanCreatingSC)
    {
        LOG_WARN("Vulkan is creating swapchain!");
        HRESULT result;

        {
            ScopedSkipDxgiLoadChecks skipDxgiLoadChecks {};
            ScopedSkipParentWrapping skipParentWrapping {};

            result = o_DLSSGCreateSwapChainForHwnd(realFactory, pDevice, hWnd, pDesc, pFullscreenDesc,
                                                   pRestrictToOutput, ppSwapChain);
        }

        if (firstCall)
            _skipFGSwapChainCreation = false;

        return result;
    }

    if (pDevice == nullptr || pDesc == nullptr)
    {
        LOG_WARN("pDevice or pDesc is nullptr!");
        HRESULT result;

        {
            ScopedSkipDxgiLoadChecks skipDxgiLoadChecks {};
            ScopedSkipParentWrapping skipParentWrapping {};
            result = o_DLSSGCreateSwapChainForHwnd(realFactory, pDevice, hWnd, pDesc, pFullscreenDesc,
                                                   pRestrictToOutput, ppSwapChain);
        }

        if (firstCall)
            _skipFGSwapChainCreation = false;

        return result;
    }

    if (pDesc->Height < 100 || pDesc->Width < 100)
    {
        LOG_WARN("Overlay call! Width: {}, Height: {}", pDesc->Width, pDesc->Height);
        HRESULT result;

        {
            ScopedSkipDxgiLoadChecks skipDxgiLoadChecks {};
            ScopedSkipParentWrapping skipParentWrapping {};
            result = o_DLSSGCreateSwapChainForHwnd(realFactory, pDevice, hWnd, pDesc, pFullscreenDesc,
                                                   pRestrictToOutput, ppSwapChain);
        }

        if (firstCall)
            _skipFGSwapChainCreation = false;

        return result;
    }

    DXGI_SWAP_CHAIN_DESC1 localDesc {};
    memcpy(&localDesc, pDesc, sizeof(DXGI_SWAP_CHAIN_DESC1));

    LOG_DEBUG("Width: {}, Height: {}, Format: {}, Count: {}, Flags: {:X}, Hwnd: {:X}, SkipWrapping: {}",
              localDesc.Width, localDesc.Height, (UINT) localDesc.Format, localDesc.BufferCount, localDesc.Flags,
              (size_t) hWnd, _skipFGSwapChainCreation);

    if (pFullscreenDesc != nullptr)
        State::Instance().realExclusiveFullscreen = !pFullscreenDesc->Windowed;

    DXGI_SWAP_CHAIN_FULLSCREEN_DESC localFullscreenDesc {};

    if (pFullscreenDesc != nullptr)
        memcpy(&localFullscreenDesc, pFullscreenDesc, sizeof(DXGI_SWAP_CHAIN_FULLSCREEN_DESC));

    if (State::Instance().activeFgOutput == FGOutput::XeFG &&
        Config::Instance()->FGXeFGForceBorderless.value_or_default())
    {
        if (pFullscreenDesc != nullptr && !localFullscreenDesc.Windowed)
        {

            State::Instance().SCExclusiveFullscreen = true;
            localFullscreenDesc.Windowed = true;
        }

        localDesc.Flags &= ~DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
        localDesc.Scaling = DXGI_SCALING_STRETCH;
    }

    // For vsync override
    if (pFullscreenDesc != nullptr && !localFullscreenDesc.Windowed)
    {
        LOG_INFO("Game is creating fullscreen swapchain, disabled V-Sync overrides");
        Config::Instance()->OverrideVsync.set_volatile_value(false);
    }

    if (Config::Instance()->OverrideVsync.value_or_default())
    {
        localDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        localDesc.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

        if (localDesc.BufferCount < 2)
            localDesc.BufferCount = 2;
    }

    State::Instance().SCAllowTearing = (localDesc.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) > 0;
    State::Instance().SCLastFlags = localDesc.Flags;
    State::Instance().realExclusiveFullscreen = pFullscreenDesc != nullptr && !localFullscreenDesc.Windowed;

#ifdef VER_PRE_RELEASE
    LOG_TRACE("localDesc.AlphaMode : {}", magic_enum::enum_name(localDesc.AlphaMode));
    LOG_TRACE("localDesc.BufferCount : {}", localDesc.BufferCount);
    LOG_TRACE("localDesc.BufferUsage : {}", localDesc.BufferUsage);
    LOG_TRACE("localDesc.Flags : {}", localDesc.Flags);
    LOG_TRACE("localDesc.Format : {}", magic_enum::enum_name(localDesc.Format));
    LOG_TRACE("localDesc.Height : {}", localDesc.Height);
    LOG_TRACE("localDesc.SampleDesc.Count : {}", localDesc.SampleDesc.Count);
    LOG_TRACE("localDesc.SampleDesc.Quality : {}", localDesc.SampleDesc.Quality);
    LOG_TRACE("localDesc.Scaling : {}", magic_enum::enum_name(localDesc.Scaling));
    LOG_TRACE("localDesc.Stereo : {}", localDesc.Stereo);

    if (pFullscreenDesc != nullptr)
    {
        LOG_TRACE("localFullscreenDesc.RefreshRate.Denominator : {}", localFullscreenDesc.RefreshRate.Denominator);
        LOG_TRACE("localFullscreenDesc.RefreshRate.Numerator : {}", localFullscreenDesc.RefreshRate.Numerator);
        LOG_TRACE("localFullscreenDesc.Scaling : {}", magic_enum::enum_name(localFullscreenDesc.Scaling));
        LOG_TRACE("localFullscreenDesc.ScanlineOrdering : {}",
                  magic_enum::enum_name(localFullscreenDesc.ScanlineOrdering));
        LOG_TRACE("localFullscreenDesc.Windowed : {}", localFullscreenDesc.Windowed);
    }
#endif

    // Check for SL proxy, get real queue
    ID3D12CommandQueue* cq = nullptr;
    IUnknown* real = nullptr;
    HRESULT FGSCResult = E_NOTIMPL;

    if (pDevice->QueryInterface(IID_PPV_ARGS(&cq)) == S_OK)
    {
        if (State::Instance().currentD3D12Device == nullptr)
        {
            ID3D12Device* device = nullptr;
            if (cq->GetDevice(IID_PPV_ARGS(&device)) == S_OK)
            {
                if (device != nullptr)
                {
                    // Update current D3D12 device and adapter
                    if (State::Instance().currentD3D12Device != device)
                    {
                        State::Instance().currentD3D12Device = device;

                        IDXGIDevice* dxgiDevice = nullptr;
                        if (device->QueryInterface(IID_PPV_ARGS(&dxgiDevice)) == S_OK)
                        {
                            IDXGIAdapter* adapter = nullptr;
                            if (dxgiDevice->GetAdapter(&adapter) == S_OK)
                                adapter->Release();

                            dxgiDevice->Release();
                        }
                    }

                    LOG_INFO("Captured D3D12 device from command queue: {:X}", (UINT64) device);
                    D3D12Hooks::HookDevice(State::Instance().currentD3D12Device);
                }
            }
        }

        if (!Util::CheckForRealObject(__FUNCTION__, cq, &real))
            real = cq;

        State::Instance().currentCommandQueue = (ID3D12CommandQueue*) real;

        if (State::Instance().currentD3D12Device != nullptr)
            WithDx12::SetD3D12Objects(State::Instance().currentD3D12Device, State::Instance().currentCommandQueue,
                                      D3D12_COMMAND_LIST_TYPE_DIRECT);

        // Create FG SwapChain
        if (!_skipFGSwapChainCreation)
        {
            ScopedSkipFGSCCreation skipFGSCCreation {};
            FGSCResult = FGHooks::CreateSwapChainForHwnd(realFactory, real, hWnd, &localDesc,
                                                         pFullscreenDesc != nullptr ? &localFullscreenDesc : nullptr,
                                                         pRestrictToOutput, ppSwapChain);

            if (FGSCResult == S_OK)
            {
                ((IDXGISwapChain*) *ppSwapChain)->GetDesc(&State::Instance().currentSwapchainDesc);
                return FGSCResult;
            }
        }
    }
    else
    {
        LOG_INFO("Failed to get ID3D12CommandQueue from pDevice, creating Dx11 swapchain!");

        ID3D11Device* device = nullptr;

        if (pDevice->QueryInterface(IID_PPV_ARGS(&device)) == S_OK)
        {
            D3D11Hooks::HookToDevice(device);

            device->Release();

            // Update current D3D11 device and adapter
            if (State::Instance().currentD3D11Device != device)
            {
                State::Instance().currentD3D11Device = device;

                IDXGIDevice* dxgiDevice = nullptr;
                if (device->QueryInterface(IID_PPV_ARGS(&dxgiDevice)) == S_OK)
                {
                    IDXGIAdapter* adapter = nullptr;
                    if (dxgiDevice->GetAdapter(&adapter) == S_OK)
                        adapter->Release();

                    dxgiDevice->Release();
                }
            }

            // Legacy DX11 FG path intentionally removed.
            // DX11 FG must now go through Dx11wDx12SC; if interop creation failed, fall back to the normal wrapper path
            // below.
        }
    }

    HRESULT result = E_FAIL;

    // If FG is disabled or call is coming from FG library
    // Create the DXGI SwapChain and wrap it
    if (_skipFGSwapChainCreation || FGSCResult != S_OK)
    {

        // !_skipFGSwapChainCreation for preventing early enablement flags
        if (!_skipFGSwapChainCreation)
        {
            State::Instance().skipDxgiLoadChecks = true;

            if (Config::Instance()->FGDontUseSwapchainBuffers.value_or_default())
                State::Instance().skipHeapCapture = true;
        }

        {
            ScopedSkipParentWrapping skipParentWrapping {};
            result = o_DLSSGCreateSwapChainForHwnd(realFactory, pDevice, hWnd, &localDesc,
                                                   pFullscreenDesc != nullptr ? &localFullscreenDesc : nullptr,
                                                   pRestrictToOutput, ppSwapChain);
        }

        if (!_skipFGSwapChainCreation)
        {
            if (Config::Instance()->FGDontUseSwapchainBuffers.value_or_default())
                State::Instance().skipHeapCapture = false;

            State::Instance().skipDxgiLoadChecks = false;
        }

        if (SwapchainCreate::Created(result, ppSwapChain))
        {
            // check for SL proxy
            IDXGISwapChain1* realSC = nullptr;
            if (!Util::CheckForRealObject(__FUNCTION__, *ppSwapChain, (IUnknown**) &realSC))
                realSC = *ppSwapChain;

            State::Instance().currentRealSwapchain = realSC;

            IUnknown* readDevice = nullptr;
            if (!Util::CheckForRealObject(__FUNCTION__, pDevice, (IUnknown**) &readDevice))
                readDevice = pDevice;

            if (Util::GetProcessWindow() == hWnd)
            {
                State::Instance().screenWidth = static_cast<float>(localDesc.Width);
                State::Instance().screenHeight = static_cast<float>(localDesc.Height);
            }

            realSC->GetDesc(&State::Instance().currentSwapchainDesc);

            NoteCreateStatus(result, *ppSwapChain, State::Instance().currentFGSwapchain != nullptr
                ? State::Instance().currentFGSwapchain : (!_skipFGSwapChainCreation ? realSC : nullptr));

            LOG_DEBUG("Created new swapchain: {0:X}, hWnd: {1:X}", (uintptr_t) *ppSwapChain, (uintptr_t) hWnd);

            WrappedIDXGISwapChain4* wrapped;
            if ((*ppSwapChain)->QueryInterface(IID_PPV_ARGS(&wrapped)) != S_OK)
            {
                *ppSwapChain = new WrappedIDXGISwapChain4(realSC, readDevice, hWnd, localDesc.Flags, false);
                LOG_DEBUG("Created new WrappedIDXGISwapChain4: {0:X}, pDevice: {1:X}", (uintptr_t) *ppSwapChain,
                          (uintptr_t) pDevice);

                if (!_skipFGSwapChainCreation)
                    State::Instance().currentSwapchain = *ppSwapChain;

                State::Instance().currentWrappedSwapchain = *ppSwapChain;
            }
            else
            {
                wrapped->Release();
            }
        }
        else
        {
            LOG_ERROR("CreateSwapChainForHwnd failed: {:X}", (UINT) result);
        }
    }

    if (firstCall)
    {
        LOG_DEBUG("Unsetting skip FG swapchain creation");
        _skipFGSwapChainCreation = false;
        firstCall = false;
    }

    return result;
}

HRESULT DxgiFactoryHooks::DLSSGCreateSwapChainForCoreWindow(IDXGIFactory2* realFactory, IUnknown* pDevice,
                                                            IUnknown* pWindow, const DXGI_SWAP_CHAIN_DESC1* pDesc,
                                                            IDXGIOutput* pRestrictToOutput,
                                                            IDXGISwapChain1** ppSwapChain)
{
    if (State::Instance().vulkanCreatingSC)
    {
        LOG_WARN("Vulkan is creating swapchain!");

        if (pDesc != nullptr)
            LOG_DEBUG("Width: {}, Height: {}, Format: {}, Flags: {:X}, Count: {}, SkipWrapping: {}", pDesc->Width,
                      pDesc->Height, (UINT) pDesc->Format, pDesc->Flags, pDesc->BufferCount, _skipFGSwapChainCreation);

        ScopedSkipDxgiLoadChecks skipDxgiLoadChecks {};
        return realFactory->CreateSwapChainForCoreWindow(pDevice, pWindow, pDesc, pRestrictToOutput, ppSwapChain);
    }

    if (pDevice == nullptr || pDesc == nullptr)
    {
        LOG_WARN("pDevice or pDesc is nullptr!");
        ScopedSkipDxgiLoadChecks skipDxgiLoadChecks {};
        return realFactory->CreateSwapChainForCoreWindow(pDevice, pWindow, pDesc, pRestrictToOutput, ppSwapChain);
    }

    if (pDesc->Height < 100 || pDesc->Width < 100)
    {
        LOG_WARN("Overlay call! Width: {}, Height: {}", pDesc->Width, pDesc->Height);

        ScopedSkipDxgiLoadChecks skipDxgiLoadChecks {};
        return realFactory->CreateSwapChainForCoreWindow(pDevice, pWindow, pDesc, pRestrictToOutput, ppSwapChain);
    }

    DXGI_SWAP_CHAIN_DESC1 localDesc {};
    memcpy(&localDesc, pDesc, sizeof(DXGI_SWAP_CHAIN_DESC1));

    LOG_DEBUG("Width: {}, Height: {}, Format: {}, Count: {}, SkipWrapping: {}", localDesc.Width, localDesc.Height,
              (UINT) localDesc.Format, localDesc.BufferCount, _skipFGSwapChainCreation);

    // For vsync override
    if (Config::Instance()->OverrideVsync.value_or_default())
    {
        localDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        localDesc.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

        if (localDesc.BufferCount < 2)
            localDesc.BufferCount = 2;
    }

    State::Instance().SCAllowTearing = (localDesc.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) > 0;
    State::Instance().SCLastFlags = localDesc.Flags;
    State::Instance().realExclusiveFullscreen = false;

    ID3D12CommandQueue* cq = nullptr;
    IUnknown* real = nullptr;
    if (pDevice->QueryInterface(IID_PPV_ARGS(&cq)) == S_OK)
    {
        if (!Util::CheckForRealObject(__FUNCTION__, cq, &real))
            real = cq;

        State::Instance().currentCommandQueue = (ID3D12CommandQueue*) real;

        if (State::Instance().currentD3D12Device != nullptr)
            WithDx12::SetD3D12Objects(State::Instance().currentD3D12Device, State::Instance().currentCommandQueue,
                                      D3D12_COMMAND_LIST_TYPE_DIRECT);
    }

    HRESULT result = E_FAIL;
    {
        ScopedSkipDxgiLoadChecks skipDxgiLoadChecks {};
        result = o_DLSSGCreateSwapChainForCoreWindow(realFactory, pDevice, pWindow, &localDesc, pRestrictToOutput,
                                                          ppSwapChain);
    }

    if (SwapchainCreate::Created(result, ppSwapChain))
    {
        // check for SL proxy
        IDXGISwapChain* realSC = nullptr;
        if (!Util::CheckForRealObject(__FUNCTION__, *ppSwapChain, (IUnknown**) &realSC))
            realSC = *ppSwapChain;

        State::Instance().currentRealSwapchain = realSC;

        IUnknown* readDevice = nullptr;
        if (!Util::CheckForRealObject(__FUNCTION__, pDevice, (IUnknown**) &readDevice))
            readDevice = pDevice;

        realSC->GetDesc(&State::Instance().currentSwapchainDesc);

        State::Instance().screenWidth = static_cast<float>(localDesc.Width);
        State::Instance().screenHeight = static_cast<float>(localDesc.Height);

        NoteCreateStatus(result, *ppSwapChain, State::Instance().currentFGSwapchain != nullptr
            ? State::Instance().currentFGSwapchain : (!_skipFGSwapChainCreation ? realSC : nullptr));

        LOG_DEBUG("Created new swapchain: {0:X}, hWnd: {1:X}", (UINT64) *ppSwapChain, (UINT64) pWindow);

        WrappedIDXGISwapChain4* wrapped;
        if ((*ppSwapChain)->QueryInterface(IID_PPV_ARGS(&wrapped)) != S_OK)
        {
            *ppSwapChain = new WrappedIDXGISwapChain4(realSC, readDevice, (HWND) pWindow, localDesc.Flags, true);

            if (!_skipFGSwapChainCreation)
                State::Instance().currentSwapchain = *ppSwapChain;

            State::Instance().currentWrappedSwapchain = *ppSwapChain;

            LOG_DEBUG("Created new WrappedIDXGISwapChain4: {0:X}, pDevice: {1:X}", (UINT64) *ppSwapChain,
                      (UINT64) pDevice);
        }
        else
        {
            wrapped->Release();
        }
    }

    return result;
}

HRESULT DxgiFactoryHooks::EnumAdapters(IDXGIFactory* realFactory, UINT Adapter, IDXGIAdapter** ppAdapter)
{
    HRESULT result = S_FALSE;

    if (State::Instance().skipDxgiLoadChecks)
        return o_EnumAdapters(realFactory, Adapter, ppAdapter);

    if (Config::Instance()->PreferFirstDedicatedGpu.value_or_default() && Adapter > 0)
    {
        LOG_DEBUG("{}, returning not found", Adapter);
        return DXGI_ERROR_NOT_FOUND;
    }

    IDXGIFactory6* factory6 = nullptr;
    if (realFactory->QueryInterface(IID_PPV_ARGS(&factory6)) == S_OK && factory6 != nullptr)
    {
        auto allGpus = IdentifyGpu::getAllGpus();
        if (Adapter < allGpus.size())
        {
            LOG_DEBUG("Trying to select: {}", allGpus[Adapter].name);

            auto gpuLuid = allGpus[Adapter].luid;

            ScopedSkipDxgiLoadChecks skipDxgiLoadChecks {};
            result = o_EnumAdapterByLuid(factory6, gpuLuid, __uuidof(IDXGIAdapter), (void**) ppAdapter);
        }
        else
            result = DXGI_ERROR_NOT_FOUND;

        factory6->Release();
    }

    if (result != S_OK && result != DXGI_ERROR_NOT_FOUND)
    {
        ScopedSkipDxgiLoadChecks skipDxgiLoadChecks {};
        result = o_EnumAdapters(realFactory, Adapter, ppAdapter);
    }

    if (result == S_OK)
        DxgiSpoofing::AttachToAdapter(*ppAdapter);

#if _DEBUG
    LOG_TRACE("result: {:X}, Adapter: {}, pAdapter: {:X}", (UINT) result, Adapter, (uintptr_t) *ppAdapter);
#endif

    return result;
}

HRESULT DxgiFactoryHooks::EnumAdapters1(IDXGIFactory1* realFactory, UINT Adapter, IDXGIAdapter1** ppAdapter)
{
    HRESULT result = S_FALSE;

    if (State::Instance().skipDxgiLoadChecks)
        return o_EnumAdapters1(realFactory, Adapter, ppAdapter);

    if (Config::Instance()->PreferFirstDedicatedGpu.value_or_default() && Adapter > 0)
    {
        LOG_DEBUG("{}, returning not found", Adapter);
        return DXGI_ERROR_NOT_FOUND;
    }

    IDXGIFactory6* factory6 = nullptr;
    if (realFactory->QueryInterface(IID_PPV_ARGS(&factory6)) == S_OK && factory6 != nullptr)
    {
        auto allGpus = IdentifyGpu::getAllGpus();
        if (Adapter < allGpus.size())
        {
            LOG_DEBUG("Trying to select: {}", allGpus[Adapter].name);

            auto gpuLuid = allGpus[Adapter].luid;

            ScopedSkipDxgiLoadChecks skipDxgiLoadChecks {};
            result = o_EnumAdapterByLuid(factory6, gpuLuid, __uuidof(IDXGIAdapter), (void**) ppAdapter);
        }
        else
            result = DXGI_ERROR_NOT_FOUND;

        factory6->Release();
    }

    if (result != S_OK && result != DXGI_ERROR_NOT_FOUND)
    {
        ScopedSkipDxgiLoadChecks skipDxgiLoadChecks {};
        result = o_EnumAdapters1(realFactory, Adapter, ppAdapter);
    }

    if (result == S_OK)
        DxgiSpoofing::AttachToAdapter(*ppAdapter);

#if _DEBUG
    LOG_TRACE("result: {:X}, Adapter: {}, pAdapter: {:X}", (UINT) result, Adapter, (uintptr_t) *ppAdapter);
#endif

    return result;
}

HRESULT DxgiFactoryHooks::EnumAdapterByLuid(IDXGIFactory4* realFactory, LUID AdapterLuid, REFIID riid,
                                            void** ppvAdapter)
{
    HRESULT result;
    {
        ScopedSkipDxgiLoadChecks skipDxgiLoadChecks {};
        result = o_EnumAdapterByLuid(realFactory, AdapterLuid, riid, ppvAdapter);
    }

    if (result == S_OK)
        DxgiSpoofing::AttachToAdapter((IUnknown*) *ppvAdapter);

#if _DEBUG
    LOG_TRACE("result: {:X}, pAdapter: {:X}", (UINT) result, (uintptr_t) *ppvAdapter);
#endif

    return result;
}

HRESULT DxgiFactoryHooks::EnumAdapterByGpuPreference(IDXGIFactory6* realFactory, UINT Adapter,
                                                     DXGI_GPU_PREFERENCE GpuPreference, REFIID riid, void** ppvAdapter)
{
    HRESULT result;

    {
        ScopedSkipDxgiLoadChecks skipDxgiLoadChecks {};
        result = o_EnumAdapterByGpuPreference(realFactory, Adapter, GpuPreference, riid, ppvAdapter);
    }

    // Log because that's not something we usually expect
    if (GpuPreference & DXGI_GPU_PREFERENCE_MINIMUM_POWER)
        LOG_WARN("Game asked for minimum power GPU");

    if (result == S_OK)
        DxgiSpoofing::AttachToAdapter((IUnknown*) *ppvAdapter);

#if _DEBUG
    LOG_TRACE("result: {:X}, Adapter: {}, pAdapter: {:X}", (UINT) result, Adapter, (uintptr_t) *ppvAdapter);
#endif

    return result;
}

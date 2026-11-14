// Real DXGI resize validation of the separate DX11 source / DX12 presenter contracts.
#define NOMINMAX
#include "../Dx11InteropSwapchainPolicy.h"
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdio>
#include <stdexcept>

using Microsoft::WRL::ComPtr;
namespace
{
unsigned checks = 0;
void Check(bool ok, const char* why) { ++checks; if (!ok) throw std::runtime_error(why); }
void Hr(HRESULT result) { Check(SUCCEEDED(result), "DXGI/D3D setup failed"); }
struct Window
{
    HWND handle = CreateWindowExW(0, L"STATIC", L"F6 resize test", WS_OVERLAPPEDWINDOW,
        0, 0, 320, 240, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Window() { Check(handle != nullptr, "hidden test window exists"); }
    ~Window() { if (handle) DestroyWindow(handle); }
};

void CheckResize(IDXGIFactory5* factory, ID3D12CommandQueue* queue, UINT providerFlags, bool negative)
{
    Window window;
    DXGI_SWAP_CHAIN_DESC1 created {};
    created.Width = 320; created.Height = 240; created.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    created.SampleDesc.Count = 1; created.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    created.BufferCount = 6; created.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    created.Flags = providerFlags;
    ComPtr<IDXGISwapChain1> chain;
    Hr(factory->CreateSwapChainForHwnd(queue, window.handle, &created, nullptr, nullptr, &chain));
    DXGI_SWAP_CHAIN_DESC before {};
    Hr(chain->GetDesc(&before));
    Check(before.BufferCount == 6 && before.Flags == providerFlags, "actual presenter owns its six-buffer creation flags");

    if (negative)
    {
        // The deployed mapping already converted sRGB/count, but incorrectly
        // removed ALLOW_TEARING requested by DLSSG's CreateSwapchain1.
        const auto rejected = chain->ResizeBuffers(0, 320, 240, DXGI_FORMAT_R8G8B8A8_UNORM, 2);
        std::printf("negative old flags: created=0x%X resize=0x2 result=0x%08X\n", providerFlags, unsigned(rejected));
        Check(FAILED(rejected), "real DXGI rejects the deployed native-flags mapping");
    }

    const auto args = Dx11InteropSwapchain::PresenterResizeArgs(1, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 2, before.Flags);
    Check(Dx11InteropSwapchain::Matches(before, args, 320, 240), "unchanged source keeps a healthy presenter");
    // Exercise real resizing as well as the equivalent-descriptor skip case.
    Hr(chain->ResizeBuffers(args.bufferCount, 384, 256, args.format, args.flags));
    DXGI_SWAP_CHAIN_DESC after {};
    Hr(chain->GetDesc(&after));
    Check(after.BufferCount == 6 && after.Flags == providerFlags && after.BufferDesc.Width == 384 &&
        after.BufferDesc.Height == 256 && after.BufferDesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM,
        "real presenter resize retains count/flags and applies the new extent");
    Check(SetWindowPos(window.handle, nullptr, 0, 0, 480, 320,
        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) != FALSE, "hidden client extent changes");
    RECT client {};
    Check(GetClientRect(window.handle, &client) != FALSE, "actual window client extent is known");
    Check(!Dx11InteropSwapchain::Matches(after, args, 0, 0), "zero extents never skip a real window-size change");
    Hr(chain->ResizeBuffers(args.bufferCount, 0, 0, args.format, args.flags));
    Hr(chain->GetDesc(&after));
    Check(after.BufferDesc.Width == UINT(client.right) && after.BufferDesc.Height == UINT(client.bottom),
        "zero extents resolve to the current client area rather than old buffers");
}

void NativeSource(IDXGIFactory5* factory)
{
    Window window;
    ComPtr<ID3D11Device> device;
    Hr(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
        &device, nullptr, nullptr));
    DXGI_SWAP_CHAIN_DESC1 desc {};
    desc.Width = 320; desc.Height = 240; desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    desc.SampleDesc.Count = 1; desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 1; desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD; desc.Flags = 2;
    ComPtr<IDXGISwapChain1> chain;
    Hr(factory->CreateSwapChainForHwnd(device.Get(), window.handle, &desc, nullptr, nullptr, &chain));
    Hr(chain->ResizeBuffers(1, 384, 256, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 2));
    DXGI_SWAP_CHAIN_DESC actual {};
    Hr(chain->GetDesc(&actual));
    Check(actual.BufferCount == 1 && actual.BufferDesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB && actual.Flags == 2,
        "actual DX11 source retains the game's untouched count/sRGB/flags");
}
}

int main()
{
    try
    {
        ComPtr<IDXGIFactory5> factory;
        Hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
        BOOL tearing = FALSE;
        Hr(factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing)));
        Check(tearing != FALSE, "host supports the reported DLSSG tearing contract");
        ComPtr<IDXGIAdapter> warp;
        Hr(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)));
        ComPtr<ID3D12Device> device;
        Hr(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
        D3D12_COMMAND_QUEUE_DESC q {};
        ComPtr<ID3D12CommandQueue> queue;
        Hr(device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)));
        constexpr UINT tearingFlag = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
        CheckResize(factory.Get(), queue.Get(), 2 | tearingFlag, true);
        CheckResize(factory.Get(), queue.Get(), 2 | tearingFlag | DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT, false);
        CheckResize(factory.Get(), queue.Get(), 2, false);
        NativeSource(factory.Get());
        std::printf("PASS %u real DXGI/WARP resize checks; Streamline proxies and game display untested\n", checks);
        return 0;
    }
    catch (const std::exception& e) { std::fprintf(stderr, "FAIL: %s\n", e.what()); return 1; }
}

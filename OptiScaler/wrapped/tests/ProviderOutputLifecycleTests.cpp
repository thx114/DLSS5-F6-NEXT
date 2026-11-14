#include <wrapped/ProviderOutputQueue.h>
#include <memory>
#include <cstdio>
#include <cstdlib>
#include <dxgi1_6.h>
using Microsoft::WRL::ComPtr;
static unsigned checks = 0, cleanup = 0, registered = 0, unregistered = 0, completed = 0, refreshed = 0;
static bool denied = false;
static void Check(bool ok, const char* why)
{
    ++checks;
    if (!ok) { std::fprintf(stderr, "FAIL: %s hr=%08X\n", why, unsigned(GetLastError())); std::exit(1); }
}
namespace HoYoShade {
struct DirectResizeScope {
    DirectResizeScope(IDXGISwapChain*, void*) {}
    HRESULT Result() const { return S_OK; }
    void Complete(HRESULT) { ++completed; }
};
}
namespace framegen::final_output {
struct Endpoint {
    struct Resize {
        explicit Resize(Endpoint*) {}
        explicit operator bool() const { return !denied; }
        bool ConsumersSafe(IDXGISwapChain*) const { return true; }
        void Complete(HRESULT, UINT, const UINT*, IUnknown* const*, bool) { ++completed; }
    };
};
}
namespace MenuOverlayDx {
void CleanupRenderTarget(bool clear, HWND, bool sourceOwner)
{
    Check(clear && !sourceOwner, "physical cleanup must never own source FG teardown"); ++cleanup;
}
}
namespace FGHooks {
void UnregisterMenuOutput(IDXGISwapChain*) { ++unregistered; }
void RegisterMenuOutput(IDXGISwapChain*, IUnknown*, HWND) { ++registered; }
}
// Executes the extracted production physical method against a real WARP
// swapchain. Source FG state and locks do not exist in this owner at all.
class WrappedIDXGISwapChain4 {
public:
    IDXGISwapChain* _real = nullptr;
    IDXGISwapChain3* _real3 = nullptr;
    std::unique_ptr<framegen::final_output::Endpoint> _finalOutput;
    HWND _handle = nullptr;
    IUnknown* _device = nullptr;
    ComPtr<IUnknown> _providerOutputDevice;
    UINT _lastFlags = 0;
    void _RefreshFrameLatencyFlags() { ++refreshed; }
    HRESULT ResizeProviderOutput(UINT, UINT, UINT, DXGI_FORMAT, UINT, const UINT*, IUnknown* const*, bool);
};
#include <ProviderOutputLifecycleProduction.inl>
int main()
{
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter> warp;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> first, second, compute;
    Check(SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))), "factory");
    Check(SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))), "WARP");
    Check(SUCCEEDED(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))), "device");
    D3D12_COMMAND_QUEUE_DESC q {};
    Check(SUCCEEDED(device->CreateCommandQueue(&q, IID_PPV_ARGS(&first))) &&
        SUCCEEDED(device->CreateCommandQueue(&q, IID_PPV_ARGS(&second))), "direct queues");
    q.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    Check(SUCCEEDED(device->CreateCommandQueue(&q, IID_PPV_ARGS(&compute))), "compute queue");
    const auto window = CreateWindowExW(0, L"STATIC", L"F6 physical resize fixture", WS_OVERLAPPEDWINDOW,
        0, 0, 64, 64, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(window != nullptr, "unshown fixture window");
    DXGI_SWAP_CHAIN_DESC1 desc {};
    desc.Width = desc.Height = 64; desc.BufferCount = 2;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> chain1;
    ComPtr<IDXGISwapChain3> chain;
    Check(SUCCEEDED(factory->CreateSwapChainForHwnd(first.Get(), window, &desc, nullptr, nullptr, &chain1)) &&
        SUCCEEDED(chain1.As(&chain)), "real physical swapchain");
    WrappedIDXGISwapChain4 output;
    output._real = output._real3 = chain.Get(); output._handle = window;
    output._device = first.Get(); output._providerOutputDevice = first;
    UINT nodes[] { 0, 0 };
    IUnknown* same[] { first.Get(), first.Get() };
    IUnknown* split[] { first.Get(), second.Get() };
    IUnknown* wrong[] { compute.Get(), compute.Get() };
    Check(ProviderOutputQueue::AfterResize(chain.Get(), 2, nodes, same).Get() == first.Get(), "same queue authorized");
    Check(!ProviderOutputQueue::AfterResize(chain.Get(), 2, nodes, split), "split queue has no menu writer");
    Check(!ProviderOutputQueue::AfterResize(chain.Get(), 2, nodes, wrong), "non-DIRECT queue refused");
    Check(!ProviderOutputQueue::AfterResize(chain.Get(), 3, nodes, same), "actual buffer count required");
    Check(!ProviderOutputQueue::AfterResize(chain.Get(), 2, nullptr, same), "unknown nodes refused");
    denied = true;
    Check(output.ResizeProviderOutput(2, 64, 64, desc.Format, 0, nodes, same, true) == DXGI_ERROR_WAS_STILL_DRAWING &&
        !cleanup && !completed, "unretired output never calls raw resize");
    denied = false;
    ComPtr<ID3D12Resource> held;
    Check(SUCCEEDED(chain->GetBuffer(0, IID_PPV_ARGS(&held))), "hold real backbuffer");
    const auto failed = output.ResizeProviderOutput(2, 64, 64, desc.Format, 0, nodes, same, true);
    Check(FAILED(failed) && output._device == first.Get() && !registered && !unregistered && !refreshed,
        "failed resize preserves the old queue owner and registry");
    held.Reset();
    Check(SUCCEEDED(output.ResizeProviderOutput(2, 64, 64, desc.Format, 0, nodes, same, true)) &&
        output._device == first.Get() && registered == 1 && unregistered == 1 && refreshed == 1,
        "successful single-queue resize commits exact menu owner");
    Check(SUCCEEDED(output.ResizeProviderOutput(2, 64, 64, desc.Format, 0, nodes, split, true)) &&
        !output._device && !output._providerOutputDevice && registered == 1 && unregistered == 2,
        "successful split-queue resize revokes menu writes");
    // The raw chain remains live after revocation. TEST does not display or run FG.
    Check(SUCCEEDED(chain->Present(0, DXGI_PRESENT_TEST)), "revoked overlay preserves real Present");
    Check(SUCCEEDED(output.ResizeProviderOutput(2, 64, 64, desc.Format, 0, nodes, same, true)) &&
        output._device == first.Get() && registered == 2, "known queue can restore menu ownership");
    Check(SUCCEEDED(output.ResizeProviderOutput(2, 64, 64, desc.Format, 0, nullptr, nullptr, false)) &&
        output._device == first.Get(), "legacy resize retains its queue binding");
    chain.Reset(); chain1.Reset(); DestroyWindow(window);
    std::printf("PASS %u production physical resize/real WARP queue checks; no SDK/game claims\n", checks);
}

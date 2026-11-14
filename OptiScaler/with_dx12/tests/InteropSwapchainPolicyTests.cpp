#include "../Dx11InteropSwapchainPolicy.h"

#include <cstdio>
#include <cstdlib>

namespace
{
unsigned checks = 0;
void Check(bool condition, const char* message)
{
    ++checks;
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}

void PresenterArguments()
{
    struct FormatCase { DXGI_FORMAT native, presenter; };
    constexpr FormatCase formats[] = {
        { DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM },
        { DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM },
        { DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM },
        { DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM },
        { DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM },
        { DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT },
        { DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN },
        // Unsupported types must reach normal DXGI validation, not silently
        // become a supported format and change the native picture semantics.
        { DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_TYPELESS },
    };
    constexpr UINT counts[] = { 0, 1, 2, 6, 16, 17 };
    constexpr UINT flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH | DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    for (const auto& format : formats)
        for (const auto count : counts)
        {
            const auto args = Dx11InteropSwapchain::PresenterResizeArgs(count, format.native, flags, flags);
            Check(args.format == format.presenter, "only sRGB presenter storage becomes UNORM");
            Check(args.bufferCount == (count == 1 ? 0u : count), "single native buffer preserves provider ring");
            Check(args.flags == flags, "normalization does not invent or erase flags");
            const auto repeated = Dx11InteropSwapchain::PresenterResizeArgs(args.bufferCount, args.format, args.flags, flags);
            Check(repeated.format == args.format && repeated.bufferCount == args.bufferCount && repeated.flags == args.flags,
                  "nested resize adaptation is idempotent");
        }

    // PID124564: native count=1 / sRGB=29; provider created with two buffers
    // and expanded its internal ring to six. Preserve either provider count.
    const auto repro = Dx11InteropSwapchain::PresenterResizeArgs(1, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 2, 2);
    constexpr UINT providerCounts[] = { 2, 6 };
    for (const UINT providerCount : providerCounts)
    {
        const UINT actualCount = repro.bufferCount ? repro.bufferCount : providerCount;
        Check(actualCount == providerCount && repro.format == DXGI_FORMAT_R8G8B8A8_UNORM,
              "reported resize cannot pass sRGB or shrink the provider ring");
    }
}

void PresenterCreationFlags()
{
    constexpr UINT immutable = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING | DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT |
                               DXGI_SWAP_CHAIN_FLAG_FOREGROUND_LAYER;
    constexpr UINT choices[] = { 0, DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING,
        DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT, immutable };
    for (const auto native : choices) for (const auto provider : choices)
    {
        const auto args = Dx11InteropSwapchain::PresenterResizeArgs(1, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
            native | DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH | DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE, provider);
        Check((args.flags & immutable) == provider, "presenter creation flags retain their actual owner in both directions");
        Check((args.flags & DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE) == 0, "DX11 GDI capability never reaches the DX12 presenter");
        Check((args.flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH) != 0, "mutable resize flags retain the request");
    }
    DXGI_SWAP_CHAIN_DESC provider {};
    provider.BufferCount = 6; provider.BufferDesc.Width = 2560; provider.BufferDesc.Height = 1600;
    provider.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    provider.Flags = 2 | DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    const auto args = Dx11InteropSwapchain::PresenterResizeArgs(1, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 2, provider.Flags);
    Check(Dx11InteropSwapchain::Matches(provider, args, 2560, 1600), "PID227276 native descriptor matches the healthy presenter after adaptation");
    Check(!Dx11InteropSwapchain::Matches(provider, args, 0, 0), "zero extents must be resolved by the real window resize");
    Check(!Dx11InteropSwapchain::Matches(provider, args, 1920, 1080), "a real size change still resizes the presenter");
    auto old = args; old.flags = 2;
    Check(!Dx11InteropSwapchain::Matches(provider, old, 2560, 1600), "old native flags force an unnecessary incompatible provider resize");
}

void FailedResizeCannotAuthenticateProxyBuffers()
{
    Dx11InteropSwapchain::ResizeStatus status;
    Check(status.CanSkipResize() && status.Failure() == S_OK, "new presenter is admitted");
    Check(status.RecoveryFailure(true) == S_OK, "healthy presenter retains optional lower-hook skip behavior");
    status.Complete(DXGI_ERROR_INVALID_CALL, E_INVALIDARG, false);
    Check(status.CanSkipResize(), "native rejection never mutates an untouched presenter");

    status.Complete(S_OK, E_INVALIDARG, true);
    Check(!status.CanSkipResize() && status.Failure() == E_INVALIDARG,
          "failed provider resize blocks use even after native success");
    Check(status.RecoveryFailure(true) == E_INVALIDARG,
          "failed presenter cannot enter lower-hook skip/GetBuffer/Present recovery");
    Check(status.RecoveryFailure(false) == S_OK,
          "failed presenter can attempt a real resize when the lower hook cannot skip it");
    // A subsequent S_OK from GetBuffer/Present/GetDesc is not a successful
    // resize. In particular GetBuffer may expose the raw buffer after the SDK
    // freed its proxies. None of these calls can clear this failure status.
    Check(status.Failure() == E_INVALIDARG, "ordinary API success cannot authenticate proxy reconstruction");
    status.Complete(S_OK, S_OK, false);
    Check(!status.CanSkipResize(), "equivalent descriptor/skip cannot heal discarded proxy buffers");
    status.Complete(DXGI_ERROR_WAS_STILL_DRAWING, S_OK, false);
    Check(status.Failure() == E_INVALIDARG, "later native failure cannot clear provider failure");
    status.Complete(S_OK, DXGI_ERROR_DEVICE_REMOVED, true);
    Check(status.Failure() == DXGI_ERROR_DEVICE_REMOVED, "actual provider failure is preserved for Present");
    status.Complete(S_OK, S_OK, true);
    Check(status.CanSkipResize() && status.Failure() == S_OK, "actual successful resize permits fresh buffers");
    status.Complete(S_OK, S_OK, false);
    Check(status.CanSkipResize(), "healthy equivalent resize stays healthy");

    Dx11InteropSwapchain::ResizeStatus other;
    status.Complete(S_OK, E_FAIL, true);
    Check(other.CanSkipResize() && !status.CanSkipResize(), "failure belongs only to its own wrapper");
}
}

int main()
{
    PresenterArguments();
    PresenterCreationFlags();
    FailedResizeCannotAuthenticateProxyBuffers();
    std::printf("PASS %u interop swapchain CPU policy checks; no D3D/SDK/display execution\n", checks);
}

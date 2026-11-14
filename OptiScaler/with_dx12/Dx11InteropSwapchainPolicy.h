#pragma once

#include <dxgi.h>
#include <atomic>

namespace Dx11InteropSwapchain
{
struct ResizeArgs
{
    UINT bufferCount;
    DXGI_FORMAT format;
    UINT flags;
};

// Only the DX12 presenter is normalized. The game's DX11 descriptor, buffers
// and views retain their original format (including sRGB) and buffer count.
constexpr ResizeArgs PresenterResizeArgs(UINT count, DXGI_FORMAT format, UINT flags, UINT presenterFlags) noexcept
{
    if (format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
        format = DXGI_FORMAT_R8G8B8A8_UNORM;
    else if (format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)
        format = DXGI_FORMAT_B8G8R8A8_UNORM;

    // The presenter and the game's hidden DX11 chain have different creation
    // flags. DXGI forbids changing tearing/waitable ownership during resize;
    // in particular DLSSG adds ALLOW_TEARING even when the game did not ask for it.
    constexpr UINT creationFlags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING |
                                   DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT |
                                   DXGI_SWAP_CHAIN_FLAG_FOREGROUND_LAYER;
    flags = (flags & ~(creationFlags | DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE)) |
            (presenterFlags & creationFlags);

    // Zero preserves the existing presenter count, including provider-owned
    // rings. A single DX11 buffer must never shrink a flip chain to one buffer.
    // UNKNOWN likewise keeps the presenter's current format.
    return { count == 1 ? 0 : count, format, flags };
}

constexpr bool Matches(const DXGI_SWAP_CHAIN_DESC& desc, const ResizeArgs& args, UINT width, UINT height) noexcept
{
    // DXGI resolves zero extents from the current window client area, not the
    // previous buffer size. Leave those calls to DXGI even for a healthy chain.
    return (args.bufferCount == 0 || args.bufferCount == desc.BufferCount) &&
           width != 0 && width == desc.BufferDesc.Width &&
           height != 0 && height == desc.BufferDesc.Height &&
           (args.format == DXGI_FORMAT_UNKNOWN || args.format == desc.BufferDesc.Format) && args.flags == desc.Flags;
}

// Presenter validity only: this state grants no GPU completion or retirement.
// A provider may discard proxy buffers BEFORE its ResizeBuffers call fails.
// Matching GetDesc afterwards does not prove those proxy buffers still exist.
class ResizeStatus final
{
    std::atomic<HRESULT> failure_ { S_OK };

  public:
    HRESULT Failure() const noexcept { return failure_.load(std::memory_order_acquire); }
    bool CanSkipResize() const noexcept { return SUCCEEDED(Failure()); }
    // The lower hook's optional skip path can return S_OK without rebuilding
    // proxies, and may even issue GetBuffer/Present to reset its index. Do not
    // enter that path while recovering a presenter whose buffers are unknown.
    HRESULT RecoveryFailure(bool providerMaySkip) const noexcept
    {
        return providerMaySkip ? Failure() : S_OK;
    }

    void Complete(HRESULT nativeResult, HRESULT presenterResult, bool presenterCalled) noexcept
    {
        // A failed native resize did not call the presenter; keep its status.
        if (FAILED(nativeResult)) return;
        if (FAILED(presenterResult))
            failure_.store(presenterResult, std::memory_order_release);
        else if (presenterCalled)
            failure_.store(S_OK, std::memory_order_release);
    }
};
}

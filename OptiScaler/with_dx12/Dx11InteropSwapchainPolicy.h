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
constexpr ResizeArgs PresenterResizeArgs(UINT count, DXGI_FORMAT format, UINT flags) noexcept
{
    if (format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
        format = DXGI_FORMAT_R8G8B8A8_UNORM;
    else if (format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)
        format = DXGI_FORMAT_B8G8R8A8_UNORM;

    // Zero preserves the existing presenter count, including provider-owned
    // rings. A single DX11 buffer must never shrink a flip chain to one buffer.
    // UNKNOWN likewise keeps the presenter's current format.
    return { count == 1 ? 0 : count, format, flags };
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

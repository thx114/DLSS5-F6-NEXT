#pragma once

#include "SysUtils.h"
#include <OwnedMutex.h>
#include <Config.h>

#include "dxgi1_6.h"
#include "d3d12.h"

#include <atomic>
#include <cstdint>
namespace GenshinPresentNr { struct Ticket; }
namespace framegen::final_output { class Endpoint; }
#include <memory>

using Microsoft::WRL::ComPtr;

// One actual native parent call owns the picture epoch, including a nested
// child output. Construction is restricted to the native-visible wrapper.
class GenshinNativeParentFrame final
{
    friend class Dx11wDx12SC;
    inline static thread_local GenshinNativeParentFrame* current_ = nullptr;
    IDXGISwapChain* native_;
    IDXGISwapChain* child_;
    IUnknown* device_;
    HWND parentHwnd_, childHwnd_;
    std::uint64_t& identity_;
    DWORD thread_;
    bool active_ = false, called_ = false;
    bool menuFallback_ = false; // Only the owning wrapper marks its native-only fallback.
    GenshinNativeParentFrame(IDXGISwapChain*, IDXGISwapChain*, IUnknown*, HWND, HWND, std::uint64_t&);
public:
    GenshinNativeParentFrame(const GenshinNativeParentFrame&) = delete;
    GenshinNativeParentFrame& operator=(const GenshinNativeParentFrame&) = delete;
    ~GenshinNativeParentFrame();
    static bool OwnsNative(IDXGISwapChain*, IUnknown* = nullptr) noexcept;
    static bool OwnsMenuFallback(IDXGISwapChain* sc, IUnknown* device) noexcept
    { return current_ && current_->menuFallback_ && OwnsNative(sc, device); }
    static bool MatchesTicket(const GenshinPresentNr::Ticket&) noexcept;
    bool Called() const noexcept { return called_; }
    static bool IsRetainedChildWindow(HWND) noexcept;
    static void PublishColorSpace(IDXGISwapChain*,DXGI_COLOR_SPACE_TYPE);
    static bool OwnsPresent(IDXGISwapChain*, HWND) noexcept;
    static bool IsChildOutput(IDXGISwapChain*, HWND) noexcept;
    HRESULT Present(UINT, UINT, const DXGI_PRESENT_PARAMETERS*, bool capturePolled);
};

#define USE_LOCAL_MUTEX

class DECLSPEC_UUID("3af622a3-82d0-49cd-994f-cce05122c222") WrappedIDXGISwapChain4 final : public IDXGISwapChain4
{
  public:
    WrappedIDXGISwapChain4(IDXGISwapChain* real, IUnknown* pDevice, HWND hWnd, UINT flags, bool isUWP,
                          bool isComposition = false);
    virtual ~WrappedIDXGISwapChain4();

    // implement IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    // implement IDXGIObject
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID Name, UINT DataSize, const void* pData) override;
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID Name, const IUnknown* pUnknown) override;
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID Name, UINT* pDataSize, void* pData) override;
    HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void** ppParent) override;

    // implement IDXGIDeviceSubObject
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** ppDevice) override;

    // implement IDXGISwapChain
    HRESULT STDMETHODCALLTYPE Present(UINT SyncInterval, UINT Flags) override;
    HRESULT STDMETHODCALLTYPE GetBuffer(UINT Buffer, REFIID riid, void** ppSurface) override;
    HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL Fullscreen, IDXGIOutput* pTarget) override;
    HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL* pFullscreen, IDXGIOutput** ppTarget) override;
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC* pDesc) override;
    HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat,
                                            UINT SwapChainFlags) override;
    HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC* pNewTargetParameters) override;
    HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput** ppOutput) override;
    HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS* pStats) override;
    HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT* pLastPresentCount) override;

    // implement IDXGISwapChain1
    HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_SWAP_CHAIN_DESC1* pDesc) override;
    HRESULT STDMETHODCALLTYPE GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pDesc) override;
    HRESULT STDMETHODCALLTYPE GetHwnd(HWND* pHwnd) override;
    HRESULT STDMETHODCALLTYPE GetCoreWindow(REFIID refiid, void** ppUnk) override;
    HRESULT STDMETHODCALLTYPE Present1(UINT SyncInterval, UINT PresentFlags,
                                       const DXGI_PRESENT_PARAMETERS* pPresentParameters) override;
    BOOL STDMETHODCALLTYPE IsTemporaryMonoSupported(void) override;
    HRESULT STDMETHODCALLTYPE GetRestrictToOutput(IDXGIOutput** ppRestrictToOutput) override;
    HRESULT STDMETHODCALLTYPE SetBackgroundColor(const DXGI_RGBA* pColor) override;
    HRESULT STDMETHODCALLTYPE GetBackgroundColor(DXGI_RGBA* pColor) override;
    HRESULT STDMETHODCALLTYPE SetRotation(DXGI_MODE_ROTATION Rotation) override;
    HRESULT STDMETHODCALLTYPE GetRotation(DXGI_MODE_ROTATION* pRotation) override;

    // implement IDXGISwapChain2
    HRESULT STDMETHODCALLTYPE SetSourceSize(UINT Width, UINT Height) override;
    HRESULT STDMETHODCALLTYPE GetSourceSize(UINT* pWidth, UINT* pHeight) override;
    HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT MaxLatency) override;
    HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT* pMaxLatency) override;
    HANDLE STDMETHODCALLTYPE GetFrameLatencyWaitableObject(void) override;
    HRESULT STDMETHODCALLTYPE SetMatrixTransform(const DXGI_MATRIX_3X2_F* pMatrix) override;
    HRESULT STDMETHODCALLTYPE GetMatrixTransform(DXGI_MATRIX_3X2_F* pMatrix) override;

    // implement IDXGISwapChain3
    UINT STDMETHODCALLTYPE GetCurrentBackBufferIndex(void) override;
    HRESULT STDMETHODCALLTYPE CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE ColorSpace,
                                                     UINT* pColorSpaceSupport) override;
    HRESULT STDMETHODCALLTYPE SetColorSpace1(DXGI_COLOR_SPACE_TYPE ColorSpace) override;
    HRESULT STDMETHODCALLTYPE ResizeBuffers1(UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT Format,
                                             UINT SwapChainFlags, const UINT* pCreationNodeMask,
                                             IUnknown* const* ppPresentQueue) override;

    // implement IDXGISwapChain4
    HRESULT STDMETHODCALLTYPE SetHDRMetaData(DXGI_HDR_METADATA_TYPE Type, UINT Size, void* pMetaData) override;

  private:
    std::uint64_t _genshinPresentIdentity = 0;
    std::unique_ptr<framegen::final_output::Endpoint> _finalOutput;
    bool _starRailProviderOutput = false; // Immutable CPU identity; never filter/GPU consumer authority.
    std::uintptr_t _finalRawCreationQueueInput = 0; // Creation address only; never dereferenced or rebound.
    IDXGISwapChain* _real = nullptr;
    IDXGISwapChain1* _real1 = nullptr;
    IDXGISwapChain2* _real2 = nullptr;
    IDXGISwapChain3* _real3 = nullptr;
    IDXGISwapChain4* _real4 = nullptr;

    int _id = 0;
    bool _uwp = false;
    bool _composition = false;
    LONG _refcount;
    // The final Release is running (misc/REFrameworkCompat.h: a nested one returns at once)
    std::atomic_bool _finalReleaseInProgress { false };
    UINT _lastFlags = 0;

    IUnknown* _device = nullptr;

    HWND _handle = nullptr;

#ifdef USE_LOCAL_MUTEX
    OwnedMutex _localMutex;
#endif

    void CheckForHdrOutput()
    {
        ComPtr<IDXGIOutput> output;

        HRESULT hr = GetContainingOutput(&output);
        if (SUCCEEDED(hr) && output)
        {
            ComPtr<IDXGIOutput6> output6;

            if (SUCCEEDED(output.As(&output6)))
            {
                DXGI_OUTPUT_DESC1 desc {};
                if (SUCCEEDED(output6->GetDesc1(&desc)))
                {
                    State::Instance().hdrOutputActive = desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
                }
            }
        }
    }
};

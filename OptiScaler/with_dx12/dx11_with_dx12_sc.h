#pragma once
#include <wrapped/SwapchainHookBoundary.h>

#include "SysUtils.h"
#include <Config.h>

#include "dxgi1_6.h"
#include "d3d11_4.h"
#include "d3d12.h"

#include <native/MsaaPictureDx11.h>
#include "GenshinVisibleChild.h"
#include "Dx11InteropSwapchainPolicy.h"
#include <framegen/FrameLatencyOwnership.h>
#include <optional>

#include <atomic>
#include <vector>
#include <memory>
#include <mutex>
namespace GenshinBridgeFg { class ColorImport; }

using Microsoft::WRL::ComPtr;

class DECLSPEC_UUID("23b064bb-482d-416c-93b1-829acedfb3d0") Dx11wDx12SC final : public SwapchainHookBoundary
{
  public:
    Dx11wDx12SC(IDXGISwapChain* real, IDXGISwapChain4* fgSC, ID3D11Device* pDevice, HWND hWnd, UINT flags,
                  std::unique_ptr<GenshinVisibleChild> visibleChild = {}, bool nativeParentRoute = false,
                  bool sourceFrameLatencyOwner = false, std::optional<UINT> applicationFlags = std::nullopt);
    virtual ~Dx11wDx12SC();

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID Name, UINT DataSize, const void* pData) override;
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID Name, const IUnknown* pUnknown) override;
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID Name, UINT* pDataSize, void* pData) override;
    HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void** ppParent) override;

    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** ppDevice) override;

    HRESULT STDMETHODCALLTYPE Present(UINT SyncInterval, UINT Flags) override;
    HRESULT STDMETHODCALLTYPE GetBuffer(UINT Buffer, REFIID riid, void** ppSurface) override;
    HRESULT STDMETHODCALLTYPE HookSetFullscreenState(BOOL Fullscreen, IDXGIOutput* pTarget);
    HRESULT STDMETHODCALLTYPE HookGetFullscreenState(BOOL* pFullscreen, IDXGIOutput** ppTarget);
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC* pDesc) override;
    HRESULT STDMETHODCALLTYPE HookResizeBuffers(UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat,
                                            UINT SwapChainFlags);
    HRESULT STDMETHODCALLTYPE HookResizeTarget(const DXGI_MODE_DESC* pNewTargetParameters);
    HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput** ppOutput) override;
    HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS* pStats) override;
    HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT* pLastPresentCount) override;

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

    HRESULT STDMETHODCALLTYPE SetSourceSize(UINT Width, UINT Height) override;
    HRESULT STDMETHODCALLTYPE GetSourceSize(UINT* pWidth, UINT* pHeight) override;
    HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT MaxLatency) override;
    HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT* pMaxLatency) override;
    HANDLE STDMETHODCALLTYPE GetFrameLatencyWaitableObject(void) override;
    HRESULT STDMETHODCALLTYPE SetMatrixTransform(const DXGI_MATRIX_3X2_F* pMatrix) override;
    HRESULT STDMETHODCALLTYPE GetMatrixTransform(DXGI_MATRIX_3X2_F* pMatrix) override;

    UINT STDMETHODCALLTYPE GetCurrentBackBufferIndex(void) override;
    HRESULT STDMETHODCALLTYPE HookCheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE ColorSpace,
                                                     UINT* pColorSpaceSupport);
    HRESULT STDMETHODCALLTYPE HookSetColorSpace1(DXGI_COLOR_SPACE_TYPE ColorSpace);
    HRESULT STDMETHODCALLTYPE ResizeBuffers1(UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT Format,
                                             UINT SwapChainFlags, const UINT* pCreationNodeMask,
                                             IUnknown* const* ppPresentQueue) override;

    HRESULT STDMETHODCALLTYPE HookSetHDRMetaData(DXGI_HDR_METADATA_TYPE Type, UINT Size, void* pMetaData);

  private:
    framegen::latency::Flags _frameLatencyFlags;
    std::mutex _frameLatencyMutex;
    void _RefreshFrameLatencyFlags();
    struct NativeCall final {
        Dx11wDx12SC& owner;
        explicit NativeCall(Dx11wDx12SC& value):owner(value){owner.AddRef();}
        ~NativeCall(){owner.Release();}
    };
    HRESULT _PresentNativeVisible(UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* parameters);
    bool _ValidateNativeVisible(bool recovering = false);
    HRESULT _PresentNativeParent(UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
    void _RevokeNativeChild(bool recoverableDisplayChange = false);
    bool _TryRecoverNativeChild();
    bool _TryDrainNativeChild();
    ULONG _ReleaseNativeParent();
    static void _RetireNativeChildOnOwner(void*);
    void _TryRetireNativeChildOnOwner();
    struct NativeVisibleReceipt
    {
        uint64_t serial = 0, frame = 0, epoch = 0;
        uintptr_t parent = 0, child = 0, actualParent = 0, actualChild = 0;
        uintptr_t device11 = 0, device12 = 0, backbuffer = 0, snapshot = 0, fence = 0;
        uintptr_t copyQueue = 0, presentQueue = 0, colorFence = 0;
        uint64_t colorValue = 0;
        // R629 scalar receipts only; never an ownership or pixel-validity gate.
        uint64_t srGeneration = 0, srSourceFrame = 0, srPictureSerial = 0;
        uintptr_t srSelectedRtv = 0, sourceBackbuffer = 0;
        bool ownerSnapshotKnown = false, srReaderKnown = false;
        bool colorCopied = false;
        UINT index = 0, width = 0, height = 0, dpi = 0;
        HRESULT nativeResult = E_PENDING, childResult = E_PENDING;
        const char* reason = "NativeOnlyOrUnsupported";
        bool snapshotStaged = false, nativeUnchanged = false, nativeCalled = false, childCalled = false, shown = false;
    } _visibleReceipt;
    // R723: serialized by _nativeParentMutex; retain every state/result transition
    // and summarize identical receipts at most once per second (or final release).
    NativeVisibleReceipt _visibleLogLast{};
    ULONGLONG _visibleLogTick = 0;
    uint64_t _visibleLogRepeats = 0, _visibleLogFirstRepeat = 0;
    bool _visibleLogStarted = false;
    void _LogNativeVisibleReceipt(bool flush = false);
    const bool _nativeParentRoute;
    // R807: the admitted StarRail source wrapper keeps its native11 latency owner
    // for its lifetime, including FG Off/resize. Qualification arrives from the
    // successful FG factory while its provider lifetime lock is held.
    const bool _sourceFrameLatencyOwner;
    // Factory-bound latency/hidden-Present policy; never follows a mutable
    // provider pointer from an SDK callback. Copy quarantine is independent.
    const bool _starRailF5DsTransport;
    // A removed source device cannot be healed by successful child/FG Presents.
    // Keep pending GPU owners; a new source wrapper starts with a fresh result.
    std::atomic<HRESULT> _sourceDeviceFailure{S_OK};
    std::recursive_mutex _nativeParentMutex;
    bool _parentDetached = false, _childRevoked = false, _childRetirementEntered = false;
    std::atomic<bool> _nativeDisplayUnobserved{false};
    void _SuspendUnobservedNativeDisplay() noexcept;
    // R718: one drain attempt per explicit update after owner snapshot publication.
    // A terminal/copy/provider failure cannot be promoted by a later display call.
    bool _childRecoveryAllowed = false, _childRecoveryAttempted = false, _childRecoveryEntered = false;
    // A proven SDK budget refusal waits for a NEW explicit FG resume request.
    // It grants no automatic retry, display-update recovery or GPU authority.
    bool _childBudgetSuspended = false;
    uint64_t _childBudgetResumeRequest = 0;
    uint64_t _childDisplaySerial = 0;
    bool _nativeColorSpaceKnown = false, _childSdrReady = false;
    DXGI_COLOR_SPACE_TYPE _nativeColorSpace = DXGI_COLOR_SPACE_CUSTOM;
    bool _nativeHdrMetadataAbsent = true, _nativeMatrixIdentity = true;
    // Fresh factory chain only; an attempted transform setter revokes defaults.
    bool _nativeDefaultRotation = true, _nativeDefaultSourceSize = true;
    IFGFeature_Dx12* _visibleProvider = nullptr; // observed owner; dereferenced only while current
    std::unique_ptr<GenshinVisibleChild> _visibleChild;
    HWND _visibleProviderHwnd = nullptr;
    ComPtr<ID3D11Texture2D> _nativeOriginal; // actual adapter-returned private snapshot; never native B
    uint64_t _visibleSerial = 0;
    bool _visiblePresentEntered = false;
    bool _visibleFgBlocked = false; // no uncertain copy/provider ring reuse before real lifecycle drain
    std::uint64_t _genshinPresentIdentity = 0;
    bool _InitInteropObjects();
    bool _CanReuseReturnedSnapshot() const;
    bool _RequestSharedBackBuffer(UINT index, bool reuseReturnedSnapshot = true);
    // Interop slot admission begin
    // One call-local proof that the previous reader of this exact shared slot
    // retired. It never grants source/provider authority or survives a copy.
    class CopySlotAdmission final
    {
        const void* owner_ = nullptr;
        const void* ring_ = nullptr;
        ID3D12Fence* fence_ = nullptr;
        ID3D12CommandAllocator* allocator_ = nullptr;
        UINT slot_ = 0;
        UINT64 pending_ = 0;

      public:
        CopySlotAdmission() = default;
        CopySlotAdmission(const CopySlotAdmission&) = delete;
        CopySlotAdmission& operator=(const CopySlotAdmission&) = delete;
        void Clear() noexcept { owner_ = ring_ = nullptr; fence_ = nullptr; allocator_ = nullptr; }
        void Record(const void* owner, const void* ring, ID3D12Fence* fence,
                    ID3D12CommandAllocator* allocator, UINT slot, UINT64 pending) noexcept
        {
            owner_ = owner; ring_ = ring; fence_ = fence; allocator_ = allocator;
            slot_ = slot; pending_ = pending;
        }
        bool Consume(const void* owner, const void* ring, ID3D12Fence* fence,
                     ID3D12CommandAllocator* allocator, UINT slot, UINT64 pending) noexcept
        {
            const bool matches = owner && ring && fence && allocator && pending != UINT64_MAX &&
                owner_ == owner && ring_ == ring && fence_ == fence && allocator_ == allocator &&
                slot_ == slot && pending_ == pending;
            Clear(); // A mismatch also revokes the proof; never fall back to a late wait.
            return matches;
        }
    };
    // Interop slot admission end
    bool _CopyDx11BackBufferToShared(UINT index, CopySlotAdmission& admission);
    bool _WaitDx11ThenDx12();
    bool _CopyDx11SharedToDx12FGBackBuffer(UINT dx11Index, CopySlotAdmission& admission,
                                        ID3D12Resource** copiedColour=nullptr);
    // Copies native input's processed picture (NativeMotionDx11::OnFGPresent) over the FG back buffer this same
    // Present already put the raw one into, when that option is on. A no-op, not a failure, when it is off or there
    // is nothing to apply this frame; never fails the surrounding Present.
    void _ApplyNativeInputToFGBackBuffer();
    // Lazily creates _nrCopyAllocators/_nrCopyCommandLists the first time native input actually has a frame to
    // composite, so D3D11+FG-interop users who never enable native input never pay for this ring or its failure
    // path.
    bool _EnsureNativeInputCopyRing();
    bool _WaitForCopyQueueIdle();
    bool _WaitForCopyAllocator(UINT slot, std::vector<UINT64>& fenceValues);
    void _ReportCopyFenceFault(const char* phase,UINT slot,UINT64 target,UINT64 completed,DWORD waitResult) noexcept;
    std::atomic<bool> _copyFenceFaultReported{false},_copyDeviceFaultReported{false};
    bool _WaitForCopyAllocator(UINT slot, std::vector<UINT64>& fenceValues, CopySlotAdmission& admission);
    void _ReleaseInteropBackBuffers();
    bool _ReleaseInteropObjects();
    void _RefreshCachedSwapchainDesc();
    UINT _GetDx11BackBufferIndexForPresent() const;
    void _AdvanceFakeBackBufferIndex();
    bool _WaitForInteropCopyOnPresentQueue();

    IDXGISwapChain* _real = nullptr;
    IDXGISwapChain1* _real1 = nullptr;
    IDXGISwapChain2* _real2 = nullptr;
    IDXGISwapChain3* _real3 = nullptr;
    IDXGISwapChain4* _real4 = nullptr;

    IDXGISwapChain4* _fgSwapChain = nullptr;
    Dx11InteropSwapchain::ResizeStatus _fgResizeStatus;

    int _id = 0;
    LONG _refcount = 1;
    UINT _lastFlags = 0;

    IFGFeature_Dx12* _fg = nullptr;

    ID3D11Device* _dx11Device = nullptr;
    ID3D11Device5* _dx11Device5 = nullptr;
    ID3D11DeviceContext* _dx11Context = nullptr;
    native::MsaaPictureDx11 _msaaResolve; // resolves a multisampled game back buffer into the single-sample shared copy
    ID3D11DeviceContext4* _dx11Context4 = nullptr;

    ID3D12Device* _dx12Device = nullptr;
    ID3D12CommandQueue* _dx12CommandQueue = nullptr;
    std::vector<ID3D12CommandAllocator*> _copyAllocators;
    std::vector<ID3D12GraphicsCommandList*> _copyCommandLists;

    // A second ring, same sizing/lifetime as the one above, for _ApplyNativeInputToFGBackBuffer's copy: it runs after
    // _CopyDx11SharedToDx12FGBackBuffer's own command list has already closed and been submitted for this frame, so it
    // cannot reuse that one's allocator (not safe to Reset while still in flight) -- shares _copyFence/_copyFenceEvent,
    // a plain monotonic counter either ring can bump.
    std::vector<ID3D12CommandAllocator*> _nrCopyAllocators;
    std::vector<ID3D12GraphicsCommandList*> _nrCopyCommandLists;
    std::vector<UINT64> _nrCopyAllocatorFenceValues;

    UINT64 _lastInteropCopyFenceValue = 0;

    std::vector<UINT64> _copyAllocatorFenceValues;
    // Submitted interop color work without a valid completion receipt
    // retains the real wrapper/rings; no later idle/resize/Release may clear it.
    std::atomic<bool> _sourceCopyUncertain { false };
    HANDLE _copyFenceEvent = nullptr;

    ID3D12Fence* _copyFence = nullptr;
    UINT64 _copyFenceValue = 1;
    // Retain the actual queue that may have accepted an interop wait even when
    // a later call failed. It is drained on a separate fence before release.
    ComPtr<ID3D12CommandQueue> _interopPresentQueue;
    ID3D12Fence* _presentRetirementFence = nullptr;
    HANDLE _presentRetirementEvent = nullptr;
    UINT64 _presentRetirementValue = 0;

    ID3D11Fence* _dx11Fence = nullptr;
    HANDLE _dx11FenceEvent = nullptr;
    ID3D12Fence* _dx12SharedFence = nullptr;
    HANDLE _sharedFenceHandle = nullptr;
    UINT64 _sharedFenceValue = 1;

    std::vector<ID3D11Texture2D*> _sharedDx11BackBufferCopies;
    // True only for an actual OpenSharedHandle of this frame's immutable NR snapshot.
    std::vector<bool> _sharedBackBufferIsReturnedSnapshot;
    std::vector<ID3D12Resource*> _openedDx11BackBuffers;
    // The actual OpenSharedHandle operation owns this immutable association.
    std::vector<std::shared_ptr<const GenshinBridgeFg::ColorImport>> _genshinFgImports;
    std::vector<HANDLE> _sharedBackBufferHandles;
    std::vector<D3D12_RESOURCE_STATES> _openedDx11BackBufferStates;

    UINT _bufferCount = 0;
    UINT _currentFakeIndex = 0;
    DXGI_FORMAT _bufferFormat = DXGI_FORMAT_UNKNOWN;
    bool _interopInitialized = false;

    // Set once OpenSharedHandle first reports the D3D12 device gone; logs the real removed-reason a single time
    // instead of once per frame forever, since a removed device never recovers on its own.
    bool _deviceRemovedLogged = false;

    HWND _handle = nullptr;
};

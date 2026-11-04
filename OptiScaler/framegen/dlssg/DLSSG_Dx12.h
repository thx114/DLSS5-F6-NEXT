#pragma once

#include <framegen/IFGFeature_Dx12.h>
#include "DlssgInputContinuity.h"
#include <framegen/FinalOutputEndpoint.h>
#include <inputs/FG/GenshinFgTicket.h>
#include <mutex>
#include <atomic>
#include <memory>
#include <vector>
#include <shaders/depth_scale/DS_Dx12.h>

#include <proxies/Streamline_Proxy.h>

namespace GenshinBridgeFg { class Recording; }
namespace DrawUiMask { class Frame; }

class DLSSG_Dx12 : public virtual IFGFeature_Dx12
{
    struct GenshinFenceOwner;
    std::shared_ptr<framegen::final_output::Provider> _finalOutputProvider;
  public:
    // R727: only authenticated Capture may mint this immutable guide contract.
    // Raster rows match the final picture; XY values are current->previous
    // picture pixels, normalized by the sealed constants, not generic Unity inputs.
    class GenshinNormalizedGuides final
    {
        friend class GenshinBridgeFg::Recording;
        friend class DLSSG_Dx12;
        explicit GenshinNormalizedGuides(const DLSSG_Dx12& provider);
        GenshinNormalizedGuides(const GenshinNormalizedGuides&) = delete;
        GenshinNormalizedGuides& operator=(const GenshinNormalizedGuides&) = delete;
        UINT64 owner=0,swapchain=0,guideEpoch=0,guideCutSerial=0;
        GenshinBridgeFg::Key key{};
        Microsoft::WRL::ComPtr<ID3D12Device> device;
        Microsoft::WRL::ComPtr<ID3D12CommandQueue> producerQueue;
        Microsoft::WRL::ComPtr<ID3D12Resource> depth,motion;
        sl::Constants constants{};
      public:
        ~GenshinNormalizedGuides() = default;
    };
    // A frame owns its exact fence epoch/value, never the cache's latest value.
    // Publication precedes any queue submission; only its own successful Signal
    // arms completion. Failed submissions retain the real timeline owner.
    class GenshinFenceReceipt final
    {
        friend class DLSSG_Dx12;
        const std::shared_ptr<GenshinFenceOwner> timeline;
        const UINT64 value;
        std::atomic<bool> signalled{false};
        GenshinFenceReceipt(const std::shared_ptr<GenshinFenceOwner>& owner, UINT64 v)
            : timeline(owner), value(v) {}
      public:
        UINT64 Value() const noexcept {return value;}
        UINT64 Epoch() const noexcept;
        bool Completed() const noexcept;
    };
    class GenshinInputRetirement final
    {
        friend class DLSSG_Dx12;
        UINT64 owner=0,epoch=0,frame=0,presentEpoch=0,vendorValue=0;
        // Vendor zero is unresolved. Independent ordered host completion has
        // its own proof below; this fallback deadline is checked even
        // on ordinary Present/skip boundaries with no fresh authenticated input.
        UINT64 pendingDeadline=0;
        dlssg::InputOwner inputOwner{};
        DWORD thread=0;
        Microsoft::WRL::ComPtr<ID3D12Device> device;
        Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
        Microsoft::WRL::ComPtr<IDXGISwapChain> swapchain;
        Microsoft::WRL::ComPtr<ID3D12Fence> vendorFence;
        std::shared_ptr<GenshinFenceReceipt> hostCompletion;
        Microsoft::WRL::ComPtr<ID3D12Resource> color,uiHudless;
        std::shared_ptr<DrawUiMask::Frame> uiMask;
        // Retain the exact immutable guide allocations through vendor retirement,
        // including partial tag acceptance and uncertain/failed Present paths.
        std::shared_ptr<const GenshinNormalizedGuides> guides;
        // Separate from the immutable vendor receipt. Only the real post-Present
        // Signal(value) on the accepted blocking presenting queue may establish this.
        bool presentingQueueRetirement=false;
        bool acceptedBlocking=false,presentReturned=false,signalled=false;
        bool uiMaskTagged=false; // successful exact SDK HUDless tag, not input-map retention
        std::atomic<bool> retired{false};
      public:
        bool Retired() const noexcept {return retired.load(std::memory_order_acquire);}
    };
  private:
    // Exact frame-scoped camera observations for the Genshin adapter only.
    sl::Constants _genshinConstants[BUFFER_COUNT] {};
    UINT64 _genshinConstantsFrame[BUFFER_COUNT] {};
    std::shared_ptr<GenshinInputRetirement> _genshinInputs[BUFFER_COUNT];
    // Independent wait and post-Present timelines. All reservations and Signals
    // are serialized by continuity; caches never own a swapchain reference.
    std::shared_ptr<GenshinFenceOwner> _genshinWaitFence, _genshinHostFence;
    UINT64 _genshinFenceSerial=0;
    std::shared_ptr<GenshinFenceReceipt> ReserveGenshinFence(std::shared_ptr<GenshinFenceOwner>& owner);
    HRESULT SignalGenshinFence(const std::shared_ptr<GenshinFenceReceipt>& receipt);
    inline static std::atomic<UINT64> _genshinOwnerSerial{0};
    const UINT64 _genshinOwnerGeneration=++_genshinOwnerSerial;
    // Captured only at successful real factory creation, independently of
    // mutable SetCommandQueue. No extra swapchain reference across Resize.
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> _genshinCreatedPresentQueue;
    uintptr_t _genshinCreatedSwapchain=0;
    void RememberGenshinPresentQueue(IDXGISwapChain* swapchain, ID3D12CommandQueue* queue);
    UINT64 _genshinPresentEpoch=0;
    std::atomic<bool> _genshinResumePending{false};
    // Explicit menu resume events only; frames/teardown never advance this clock.
    std::atomic<UINT64> _genshinResumeRequests{0};
    UINT64 _genshinBudgetRefusalPresent=0, _genshinBudgetRefusalFrame=0;
    UINT64 _genshinBudgetRefusalResumeRequest=0;
    bool _genshinTagging=false;
    // Telemetry only. Scalars from the last existing same-Present observation;
    // never a completion receipt and never authority for a later frame.
    struct GenshinStateObservation
    {
        bool valid=false;
        UINT64 frame=0,epoch=0;
        int result=-1,status=-1;
        uint32_t presented=0,cap=0;
    } _genshinStateObservation;
    bool _genshinConstantsFailureSeen=false;
    int _genshinConstantsTraceReset=-1;
    unsigned _genshinConstantsTraceCalls=0,_genshinConstantsTraceTransitions=0;
    UINT64 _genshinConstantsTraceTick=0;
    const char* _genshinRetirementTraceStage=nullptr;
    unsigned _genshinRetirementTraceCalls=0,_genshinRetirementTraceTransitions=0;
    UINT64 _genshinRetirementTraceTick=0;
    bool IsGenshinBridgeOwner() const;
    bool GenshinGuidesCurrent(const std::shared_ptr<const GenshinNormalizedGuides>& guides) const;
    void CollectGenshinInputs(bool providerFreed=false);
    void CheckGenshinPendingInputs();

    uint32_t _width = 0;
    uint32_t _height = 0;
    std::optional<bool> _haveHudless = std::nullopt;

    sl::ViewportHandle viewport { 0 };

    ID3D12Fence* dlssgFence[BUFFER_COUNT] = {};
    UINT64 lastOptionFrame = 0;

    // Every own provider option/state call and guide stamp shares this lock. It is acquired
    // before resource locks; no code holding it acquires IFGFeature::Mutex.
    std::recursive_mutex _inputContinuityMutex;
    dlssg::InputContinuity<BUFFER_COUNT> _inputContinuity;
    uint64_t _inputOwnerEpoch = 1;
    // Successful SDK input tags, never GPU-completion or pixel receipts. Borrowed
    // source pointers are compared as identity only after eOnlyValidNow; do not
    // late-AddRef/dereference them after the producer's recording scope expires.
    struct GuideTag
    {
        dlssg::InputOwner owner {};
        Dx12Resource input {};
        D3D12_RESOURCE_DESC description {};
        UINT64 frame=0, generation=0, nativeSerial=0;
        bool accepted=false, flipped=false;
    } _guideTags[BUFFER_COUNT][2];
    // Real per-provider/slot scale owners, released only after the existing
    // host drain plus SDK free, or deliberately retained by terminal quarantine.
    std::unique_ptr<DS_Dx12> _guideDepthScale[BUFFER_COUNT];
    std::unique_ptr<RF_Dx12> _guideFlip[BUFFER_COUNT][2];
    const Dx12Resource* _upscalerGuidePublication=nullptr; // Exact stack publication, never retained.
    bool FlipOwnedUpscalerGuide(Dx12Resource* input);
    bool _guideContractOwned=false; // Sticky across route changes until actual drain/free.
    // StarRail only: actual successful swapchain creation queue, not a mutable
    // input queue inferred later. Used only for existing optional conversions.
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> _starRailGuidePresentQueue;
    uintptr_t _starRailGuideSwapchain=0;
    bool _starRailGuideQueueCurrent=false;
    bool OwnsDepthContract() const;
    bool GuideTagsCurrent(int slot, UINT64 frame);
    bool RefuseDepthContract(const char* stage, bool publicationStarted = true);
    // Rejection of a source is not GPU failure or physical resource retirement.
    bool _depthSourceRejected=false, _depthSourceGap=false;
    struct NativeGuideUse
    {
        Microsoft::WRL::ComPtr<ID3D12CommandList> list, nativeList;
        Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
        Microsoft::WRL::ComPtr<ID3D12Fence> fence;
        Microsoft::WRL::ComPtr<ID3D12Resource> depth, motion, output;
        dlssg::NativeSource source {};
        dlssg::InputOwner owner {};
        FG_Constants constants {};
        UINT64 value=0;
        bool closed=false, submitted=false, executing=false, signalled=false, srSucceeded=false, ordered=false;
    } _nativeGuideUses[BUFFER_COUNT];
    std::atomic<bool> _nativeGuideTracking{false};
    bool _nativeGuideFailed=false, _nativeGuideHooksRequested=false;
    UINT64 _nativeGuideSubmissions=0, _nativeGuideSerial=0;
    bool NativeGuideRetired(const NativeGuideUse& use) const;
    bool NativeGuidesRetired() const;
    bool OrderNativeGuides(UINT slot);
    NativeGuideUse* FindNativeGuideSource(const dlssg::NativeSource& source);
    bool NativeGuideSourceCurrent(const dlssg::NativeSource& source);
    bool NativeGuideToken(const dlssg::NativeSource& source, sl::FrameToken*& token);
    UINT64 _depthContractCalls=0, _depthContractLogTick=0;
    unsigned _depthContractRows=0;
    const char* _depthContractStage=nullptr;
    sl::DLSSGOptions _lastRunningOptions {};
    dlssg::InputOwner _runningInputOwner {};
    bool _haveRunningOptions = false;
    // R954: CPU option acknowledgements only, under the existing continuity lock.
    // Scope contains values/generations, never borrowed resources, SDK tokens or extensions.
    struct StarRailOptionsScope
    {
        UINT64 ownerEpoch=0, sourceGeneration=0;
        uint32_t viewport=0, width=0, height=0, displayWidth=0, displayHeight=0;
        uint32_t guideFlags=0, consumers=0, providerCap=0, forceReflex=0;
        DXGI_COLOR_SPACE_TYPE colorSpace=DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
        uint32_t transfer=0, primaries=0, range=0, colorModel=0;
        bool colorSpaceValid=false, flip=false, depthScale=false;
        bool operator==(const StarRailOptionsScope&) const = default;
    } _starRailOptionsScope;
    template<typename Options> struct StarRailOptionsCache
    {
        Options options {};
        UINT64 sourceFrame=0;
        bool valid=false;
    };
    StarRailOptionsCache<sl::DLSSGOptions> _starRailDlssgOptions;
    StarRailOptionsCache<sl::ReflexOptions> _starRailReflexOptions;
    bool _starRailOptionsScopeValid=false;
    // Reject publication across recursive SDK callbacks/lifecycle changes.
    UINT64 _starRailOptionsSerial=0;
    void InvalidateStarRailOptions();
    void PrepareStarRailOptionsCache(int index);
    template<typename Options, typename Submit>
    sl::Result SetStarRailDispatchOptions(StarRailOptionsCache<Options>& cache,
        const Options& options, UINT64 sourceFrame, bool force, Submit&& submit, bool* submitted=nullptr);
    // SDK Reflex mode ownership is independent of logical FG and GPU retirement.
    // Running includes an attempted enable: a rejected call leaves SDK state unknown.
    enum class GenshinReflexOwnership { None, Running, ReleasePending };
    GenshinReflexOwnership _genshinReflexOwnership = GenshinReflexOwnership::None;
    bool _genshinReflexOffReported = false;
    void RetryGenshinReflexOff(); // Caller holds the existing continuity lock.
    bool _managedRetained = false;
    bool _continuityOwner = false;
    bool _ownerChanging = false;
    bool _hookRetiring = false, _hookHostFreed = false, _hookRetirementActive = false;
    bool _hookRetired = false;
    UINT64 _hookRetiredEpoch = 0;
    // Identity only; no borrowed execution lock or swapchain reference crosses hook retries.
    FGHookRetirementRequest _hookRetirementBinding {};
    bool ReleaseObjectsChecked();
    bool _teardownPending = false;
    bool _teardownOffAccepted = false;
    bool _teardownQuarantined = false;
    // R865: admission stays closed until host completion and SDK release.
    // Recursive callbacks cannot start another drain or lifecycle transition.
    bool _teardownCompleting = false;
    // Sticky at real publication, even for flipped/copied inputs: an output pin
    // is not an immutable input/reader lease. Cleared only by successful drain/free.
    bool _teardownBorrowedInputs = false;
    struct TeardownDrain
    {
        enum class Phase { Idle, Submitting, Polling, HostComplete };
        Phase phase = Phase::Idle;
        Microsoft::WRL::ComPtr<ID3D12Device> device;
        Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
        Microsoft::WRL::ComPtr<ID3D12Fence> fence;
        IDXGISwapChain* swapchain = nullptr; // Identity; caller/F5 pin owns it, never a new self-pin.
        bool asynchronous = false; // Only the authenticated, leased Genshin producer qualifies.
        UINT64 value = 0;
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> resources;
        // Terminal quarantine only. No provider pointer or callback survives.
        std::shared_ptr<GenshinInputRetirement> inputs[BUFFER_COUNT];
        std::shared_ptr<GenshinFenceOwner> waitFence, hostFence;
    };
    // Allocate before submission so terminal quarantine can detach the complete
    // owner graph without allocating or retaining a destroyed provider object.
    std::unique_ptr<TeardownDrain> _teardownDrain = std::make_unique<TeardownDrain>();
    bool WaitForTeardownTarget();
    bool CompletePendingTeardownForTermination();
    // Close failed before Execute; retain the slot until the existing host drain.
    bool _uiCloseFailed[BUFFER_COUNT] {};
    bool _scCloseFailed[BUFFER_COUNT] {};
    // ZZZ/F5-owned SC work only. These references belong to the exact recording /
    // submission, independently of mutable game queue and frame-resource maps.
    struct ZzzSCWork
    {
        Microsoft::WRL::ComPtr<ID3D12Device> device;
        Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
        Microsoft::WRL::ComPtr<ID3D12Fence> fence;
        Microsoft::WRL::ComPtr<ID3D12Resource> backbuffer, ui, hudless;
        bool submitted = false, signalled = false, poisoned = false;
    } _zzzSCWork[BUFFER_COUNT];
    bool _zzzSCCompletion = false; // Sticky: a route change cannot bypass old work.
    bool _zzzSCFailed = false;
    // Only the last GetSCCommandList's verified pending fence may defer optional SC.
    bool _zzzSCBusy = false;
    bool _zzzSCSubmitReported = false, _zzzSCReuseReported = false, _zzzSCFailureReported = false;
    UINT64 _zzzSCFenceValue = 0; // Never rewind while the existing SC fence survives.
    bool IsZzzSCOwner() const;
    bool HasZzzSCWork() const;
    bool PollZzzSCWork(bool* busy = nullptr);
    bool SubmitZzzSCWork(UINT index);
    bool FailZzzSCWork(UINT index, const char* stage, HRESULT result);
    // Admission is under the existing Inputs guard, before StartNewFrame can
    // clear a slot. UI completion is not SDK/generated-frame reader completion.
    bool _starRailUiBusy = false, _starRailUiGap = false;
    // Source pause, SDK suppression and final GPU retirement are independent.
    // Protected by continuity. No cached token/resource gets a new lifetime.
    bool _starRailRuntimeOwner = false, _starRailGapOffAccepted = false;
    bool _starRailDispatchPending = false;
    UINT64 _starRailDispatchFrame = 0, _starRailDispatchGeneration = 0;
    // Issued only by the actual guarded source callback, never a Reflex/SDK marker.
    UINT64 _starRailRecordingFrame = 0, _starRailRecordingGeneration = 0;
    dlssg::InputOwner _starRailDispatchOwner {};
    bool IsStarRailContinuousOwner() const;
    bool IsStarRailF5DsOwner() const;
    int SelectStarRailF5DsFrame(UINT64& frame);
    // SDK callback cannot take the source lock while Present waits on that SDK.
    // Adopt this real observed ID only at the next guarded source entry.
    std::atomic<UINT64> _starRailF5DsMarker { UINT64_MAX };
    // Missing-source display uses F5-DS's existing Present counters.
    // They never certify a fresh source, new guides or GPU completion.
    bool _starRailPresentCounted = false; // Reset by early Prepare; counted at most once on final admission.
    bool PresentStarRailInputGap();
    bool SuppressStarRailGapForPresent();
    // Continuity-owned failure survives Deactivate and route changes until a safe drain.
    bool _starRailUiFailed = false;
    bool IsStarRailUiOwner() const;
    void AdmitStarRailUiFrame();
    void FailStarRailUiFrame();
    bool WaitForStarRailF5DsUIAllocator(UINT index);
    bool WaitForUIAllocator(UINT index) override final;
    bool SubmitUICommandList(UINT index) override final;
    unsigned _inputRecordingDepth = 0;
    unsigned _presentExecutionDepth = 0;
    ID3D12Fence* _teardownFence = nullptr;
    HANDLE _teardownEvent = nullptr;
    UINT64 _teardownFenceValue = 0;
    bool DisableRetainedProvider();
    void QuarantineFailedTeardown();
    bool CanRetainForInputGap() const;
    dlssg::InputOwner CurrentInputOwner() const;
    bool SuspendForInputGap();
    bool Dispatch();

  protected:
    void ReleaseObjects() override final;
    void CreateObjects(ID3D12Device* InDevice) override final;

  public:
    // IFGFeature
    const char* Name() override final { return "DLSSG"; };
    feature_version Version() override final;
    HWND Hwnd() override final;

    // IFGFeature_Dx12
    bool CreateSwapchain(IDXGIFactory* factory, ID3D12CommandQueue* cmdQueue, DXGI_SWAP_CHAIN_DESC* desc,
                         IDXGISwapChain** swapChain, bool readyToRelease) override final;
    bool CreateSwapchain1(IDXGIFactory* factory, ID3D12CommandQueue* cmdQueue, HWND hwnd, DXGI_SWAP_CHAIN_DESC1* desc,
                          DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc, IDXGISwapChain1** swapChain,
                          bool readyToRelease) override final;

    bool ReleaseSwapchain(HWND hwnd) override final;
    FGHookRetirementReceipt RetireSwapchainForHook(const FGHookRetirementRequest&) override final;

    void CreateContext(ID3D12Device* device, FG_Constants& fgConstants) override final;
    UINT64 StartNewFrame() override final;
    // Reflex markers still forward; this provider owns permission to change its clock.
    void SetFrameCountFromReflex(UINT64 frameId);
    void ObserveStarRailF5DsMarker(UINT64 frameId) noexcept
    {
        _starRailF5DsMarker.store(frameId, std::memory_order_release);
    }
    void Activate() override final;
    void Deactivate() override final;
    void DestroyFGContext() override final;
    bool Shutdown() override final;

    void EvaluateState(ID3D12Device* device, FG_Constants& fgConstants) override final;
    // Queue explicit user re-enable for the normal FG state boundary. Only
    // its warm-up delay is removed; teardown and fresh-input gates remain.
    void RequestGenshinResume();
    // CPU refusal classification, never a resource-retirement or budget receipt.
    bool GenshinBudgetRefusedAt(UINT64 presentEpoch, UINT64& resumeRequest);
    UINT64 GenshinResumeRequests() const noexcept { return _genshinResumeRequests.load(std::memory_order_acquire); }

    bool Present() override final;
    ID3D12GraphicsCommandList* GetSCCommandList(int index = -1) override final;
    FGExecutionGuard GuardExecution(FGExecutionScope scope) override final;
    bool CanSubmitInputs() override final;
    enum class StarRailPresent { Proceed, NativeFallback, Failed };
    // Early call chooses independent current-picture NR. Final admission consumes
    // one real Dispatch; F5-DS's counter-qualified gap sends Off+retain.
    // Failed vetoes SDK Present until the existing safe teardown boundary.
    StarRailPresent PrepareStarRailPresent(bool beforeSdkPresent = false);
    // Caller holds the existing Present execution guard.
    bool UsesStarRailF5DsScheduling() const { return IsStarRailF5DsOwner(); }
    bool CompletePendingTeardown() override final;
    // Hook retries may finish an EXISTING drain before advisory COM ref probes.
    // Does not request teardown of an otherwise active provider.
    FGHookRetirementResult CompletePendingTeardownForHook(bool terminal);
    bool PrepareForSwapchainChange(bool terminal = false) override final;

    // Called only after same-present ticket/depth/MV/final-color authentication.
    UINT64 BeginGenshinPresent(ID3D12Device* device, IDXGISwapChain* swapchain,
        ID3D12CommandQueue* queue, UINT64 presentEpoch, FG_Constants& constants);
    std::shared_ptr<const GenshinInputRetirement> SetGenshinConstants(
        const std::shared_ptr<const GenshinNormalizedGuides>& guides, const GenshinBridgeFg::Key& key,
        UINT64 frame, bool reset);
    // Publishes a sealed receipt before the first Wait, including partial failure.
    bool WaitGenshinInputs(ID3D12Device* device, IDXGISwapChain* swapchain, ID3D12CommandQueue* queue,
        ID3D12Fence* produced, UINT64 producedValue, ID3D12Fence* copied, UINT64 copiedValue,
        std::shared_ptr<const GenshinFenceReceipt>& receipt);
    void EndGenshinPresent(const std::shared_ptr<const GenshinInputRetirement>& receipt, bool presented);
    void SuspendGenshinPresent();
    // Wrapper must call BEFORE either color CopyResource/transition path.
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> GenshinColorWriteQueue(
        IDXGISwapChain* swapchain, ID3D12Device* device);
    bool PrepareGenshinColorWrite(ID3D12Resource* color, ID3D12CommandQueue* writeQueue = nullptr);
    bool SetGenshinResource(const std::shared_ptr<const GenshinInputRetirement>& receipt, Dx12Resource* input);
    bool SetGenshinUiMask(const std::shared_ptr<const GenshinInputRetirement>& receipt,
        const std::shared_ptr<DrawUiMask::Frame>& mask);
    // Ordinary selected-SR guides. Genshin cannot enter this API; its sealed
    // FinishedPicture/Present transaction remains the only guide authority.
    bool OwnsUpscalerDepthContract() const;
    bool OwnsNativeZzzSource() const;
    bool NativeGuidePresentToken(sl::FrameToken*& token, dlssg::NativeSource& source);
    bool SetUpscalerGuidePair(ID3D12GraphicsCommandList* list, const NVSDK_NGX_Parameter* parameters,
        ID3D12Resource* depth, ID3D12Resource* motion, UINT width, UINT height, UINT mvWidth, UINT mvHeight,
        D3D12_RESOURCE_STATES depthState, D3D12_RESOURCE_STATES motionState);
    void RejectUpscalerGuideSource(const char* stage);
    // Existing native Execute/Reset hooks only; no alternate producer or queue.
    bool TracksNativeGuides() const noexcept { return _nativeGuideTracking.load(std::memory_order_acquire); }
    void NativeGuidesSubmitting(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists);
    void NativeGuidesSubmitted(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists);
    void NativeGuidesReset(ID3D12CommandList* list);
    dlssg::NativeSource NativeGuideSource(ID3D12CommandList* list);
    void CompleteNativeGuideSource(ID3D12CommandList* list, const dlssg::NativeSource& source,
        ID3D12Resource* output, bool succeeded);
    bool SetResource(Dx12Resource* inputResource) override final;
    void SetCommandQueue(FG_ResourceType type, ID3D12CommandQueue* queue) override final;

    void* FrameGenerationContext() override final;
    void* SwapchainContext() override final;

    DLSSG_Dx12() : IFGFeature_Dx12(), IFGFeature()
    {
        if (StreamlineProxy::Module() == nullptr)
            StreamlineProxy::LoadStreamline();

        if (StreamlineProxy::Module() != nullptr && !StreamlineProxy::IsD3D12Inited() &&
            State::Instance().currentD3D12Device != nullptr)
        {
            StreamlineProxy::InitWithD3D12(State::Instance().currentD3D12Device);
        }
    }

    ~DLSSG_Dx12();

    // Inherited via IFGFeature_Dx12
    bool SetInterpolatedFrameCount(UINT interpolatedFrameCount) override;
};

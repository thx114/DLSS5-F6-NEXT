#pragma once

// The API-neutral half of the native input producer: given a FrameInput (a finished picture and the scene's depth copies, as
// D3D12 resources) it runs the optical flow, the trust mask and the guides on one command list of its own, and hands the
// picture and the guides to a consumer. It knows nothing of the game's API (native/FrameContract.h); an adapter supplies the
// frames.
//
// The consumer is not linked in here: the caller passes it in as a function (DLSS-NR's DlssNr::ApplyNativeInput, or an
// upscaler backend through native::VirtualUpscalerDriver, in the game; a stand-in in tests/nr_native_producer_gpu.cpp), so
// this class and everything it uses build and run without either.

#include "FrameContract.h"
#include "NativeWorkDevice.h"
#include "NativeSceneCutPolicy.h"
#include "NrInputObservation.h"

#include "../motion/OpticalFlow_Dx12.h"
#include "../motion/TrustMask_Dx12.h"

#include <functional>
#include <memory>
#include <string>

namespace GenshinPresentNr { class NativeParentConsumer; }

namespace native
{

// What the producer hands its consumer for one frame. The consumer works on the picture in place.
struct NativeFrame
{
    ID3D12Resource* color = nullptr;                          // the picture: in pictureState, and left in it
    DXGI_FORMAT colorFormat = DXGI_FORMAT_UNKNOWN;            // a typed format to view it as (the resource may be typeless)
    D3D12_RESOURCE_STATES pictureState = D3D12_RESOURCE_STATE_COMMON;
    ColorSpace space = ColorSpace::Srgb;
    ID3D12Resource* depth = nullptr;  // the guides (TrustMaskDx12::BuildGuides): picture-sized, unjittered, and in
    ID3D12Resource* motion = nullptr; // NON_PIXEL_SHADER_RESOURCE, which the consumer must leave them in. depth
                                      // is null on a frame without depth.
    ID3D12Resource* historyDistrust = nullptr; // the trust mask (TrustMaskDx12::Mask()): how little the last frame's
                                               // history can be trusted at each pixel, R8_UNORM at the flow's size,
                                               // in a state readable by a non-pixel shader, which the consumer leaves
    bool depthReversed = false;
    bool reset = false;               // continuity reset OR an Act scene-cut reset
    bool continuityReset = false;     // init/resize/device/game reset; independent of policy
    bool sceneCutReset = false;       // only the optional native NR reset request
    bool* sceneCutResetAccepted = nullptr; // synchronous actual Evaluate(reset=1) receipt
    uint64_t guideEpoch = 0, guideCutSerial = 0; // actual flow/trust lifetime and observed cuts
    NrInputObservation::Depth depthObservation; // this exact producer frame, never FG/raw preview

  private:
    friend class NativeProducer;
    friend class VirtualUpscalerDriver;
    // Synchronous, private owner seam. Only NativeProducer may close/submit
    // its recording; only its virtual SR consumer may request the split.
    // The resource graph is retained BEFORE Execute, through the final tail.
    struct BeforeSrResult { bool continued = false, filtered = false; };
    using BeforeSrFn = std::function<BeforeSrResult(ID3D12GraphicsCommandList*&, ID3D12Resource*, DXGI_FORMAT,
        D3D12_RESOURCE_STATES, const std::shared_ptr<void>&)>;
    BeforeSrFn beforeSr_;
    friend class GenshinPresentNr::NativeParentConsumer;
    // Minted only after this frame's successful BuildGuides, never from telemetry.
    // Finished-picture UV, prev-current in picture pixels, no raster jitter term.
    bool pictureTemporalGuides_ = false;
    // Separate capability: actual finished-picture flow + trust, without claiming depth.
    bool pictureFlowGuides_ = false;
    bool guideSceneCut_ = false; // real observed cut, independent of model Off/Count/Act
};

// How a hard scene cut is handled, end to end. Three signals can report one cut; Run() makes it one Result::sceneCut
// and at most one optional NR reset request (Off/Count/Act). A request is not
// acceptance: the Driver supplies a synchronous successful model-reset receipt,
// and the producer acknowledges it only after submission succeeds. Flow/trust
// scene handling is independent of that policy; unrelated consumers keep the
// legacy contract. Each flow readback carries its original policy epoch.
//
// Signal 1, the flow's detector (OpticalFlowDx12, Tuning().sceneCutDetector, on by default). On the GPU it compares the
// brightness histogram of this picture with the last one's; past sceneCutThreshold it sets a flag on that very frame.
// The flow then matches nothing on that frame (zero motion, no confidence) and the trust mask, given the flag in
// Inputs::sceneCut, is distrust everywhere. The consumer sees neither (it takes the guides only), so its history still
// holds the last scene until signal 2 resets it.
//
// Signal 2, the flag's readback. Run() copies the flag into a readback slot of the frame's ring entry and reads it once
// the GPU has finished that frame, usually in the next Run(). A set flag outside the quiet window (below) reports the
// cut (sceneCut, distrustedShare 1) and asks for the consumer's reset (_consumerResetPending): the next time the
// consumer runs, NativeFrame::reset is set, and the request stays until it has run. This restarts the consumer's
// history a couple of frames before signal 3 could.
//
// Signal 3, the mask's late count. TrustMaskDx12 counts on the GPU the frames in which at least cutShare of the pixels
// are fully distrusted (only frames that had a history count) and reports it through SceneCutSeen() a couple of frames
// later. Seen outside the quiet window it reports the cut and asks for the consumer's reset for this frame. With the
// detector on, the flow and the mask carry on, the consumer runs on this frame too (skipping it showed one frame
// without it, a flash over the whole picture) and a quiet window opens. With the detector off, the flow and the mask
// are reset instead and the consumer does not run until they have a flow again; the reset is then kept asked for
// (_consumerResetPending) until it does. This is the only signal then.
//
// The quiet window (_cutQuietUntil, kRing + 2 frames after a reported cut) is what keeps the late count of a cut that
// signal 2 already reported from being a second cut: a signal inside it is ignored. A cut hint from the adapter
// (FrameInput::cutHint) and a size change are not detected cuts: their mandatory
// continuity resets are retained separately from optional scene-cut policy. The
// caller counts Result::sceneCut and logs it (native/NativeDriver.cpp).
class NativeProducer
{
  public:
    // Runs the consumer on `frame`, on `cmd`. True when it ran.
    using ApplyFn = std::function<bool(ID3D12GraphicsCommandList* cmd, const NativeFrame& frame)>;

    struct FilterResult { bool filtered = false, uncertain = false; };
    using FilterFn = std::function<FilterResult(const FrameInput&, SyncPoint)>;
    struct Options
    {
        // Disabled for unrelated consumers. NR Off/Count/Act never gates flow/trust handling.
        bool explicitConsumerReset = false;
        bool sceneCutPolicy = false;
        DlssNrSceneCut::Mode sceneCutMode = DlssNrSceneCut::Mode::Count;
        bool apply = false;           // run the consumer (needs flow; depth is optional); off: flow and trust mask only
        FilterFn beforeSr, beforeNr, finalOriginal;
        bool colorOnly = false;
        bool submitProducerBeforeConsumer = false; // consumer needs the producer GPU work and any owned immediate list ordered first
        bool flowPreview = false;     // also draw the flow picture for the menu
        float previewMaxSpeed = 24.0f; // pixels per frame that show as full brightness
        NrInputObservation::SourceDepth sourceDepth = NrInputObservation::SourceDepth::Unobserved;
    };

    struct Result
    {
        bool submitted = false;  // the work was recorded and sent to the queue (the output is then valid)
        bool flowValid = false;
        bool detectorRan = false;
        bool detectorFailed = false;
        bool trustRan = false;   // the trust mask ran in this frame (depth or not; it degrades gracefully with none)
        bool sceneCut = false;   // a cut was observed; not proof of an accepted NR reset
        float distrustedShare = 0.0f;
        bool nativeRan = false;  // the consumer ran on the picture
        const char* stoppedAt = nullptr; // when nothing was submitted: the step that gave up (for the log)
        long stoppedHr = 0;              // and the HRESULT it got, when it had one
        long deviceRemoved = 0;          // the device's removed reason at that moment (0: not removed)
        NrInputObservation::Depth depthObservation;
    };

    NativeProducer() = default;
    ~NativeProducer();

    NativeProducer(const NativeProducer&) = delete;
    NativeProducer& operator=(const NativeProducer&) = delete;

    // Creates on the exact picture/queue-derived entry; each real object has an ownership receipt.
    // False with Create/GetDevice predicates and HRESULTs in Error()/CreationReport().
    bool Init(const NativeWorkDevice& work, bool prepareGuides = true);
    const std::string& CreationReport() const { return _domain->Report(); }
    bool GuidesReady() const noexcept { return _guidesReady; }
    bool Ready() const { return _initialized; }
    ID3D12Device* Device() const { return _device.Get(); }
    const std::string& Error() const { return _error; }
    bool CanRetire() const;
    bool Quarantined() const { return _quarantined; }
    bool BeforeSrPending() const { return _beforeSrOwned && !CanRetire(); }
    void SceneCutMode(DlssNrSceneCut::Mode mode) { _scenePolicy.SetMode(mode); }
    const NativeSceneCutPolicy& SceneCuts() const { return _scenePolicy; }
    unsigned long long SceneCutReadbackFailures() const { return _cutReadbackFailures; }

    // One frame. `queue` is a direct queue of the creation entry; allocators/fence may expose
    // its separately receipted publicOwner. Work follows input.ready, and output.done is
    // the point it finishes at. The picture is processed in place.
    Result Run(ID3D12CommandQueue* queue, const FrameInput& input, const Options& options, const ApplyFn& apply,
               FrameOutput& output);

    // Owner-thread polling before the next FG motion Prepare. Release only this
    // producer's redundant input reference after its exact final submission.
    // Provider/preview roots and motion-pool admission remain independent.
    unsigned CollectCompletedMotionReferences() noexcept;

    // Explicit continuity reset; detector-only flow/trust resets use ResetGuides instead.
    void Reset();

    // For the menu.
    OpticalFlowDx12* Flow() { return _flow.get(); }
    TrustMaskDx12* Trust() { return _trust.get(); }
    bool PreviewReady() const { return _previewReady; }

  private:
    static constexpr uint32_t kRing = 3;

    bool WaitFor(UINT64 value);
    FilterResult FilterSubmitted(const FilterFn& filter, const FrameInput& input, ID3D12CommandQueue* queue,
                                 ID3D12CommandAllocator* anchor = nullptr);
    bool SubmitBeforeSrPrefix(ID3D12CommandQueue* queue, UINT slot,
        ID3D12GraphicsCommandList*& list, ID3D12CommandAllocator*& allocator,
        const FrameInput& input, const FilterFn& filter, ID3D12Resource* color, DXGI_FORMAT format,
        D3D12_RESOURCE_STATES state, const std::shared_ptr<void>& resources, Result& result, FrameOutput& output);
    HRESULT SignalNext(ID3D12CommandQueue* queue);
    HRESULT ExecuteAndSignal(ID3D12CommandQueue* queue, ID3D12GraphicsCommandList* list, UINT slot) noexcept;
    struct HeldFinalSubmission {
        Microsoft::WRL::ComPtr<ID3D12Device> creator, publicOwner;
        Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
        Microsoft::WRL::ComPtr<ID3D12Fence> fence;
        uint64_t generation=0, inputEpoch=0, frame=0, value=0;
    };
    struct HeldFrame {
        const uint64_t inputEpoch;
        HeldFinalSubmission finalSubmission; // absent until the entire Run succeeds
        FrameInput input;
        std::shared_ptr<void> beforeSrResources; // selected provider + every virtual resource in the prefix/continuation
        Microsoft::WRL::ComPtr<ID3D12Resource> picture, depth[FrameInput::kMaxDepthCopies];
        Microsoft::WRL::ComPtr<ID3D12Fence> ready;
        HeldFrame(const FrameInput& in, uint64_t epoch) : inputEpoch(epoch),input(in),picture(in.picture),ready(in.ready.fence) {
            for (int i=0;i<in.depthCount;++i) depth[i]=in.depth[i];
        }
    };
    std::unique_ptr<NativeCreationDomain> _domain = std::make_unique<NativeCreationDomain>();
    std::unique_ptr<HeldFrame> _held[kRing];
    const DWORD _heldOwnerThread=GetCurrentThreadId();
    uint64_t _heldInputEpoch=0; // attempt identity, including failed Runs; never wraps
    bool _heldAccessActive=false;
    struct HeldAccessScope {
        bool& active;
        const bool previous;
        explicit HeldAccessScope(bool& value) : active(value),previous(value) {active=true;}
        ~HeldAccessScope() {active=previous;}
    };
    void SealHeldFinalSubmission(UINT slot) noexcept;
    void ResetGuides();
    bool _quarantined = false, _guidesReady = false, _beforeSrOwned = false;
    bool EnsureGuides();
    uint64_t _guideEpoch = 1, _guideCutSerial = 0;

    std::unique_ptr<OpticalFlowDx12> _flow;
    std::unique_ptr<TrustMaskDx12> _trust;
    Microsoft::WRL::ComPtr<ID3D12Device> _device;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> _queue;
    bool _initialized = false;
    ID3D12CommandAllocator* _allocators[kRing] = {};
    ID3D12CommandAllocator* _consumerAllocators[kRing] = {};
    ID3D12GraphicsCommandList* _list = nullptr;
    ID3D12GraphicsCommandList* _consumerList = nullptr;
    ID3D12Fence* _fence = nullptr;
    HANDLE _event = nullptr;
    UINT64 _values[kRing] = {};
    UINT64 _signalled = 0;
    uint64_t _frame = 0;
    uint64_t _cutQuietUntil = 0; // a cut's consumer reset was asked for: the late count of the same cut is not another

    // The flow's scene-cut flag of each frame in the ring, copied out to be read once the GPU has finished that frame.
    ID3D12Resource* _cutReadback[kRing] = {};
    bool _cutPending[kRing] = {};
    NativeSceneCutPolicy _scenePolicy;
    NativeSceneCutPolicy::Stamp _cutStamp[kRing] = {};
    unsigned long long _cutReadbackFailures = 0;
    bool _continuityResetPending = true; // init/resize/device/explicit reset is never policy-gated
    bool _consumerResetPending = false; // a cut was found and the consumer has not run since
    bool _guideSceneCutPending = false; // same real cut retained for picture history under every policy
    uint32_t _width = 0;
    uint32_t _height = 0;
    bool _previewReady = false;
    std::string _error;
};

} // namespace native

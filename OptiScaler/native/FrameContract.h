#pragma once

// The contract between a game-API adapter and the native input producer (DLSS-NR in a game that makes no upscaler call).
//
// The producer (optical flow, trust mask, guides, DLSS-NR) runs on one D3D12 device and knows nothing of the game's API. An
// adapter (Dx12 today; D3D11, D3D10 and D3D9 later) does the rest: it observes how the game uses depth (native/DepthFinderCore.h),
// and once per presented frame it hands the producer a FrameInput, and takes the FrameOutput back. Everything crosses as D3D12
// resources and fence points, never as pointers into the game's API, so the same contract works when the adapter lives in
// another process (the 32-bit bridge) and the resources are shared handles. See docs/NATIVE-INPUT-ADAPTERS.md.

#include <d3d12.h>
#include <dxgi.h>
#include <cstdint>
#include <memory>

namespace HoYoShade::NaturalAssociation {class Source;}
namespace GenshinGameMotion {class Lease;}
namespace GenshinGuides {class PreparedDepthSource;}
namespace GenshinPresentNr {class NativeColorCapability;}
namespace native {class NativeSubmissionReceipt;}
namespace DrawUiMask {class Frame;}

namespace native
{

enum class Api
{
    D3D9,
    D3D10,
    D3D11,
    D3D12,
    Vulkan
};

// The frame number native input hands to NR (DlssNr::ApplyNativeInput's epoch): the present count of the API the picture
// came from, `dxgiPresents` (State::frameCount) or `vulkanPresents` (State::vulkanPresentCount). Neither a sum nor the
// larger of the two moves by exactly 1 per frame: a dxvk game ticks both, on different threads, and a native Vulkan game
// bridged to frame generation ticks the DXGI one for every generated frame. Detail reuse reads any other step as a
// skipped frame, and a count that stops moving keeps a new NR model on "Preparing".
inline uint64_t PresentEpoch(Api api, uint64_t dxgiPresents, uint64_t vulkanPresents)
{
    return api == Api::Vulkan ? vulkanPresents : dxgiPresents;
}

// What the picture's values mean. The producer supports Srgb (SDR) and ScRgb (linear, 1.0 = 80 nits) today; Pq (HDR10) is
// reported, not processed.
enum class ColorSpace
{
    Srgb,
    ScRgb,
    Pq
};

// A point on a D3D12 fence: wait until `value` has been reached before using what it guards. A null fence is "already there".
struct SyncPoint
{
    ID3D12Fence* fence = nullptr;
    uint64_t value = 0;
    std::shared_ptr<const NativeSubmissionReceipt> submission; // optional exact successfully submitted native batch
};

// A producer-owned prepared depth reader lease. The adapter may expose the
// resource only while Ready() is true; the producer is not allowed to release
// its source/copy until BOTH the copy-ready and reader-done fences complete.
// RetireReader() requests retirement; it is never a GPU completion witness.
// Missing/failed completion retains ownership and disables another depth lease.
class BridgeDepthLease
{
  public:
    virtual ~BridgeDepthLease() = default;
    virtual bool Ready() const noexcept = 0;
    // Called BEFORE the D3D11 source read. Retain both shared texture interfaces
    // and its copy-ready fence even if Acquire/Return later aborts.
    virtual bool BeginReader(IUnknown*, ID3D12Resource*, ID3D12Fence*, uint64_t) noexcept = 0;
    virtual bool MatchesReader(ID3D12Resource*, ID3D12Fence*, uint64_t) const noexcept = 0;
    virtual bool BindReaderCompletion(ID3D12Fence*, uint64_t) noexcept = 0;
    virtual bool RetireReader() noexcept = 0;
    virtual bool ReaderRetired() const noexcept = 0;
};

using BridgeDepthLeasePtr = std::shared_ptr<BridgeDepthLease>;

// Whether the adapter could read the game's depth at all (D3D9 depth is often not readable). The producer shows it as a
// status line; without depth it runs on motion only.
enum class DepthReadability
{
    Readable,
    NotReadable,
    Unknown
};

// Observation only. NativeInputColourSpace does not expose its private-data
// HRESULT, so even its exact-swapchain raw result has UNKNOWN transfer and
// default-versus-private provenance. No default G22/global !HDR inference.
enum class NativePictureColorOrigin : unsigned { Unobserved, SwapchainHelperDefaultOrPrivateUnknown };
struct NativePictureColorObservation
{
    DXGI_FORMAT sourceFormat = DXGI_FORMAT_UNKNOWN, sharedFormat = DXGI_FORMAT_UNKNOWN;
    DXGI_COLOR_SPACE_TYPE rawColorSpace = DXGI_COLOR_SPACE_CUSTOM;
    NativePictureColorOrigin origin = NativePictureColorOrigin::Unobserved;
    bool knownHdr = false; // conservative rejection only, never SDR authority
    bool operator==(const NativePictureColorObservation&) const noexcept = default;
};

// The picture-space producer may only resample a depth plane whose source
// already follows that picture. A producer-grid extent/lease alone is not a
// Scene/Picture UV transform. Other adapters keep their existing picture contract.
enum class DepthCoordinates : uint8_t { Picture, UnmappedProducer };

struct FrameInput
{
    static constexpr int kMaxDepthCopies = 8;

    // Retained by HeldFrame through the actual consumer fence/quarantine.
    std::shared_ptr<DrawUiMask::Frame> uiMask;
    Api api = Api::D3D12;
    uint64_t frame = 0; // the adapter's present count

    // The finished picture, as a D3D12 resource the producer may read and write (NR's result is written over it).
    ID3D12Resource* picture = nullptr;
    DXGI_FORMAT pictureFormat = DXGI_FORMAT_UNKNOWN; // a typed format to read it through (the resource may be typeless)
    D3D12_RESOURCE_STATES pictureState = D3D12_RESOURCE_STATE_COMMON; // the state it is in on entry and must be left in
    ColorSpace colorSpace = ColorSpace::Srgb;
    uint32_t width = 0;
    uint32_t height = 0;

    // The scene's depth: one or more copies of the picked buffer (a game that splits the scene over several lists leaves each
    // copy holding part of it; the nearest surface over all of them is the scene). They rest in
    // NON_PIXEL_SHADER_RESOURCE | PIXEL_SHADER_RESOURCE and are read through depthView.
    ID3D12Resource* depth[kMaxDepthCopies] = {};
    int depthCount = 0;
    DXGI_FORMAT depthView = DXGI_FORMAT_UNKNOWN; // a typed format that reads the depth: R32_FLOAT, R16_UNORM, ...
    uint32_t depthWidth = 0;
    uint32_t depthHeight = 0;
    bool depthReversed = false; // near is 1.0
    bool depthValidityPaired = false; // R32G32_FLOAT: x=device Z, y=scene-valid (0/1)
    DepthCoordinates depthCoordinates = DepthCoordinates::Picture;
    DepthReadability depthReadability = DepthReadability::Readable;

    // Present-side producer depth provenance. These fields are meaningful only
    // when depthCount == 1; any mismatch must leave the frame motion-only.
    uint64_t depthGeneration = 0, depthSourceFrame = 0, depthPictureSerial = 0, depthEpoch = 0;
    uint32_t depthOriginX = 0, depthOriginY = 0;
    bool depthYFlip = false, depthCoverage = false, depthProducerReady = false;
    ID3D12Resource* depthResource = nullptr;
    BridgeDepthLeasePtr depthLease;
    // Immutable original producer semantics, held through the existing native
    // input/reader fence. Picture authority still requires executed mapping.
    std::shared_ptr<const GenshinGuides::PreparedDepthSource> preparedDepthSource;
    // Optional authentic Genshin motion, consumed only by its FG recording.
    std::shared_ptr<GenshinGameMotion::Lease> gameMotion;

    // Color scheduling is independent of NR guide admission. The private
    // D3D12 texture still returns to this exact natural game backbuffer.
    uint64_t naturalDevice = 0, naturalBackbuffer = 0;
    // Local synchronous filter association only, never transport/GPU authority.
    // NativeDriver moves this out before producer input retention.
    std::shared_ptr<const HoYoShade::NaturalAssociation::Source> naturalSource;
    bool colorOnly = false, finalOriginal = false, colorTransferKnown = false;
    NativePictureColorObservation nativePictureColor;
    // The adapter alone mints this after actual Present match/copy/upload and
    // successful BindInput. Driver moves it out before HeldFrame retention;
    // only an admitted owned transaction retains the final-picture contents.
    std::shared_ptr<const GenshinPresentNr::NativeColorCapability> beforeNrNativeColor;

    bool cutHint = false; // the adapter knows this frame is not a continuation (a resize, a loading screen)
    SyncPoint ready;      // the producer waits on this before reading `picture` and `depth`
};

struct FrameOutput
{
    SyncPoint done; // the adapter waits on this before it presents `picture` (the same resource, processed in place)
    bool processed = false; // actual NR/virtual consumer, never a filter counter
    bool filtered = false;
    bool uncertain = false; // recorded/submitted work lacks a usable completion witness
    bool PictureChanged() const noexcept { return processed || filtered; }
};

// Why Acquire gave no frame.
enum class AcquireStatus
{
    Ready,              // a frame was filled in
    WaitingForUpscaler, // the game calls an upscaler of its own: this is not the case the producer is for (the status line says so)
    Unavailable         // nothing to process now: no picture could be had, a resource could not be shared
};

// An adapter for one game API. All calls come on the thread that presents unless the adapter's own API says otherwise.
class IFrameSource
{
  public:
    virtual ~IFrameSource() = default;

    virtual Api GetApi() const = 0;

    // The D3D12 device and direct queue the producer runs on, valid after an Acquire that returned Ready. For a D3D12
    // game they are the game's own; a D3D11 adapter has a private pair (the picture is shared across to it).
    virtual ID3D12Device* Device() const = 0;
    virtual ID3D12CommandQueue* Queue() const = 0;

    // Once per presented frame, before the overlay is drawn: when a frame is ready, fill `input`. Missing depth is not a
    // failure (the producer then runs the flow only); the game's picture is left alone whenever the status is not Ready.
    virtual AcquireStatus Acquire(FrameInput& input) = 0;

    // The producer finished with the frame it was given. The adapter gets its picture ready for the game's present: for a
    // zero-copy adapter nothing; for a shared-texture adapter, a copy back into the game's back buffer.
    virtual void Return(const FrameInput& input, const FrameOutput& output) = 0;

    // The picture size or the device changed; drop what depends on them.
    virtual void OnResize() = 0;
};

} // namespace native

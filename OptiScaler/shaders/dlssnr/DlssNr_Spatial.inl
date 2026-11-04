// Compress screen edges, D3D12 (menu: NR Input, "Compress screen edges"; ini [DlssNr] Spatial*).
//
// NR works on a smaller picture: the middle of the frame stays 1:1 and the edges are squeezed into the rest of the space
// (DlssNr_Spatial.h has the layout, precompile/dlssnr_spatial.hlsl the three passes). A frame goes
//   encode -> pack the colour (100) and the depth + motion (101) -> the model at the packed size ->
//   unpack the packed model input and the model's answer to the ordinary grid (102) -> the resolve.
// The ordinary grid is the one NR works on without compression (the model resolution, 16-pixel grid), so the resolve,
// the enlargement and the supersample down-leg see the same kind of pair they always did, only softer at the edges:
// both halves go through the same resampling, so what the resolve carries over to the frame is the model's change.
//
// Motion is packed by its end points (Pack(p + v) - Pack(p)), so the packed vectors are exact in packed pixels and the
// model gets a scale of 1. The packed textures are RGBA16F, also the model's own input and output surfaces: the answer
// is a resample of a resample, not something the swap chain's format should quantise. (So against no compression an 8 or
// 10-bit game's model works on RGBA16F; what is bit-identical is the 1:1 middle between the Replace correction, mode 103,
// and the plain unpack, mode 102.)
//
// Anything that goes wrong turns compression off (Status::TurnedOff*), not NR: the next frame runs the ordinary path,
// and a change of layout (or Retry) tries again. Reuse detail between frames does not run while this does.
//
// Included inside DlssNr_Dx12.cpp's anonymous namespace, after DlssNr_SceneCut.inl. Dispatch calls Begin where it
// decides the frame, Prepare once the layout is known to be wanted, and the rest around the model, all under g_nrMutex.
// The menu reads published, a copy made under a lock of its own.
namespace EdgeCompression
{
namespace Sp = DlssNr::Spatial;

constexpr DXGI_FORMAT kFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

Sp::Tracker tracker;

const Sp::Layout& Layout() { return tracker.layout(); }

ID3D12Resource* color = nullptr;  // the packed colour (model input)
ID3D12Resource* depth = nullptr;  // R32F
ID3D12Resource* motion = nullptr; // RG32F
ID3D12Resource* proxy = nullptr;  // the packed model input, unpacked to the ordinary grid
ID3D12Resource* answer = nullptr; // the model's answer, unpacked the same way
ID3D12Resource* proxyNative = nullptr;  // supersampling only: both averaged down to native
ID3D12Resource* answerNative = nullptr;
OS_Dx12* proxyDown = nullptr; // the down-leg for the proxy; the answer goes through g_nr.superDown, the same filter
Scaler proxyDownScaler = Scaler::Count; // the filter proxyDown was built with: it must be the answer's, or halos
unsigned int madeFor[6] = {}; // packed w/h, ordinary w/h, native w/h the textures above were made for
bool madeSuper = false;

// Descriptor-only cache: no resources, heaps, fences or vectors are allocated by admission.
// Keep the driver allocation query off the steady-state path. Eviction only forgets an estimate.
struct AllocationEstimate
{
    ID3D12Device* device = nullptr;
    D3D12_RESOURCE_DESC desc {};
    uint64_t bytes = 0;
};
AllocationEstimate allocationEstimates[64] {};
unsigned nextAllocationEstimate = 0;

bool SameDesc(const D3D12_RESOURCE_DESC& a, const D3D12_RESOURCE_DESC& b)
{
    return a.Dimension == b.Dimension && a.Alignment == b.Alignment && a.Width == b.Width &&
           a.Height == b.Height && a.DepthOrArraySize == b.DepthOrArraySize && a.MipLevels == b.MipLevels &&
           a.Format == b.Format && a.SampleDesc.Count == b.SampleDesc.Count &&
           a.SampleDesc.Quality == b.SampleDesc.Quality && a.Layout == b.Layout && a.Flags == b.Flags;
}

uint64_t AllocationBytes(ID3D12Device* device, const D3D12_RESOURCE_DESC& desc)
{
    for (const auto& estimate : allocationEstimates)
        if (estimate.device == device && SameDesc(estimate.desc, desc)) return estimate.bytes;
    if (device == nullptr) return 0;
    const auto info = device->GetResourceAllocationInfo(0, 1, &desc);
    auto& estimate = allocationEstimates[nextAllocationEstimate++ % std::size(allocationEstimates)];
    estimate = { device, desc, info.SizeInBytes };
    return info.SizeInBytes;
}

D3D12_RESOURCE_DESC ScratchDesc(DXGI_FORMAT format, unsigned w, unsigned h)
{
    D3D12_RESOURCE_DESC desc {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = w;
    desc.Height = h;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    return desc;
}

// A whole-frame reservation counts old surfaces until TickNrRetired has actually released them.
// Even an unknown/unsubmitted/device-removed token stays charged; fence completion alone does
// not subtract a resource still present in the retirement list. Current resources remain charged
// too when their replacement is reserved. There is no fence wait or frame-count retirement here.
struct FrameReservation
{
    ID3D12Device* device;
    Sp::ScratchUsage usage;
    ID3D12Resource* seen[Sp::kScratchSlotLimit] {};
    unsigned seenCount = 0;

    explicit FrameReservation(ID3D12Device* d) : device(d) {}

    void Keep(ID3D12Resource* resource)
    {
        if (!resource) return;
        for (unsigned i = 0; i < seenCount; ++i)
            if (seen[i] == resource) return;
        if (seenCount == std::size(seen)) { usage.known = false; return; }
        seen[seenCount++] = resource;
        usage.Add(AllocationBytes(device, resource->GetDesc()));
    }

    void Require(ID3D12Resource* resource, const D3D12_RESOURCE_DESC& want, bool replace = false)
    {
        Keep(resource);
        if (!resource || replace || !SameDesc(resource->GetDesc(), want))
            usage.Add(AllocationBytes(device, want));
    }

    void Require(ID3D12Resource* resource, DXGI_FORMAT format, unsigned w, unsigned h, bool replace = false)
    {
        Require(resource, ScratchDesc(format, w, h), replace);
    }
};

// Called under g_nrMutex before Prepare, feature creation, encode, guide packing or model work.
// The descriptor-ring reservation in Dispatch is a separate necessary condition. Provider-private
// feature allocations are opaque; the existing previous-generation fence drain still guards them.
// colourFormat describes canonical internal surfaces; gameColourFormat is exact for direct game copies.
// They must remain separate even for RGBA8/BGRA8: a copy-surface replacement keeps the old allocation
// charged and reserves the new exact descriptor without declaring the whole model generation stale.
bool ReserveFrame(ID3D12Device* device, const Config& cfg, const Sp::Layout& l, unsigned passes,
                  DXGI_FORMAT colourFormat, DXGI_FORMAT gameColourFormat, DXGI_FORMAT keepFormat,
                  bool cropColor, bool normalizeGuides,
                  const DlssNr::GuideRegions& guides, ID3D12Resource* depthSource, ID3D12Resource* motionSource,
                  bool beforeUpscale, bool rayReconstruction)
{
    FrameReservation plan(device);
    for (const auto& retired : g_nrRetired) plan.Keep(retired.resource);
    for (auto* current : { color, depth, motion, proxy, answer, proxyNative, answerNative,
             g_nr.output, g_nr.passScratch, g_nr.passClampScratch, g_nr.passClampScratch2, g_nr.colorCopy,
             g_nr.hdrCopy, g_nr.colorSmall, g_nr.outputNative, g_nr.activeColor, g_nr.lutScratch,
             g_nr.guidePreview, g_nr.heldColor, g_nr.depthClone, g_nr.motionClone, g_nr.coordinateDepth,
             g_nr.coordinateMotion, g_nr.depthConstant, g_nr.meter, g_nr.autoExposure, g_nr.autoExposureRaw,
             g_nr.calib, g_nr.diagGridReadback, g_nr.diagExposureReadback, g_nr.diagProxyReadback })
        plan.Keep(current);
    for (auto* current : g_nr.meterReadback) plan.Keep(current);
    for (auto* current : g_nr.calibReadback) plan.Keep(current);
    // Reuse is gated off for a packed frame, but its old history may still be resident
    // before BeforeModel parks it. Moving it to g_nrRetired later must not hide its cost.
    for (auto* current : { DetailReuse::detail[0], DetailReuse::detail[1], DetailReuse::colourDepth[0],
             DetailReuse::colourDepth[1], DetailReuse::prevMotion[0], DetailReuse::prevMotion[1],
             DetailReuse::shownDetail, DetailReuse::composed, DetailReuse::estimate, DetailReuse::steadied,
             DetailReuse::temporalFused, DetailReuse::debugAnswer, DetailReuse::coverage })
        plan.Keep(current);
    for (const auto& slot : DetailReuse::coverageSlots) plan.Keep(slot.buffer);

    const bool supersample = l.globalScale > 1.0f;
    const unsigned want[6] = { l.modelW, l.modelH, l.ordinaryW, l.ordinaryH, l.nativeW, l.nativeH };
    const bool replaceSpatial = !color || !std::equal(std::begin(want), std::end(want), std::begin(madeFor)) ||
                                madeSuper != supersample;
    plan.Require(color, kFormat, l.modelW, l.modelH, replaceSpatial);
    plan.Require(depth, DXGI_FORMAT_R32_FLOAT, l.modelW, l.modelH, replaceSpatial);
    plan.Require(motion, DXGI_FORMAT_R32G32_FLOAT, l.modelW, l.modelH, replaceSpatial);
    plan.Require(proxy, kFormat, l.ordinaryW, l.ordinaryH, replaceSpatial);
    plan.Require(answer, kFormat, l.ordinaryW, l.ordinaryH, replaceSpatial);
    if (supersample)
    {
        plan.Require(proxyNative, kFormat, l.nativeW, l.nativeH, replaceSpatial);
        plan.Require(answerNative, kFormat, l.nativeW, l.nativeH, replaceSpatial);
    }

    const bool staleFormat = (g_nr.output && g_nr.output->GetDesc().Format != kFormat) ||
                            (g_nr.colorCopy && g_nr.colorCopy->GetDesc().Format != colourFormat);
    const bool resolutionChanged = g_nr.width != l.nativeW || g_nr.height != l.nativeH ||
                                   g_nr.workWidth != l.modelW || g_nr.workHeight != l.modelH;
    const bool placementChanged = g_nr.feature &&
        (g_nr.beforeUpscale != beforeUpscale || g_nr.rayReconstruction != rayReconstruction);
    const bool replaceModel = staleFormat || (g_nr.feature && (resolutionChanged || placementChanged));
    plan.Require(g_nr.output, kFormat, l.modelW, l.modelH, replaceModel);
    plan.Require(g_nr.colorCopy, colourFormat, l.nativeW, l.nativeH, replaceModel || !g_nr.output);
    plan.Require(g_nr.hdrCopy, keepFormat, l.nativeW, l.nativeH, replaceModel);
    if (cropColor) plan.Require(g_nr.activeColor, gameColourFormat, l.nativeW, l.nativeH, replaceModel);
    if (passes > 1)
    {
        plan.Require(g_nr.passScratch, kFormat, l.modelW, l.modelH, replaceModel);
        plan.Require(g_nr.passClampScratch, kFormat, l.modelW, l.modelH, replaceModel);
    }
    if (passes > 2) plan.Require(g_nr.passClampScratch2, kFormat, l.modelW, l.modelH, replaceModel);
    const bool reduced = l.ordinaryW != l.nativeW || l.ordinaryH != l.nativeH;
    if (reduced && DlssNrProxyCurve::IsReplace(cfg.DlssNrReversibleMode.value_or_default()))
        plan.Require(g_nr.colorSmall, colourFormat, l.ordinaryW, l.ordinaryH, replaceModel);
    if (l.globalScale != 1.0f && !supersample)
        plan.Require(g_nr.outputNative, colourFormat, l.nativeW, l.nativeH, replaceModel);
    if (!cfg.DlssNrLutFile.value_or_default().empty())
        plan.Require(g_nr.lutScratch, gameColourFormat, l.nativeW, l.nativeH, replaceModel);
    if (cfg.DlssNrGuidePreview.value_or_default())
        plan.Require(g_nr.guidePreview, gameColourFormat, l.nativeW, l.nativeH);
    if (cfg.DlssNrHoldFrame.value_or_default())
        plan.Require(g_nr.heldColor, gameColourFormat, l.nativeW, l.nativeH, !g_nr.heldActive);

    if (normalizeGuides)
    {
        const unsigned w = std::max(guides.depth.width, guides.motion.width);
        const unsigned h = std::max(guides.depth.height, guides.motion.height);
        plan.Require(g_nr.coordinateDepth, DXGI_FORMAT_R32_FLOAT, w, h);
        plan.Require(g_nr.coordinateMotion, DXGI_FORMAT_R32G32_FLOAT, w, h);
    }
    const auto clone = [&](ID3D12Resource* source, ID3D12Resource* current)
    {
        if (!source || !IsTypeless(source->GetDesc().Format)) return;
        auto desc = source->GetDesc();
        desc.Format = TypedGuideFormat(desc.Format);
        desc.Flags = D3D12_RESOURCE_FLAG_NONE;
        plan.Require(current, desc);
    };
    clone(depthSource, g_nr.depthClone);
    clone(motionSource, g_nr.motionClone);
    plan.Require(g_nr.meter, DXGI_FORMAT_R32_FLOAT, kDlssNrMeterGrid, kDlssNrMeterGrid);
    plan.Require(g_nr.autoExposure, DXGI_FORMAT_R32_FLOAT, 1, 1);
    if (!g_nr.autoExposureRawFailed) plan.Require(g_nr.autoExposureRaw, DXGI_FORMAT_R32_FLOAT, 1, 1);
    if (!g_nr.meter)
    {
        D3D12_RESOURCE_DESC readback {};
        readback.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        readback.Width = kMeterBytes;
        readback.Height = readback.DepthOrArraySize = readback.MipLevels = 1;
        readback.SampleDesc.Count = 1;
        readback.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        // Four meter and three diagnostic buffers are allocated in this branch.
        for (unsigned i = 0; i < 7; ++i) plan.Require(nullptr, readback);
    }

    const bool allowed = plan.usage.Allowed();
    static bool blocked = false;
    if (!allowed && !blocked)
        LOG_WARN("DLSS-NR spatial frame admission blocked: scratchBytes={} scratchSlots={} known={} "
                 "limits={}/{}; whole frame bypassed without waiting or changing settings",
                 plan.usage.bytes, plan.usage.slots, plan.usage.known, Sp::kScratchByteLimit, Sp::kScratchSlotLimit);
    else if (allowed && blocked)
        LOG_INFO("DLSS-NR spatial frame admission recovered: scratchBytes={} scratchSlots={}",
                 plan.usage.bytes, plan.usage.slots);
    blocked = !allowed;
    return allowed;
}

std::mutex publishedLock;
Sp::Published published;

void Publish(const Sp::Layout& l, Sp::Status status)
{
    Sp::Published p = Sp::Publish(l, status, false);
    std::lock_guard<std::mutex> lock(publishedLock);
    published = p;
}

void Release()
{
    for (ID3D12Resource** r : { &color, &depth, &motion, &proxy, &answer, &proxyNative, &answerNative })
        ParkNrResource(*r);

    for (unsigned int& v : madeFor)
        v = 0;
}

void ReleaseDown()
{
    delete proxyDown;
    proxyDown = nullptr;
    proxyDownScaler = Scaler::Count;
}

// What makes last frame's packed picture a different one besides the layout: the formats and sizes that are packed.
uint64_t SignatureOf(DXGI_FORMAT colour, DXGI_FORMAT depthFormat, unsigned int depthW, unsigned int depthH,
                     DXGI_FORMAT motionFormat, unsigned int motionW, unsigned int motionH)
{
    uint64_t hash = 0;
    for (const uint64_t v : { (uint64_t) colour, (uint64_t) depthFormat, (uint64_t) depthW, (uint64_t) depthH,
                              (uint64_t) motionFormat, (uint64_t) motionW, (uint64_t) motionH })
        hash = Sp::Mix(hash, v);
    return hash;
}

// Decides this frame. Returns whether the picture is packed. resetHistory is set when the packed picture is a different
// one from last frame's.
bool Begin(const Config& cfg, unsigned int width, unsigned int height, float scale, bool proxyBackend,
           uint64_t signature, bool& resetHistory, bool gameFgInput)
{
    const Sp::Settings settings = Sp::ReadSettings(cfg);
    const Sp::Tracker::Frame frame = tracker.Begin(settings, width, height, scale, true, proxyBackend, signature, gameFgInput);
    const Sp::Layout& next = tracker.layout();

    if (frame.resetHistory)
        resetHistory = true;

    Publish(next, frame.status);

    if (frame.changed)
    {
        if (frame.active)
            LOG_INFO("DLSS-NR compress screen edges: on, the model works on {}x{} instead of {}x{} ({:.0f}% of the "
                     "pixels), middle {:.0f}%x{:.0f}% of the frame",
                     next.modelW, next.modelH, next.ordinaryW, next.ordinaryH,
                     100.0 * next.modelW * next.modelH / (double) (next.ordinaryW * (double) next.ordinaryH),
                     100.0 * (next.centerBounds.right - next.centerBounds.left),
                     100.0 * (next.centerBounds.bottom - next.centerBounds.top));
        else if (settings.enabled)
            LOG_INFO("DLSS-NR compress screen edges: not applied ({})", Sp::Describe(frame.status));
        else
            LOG_INFO("DLSS-NR compress screen edges: off");
    }

    return frame.active;
}

// Compression turned itself off; NR carries on from the next frame without it.
void TurnOff(Sp::Status why)
{
    tracker.TurnOff(why);
    Publish(tracker.layout(), why);
    LOG_WARN("DLSS-NR compress screen edges turned itself off: {}; NR continues without it", Sp::Describe(why));
}

bool Make(ID3D12Resource*& slot, ID3D12Device* device, DXGI_FORMAT format, unsigned int w, unsigned int h)
{
    slot = CreateScratch(device, format, w, h);
    return slot != nullptr;
}

// The textures for this layout, made again when the sizes change. False if one could not be made.
bool Prepare(ID3D12Device* device, const Sp::Layout& l, bool supersample)
{
    const unsigned int want[6] = { l.modelW, l.modelH, l.ordinaryW, l.ordinaryH, l.nativeW, l.nativeH };

    if (color && depth && motion && proxy && answer && (!supersample || (proxyNative && answerNative)) &&
        std::equal(std::begin(want), std::end(want), std::begin(madeFor)) &&
        madeSuper == supersample)
        return true;

    Release();

    bool ok = Make(color, device, kFormat, l.modelW, l.modelH) &&
              Make(depth, device, DXGI_FORMAT_R32_FLOAT, l.modelW, l.modelH) &&
              Make(motion, device, DXGI_FORMAT_R32G32_FLOAT, l.modelW, l.modelH) &&
              Make(proxy, device, kFormat, l.ordinaryW, l.ordinaryH) &&
              Make(answer, device, kFormat, l.ordinaryW, l.ordinaryH);

    if (ok && supersample)
        ok = Make(proxyNative, device, kFormat, l.nativeW, l.nativeH) &&
             Make(answerNative, device, kFormat, l.nativeW, l.nativeH);

    if (!ok)
    {
        Release();
        return false;
    }

    std::copy(std::begin(want), std::end(want), std::begin(madeFor));
    madeSuper = supersample;
    return true;
}

// NR was shut down.
void Shutdown()
{
    for (ID3D12Resource** r : { &color, &depth, &motion, &proxy, &answer, &proxyNative, &answerNative })
    {
        if (*r != nullptr)
            (*r)->Release();
        *r = nullptr;
    }

    for (unsigned int& v : madeFor)
        v = 0;

    ReleaseDown();
    for (auto& estimate : allocationEstimates) estimate = {};
    nextAllocationEstimate = 0;
    tracker.Reset();
    std::lock_guard<std::mutex> lock(publishedLock);
    published = {};
}
} // namespace EdgeCompression

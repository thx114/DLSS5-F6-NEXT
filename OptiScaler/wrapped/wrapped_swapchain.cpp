#include "pch.h"
#include <framegen/FinalOutputEndpoint.h>
#include <integration/HoYoShade.h>
#include <optional>
#include <hooks/FG_Hooks.h>
#include <genshin/GenshinLegacyCapture.h>
#include <genshin/GenshinDx12Transport.h>
#include <genshin/GenshinHookCpuTiming.h>
#include <genshin/GenshinPresentLifecycle.h>
#include <genshin/GenshinPresentNr.h>
#include "wrapped_swapchain.h"
#include <dlssnr/DlssNr.h>
#include <dlssnr/DlssNrCurrentOutputCapture.h>
#include <hooks/DxgiSwapchainSizing.h>
#include <mods/gimi/GimiRuntime.h>
#include <resource_tracking/GenericDepth_Dx11.h>

#include <Util.h>
#include <Logger.h>
#include <Config.h>

#include <nvapi/fakenvapi.h>
#include <hooks/Reflex_Hooks.h>
#include <hooks/D3D12_Hooks.h>
#include <with_dx12/dx11_with_dx12_sync.h>
#include <with_dx12/with_dx12.h>
#include <with_dx12/GenshinVisibleChild.h>
#include <inputs/GenshinFsrContract.h>

#include <menu/menu_overlay_dx.h>
#include <menu/menu_overlay_base.h>
#include <native/NativeDriverDx12.h>
#include <native/OpticalFgNrPolicy.h>
#include <native/NativeLowLatency.h>
#include <native/VkPresentBridge.h>

#include <misc/FrameLimit.h>

#include <d3d11.h>
#include <d3d12.h>
#include <misc/IdentifyGpu.h>
#include <misc/REFrameworkCompat.h>
#include <hooks/Xell_Hooks.h>

#include <magic_enum.hpp>

#ifdef LOW_LATENCY_INPUTS
#include <low_latency/input/input_antilag2.h>
#endif

#ifdef DXGI_DEBUG_ENABLED
#include <magic_enum.hpp>
#include <dxgidebug.h>

#pragma comment(lib, "dxguid.lib")

#ifdef ENABLE_DEBUG_LAYER_DX12
#include <d3d12sdklayers.h>
#endif
#endif

#pragma intrinsic(_ReturnAddress)

// Used RenderDoc's wrapped object as referance
// https://github.com/baldurk/renderdoc/blob/v1.x/renderdoc/driver/dxgi/dxgi_wrapped.cpp

static int scCount = 0;
static UINT64 _frameCounter = 0;
static double _lastFrameTime = 0;
static bool _dx11Device = false;
static bool _dx12Device = false;

const GUID IID_IUnwrappedDXGISwapChain = {
    0xe8a33b4a, 0x1405, 0x424c, { 0xae, 0x88, 0xd, 0x3e, 0x9d, 0x46, 0xc9, 0x14 }
};

static ID3D12Fence* resizeFence = nullptr;
static UINT64 resizeFenceValue = 0;
static HANDLE resizeFenceEvent = nullptr;

static void UpdateOutputColorSpace(DXGI_COLOR_SPACE_TYPE colorSpace)
{
    auto& state = State::Instance();

    OutputColorSpace info {};
    info.dxgiColorSpace = colorSpace;

    switch (colorSpace)
    {
        // ------------------------------------------------------------
        // SDR / Rec.709
        // ------------------------------------------------------------

    case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709:
        info.transfer = ColorTransfer::SRGB;
        info.primaries = ColorPrimaries::Rec709;
        info.range = ColorRange::Full;
        info.model = ColorModel::RGB;
        info.valid = true;

        break;

    case DXGI_COLOR_SPACE_RGB_STUDIO_G22_NONE_P709:
        info.transfer = ColorTransfer::SRGB;
        info.primaries = ColorPrimaries::Rec709;
        info.range = ColorRange::Studio;
        info.model = ColorModel::RGB;
        info.valid = true;

        break;

        // ------------------------------------------------------------
        // scRGB / linear Rec.709
        // ------------------------------------------------------------

    case DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709:
        info.transfer = ColorTransfer::Linear;
        info.primaries = ColorPrimaries::Rec709;
        info.range = ColorRange::Full;
        info.model = ColorModel::RGB;
        info.valid = true;

        break;

        // ------------------------------------------------------------
        // HDR10 / PQ / Rec.2020
        // ------------------------------------------------------------

    case DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020:
        info.transfer = ColorTransfer::PQ;
        info.primaries = ColorPrimaries::Rec2020;
        info.range = ColorRange::Full;
        info.model = ColorModel::RGB;
        info.valid = true;

        break;

    case DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020:
        info.transfer = ColorTransfer::PQ;
        info.primaries = ColorPrimaries::Rec2020;
        info.range = ColorRange::Studio;
        info.model = ColorModel::RGB;
        info.valid = true;

        break;

    case DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020:
    case DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_TOPLEFT_P2020:
        info.transfer = ColorTransfer::PQ;
        info.primaries = ColorPrimaries::Rec2020;
        info.range = ColorRange::Studio;
        info.model = ColorModel::YCbCr;
        info.valid = true;

        break;

        // ------------------------------------------------------------
        // HLG / Rec.2020
        // ------------------------------------------------------------

    case DXGI_COLOR_SPACE_YCBCR_FULL_GHLG_TOPLEFT_P2020:
        info.transfer = ColorTransfer::HLG;
        info.primaries = ColorPrimaries::Rec2020;
        info.range = ColorRange::Full;
        info.model = ColorModel::YCbCr;
        info.valid = true;

        break;

    case DXGI_COLOR_SPACE_YCBCR_STUDIO_GHLG_TOPLEFT_P2020:
        info.transfer = ColorTransfer::HLG;
        info.primaries = ColorPrimaries::Rec2020;
        info.range = ColorRange::Studio;
        info.model = ColorModel::YCbCr;
        info.valid = true;

        break;

        // ------------------------------------------------------------
        // Unknown / unsupported
        // ------------------------------------------------------------

    default:
        info.transfer = ColorTransfer::Unknown;
        info.primaries = ColorPrimaries::Unknown;
        info.range = ColorRange::Unknown;
        info.model = ColorModel::Unknown;
        info.valid = false;

        break;
    }

    state.outputColorSpace = info;

    LOG_INFO("Output color space updated: DXGI: {}, Transfer: {}, Primaries: {}, Range: {}, Model: {}, Valid: {}",
             magic_enum::enum_name(info.dxgiColorSpace), magic_enum::enum_name(info.transfer),
             magic_enum::enum_name(info.primaries), magic_enum::enum_name(info.range),
             magic_enum::enum_name(info.model), info.valid);
}

static void WaitForGPUIdle(IUnknown* object)
{
    if (State::Instance().currentD3D12Device == nullptr || object == nullptr)
        return;

    ID3D12CommandQueue* queue = nullptr;

    if (object->QueryInterface(IID_PPV_ARGS(&queue)) == S_OK)
    {
        LOG_DEBUG("Command queue obtained for GPU idle wait");
        queue->Release();
    }

    if (queue != nullptr && resizeFence != nullptr && resizeFenceEvent != nullptr)
    {
        if (State::Instance().currentD3D12Device != nullptr)
        {
            if (resizeFence != nullptr)
            {
                resizeFence->Release();
                resizeFence = nullptr;
            }

            if (resizeFenceEvent != nullptr)
            {
                CloseHandle(resizeFenceEvent);
                resizeFenceEvent = nullptr;
            }

            State::Instance().currentD3D12Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&resizeFence));
            resizeFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        }

        LOG_DEBUG("Waiting for GPU to finish before resizing buffers");

        resizeFenceValue++;
        queue->Signal(resizeFence, resizeFenceValue);

        if (resizeFence->GetCompletedValue() < resizeFenceValue)
        {
            resizeFence->SetEventOnCompletion(resizeFenceValue, resizeFenceEvent);
            // Max 5 sec
            auto waitResult = WaitForSingleObject(resizeFenceEvent, 5000);
            LOG_DEBUG("WaitForSingleObject result: {:X}", waitResult);
        }
    }
}

#ifdef DXGI_DEBUG_ENABLED
void ReportDXGILiveObjects()
{
    IDXGIDebug1* dxgiDebug = nullptr;

    if (SUCCEEDED(DXGIGetDebugInterface1(0, IID_PPV_ARGS(&dxgiDebug))))
    {
        dxgiDebug->ReportLiveObjects(DXGI_DEBUG_ALL, DXGI_DEBUG_RLO_ALL);
        dxgiDebug->Release();
    }
}

void ReadDxgiInfoQueue()
{
    IDXGIInfoQueue* dxgiInfoQueue = nullptr;
    if (DXGIGetDebugInterface1(0, IID_PPV_ARGS(&dxgiInfoQueue)) == S_OK)
    {
        UINT64 msgCount = dxgiInfoQueue->GetNumStoredMessages(DXGI_DEBUG_ALL);
        for (UINT64 i = 0; i < msgCount; ++i)
        {
            SIZE_T msgLen = 0;
            dxgiInfoQueue->GetMessage(DXGI_DEBUG_ALL, i, nullptr, &msgLen);
            std::vector<char> buf(msgLen);
            auto* msg = reinterpret_cast<DXGI_INFO_QUEUE_MESSAGE*>(buf.data());
            dxgiInfoQueue->GetMessage(DXGI_DEBUG_ALL, i, msg, &msgLen);

            auto description = std::string(msg->pDescription, msg->DescriptionByteLength);
            LOG_DEBUG("DXGI Debug Message: Category: {}, Severity: {}, ID: {}, Description: {}",
                      magic_enum::enum_name(msg->Category), magic_enum::enum_name(msg->Severity), msg->ID, description);
        }
    }
}

#ifdef ENABLE_DEBUG_LAYER_DX12
void ReportD3D12LiveObjects(ID3D12Device* device)
{
    ID3D12DebugDevice* debugDevice = nullptr;

    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&debugDevice))))
    {
        debugDevice->ReportLiveDeviceObjects(D3D12_RLDO_DETAIL | D3D12_RLDO_IGNORE_INTERNAL);
        debugDevice->Release();
    }
}
#endif
#endif

// R082: observation of the actual native API boundary, not LocalPresent's
// overlay/NR work. Like MenuCpuWindow, use bounded TLS storage and one-second
// aggregation. QPC host elapsed includes descheduling/driver waits, not GPU
// execution, scheduled-thread CPU or displayed FPS. Never take its reciprocal.
namespace
{
LONGLONG GenshinPresentCpuTick() noexcept
{
    const DWORD error = GetLastError();
    LARGE_INTEGER value {};
    const bool valid = QueryPerformanceCounter(&value) != FALSE;
    SetLastError(error);
    return valid ? value.QuadPart : 0;
}

// R086: distinguish a confirmed other executable from a failed identity read.
// Cached module metadata only; no path allocation, filesystem scan or polling.
struct GenshinPresentCpuProcess
{
    bool Known = false, Genshin = false;
    DWORD Error = ERROR_SUCCESS;
    const char* Reason = "module_filename_failed";
};
const GenshinPresentCpuProcess& GenshinPresentCpuProcessFact() noexcept
{
    static const GenshinPresentCpuProcess fact = []() noexcept {
        wchar_t path[MAX_PATH] {};
        const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
        if (length == 0) return GenshinPresentCpuProcess { false, false, GetLastError(), "module_filename_failed" };
        if (length >= MAX_PATH)
            return GenshinPresentCpuProcess { false, false, ERROR_INSUFFICIENT_BUFFER, "module_filename_truncated" };
        const wchar_t* name = wcsrchr(path, L'\\');
        name = name ? name + 1 : path;
        const bool match = _wcsicmp(name, L"YuanShen.exe") == 0 || _wcsicmp(name, L"GenshinImpact.exe") == 0;
        return GenshinPresentCpuProcess { true, match, ERROR_SUCCESS, match ? "exact_genshin_executable" : "other_executable" };
    }();
    return fact;
}

// R123: host elapsed of the existing Present-side timer reads only. This is
// diagnostic state, never a read-frequency, feature or resource admission gate.
enum class GenshinTimerReadStage : unsigned { Upscaler, Detailed };
ULONGLONG GenshinTimerReadMsTick() noexcept
{
    const DWORD error = GetLastError();
    const ULONGLONG tick = GetTickCount64();
    SetLastError(error);
    return tick;
}
struct GenshinTimerReadMetric
{
    UINT64 Attempts = 0, Calls = 0, Ticks = 0, Maximum = 0;
    UINT64 BeginFailures = 0, EndFailures = 0, OrderFailures = 0, FrequencyUnavailable = 0;
    // Fixed 0.1 ms bins; the final bin is [12.7 ms, infinity).
    std::array<UINT64, 128> Histogram {};
    void Add(UINT64 ticks, double msPerTick) noexcept
    {
        ++Calls;
        Ticks += ticks;
        if (ticks > Maximum) Maximum = ticks;
        const double tenths = ticks * msPerTick * 10.0;
        const unsigned bin = tenths >= 127.0 ? 127u : static_cast<unsigned>(tenths);
        ++Histogram[bin];
    }
};
struct GenshinTimerReadWindow
{
    // Menu and FPS bits are snapshots immediately before the timer reads. They
    // do not assert that the overlay subsequently rendered on this Present.
    std::array<std::array<GenshinTimerReadMetric, 2>, 4> Buckets {};
    LONGLONG Start = 0;
    ULONGLONG AttemptStartMs = 0;
    bool HasAttempts = false;
    UINT64 QpcFailures = 0, ReportQpcFailures = 0;
    double MsPerTick = 0.0;
    GenshinTimerReadWindow() noexcept
    {
        const DWORD error = GetLastError();
        LARGE_INTEGER frequency {};
        if (QueryPerformanceFrequency(&frequency) && frequency.QuadPart > 0)
            MsPerTick = 1000.0 / static_cast<double>(frequency.QuadPart);
        SetLastError(error);
    }
    void Add(unsigned context, GenshinTimerReadStage stage, LONGLONG begin, LONGLONG end) noexcept
    {
        auto& metric = Buckets[context][static_cast<unsigned>(stage)];
        if (!begin) { ++metric.BeginFailures; ++QpcFailures; }
        if (!end) { ++metric.EndFailures; ++QpcFailures; }
        if (!begin || !end) return;
        if (end < begin) { ++metric.OrderFailures; ++QpcFailures; return; }
        if (!Start) Start = begin;
        metric.Add(static_cast<UINT64>(end - begin), MsPerTick);
    }
    void Report() noexcept
    {
        if (!HasAttempts) return;
        LONGLONG end = 0;
        bool qpcDue = false;
        if (Start && MsPerTick > 0.0)
        {
            end = GenshinPresentCpuTick();
            if (!end || end < Start) { ++QpcFailures; ++ReportQpcFailures; }
            else qpcDue = (end - Start) * MsPerTick >= 1000.0;
        }
        // Normal windows retain the original QPC cadence. A failed read or
        // frequency uses the independent monotonic tick only for report timing.
        if (!qpcDue && !QpcFailures) return;
        const ULONGLONG nowMs = GenshinTimerReadMsTick();
        if (!qpcDue && nowMs - AttemptStartMs < 1000) return;
        const bool fallbackClock = !qpcDue;
        const auto snapshot = Buckets;
        const auto start = Start;
        const auto attemptStartMs = AttemptStartMs;
        const auto qpcFailures = QpcFailures;
        const auto reportQpcFailures = ReportQpcFailures;
        Buckets = {};
        QpcFailures = 0;
        ReportQpcFailures = 0;
        Start = 0;
        HasAttempts = false;
        AttemptStartMs = 0;
        const DWORD error = GetLastError();
        try
        {
            for (unsigned context = 0; context < 4; ++context)
                for (unsigned stage = 0; stage < 2; ++stage)
                {
                    const auto& metric = snapshot[context][stage];
                    if (!metric.Attempts) continue;
                    const UINT64 rank = (metric.Calls * 95 + 99) / 100;
                    UINT64 cumulative = 0;
                    unsigned p95Bin = 127;
                    for (unsigned bin = 0; bin < 128; ++bin)
                    {
                        cumulative += metric.Histogram[bin];
                        if (cumulative >= rank) { p95Bin = bin; break; }
                    }
                    LOG_INFO("R123 timer-read CPU: pid={} tid={} window_start_qpc={} window_end_qpc={} window_ms={:.3f} "
                             "stage={} calls={} measured_n={} host_ms_total={:.3f} host_ms_mean={:.4f} host_ms_max={:.4f} "
                             "p95_bin_lower_ms={:.1f} p95_bin_upper_ms={:.1f} p95_upper_known={} "
                             "menu_visible_at_read={} fps_overlay_requested_at_read={} qpc_failures={} "
                             "qpc_begin_failures={} qpc_end_failures={} qpc_order_failures={} "
                             "qpc_frequency_unavailable={} qpc_report_failures={} "
                             "report_clock={} window_start_tick_ms={} window_end_tick_ms={} "
                             "fg_requested=false nr_requested=false fg_instance_present=false external_fg=false "
                             "host_ms_source=qpc_valid_pairs_only gpu_ms=UNKNOWN display_fps=UNKNOWN p95_bin_width_ms=0.1",
                        GetCurrentProcessId(), GetCurrentThreadId(), start, end,
                        fallbackClock ? static_cast<double>(nowMs - attemptStartMs) : (end - start) * MsPerTick,
                        stage == 0 ? "ReadUpscalerTime" : "ReadDetailedGpuTimes", metric.Attempts, metric.Calls,
                        metric.Ticks * MsPerTick, metric.Calls ? metric.Ticks * MsPerTick / metric.Calls : 0.0,
                        metric.Maximum * MsPerTick, metric.Calls ? p95Bin * 0.1 : -1.0,
                        metric.Calls && p95Bin != 127 ? (p95Bin + 1) * 0.1 : -1.0,
                        metric.Calls && p95Bin != 127,
                        (context & 1u) != 0, (context & 2u) != 0, qpcFailures,
                        metric.BeginFailures, metric.EndFailures, metric.OrderFailures,
                        metric.FrequencyUnavailable, reportQpcFailures,
                        fallbackClock ? "tickcount64" : "qpc", attemptStartMs, nowMs);
                }
        }
        catch (...) {} // Reporting must not change Present or feature behavior.
        SetLastError(error);
    }
};
GenshinTimerReadWindow& GenshinTimerReadWindowForThread() noexcept
{
    static thread_local GenshinTimerReadWindow window;
    return window;
}
class GenshinTimerReadScope final
{
    GenshinTimerReadWindow* Window = nullptr;
    GenshinTimerReadStage Stage;
    unsigned Context = 0;
    LONGLONG Begin = 0;
public:
    GenshinTimerReadScope(bool enabled, unsigned context, GenshinTimerReadStage stage) noexcept
        : Stage(stage), Context(context)
    {
        if (!enabled) return;
        Window = &GenshinTimerReadWindowForThread();
        if (!Window->HasAttempts)
        {
            Window->AttemptStartMs = GenshinTimerReadMsTick();
            Window->HasAttempts = true;
        }
        ++Window->Buckets[Context][static_cast<unsigned>(Stage)].Attempts;
        if (Window->MsPerTick > 0.0) Begin = GenshinPresentCpuTick();
        else
        {
            ++Window->Buckets[Context][static_cast<unsigned>(Stage)].FrequencyUnavailable;
            ++Window->QpcFailures;
        }
    }
    ~GenshinTimerReadScope() noexcept
    {
        if (Window && Window->MsPerTick > 0.0)
            Window->Add(Context, Stage, Begin, GenshinPresentCpuTick());
    }
    GenshinTimerReadScope(const GenshinTimerReadScope&) = delete;
    GenshinTimerReadScope& operator=(const GenshinTimerReadScope&) = delete;
};

struct GenshinPresentCpuMetric
{
    UINT64 Ticks = 0, Minimum = UINT64_MAX, Maximum = 0, Samples = 0;
    // [0,1), [1,4), [4,8), [8,16), [16,24), [24,40),
    // [40,80), [80,160), [160,+inf) milliseconds; no clipped samples.
    std::array<UINT64, 9> Histogram {};
    void Add(UINT64 ticks, double msPerTick) noexcept
    {
        Ticks += ticks;
        if (ticks < Minimum) Minimum = ticks;
        if (ticks > Maximum) Maximum = ticks;
        ++Samples;
        constexpr double limits[] = { 1, 4, 8, 16, 24, 40, 80, 160 };
        const double ms = ticks * msPerTick;
        unsigned bin = 0;
        while (bin < 8 && ms >= limits[bin]) ++bin;
        ++Histogram[bin];
    }
};

struct GenshinPresentCpuWindow
{
    struct Key
    {
        // Only a known existing ticket lends a lifetime-bearing key. For an
        // unproven lifetime, pool single-call wall by observed labels below;
        // never cache a naked chain/COM address as a pairing identity.
        std::uintptr_t Chain = 0;
        UINT64 Identity = 0, SwapchainEpoch = 0;
        GenshinLegacy::Presentation::Owner Owner {};
        UINT Sync = 0, Flags = 0;
        int DetectedInterpolations = 0;
        bool Present1 = false, IdentityKnown = false, LifetimeKnown = false;
        bool D3D11Observed = false, NativeParentOwnerObserved = false;
        bool FgRequestedObserved = false, NrRequestedObserved = false;
        bool FeaturePresentObserved = false, FgInstancePresentObserved = false, ExternalFgObserved = false;
        bool operator==(const Key&) const = default;
    };
    struct Bucket
    {
        Key Route {};
        GenshinPresentCpuMetric Wall, EntryInterval, OutsideWall;
        UINT64 Returned = 0, Ok = 0, Status = 0, Failed = 0, TimingFailures = 0;
        UINT64 ReenteredSkipped = 0, PairSkipped = 0;
        std::uintptr_t LastChainObserved = 0;
        UINT64 LastIdentityObserved = 0;
        HRESULT LastResult = S_OK;
        bool Used = false;
    };
    std::array<Bucket, 8> Buckets {};
    Key PreviousRoute {};
    LONGLONG PreviousEntry = 0, PreviousReturn = 0;
    UINT64 PreviousAttempt = 0;
    bool PreviousValid = false, Reentered = false;
    unsigned InFlight = 0;
    ULONGLONG Start = GetTickCount64();
    UINT64 Unbucketed = 0;
    double MsPerTick = 0.0;

    GenshinPresentCpuWindow() noexcept
    {
        LARGE_INTEGER frequency {};
        if (QueryPerformanceFrequency(&frequency) && frequency.QuadPart > 0)
            MsPerTick = 1000.0 / static_cast<double>(frequency.QuadPart);
    }
    void ClearBuckets() noexcept
    {
        for (auto& bucket : Buckets) bucket = {};
        Unbucketed = 0;
    }
    void Report(ULONGLONG now)
    {
        if (now - Start < 1000) return;
        // Snapshot only, at the first reporting boundary. This says nothing
        // about entering FrameLimit::sleep or the duration of any limiter wait.
        static std::atomic<bool> limitReported { false };
        if (!limitReported.exchange(true, std::memory_order_relaxed))
            LOG_INFO("R082 Present config snapshot: pid={} FramerateLimit_effective={} snapshot=first_report_only limiter_wait=UNKNOWN",
                GetCurrentProcessId(), Config::Instance()->FramerateLimit.value_or_default());
        for (const auto& bucket : Buckets)
        {
            if (!bucket.Used || bucket.Returned == 0) continue;
            std::string phases;
            const GenshinPresentCpuMetric* metrics[] = { &bucket.Wall, &bucket.EntryInterval, &bucket.OutsideWall };
            constexpr const char* names[] = { "present_wall", "entry_interval", "outside_wall" };
            for (unsigned i = 0; i < 3; ++i)
            {
                const auto& metric = *metrics[i];
                const double total = metric.Ticks * MsPerTick;
                const auto& h = metric.Histogram;
                phases += std::format(" {}_n={} {}_ms_total={:.3f} {}_ms_avg={:.3f} {}_ms_min={:.3f} {}_ms_max={:.3f} {}_hist=[{},{},{},{},{},{},{},{},{}]",
                    names[i], metric.Samples, names[i], total,
                    names[i], metric.Samples ? total / metric.Samples : 0.0,
                    names[i], metric.Samples ? metric.Minimum * MsPerTick : 0.0,
                    names[i], metric.Maximum * MsPerTick, names[i],
                    h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7], h[8]);
            }
            LOG_INFO("R082 Present CPU window: schema=R086 pid={} tid={} window_ms={} sc_last_observed={:X} identity_last_observed={} identity_known={} lifetime_known={} swapchain_epoch={} owner_epoch={} device_epoch={} controls_epoch=UNKNOWN api={} SyncInterval={} Flags={:X} test=false test_excluded=true d3d11_observed={} native_parent_owner_observed={} fg_requested_observed={} nr_requested_observed={} feature_present_observed={} fg_instance_present_observed={} external_fg_observed={} detected_interpolations_observed={} sr_execution=UNKNOWN returned={} ok={} status={} failed={} last_hr={:08X} timing_ready={} timing_failures={} reentered_skipped={} pair_skipped={} pair_unknown_lifetime={} unbucketed={} clock=qpc_host_elapsed gpu_ms=UNKNOWN display_fps=UNKNOWN pair=existing_ticket_owner_swapchain_epoch_adjacent_attempt_same_observed_key wall_unproven_lifetime_pool={} outside_includes_diagnostics=true hist_ms_edges=1,4,8,16,24,40,80,160{}",
                GetCurrentProcessId(), GetCurrentThreadId(), now - Start, bucket.LastChainObserved,
                bucket.LastIdentityObserved, bucket.Route.IdentityKnown, bucket.Route.LifetimeKnown,
                bucket.Route.SwapchainEpoch, bucket.Route.Owner.ownerEpoch, bucket.Route.Owner.deviceEpoch,
                bucket.Route.Present1 ? "Present1" : "Present", bucket.Route.Sync, bucket.Route.Flags,
                bucket.Route.D3D11Observed, bucket.Route.NativeParentOwnerObserved,
                bucket.Route.FgRequestedObserved, bucket.Route.NrRequestedObserved, bucket.Route.FeaturePresentObserved,
                bucket.Route.FgInstancePresentObserved, bucket.Route.ExternalFgObserved, bucket.Route.DetectedInterpolations,
                bucket.Returned, bucket.Ok, bucket.Status, bucket.Failed, static_cast<unsigned>(bucket.LastResult),
                MsPerTick > 0.0, bucket.TimingFailures, bucket.ReenteredSkipped, bucket.PairSkipped,
                bucket.Route.LifetimeKnown ? UINT64(0) : bucket.Returned, Unbucketed, !bucket.Route.LifetimeKnown, phases);
        }
        ClearBuckets();
        // Do not advance PreviousReturn past logger cost: that wall time really
        // lies outside the original API and must remain visible on the next pair.
        Start = GetTickCount64();
    }
};

class GenshinPresentCpuScope final
{
    GenshinPresentCpuWindow* Window = nullptr;
    GenshinPresentCpuWindow::Bucket* Bucket = nullptr;
    LONGLONG Entry = 0, PreviousEntry = 0, PreviousReturn = 0;
    UINT64 Attempt = 0;
    bool ReportDue = false, Finished = false;

  public:
    GenshinPresentCpuScope(IDXGISwapChain* chain, UINT sync, UINT flags, bool present1,
                           UINT64 identity, bool d3d11Observed, bool nativeParentOwnerObserved,
                           const GenshinLegacy::Presentation::Ticket& ticket) noexcept
    {
        const DWORD error = GetLastError();
        try
        {
            const auto& process = GenshinPresentCpuProcessFact();
            if (!process.Known)
            {
                static std::atomic<bool> reported { false };
                if (!reported.exchange(true, std::memory_order_relaxed))
                    LOG_WARN("R082 Present observation unavailable: schema=R086 pid={} genshin_identity=UNKNOWN reason={} win32_error={} report=once no_sampling=true",
                        GetCurrentProcessId(), process.Reason, process.Error);
            }
            else if (process.Genshin)
            {
                static thread_local GenshinPresentCpuWindow window;
                Window = &window;
                if (++window.InFlight != 1)
                {
                    window.Reentered = true;
                    window.PreviousValid = false;
                }
                else
                {
                    window.Reentered = false;
                    const bool previousValid = window.PreviousValid;
                    window.PreviousValid = false;
                    // No business predicate admits/rejects this observation.
                    // These are entry-time scalar labels, not resource gates or
                    // evidence of SR execution/FG activity throughout a window.
                    if (!(flags & DXGI_PRESENT_TEST))
                    {
                        ReportDue = true;
                        const auto& state = State::Instance();
                        const auto* config = Config::Instance();
                        GenshinPresentCpuWindow::Key key {};
                        key.Sync = sync;
                        key.Flags = flags;
                        key.Present1 = present1;
                        key.IdentityKnown = identity != 0;
                        key.LifetimeKnown = key.IdentityKnown && ticket.Known() && ticket.swapchain == identity &&
                            ticket.owner.thread == GetCurrentThreadId() && ticket.flags == flags && ticket.present1 == present1;
                        if (key.LifetimeKnown)
                        {
                            key.Chain = reinterpret_cast<std::uintptr_t>(chain);
                            key.Identity = identity;
                            key.SwapchainEpoch = ticket.swapchainEpoch;
                            key.Owner = ticket.owner;
                            Attempt = ticket.attempt;
                        }
                        key.D3D11Observed = d3d11Observed;
                        key.NativeParentOwnerObserved = nativeParentOwnerObserved;
                        key.FgRequestedObserved = config->FGEnabled.value_or_default();
                        key.NrRequestedObserved = config->DlssNrEnabled.value_or_default();
                        key.FeaturePresentObserved = state.currentFeature != nullptr;
                        key.FgInstancePresentObserved = state.currentFG != nullptr;
                        key.ExternalFgObserved = state.externalFrameGeneration;
                        key.DetectedInterpolations = state.dlssgDetectedInterpolationCount;
                        for (auto& bucket : window.Buckets)
                            if (bucket.Used && bucket.Route == key) { Bucket = &bucket; break; }
                        if (Bucket == nullptr)
                            for (auto& bucket : window.Buckets)
                                if (!bucket.Used) { bucket.Used = true; bucket.Route = key; Bucket = &bucket; break; }
                        if (Bucket != nullptr)
                        {
                            // Last-observed addresses describe this bucket's
                            // latest call only; they never establish a lifetime.
                            Bucket->LastChainObserved = reinterpret_cast<std::uintptr_t>(chain);
                            Bucket->LastIdentityObserved = identity;
                            if (previousValid && key.LifetimeKnown && window.PreviousRoute == key &&
                                window.PreviousAttempt < UINT64_MAX && Attempt == window.PreviousAttempt + 1)
                            {
                                PreviousEntry = window.PreviousEntry;
                                PreviousReturn = window.PreviousReturn;
                            }
                            // Last diagnostic action before the raw call.
                            Entry = GenshinPresentCpuTick();
                        }
                        else ++window.Unbucketed; // Fixed capacity, explicitly reported.
                    }
                }
            }
        }
        catch (...) { if (Window) Window->PreviousValid = false; }
        SetLastError(error);
    }
    void Finish(HRESULT result) noexcept
    {
        const DWORD error = GetLastError();
        const LONGLONG end = Bucket ? GenshinPresentCpuTick() : 0;
        Finished = true;
        if (Window && Window->InFlight == 1)
        {
            try
            {
                if (Bucket)
                {
                    ++Bucket->Returned;
                    ++Bucket->PairSkipped; // Removed only when a proven pair is actually recorded.
                    Bucket->Ok += result == S_OK;
                    Bucket->Status += result != S_OK && SUCCEEDED(result);
                    Bucket->Failed += FAILED(result);
                    Bucket->LastResult = result;
                    if (Window->Reentered)
                    {
                        // Exclude the entire inclusive outer sample, not just
                        // its pairs. Count once per returned outer call in the
                        // existing bucket; ClearBuckets resets it each window.
                        ++Bucket->ReenteredSkipped;
                        Window->PreviousValid = false;
                    }
                    else if (Window->MsPerTick > 0.0 && Entry > 0 && end >= Entry)
                    {
                        Bucket->Wall.Add(static_cast<UINT64>(end - Entry), Window->MsPerTick);
                        if (!Window->Reentered && PreviousEntry > 0 && PreviousReturn >= PreviousEntry && Entry >= PreviousReturn)
                        {
                            Bucket->EntryInterval.Add(static_cast<UINT64>(Entry - PreviousEntry), Window->MsPerTick);
                            Bucket->OutsideWall.Add(static_cast<UINT64>(Entry - PreviousReturn), Window->MsPerTick);
                            --Bucket->PairSkipped;
                        }
                        if (Bucket->Route.LifetimeKnown)
                        {
                            Window->PreviousRoute = Bucket->Route;
                            Window->PreviousEntry = Entry;
                            Window->PreviousReturn = end;
                            Window->PreviousAttempt = Attempt;
                            Window->PreviousValid = !Window->Reentered;
                        }
                        // Unknown/missing lifetime still contributes single-call
                        // wall, but never seeds a later entry/outside pair.
                    }
                    else ++Bucket->TimingFailures;
                }
                if (ReportDue) Window->Report(GetTickCount64());
            }
            catch (...)
            {
                // Only diagnostics are contained. The actual API call, HRESULT,
                // hook callbacks and their exception behavior remain untouched.
                Window->PreviousValid = false;
                Window->ClearBuckets();
                Window->Start = GetTickCount64();
            }
        }
        SetLastError(error);
    }
    ~GenshinPresentCpuScope() noexcept
    {
        if (!Window) return;
        // An original Present exception propagates normally; it cannot create a
        // completed return stamp or a pair with a later call.
        if (!Finished) Window->PreviousValid = false;
        --Window->InFlight;
    }
    GenshinPresentCpuScope(const GenshinPresentCpuScope&) = delete;
    GenshinPresentCpuScope& operator=(const GenshinPresentCpuScope&) = delete;
};
} // namespace

static bool IsStarRailProcess() noexcept
{
    const auto error = GetLastError();
    static const bool starRail = []() noexcept {
        try { return _wcsicmp(Util::ExePath().filename().c_str(), L"StarRail.exe") == 0; }
        catch (...) { return false; }
    }();
    SetLastError(error);
    return starRail;
}
// R847: immutable physical-wrapper binding; filter Endpoint admission is separate.
// Keep identities as scalars and never query/reenter a provider on Present.
template<class F> static HRESULT ObserveStarRailFinalRaw(
    const void* endpoint,IDXGISwapChain* chain,std::uintptr_t creationQueue,
    UINT sync,UINT flags,bool present1,F&& raw)
{
    if(!GenshinHookCpuTiming::Enabled() || !endpoint || !creationQueue)return raw();
    if(!IsStarRailProcess())return raw();
    GenshinHookCpuTiming::FinalRawPresentScope timing(chain,reinterpret_cast<const void*>(creationQueue),endpoint,sync,flags,present1);
    return timing.Return(raw());
}

// Only the ordinary existing native D3D11 route can acquire a GIMI global
// pair. Provider/physical children, DXVK/Vulkan and interop output have no
// authenticated global-command contract here. Begin also checks the exact
// swap chain's D3D11 device/context against the retained capture generation.
static void BeginGimiNativePresent(gimi::runtime::NativePresentScope& scope, IDXGISwapChain* chain,
                                   UINT sync, UINT flags, bool present1, bool providerOutput) noexcept
{
    if (!gimi::runtime::NeedsPresent() || (flags & DXGI_PRESENT_TEST)) return;
    static thread_local bool preparing = false;
    if (preparing) return; // creation/preparation reentry never recursively prepares a plan
    preparing = true;
    struct Unprepare { bool& value; ~Unprepare() { value = false; } } unprepare{preparing};
    try
    {
        if (providerOutput || State::Instance().isShuttingDown ||
            State::Instance().swapchainInteropApi != SwapchainInteropApi::None ||
            State::Instance().swapchainApi == Vulkan || IdentifyGpu::getPrimaryGpu().usesDxvk) return;
        GenericDepthDx11::PrepareModPresent(chain);
        scope.Begin(chain, sync, flags, present1);
    }
    catch (...) { gimi::runtime::StopAccepting(); } // optional failure cannot suppress the original call
}
template<class F> static HRESULT CallGimiNativePresent(IDXGISwapChain* chain, UINT sync, UINT flags,
                                                       bool present1, bool providerOutput, F&& original)
{
    gimi::runtime::NativePresentScope scope;
    BeginGimiNativePresent(scope, chain, sync, flags, present1, providerOutput);
    const HRESULT result = original(); // exactly once; an exception only cancels the pair
    scope.Returned(chain, sync, flags, present1, result);
    return result;
}

static HRESULT LocalPresent(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags,
                            const DXGI_PRESENT_PARAMETERS* pPresentParameters, IUnknown* pDevice, HWND hWnd, bool isUWP,
                            std::uint64_t& genshinPresentIdentity, bool capturePolled = false, bool* actualCall = nullptr,
                            framegen::final_output::Endpoint* finalOutput = nullptr,
                            bool starRailProviderOutput = false,
                            const void* rawEndpoint = nullptr,
                            std::uintptr_t rawCreationQueueInput = 0)
{
    GenshinHookCpuTiming::HookScope r448LocalHook(pPresentParameters?GenshinHookCpuTiming::Hook::Present1:GenshinHookCpuTiming::Hook::Present,
        GenshinHookCpuTiming::presentOwner==nullptr || GenshinHookCpuTiming::presentOwner->InOriginal());
    auto& r448Hook=r448LocalHook.Active()?r448LocalHook:
        (GenshinHookCpuTiming::presentOwner?*GenshinHookCpuTiming::presentOwner:r448LocalHook);
    GenshinHookCpuTiming::PresentOwnerScope r448LocalOwner(r448Hook);
    const auto rawPresent=[&](const DXGI_PRESENT_PARAMETERS* params){
        return ObserveStarRailFinalRaw(rawEndpoint,pSwapChain,rawCreationQueueInput,SyncInterval,Flags,params!=nullptr,[&]{
            return params ? static_cast<IDXGISwapChain1*>(pSwapChain)->Present1(SyncInterval,Flags,params)
                          : pSwapChain->Present(SyncInterval,Flags);
        });
    };

    GenshinTransport::PresentOutputScope outputSubmission(!(Flags & DXGI_PRESENT_TEST));
    if (HoYoShade::ColorConsumers(pSwapChain) != HoYoShade::ConsumerAccess::Safe) return E_FAIL;
    // Physical child output can run nested or on a provider worker. It owns
    // only output counters and immutable menu replay, never native State clocks.
    const bool physicalChild=GenshinNativeParentFrame::IsChildOutput(pSwapChain,hWnd);
    if (physicalChild || GenshinNativeParentFrame::IsRetainedChildWindow(hWnd))
    {
        if(!(Flags & DXGI_PRESENT_TEST))
            GenshinTransport::FinishPendingOutput(nullptr,GenshinTransport::OutputFlushReason::Consumer);
        static std::atomic<uint64_t> physicalCalls{0},physicalSucceeded{0};
        if (!(Flags & DXGI_PRESENT_TEST))
        {
            ++physicalCalls;
        }
        const auto raw = [&](UINT physicalFlags) {
            if (physicalChild && !(Flags & DXGI_PRESENT_TEST))
                MenuOverlayDx::Present(pSwapChain,SyncInterval,Flags,pPresentParameters,pDevice,hWnd,isUWP,true);
            return pPresentParameters
                ? r448Hook.Original([&]{return static_cast<IDXGISwapChain1*>(pSwapChain)->Present1(SyncInterval,physicalFlags,pPresentParameters);})
                : r448Hook.Original([&]{return pSwapChain->Present(SyncInterval,physicalFlags);});
        };
        // A retained HWND is not source/queue authority. Unknown asynchronous
        // output must not bypass provenance simply because FinalOutput is OFF.
        if(!(Flags & DXGI_PRESENT_TEST) && (!physicalChild || !finalOutput)) {
            GenshinVisibleChild::PresentPermit::PhysicalOutput::Refused("PhysicalEndpointUnavailable",pSwapChain);
            return DXGI_ERROR_WAS_STILL_DRAWING;
        }
        const auto originalRaw=[&]{return raw(Flags);};
        const auto hr = Flags & DXGI_PRESENT_TEST ?
            (finalOutput ? finalOutput->Present(Flags,pPresentParameters,originalRaw) : originalRaw()) :
            finalOutput->PresentGenshin(Flags,pPresentParameters,raw);
        if (!(Flags & DXGI_PRESENT_TEST) && hr==S_OK)++physicalSucceeded;
        return hr;
    }
    GenshinPresentNrTrace::Scope nrTrace(pSwapChain,Flags,State::Instance().frameCount,
        GenshinPresentNrTrace::IsGenshinProcess()?Config::Instance()->LogFileName.value_or_default():L"");
    if (State::Instance().isShuttingDown)
    {
        if(!(Flags & DXGI_PRESENT_TEST))
            GenshinTransport::FinishPendingOutput(nullptr,GenshinTransport::OutputFlushReason::Consumer);
        const auto result=pPresentParameters
            ? r448Hook.Original([&]{return rawPresent(pPresentParameters);})
            : r448Hook.Original([&]{return rawPresent(nullptr);});
        if(actualCall)*actualCall=true;
        return result;
    }

    // Real non-child, non-TEST Present owns settings, including nested children
    // and its menu callback. No boundary is introduced on the Vulkan path.
    std::optional<HoYoShade::RenderOwnerScope> hoYoSettingsOwner;
    if (!(Flags & DXGI_PRESENT_TEST) && State::Instance().swapchainApi != Vulkan)
        hoYoSettingsOwner.emplace();

    // This lock will delay/prevent release of buffers while present is ongoing. Not for a dxvk game on the Vulkan bridge:
    // the swapchain here is then the game's own (frame generation's comes from the system DXGI, unwrapped), and dxvk's
    // present remakes its Vulkan swapchain after a resize, which takes the exclusive side on this same thread
    // (VkPresentBridge BeginResize): it would wait for itself. A native Vulkan game's present takes none either.
    std::shared_lock<std::shared_mutex> dx11wDx12PresentLock(Dx11wDx12Sync::PresentResizeMutex(), std::defer_lock);
    std::optional<GenshinHookCpuTiming::Scope> r448PresentResizeHeld;
    if (State::Instance().swapchainInteropApi != SwapchainInteropApi::None &&
        State::Instance().activeFgOutput == FGOutput::XeFG && !VkPresentBridge::DxvkThroughVulkan())
    {
        GenshinHookCpuTiming::Scope r448Wait(GenshinHookCpuTiming::Stage::PresentResizeLock);
        dx11wDx12PresentLock.lock();r448Wait.Finish();
        r448PresentResizeHeld.emplace(GenshinHookCpuTiming::Stage::PresentResizeLocked);
    }

    LOG_DEBUG("{}", _frameCounter);
    NoteDroppedLogLinesOnPresent();

    HRESULT presentResult;
    MenuOverlayDx::PresentResult overlayResult;
    bool overlayOutputRoute = false, overlayNativeRoute = false, overlayOwner = false;

    auto willPresent = (Flags & DXGI_PRESENT_TEST) == 0;
    // Diagnostic-only cadence, independent of native frame ownership; exclude TEST calls.
    static std::atomic<std::uint64_t> routinePresentLogFrames { 0 };
    const bool logRoutinePresent = willPresent &&
        routinePresentLogFrames.fetch_add(1, std::memory_order_relaxed) % 120 == 0;
    const bool nativeParentOwner = GenshinNativeParentFrame::OwnsNative(pSwapChain, pDevice);
    const bool deferNativeEpoch = GenshinNativeParentFrame::OwnsPresent(pSwapChain, hWnd) ||
        GenshinNativeParentFrame::IsChildOutput(pSwapChain, hWnd);

    if (willPresent)
    {
        double ftDelta = 0.0;

        auto now = Util::MillisecondsNow();

        if (_lastFrameTime != 0)
            ftDelta = now - _lastFrameTime;

        _lastFrameTime = now;
        State::Instance().presentFrameTime = ftDelta;

        if (State::Instance().currentFG == nullptr)
            State::Instance().lastFGFrameTime = ftDelta;

        if (logRoutinePresent)
            LOG_DEBUG("SyncInterval: {}, Flags: {:X}, Frametime: {:0.3f} ms", SyncInterval, Flags, ftDelta);

        // Update swapchain info evey frame
        if (pSwapChain->GetDesc(&State::Instance().currentSwapchainDesc) != S_OK)
            LOG_WARN("Can't get swapchain desc!");
    }

    ID3D11Device* device = nullptr;
    ID3D12Device* device12 = nullptr;
    ID3D12CommandQueue* cq = nullptr;

    bool isD3D11 = false;

    // try to obtain directx objects and find the path
    if (pDevice->QueryInterface(IID_PPV_ARGS(&device)) == S_OK)
    {
        isD3D11 = true;
        device->Release();

        if (!_dx11Device)
            LOG_DEBUG("D3D11Device captured");

        _dx11Device = true;
        State::Instance().swapchainApi = DX11;
        State::Instance().currentD3D11Device = device;
        if (willPresent && !capturePolled) GenshinLegacy::OnPresent(device);
    }
    else if (pDevice->QueryInterface(IID_PPV_ARGS(&cq)) == S_OK)
    {
        cq->Release();

        if (!_dx12Device)
            LOG_DEBUG("D3D12CommandQueue captured");

        ID3D12CommandQueue* realQueue = nullptr;
        if (Util::CheckForRealObject(__FUNCTION__, cq, (IUnknown**) &realQueue))
            cq = realQueue;

        // Not on the Vulkan bridge (native/VkPresentBridge.h): this is XeFG's D3D12 swapchain for a Vulkan game, whose own
        // present sets swapchainApi to Vulkan. Writing D3D12 here made the menu flip between the two. The D3D12 queue and
        // device below are still captured: frame generation needs them.
        if (State::Instance().swapchainInteropApi != SwapchainInteropApi::VkwDx12)
            State::Instance().swapchainApi = DX12;

        // Not while a Vulkan bridge's output is up or being let go of: this is XeFG's present on the bridge's private
        // queue, and the state's pointer to it is cleared on purpose when the output goes (native/VkPresentBridge.cpp)
        if (State::Instance().currentCommandQueue == nullptr && !VkPresentBridge::OutputActive())
            State::Instance().currentCommandQueue = cq;

        if (cq->GetDevice(IID_PPV_ARGS(&device12)) == S_OK)
        {
            device12->Release();

            if (!_dx12Device)
                LOG_DEBUG("D3D12Device captured");

            _dx12Device = true;

            State::Instance().currentD3D12Device = device12;
            D3D12Hooks::HookDevice(device12);
        }
    }

    // On the Vulkan bridge (native/VkPresentBridge.h) this is frame generation's D3D12 swapchain for a Vulkan game, whose
    // own present also calls update() with isVulkan: the same answer from both, or reflexLimitsFps flips every frame
    // and setFPSLimit resends the cap each time
    const bool vulkanGame = State::Instance().swapchainInteropApi == SwapchainInteropApi::VkwDx12;
    auto fg = State::Instance().currentFG;
    static const bool starRailExecutable =
        _wcsicmp(Util::ExePath().filename().c_str(), L"StarRail.exe") == 0;
    // Creation-time provider/queue binding identifies this physical output
    // independently of optional filter admission (which excludes FLIP_SEQUENTIAL).
    // The source FGPresent updates the cap under continuity before entering
    // the SDK; worker cap bookkeeping must not take that source-owned lock.
    // This CPU identity grants no filter, GPU-resource or consumer authority.
    const bool starRailOwnedOutput = willPresent && starRailExecutable && starRailProviderOutput && cq != nullptr &&
        !State::Instance().externalFrameGeneration &&
        State::Instance().swapchainInteropApi == SwapchainInteropApi::Dx11wDx12 &&
        State::Instance().activeFgInput == FGInput::Upscaler && State::Instance().activeFgOutput == FGOutput::DLSSG;
    if (!starRailOwnedOutput)
    {
        if (willPresent && fg != nullptr)
            ReflexHooks::update(fg->IsActive(), vulkanGame);
        else
            ReflexHooks::update(false, vulkanGame);
    }

    XellHooks::update();

    // Upscaler GPU time computation
    if (willPresent && (fg == nullptr || !fg->IsActive() || fg->IsPaused()))
    {
        if (auto currentFeature = State::Instance().currentFeature; currentFeature != nullptr)
        {
            std::optional<double> upscalerTimeOpt {};
            const bool observeTimerReads = GenshinPresentCpuProcessFact().Genshin &&
                !Config::Instance()->FGEnabled.value_or_default() &&
                !Config::Instance()->DlssNrEnabled.value_or_default() &&
                (fg == nullptr || !fg->IsActive() || fg->IsPaused()) &&
                !State::Instance().externalFrameGeneration;
            const unsigned timerReadContext = observeTimerReads
                ? (MenuOverlayBase::IsVisible() ? 1u : 0u) |
                  (Config::Instance()->ShowFps.value_or_default() ? 2u : 0u)
                : 0u;

            // ReadUpscalerTime takes a void*: a plain D3D12 feature casts it to ID3D12CommandQueue*, so it must never be
            // handed a D3D11 context. A D3D11 game can own one (native::VirtualUpscalerDriver, run on the paired D3D12
            // queue) with no D3D12 queue on this swap chain; its timers live on that paired queue.
            const bool pureDx12Feature = currentFeature->Api() == API::DX12 && !currentFeature->IsWithDx12();

            if (pureDx12Feature)
            {
                auto timingQueue = cq != nullptr ? cq : WithDx12::GetD3D12CommandQueue();

                if (timingQueue != nullptr)
                {
                    {
                        GenshinTimerReadScope timing(observeTimerReads, timerReadContext, GenshinTimerReadStage::Upscaler);
                        upscalerTimeOpt = currentFeature->ReadUpscalerTime(timingQueue);
                    }
                    if (upscalerTimeOpt.has_value())
                    {
                        GenshinTimerReadScope timing(observeTimerReads, timerReadContext, GenshinTimerReadStage::Detailed);
                        currentFeature->ReadDetailedGpuTimes(timingQueue, State::Instance().detailedGpuTimes);
                    }
                }
            }
            else if (device != nullptr)
            {
                ID3D11DeviceContext* context = nullptr;
                device->GetImmediateContext(&context);

                {
                    GenshinTimerReadScope timing(observeTimerReads, timerReadContext, GenshinTimerReadStage::Upscaler);
                    upscalerTimeOpt = currentFeature->ReadUpscalerTime(context);
                }
                if (upscalerTimeOpt.has_value())
                {
                    GenshinTimerReadScope timing(observeTimerReads, timerReadContext, GenshinTimerReadStage::Detailed);
                    currentFeature->ReadDetailedGpuTimes(context, State::Instance().detailedGpuTimes);
                }

                context->Release();
            }
            if (!pureDx12Feature && State::Instance().swapchainInteropApi == SwapchainInteropApi::Dx11wDx12 &&
                State::Instance().currentD3D11Device != nullptr)
            {
                ID3D11DeviceContext* context = nullptr;
                State::Instance().currentD3D11Device->GetImmediateContext(&context);

                {
                    GenshinTimerReadScope timing(observeTimerReads, timerReadContext, GenshinTimerReadStage::Upscaler);
                    upscalerTimeOpt = currentFeature->ReadUpscalerTime(context);
                }
                if (upscalerTimeOpt.has_value())
                {
                    GenshinTimerReadScope timing(observeTimerReads, timerReadContext, GenshinTimerReadStage::Detailed);
                    currentFeature->ReadDetailedGpuTimes(context, State::Instance().detailedGpuTimes);
                }

                context->Release();
            }

            if (observeTimerReads) GenshinTimerReadWindowForThread().Report();
            if (upscalerTimeOpt.has_value())
            {
                static const bool genshinTiming=GenshinFsr::IsGenshinExecutable(Util::ExePath().wstring());
                if(genshinTiming){static ULONGLONG nextReport=0;const auto tick=GetTickCount64();
                    if(tick>=nextReport){nextReport=tick+500;std::string details;
                        for(const auto& item:State::Instance().detailedGpuTimes)details+=std::format(" {}={:.4f}ms",item.name,item.time);
                        LOG_INFO("R858 SR model GPU: pid={} total={:.4f}ms details={} sample=500ms no_cpu_wait=true",
                            GetCurrentProcessId(),upscalerTimeOpt.value(),details);}}
                auto upscalerTime = upscalerTimeOpt.value();
                // filter out possibly wrong measured high values
                if (upscalerTime < 100.0)
                {
                    State::Instance().frameTimeMutex.lock();
                    State::Instance().upscaleTimes.push_back(upscalerTime);
                    State::Instance().upscaleTimes.pop_front();
                    State::Instance().frameTimeMutex.unlock();
                }
            }
        }
    }

    // Fallback when FGPresent is not hooked for V-sync
    if (willPresent && Config::Instance()->ForceVsync.has_value())
    {
        LOG_DEBUG("ForceVsync: {}, VsyncInterval: {}, SCAllowTearing: {}, realExclusiveFullscreen: {}",
                  Config::Instance()->ForceVsync.value(), Config::Instance()->VsyncInterval.value_or_default(),
                  State::Instance().SCAllowTearing, State::Instance().realExclusiveFullscreen);

        if (!Config::Instance()->ForceVsync.value())
        {
            SyncInterval = 0;

            if (State::Instance().SCAllowTearing && !State::Instance().realExclusiveFullscreen)
            {
                LOG_DEBUG("Adding DXGI_PRESENT_ALLOW_TEARING");
                Flags |= DXGI_PRESENT_ALLOW_TEARING;
            }
        }
        else
        {
            // Remove allow tearing
            SyncInterval = Config::Instance()->VsyncInterval.value_or_default();

            if (SyncInterval < 1)
                SyncInterval = 1;

            LOG_DEBUG("Removing DXGI_PRESENT_ALLOW_TEARING");
            Flags &= ~DXGI_PRESENT_ALLOW_TEARING;
        }

        LOG_DEBUG("Final SyncInterval: {}", SyncInterval);
    }

    // DXVK check, it's here because of upscaler time calculations
    if (IdentifyGpu::getPrimaryGpu().usesDxvk)
    {
        if (pPresentParameters == nullptr)
            presentResult = r448Hook.Original([&]{return rawPresent(nullptr);});
        else
            presentResult = r448Hook.Original([&]{return rawPresent(pPresentParameters);});

        if(actualCall)*actualCall=true;
        nrTrace.Presented(presentResult);
        if (presentResult == S_OK)
        {
            // DXVK returns below before the native path advances the NR epoch.
            // Count a completed Present; retain the deferred NR evaluation guard.
            if (willPresent && !deferNativeEpoch)
            {
                _frameCounter++;
                State::Instance().frameCount = _frameCounter;
            }

            LOG_TRACE("3 {}", (UINT) presentResult);
        }
        else if (presentResult == DXGI_ERROR_DEVICE_REMOVED)
        {
            if (isD3D11)
            {
                if (State::Instance().currentD3D11Device != nullptr)
                    Util::GetDeviceRemovedReason(State::Instance().currentD3D11Device);
            }
            else
            {
                if (State::Instance().currentD3D12Device != nullptr)
                    Util::GetDeviceRemovedReason(State::Instance().currentD3D12Device);
            }
        }
        else
        {
            LOG_ERROR("3 {:X}", (UINT) presentResult);
        }

        return presentResult;
    }

    if (willPresent)
    {
        // Tick feature to let it know if it's frozen
        if(nativeParentOwner || cq)
            GenshinTransport::FinishPendingOutput(device,GenshinTransport::OutputFlushReason::Consumer);
        if (auto currentFeature = State::Instance().currentFeature; currentFeature != nullptr)
        {
            if (auto currentFg = State::Instance().currentFG; currentFg != nullptr)
                currentFeature->TickFrozenCheck(currentFg->GetInterpolatedFrameCount());
            else
                currentFeature->TickFrozenCheck();
        }

        // Before the finished-picture pass, which composes from what the virtual upscaler's call captured this frame.
        if (cq && device12 && NativeMotionDx12::OnPresent(pSwapChain, cq, device12) != HoYoShade::ConsumerAccess::Safe)
            return E_FAIL; // no FinishedPicture NR, FG, capture, overlay or real Present

        if (cq && (fg == nullptr || !fg->IsActive() || fg->IsPaused()))
            DlssNr::ApplyToFinishedPicture(pSwapChain, cq);
        if (HoYoShade::ColorConsumers(pSwapChain) != HoYoShade::ConsumerAccess::Safe) return E_FAIL;

        // Final ORIGINAL output. A vendor physical/generated Present has no
        // proven post-generation producer receipt here and remains unsupported.
        if (cq && device12 && State::Instance().swapchainInteropApi == SwapchainInteropApi::None &&
            State::Instance().dlssgDetectedInterpolationCount == 0 &&
            (fg == nullptr || !fg->IsActive() || fg->IsPaused()))
        {
            const auto filtered = HoYoShade::RenderPresentOriginal(pSwapChain, cq,
                State::Instance().outputColorSpace.valid && State::Instance().outputColorSpace.dxgiColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709,
                _frameCounter + 1);
            if (filtered.Uncertain()) {
                // The helper already pinned/quarantined the exact acquired color;
                // do not re-query a possibly advanced current backbuffer here.
                LOG_ERROR("HoYoShade final-original has uncertain GPU work; retaining resources and refusing this Present");
                return E_FAIL; // adapter failure, never fabricate a device-removal diagnosis
            }
        }

        // Exact current DX12 backbuffer after Present NR, before Opt overlay. This
        // remains available when NR is disabled; no desktop or stale SR substitution.
        if (cq) DlssNr::CaptureCurrentPresentOutput(pSwapChain, cq);

    }

    // Preserve normal shutdown/DXVK, timing, V-sync, NR and capture above.
    // The transaction encloses the existing overlay/raw boundary and all of
    // its original arguments and post-Present bookkeeping. OFF takes this
    // same continuation directly, without entering the physical endpoint.
    const auto overlayAndPresent = [&]() -> HRESULT {
        gimi::runtime::NativePresentScope gimiPresent;
        if (isD3D11)
            BeginGimiNativePresent(gimiPresent, pSwapChain, SyncInterval, Flags,
                                   pPresentParameters != nullptr, starRailProviderOutput);
        if (willPresent)
        {
            // The captured DLSSG output queue presents every displayed frame, including generated frames.
            // Exactly this route owns the menu; other hooks keep S's input/unwrapped fallback.
            const bool menuOutput = cq != nullptr && FGHooks::IsWrappedMenuOutput(pSwapChain, cq, hWnd);
            // Native game Streamline DLSSG can present its captured physical chain without
            // an OptiScaler IFGFeature/currentFGSwapchain. T's internal-FG ownership gate
            // therefore never selected its output mode. Actual detected interpolation,
            // the exact current raw chain and its creation queue qualify this local route.
            const auto& menuState = State::Instance();
            const bool nativeMenuOutput = cq != nullptr && fg == nullptr &&
                !menuState.externalFrameGeneration && menuState.swapchainInteropApi == SwapchainInteropApi::None &&
                menuState.currentRealSwapchain == pSwapChain && menuState.dlssgDetectedInterpolationCount > 0;
            overlayOutputRoute = menuOutput;
            overlayNativeRoute = nativeMenuOutput;
            overlayOwner = FGHooks::OwnsMenuOverlay(hWnd);
            overlayResult.Decision = "owner-excluded";
            if (nativeParentOwner || menuOutput || !overlayOwner)
            {
                if (menuOutput)
                {
                    static std::atomic<UINT64> menuOutputPresents { 0 };
                    const auto count = ++menuOutputPresents;
                    if (count == 1 || count % 600 == 0)
                        LOG_INFO("T FG menu physical output: presents={} swapchain={:X} queue={:X} clientQueue={:X} window={:X}",
                                 count, (size_t)pSwapChain, (size_t)cq,
                                 (size_t)State::Instance().currentFG->GetCommandQueue(), (size_t)hWnd);
                }
                if (nativeMenuOutput)
                {
                    static std::atomic<UINT64> nativeOutputPresents { 0 };
                    const auto count = ++nativeOutputPresents;
                    if (count == 1 || count % 600 == 0)
                        LOG_INFO("FG menu native physical output: calls={} swapchain={:X} queue={:X} interpolations={}",
                                 count, (size_t)pSwapChain, (size_t)cq, menuState.dlssgDetectedInterpolationCount);
                }
                MenuOverlayDx::Present(pSwapChain, SyncInterval, Flags, pPresentParameters, pDevice, hWnd, isUWP,
                                       menuOutput || nativeMenuOutput, &overlayResult);
            }

    #ifdef LOW_LATENCY_INPUTS
            if (State::Instance().activeFgOutput == FGOutput::FSRFG)
            {
                auto fgIsActive = fg != nullptr && fg->IsActive() && !fg->IsPaused();
                InputAntiLag2::injectAl2Context(pSwapChain, fgIsActive);
            }
    #else
            if (State::Instance().activeFgOutput == FGOutput::FSRFG || State::Instance().activeFgOutput == FGOutput::XeFG)
            {
                LOG_DEBUG("Calling fakenvapi");

                static UINT64 fgPresentFrame = 0;
                auto fgIsActive = fg != nullptr && fg->IsActive() && !fg->IsPaused();

                if (State::Instance().fgPresentIsCalled)
                {
                    State::Instance().fgPresentIsCalled = false;
                    fgPresentFrame = _frameCounter;
                }

                auto isInterpolated = fgIsActive && (_frameCounter - fgPresentFrame) > 0;

                fakenvapi::reportFGPresent(pSwapChain, fgIsActive, isInterpolated);
            }
    #endif

            if (!deferNativeEpoch) {
                _frameCounter++;
                State::Instance().frameCount = _frameCounter;
            }
        }

        if (logRoutinePresent)
            LOG_DEBUG("Calling original present");

        // Optical F5Low's low latency (native/NativeLowLatency.h): Reflex markers around the game's present
        IUnknown* lowLatencyDevice = nullptr;

        // (A D3D11 game behind OptiScaler's D3D12 bridge is covered in Dx11wDx12SC::Present, the game's own present.)
        if (willPresent && State::Instance().swapchainInteropApi == SwapchainInteropApi::None)
        {
            if (isD3D11)
                lowLatencyDevice = device;
            else
                lowLatencyDevice = device12;

            if (lowLatencyDevice != nullptr)
                native::lowlatency::OnPresentBegin(lowLatencyDevice);
        }

        if (isD3D11 && willPresent && !genshinPresentIdentity)
            genshinPresentIdentity=GenshinLegacy::ResolveNativeSwapchainIdentity(pSwapChain);
        if (isD3D11 && willPresent) GenshinLegacy::ObserveNativePresentTarget(pSwapChain,genshinPresentIdentity);
        const auto genshinPresentTicket=isD3D11 && willPresent
            ? GenshinLegacy::BeginNativePresent(device,genshinPresentIdentity,Flags,pPresentParameters!=nullptr)
            : GenshinLegacy::Presentation::Ticket{};
        // A caller-declared full update already guarantees a coherent target. Once
        // the menu wrote it, scrolling old presented pixels over those writes would
        // violate that output. Partial dirty updates need coherent background
        // reconstruction; widening their rectangles alone cannot establish it.
        DXGI_PRESENT_PARAMETERS overlayPresentParameters {};
        const DXGI_PRESENT_PARAMETERS* effectivePresentParameters = pPresentParameters;
        bool overlayScrollSuppressed = false;
        if (overlayResult.OverlayWritten && pPresentParameters != nullptr && pPresentParameters->DirtyRectsCount == 0 &&
            (pPresentParameters->pScrollRect != nullptr || pPresentParameters->pScrollOffset != nullptr))
        {
            overlayPresentParameters = *pPresentParameters;
            overlayPresentParameters.pScrollRect = nullptr;
            overlayPresentParameters.pScrollOffset = nullptr;
            effectivePresentParameters = &overlayPresentParameters;
            overlayScrollSuppressed = true;
        }
        // Measure only the original API, after overlay, ticket and latency setup.
        // Keep the exact argument selection and all existing callbacks/returns.
        GenshinPresentCpuScope presentCpu(pSwapChain, SyncInterval, Flags, pPresentParameters != nullptr,
                                          genshinPresentIdentity, isD3D11, nativeParentOwner, genshinPresentTicket);
        // swapchain present
        if (pPresentParameters == nullptr)
            presentResult = r448Hook.Original([&]{return rawPresent(nullptr);});
        else
            presentResult = r448Hook.Original([&]{return rawPresent(effectivePresentParameters);});
        presentCpu.Finish(presentResult);
        gimiPresent.Returned(pSwapChain, SyncInterval, Flags, effectivePresentParameters != nullptr, presentResult);

        if(actualCall)*actualCall=true;
        nrTrace.Presented(presentResult);

        // Bounded samples plus throttled state transitions expose missing menu
        // submissions without a per-frame log flood or additional GPU waits.
        if (willPresent)
        {
            static std::atomic<UINT64> calls { 0 }, written { 0 }, skipped { 0 };
            static thread_local const char* lastDecision = nullptr;
            static thread_local bool lastWritten = false;
            static thread_local ULONGLONG lastTransitionAt = 0;
            const UINT64 count = ++calls;
            if (overlayResult.OverlayWritten) ++written; else ++skipped;
            const ULONGLONG now = GetTickCount64();
            const bool changed = lastDecision != overlayResult.Decision || lastWritten != overlayResult.OverlayWritten;
            if (count <= 12 || count % 180 == 0 || (changed && now - lastTransitionAt >= 250))
            {
                lastDecision = overlayResult.Decision;
                lastWritten = overlayResult.OverlayWritten;
                lastTransitionAt = now;
                LOG_INFO("Overlay present evidence: pid={} tick={} calls={} written={} skipped={} sc={:X} incomingQueue={:X} queue={:X} window={:X} output={} native={} owner={} immutable={} decision={} overlayWritten={} index={} slot={} busySlots={} route={} refresh={} generation={}/{} epoch={} completion={} fence={}/{} bufferFence={} drain={}:{} wait={} present1={} dirty={} scroll={}/{} scrollSuppressed={} sync={} flags={:X} hr={:X}",
                         GetCurrentProcessId(), now, count, written.load(), skipped.load(), (size_t)pSwapChain,
                         (size_t)pDevice, (size_t)overlayResult.Queue, (size_t)hWnd, overlayOutputRoute, overlayNativeRoute, overlayOwner,
                         overlayResult.ImmutableOutput, overlayResult.Decision, overlayResult.OverlayWritten,
                         overlayResult.BackbufferIndex, overlayResult.ReplaySlot, overlayResult.BusySlots,
                         overlayResult.RouteReason, overlayResult.RefreshReason, overlayResult.Generation, overlayResult.SnapshotGeneration,
                         overlayResult.Epoch, overlayResult.CompletionState, overlayResult.CompletedFence,
                         overlayResult.SubmittedFence, overlayResult.BackbufferFence, overlayResult.DrainPending,
                         overlayResult.DrainFence, overlayResult.WaitAttempted, pPresentParameters != nullptr,
                         pPresentParameters != nullptr ? pPresentParameters->DirtyRectsCount : 0,
                         pPresentParameters != nullptr && pPresentParameters->pScrollRect != nullptr,
                         pPresentParameters != nullptr && pPresentParameters->pScrollOffset != nullptr,
                         overlayScrollSuppressed, SyncInterval, Flags, (unsigned long)presentResult);
            }
        }

        GenshinLegacy::CompleteNativePresent(genshinPresentTicket,presentResult);

        if (lowLatencyDevice != nullptr)
            native::lowlatency::OnPresentEnd(lowLatencyDevice);

        if (presentResult == S_OK)
        {
            if (logRoutinePresent)
                LOG_DEBUG("Original present result: {:X}", (UINT) presentResult);
        }
        else
        {
            LOG_ERROR("Original present result: {:X}", (UINT) presentResult);

            if (presentResult == DXGI_ERROR_DEVICE_REMOVED && nativeParentOwner)
            {
                // The Genshin parent is D3D11; the global D3D12 pointer can describe
                // a healthy optional bridge device. Diagnose the actual presenter.
                try {
                    static std::atomic<unsigned> reports{0};
                    if(reports.fetch_add(1,std::memory_order_relaxed)<8) {
                        Microsoft::WRL::ComPtr<ID3D11Device> nativeDevice;
                        const HRESULT deviceResult=pSwapChain->GetDevice(IID_PPV_ARGS(&nativeDevice));
                        const HRESULT reason=SUCCEEDED(deviceResult) && nativeDevice ?
                            nativeDevice->GetDeviceRemovedReason() : E_NOINTERFACE;
                        LOG_ERROR("R827 phase40 native removal: pid={} thread={} swapchain={:X} incoming_device={:X} native_d11={:X} get_device_hr={:X} removed_reason_known={} removed_reason={:X} native_present_hr={:X} diagnostic_only=true",
                            GetCurrentProcessId(),GetCurrentThreadId(),reinterpret_cast<uintptr_t>(pSwapChain),
                            reinterpret_cast<uintptr_t>(pDevice),reinterpret_cast<uintptr_t>(nativeDevice.Get()),
                            static_cast<unsigned>(deviceResult),SUCCEEDED(deviceResult) && nativeDevice.Get()!=nullptr,static_cast<unsigned>(reason),static_cast<unsigned>(presentResult));
                    }
                }catch(...){} // Diagnostics never replace the actual native HRESULT.
            }
            else if (presentResult == DXGI_ERROR_DEVICE_REMOVED && State::Instance().currentD3D12Device != nullptr)
                Util::GetDeviceRemovedReason(State::Instance().currentD3D12Device);
        }

        return presentResult;
    };
    return finalOutput && HoYoShade::Selected(HoYoShade::Stage::FinalOutput)
        ? finalOutput->Present(Flags, pPresentParameters, overlayAndPresent)
        : overlayAndPresent();
}

// R612 uses the real R610 LocalPresent consumer for the game-facing parent.
// Child/provider output may be nested inside this call's outer frame lifetime;
// it cannot tick the native picture epoch a second time.
GenshinNativeParentFrame::GenshinNativeParentFrame(IDXGISwapChain* native, IDXGISwapChain* child,
    IUnknown* device, HWND parent, HWND childWindow, std::uint64_t& identity)
    : native_(native), child_(child), device_(device), parentHwnd_(parent), childHwnd_(childWindow),
      identity_(identity), thread_(GetCurrentThreadId())
{
    if (current_ || !native_ || !device_ || !parentHwnd_ ||
        !GenshinFsr::IsGenshinExecutable(Util::ExePath().wstring()))return;
    // The wrapper owns this actual native chain even after a failed child
    // factory, route change, window destruction or unsupported child topology.
    active_=true;current_=this;
}
bool GenshinNativeParentFrame::OwnsNative(IDXGISwapChain* sc,IUnknown* device) noexcept
{
    const auto* p=current_;
    if(!p || !p->active_ || p->thread_!=GetCurrentThreadId() || sc!=p->native_ ||
       State::Instance().currentRealSwapchain!=sc ||
       State::Instance().swapchainInteropApi!=SwapchainInteropApi::Dx11wDx12)return false;
    DXGI_SWAP_CHAIN_DESC d{};ComPtr<ID3D11Device> actual;
    if(FAILED(sc->GetDesc(&d)) || d.OutputWindow!=p->parentHwnd_ ||
       FAILED(sc->GetDevice(IID_PPV_ARGS(&actual))) ||
       GenshinPresentNr::Identity(actual.Get())!=GenshinPresentNr::Identity(p->device_))return false;
    return !device || GenshinPresentNr::Identity(device)==GenshinPresentNr::Identity(p->device_);
}
bool GenshinNativeParentFrame::MatchesTicket(const GenshinPresentNr::Ticket& ticket) noexcept
{
    const auto* p=current_;
    const auto& state=State::Instance();const auto& controls=*Config::Instance();
    if(state.isShuttingDown || state.api==Vulkan || state.swapchainApi==Vulkan ||
       !controls.DlssNrEnabled.value_or_default() || !controls.DlssNrFinishedPicture.value_or_default() ||
       controls.DlssNrRunBeforeSr.value_or_default() || controls.DlssNrDeferredDlss.value_or_default() ||
       native::OpticalFgNrEnabled() || !controls.GenshinControlsCurrent(ticket.source.configEpoch))return false;
    if(!p || !ticket.Known() || !OwnsNative(p->native_,p->device_) ||
       ticket.swapchain!=GenshinPresentNr::Identity(p->native_) ||
       ticket.source.device!=GenshinPresentNr::Identity(p->device_) ||
       ticket.source.thread!=GetCurrentThreadId() || ticket.presentEpoch!=State::Instance().frameCount)return false;
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain3> indexed;ComPtr<ID3D11Texture2D> buffer;
    if(FAILED(p->native_->GetDevice(IID_PPV_ARGS(&device))))return false;
    device->GetImmediateContext(&context);
    UINT index=0;if(SUCCEEDED(p->native_->QueryInterface(IID_PPV_ARGS(&indexed))))index=indexed->GetCurrentBackBufferIndex();
    return context && context->GetType()==D3D11_DEVICE_CONTEXT_IMMEDIATE &&
        GenshinPresentNr::Identity(context.Get())==ticket.source.context &&
        GenshinPresentNr::Identity(ticket.context.Get())==ticket.source.context &&
        SUCCEEDED(p->native_->GetBuffer(index,IID_PPV_ARGS(&buffer))) &&
        GenshinPresentNr::Identity(buffer.Get())==GenshinPresentNr::Identity(ticket.backbuffer.Get());
}
bool GenshinPresentNr::MatchesNativeParentTicket(const Ticket& ticket) noexcept
{return GenshinNativeParentFrame::MatchesTicket(ticket);}
bool GenshinNativeParentFrame::OwnsPresent(IDXGISwapChain* sc, HWND hwnd) noexcept
{
    const auto* p = current_;
    if (!p || !p->active_ || p->thread_!=GetCurrentThreadId()) return false;
    if (sc == p->native_ && hwnd == p->parentHwnd_) return true;
    // A Streamline physical output can differ from its proxy COM identity.
    // It must still be the actual swapchain on this exact owned child window.
    DXGI_SWAP_CHAIN_DESC desc{};
    return sc && hwnd == p->childHwnd_ && SUCCEEDED(sc->GetDesc(&desc)) && desc.OutputWindow == p->childHwnd_;
}
bool GenshinNativeParentFrame::IsChildOutput(IDXGISwapChain* sc, HWND hwnd) noexcept
{
    // Streamline can issue physical/generated output on another thread, outside
    // the parent's TLS scope. Such output is not another native game picture.
    // Recognize only our retained, still-owned child and its actual descriptor.
    return sc && GenshinFsr::IsGenshinExecutable(Util::ExePath().wstring()) &&
        GenshinVisibleChild::IsPhysicalOutput(sc,hwnd);
}
bool GenshinNativeParentFrame::IsRetainedChildWindow(HWND hwnd) noexcept
{
    return GenshinFsr::IsGenshinExecutable(Util::ExePath().wstring()) &&
        GenshinVisibleChild::IsRetainedChild(hwnd) && GenshinVisibleChild::IsOwnedChild(hwnd);
}
void GenshinNativeParentFrame::PublishColorSpace(IDXGISwapChain* sc,DXGI_COLOR_SPACE_TYPE space)
{
    if(sc && State::Instance().currentRealSwapchain==sc &&
       GenshinFsr::IsGenshinExecutable(Util::ExePath().wstring())) {
        UpdateOutputColorSpace(space);
        DlssNr::FinishedPictureColorSpace(sc,space);
    }
}
HRESULT GenshinNativeParentFrame::Present(UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* parameters,
                                         bool capturePolled)
{
    GenshinHookCpuTiming::HookScope r448Hook((parameters!=nullptr)?GenshinHookCpuTiming::Hook::Present1:GenshinHookCpuTiming::Hook::Present);
    GenshinHookCpuTiming::PresentOwnerScope r448Owner(r448Hook);

    if (!native_ || called_) return DXGI_ERROR_INVALID_CALL;
    // LocalPresent always reaches the actual native API. Eligibility for NR is
    // checked separately by OwnsNative/MatchesTicket, never by this forwarder.
    bool actualCall=false;
    const auto result=LocalPresent(native_,sync,flags,parameters,device_,parentHwnd_,false,identity_,capturePolled,&actualCall);
    called_=actualCall && (flags & DXGI_PRESENT_TEST)==0;
    return result;
}
GenshinNativeParentFrame::~GenshinNativeParentFrame()
{
    if (current_ != this) return;
    // Preserve LocalPresent's attempted-native-Present clock even when its
    // HRESULT is failure; provider/child calls never authorize another tick.
    if (called_ && !State::Instance().isShuttingDown) {
        ++_frameCounter; State::Instance().frameCount = _frameCounter;
    }
    current_ = nullptr;
}

WrappedIDXGISwapChain4::WrappedIDXGISwapChain4(IDXGISwapChain* real, IUnknown* pDevice, HWND hWnd, UINT flags,
                                               bool isUWP, bool isComposition)
    : _real(real), _device(pDevice), _handle(hWnd), _refcount(1), _uwp(isUWP), _composition(isComposition)
{
    _id = ++scCount;
    _lastFlags = flags;

    _real->QueryInterface(IID_PPV_ARGS(&_real1));
    if (_real1 != nullptr)
        _real1->Release();

    _real->QueryInterface(IID_PPV_ARGS(&_real2));
    if (_real2 != nullptr)
        _real2->Release();

    _real->QueryInterface(IID_PPV_ARGS(&_real3));
    if (_real3 != nullptr)
        _real3->Release();

    _real->QueryInterface(IID_PPV_ARGS(&_real4));
    if (_real4 != nullptr)
        _real4->Release();

    _real->AddRef();
    auto refCount = _real->Release();

    CheckForHdrOutput();
    FGHooks::RegisterMenuOutput(_real, _device, _handle);
    _starRailProviderOutput = IsStarRailProcess() &&
        framegen::final_output::CaptureProviderOutputBinding(_real, _device);
    if (_starRailProviderOutput) _finalRawCreationQueueInput = reinterpret_cast<std::uintptr_t>(_device);
    _finalOutput = framegen::final_output::Endpoint::Capture(_real, _device);
    HoYoShade::RegisterDirectChain(_real, this); // R061 exact resize coverage, CPU metadata only

    LOG_INFO("{} created, real: {:X}, refCount: {}", _id, (UINT64) real, refCount);
}

WrappedIDXGISwapChain4::~WrappedIDXGISwapChain4() { GenshinLegacy::ForgetNativeSwapchain(_genshinPresentIdentity); }

//
HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::QueryInterface(REFIID riid, void** ppvObject)
{
    LOG_TRACE("Caller: {}", Util::WhoIsTheCaller(_ReturnAddress()));

    if (riid == __uuidof(IDXGISwapChain))
    {
        AddRef();
        *ppvObject = (IDXGISwapChain*) this;
        return S_OK;
    }
    else if (riid == __uuidof(IDXGISwapChain1))
    {
        if (_real1)
        {
            AddRef();
            *ppvObject = (IDXGISwapChain1*) this;
            return S_OK;
        }
        else
        {
            return E_NOINTERFACE;
        }
    }
    else if (riid == __uuidof(IDXGISwapChain2))
    {
        if (_real2)
        {
            AddRef();
            *ppvObject = (IDXGISwapChain2*) this;
            return S_OK;
        }
        else
        {
            return E_NOINTERFACE;
        }
    }
    else if (riid == __uuidof(IDXGISwapChain3))
    {
        if (_real3)
        {
            AddRef();
            *ppvObject = (IDXGISwapChain3*) this;
            return S_OK;
        }
        else
        {
            return E_NOINTERFACE;
        }
    }
    else if (riid == __uuidof(IDXGISwapChain4))
    {
        if (_real4)
        {
            AddRef();
            *ppvObject = (IDXGISwapChain4*) this;
            return S_OK;
        }
        else
        {
            return E_NOINTERFACE;
        }
    }
    else if (riid == __uuidof(WrappedIDXGISwapChain4))
    {
        AddRef();
        *ppvObject = this;
        return S_OK;
    }
    else if (riid == __uuidof(IUnknown))
    {
        AddRef();
        *ppvObject = (IUnknown*) this;
        return S_OK;
    }
    else if (riid == __uuidof(IDXGIObject))
    {
        AddRef();
        *ppvObject = (IDXGIObject*) this;
        return S_OK;
    }
    else if (riid == __uuidof(IDXGIDeviceSubObject))
    {
        AddRef();
        *ppvObject = (IDXGIDeviceSubObject*) this;
        return S_OK;
    }

    *ppvObject = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE WrappedIDXGISwapChain4::AddRef()
{
    InterlockedIncrement(&_refcount);
    LOG_TRACE("Count: {}, caller: {}", _refcount, Util::WhoIsTheCaller(_ReturnAddress()));
    return _refcount;
}

ULONG STDMETHODCALLTYPE WrappedIDXGISwapChain4::Release()
{
    ULONG ret = InterlockedDecrement(&_refcount);

    LOG_TRACE("Count: {}, caller: {}", _refcount, Util::WhoIsTheCaller(_ReturnAddress()));

    if (ret == 0)
    {
        // The teardown below asks REFramework to let go of frame generation's swapchain, and it can take and drop a
        // reference to this one while it does: that second final release would tear down and delete this twice
        const bool nested = _finalReleaseInProgress.exchange(true);

        if (nested && REFrameworkCompat::Active())
        {
            LOG_DEBUG("Nested final release while this swapchain is being released, left to the first one");
            return 0;
        }

#ifdef USE_LOCAL_MUTEX
        OwnedLockGuard lock(_localMutex, 999);
#endif

        // A dxvk game whose frames the Vulkan present bridge carries (VkPresentBridge::DxvkThroughVulkan): the menu and frame
        // generation are the bridge's, on this same window, and it lets go of them itself when dxvk's Vulkan swapchain goes
        // (in _real->Release() below). Tearing them down here would pull XeFG out from under its queued presents.
        HoYoShade::ForgetDirectChain(_real, this);
        _finalOutput.reset();
        FGHooks::UnregisterMenuOutput(_real);
        const bool bridgeOwns = VkPresentBridge::DxvkThroughVulkan();

        if (!bridgeOwns)
            MenuOverlayDx::CleanupRenderTarget(true, _handle);

        if (State::Instance().currentSwapchain == this)
            State::Instance().currentSwapchain = nullptr;

        if (State::Instance().currentRealSwapchain == this)
            State::Instance().currentRealSwapchain = nullptr;

        auto fg = State::Instance().currentFG;
        if (!bridgeOwns && fg != nullptr && fg->Mutex.getOwner() != 1 && fg->SwapchainContext() != nullptr)
        {
            fg->Deactivate();
            fg->ReleaseSwapchain(_handle);

            if (State::Instance().currentFGSwapchain != nullptr)
                State::Instance().currentFGSwapchain = nullptr;
        }

        auto refCount = _real->Release();

        // Disabled for now, cause issues with some games
        /*
        IDXGISwapChain* skSC = nullptr;
        if (_real->QueryInterface(IID_IUnwrappedDXGISwapChain, (void**) &skSC) == S_OK && skSC != nullptr)
        {
            skSC->Release();
            LOG_DEBUG("Found SK swapchain, skip releasing of main swapchain");
        }
        else
        {
            // Release real swapchain, otherwise it can cause issues when re-creating swapchain with same handle
            while (refCount > 0)
            {
                LOG_DEBUG("Waiting for real swapchain to be released, refCount: {}", refCount);
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                refCount = _real->Release();
            }
        }
        */

        LOG_DEBUG("Real swapchain released, refCount: {}", refCount);

        delete this;
    }

    return ret;
}

//
HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::SetPrivateData(REFGUID Name, UINT DataSize, const void* pData)
{
    return _real->SetPrivateData(Name, DataSize, pData);
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::SetPrivateDataInterface(REFGUID Name, const IUnknown* pUnknown)
{
    return _real->SetPrivateDataInterface(Name, pUnknown);
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetPrivateData(REFGUID Name, UINT* pDataSize, void* pData)
{
    return _real->GetPrivateData(Name, pDataSize, pData);
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetParent(REFIID riid, void** ppParent)
{
    return _real->GetParent(riid, ppParent);
}

//
HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetDevice(REFIID riid, void** ppDevice)
{
    return _real->GetDevice(riid, ppDevice);
}

//
HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::Present(UINT SyncInterval, UINT Flags)
{
    GenshinHookCpuTiming::HookScope r448Hook((false)?GenshinHookCpuTiming::Hook::Present1:GenshinHookCpuTiming::Hook::Present);
    GenshinHookCpuTiming::PresentOwnerScope r448Owner(r448Hook);

    GenshinTransport::PresentOutputScope outputSubmission(!(Flags & DXGI_PRESENT_TEST));
    if (_real == nullptr)
        return DXGI_ERROR_DEVICE_REMOVED;
    if (HoYoShade::ColorConsumers(_real) != HoYoShade::ConsumerAccess::Safe) return E_FAIL;

#ifdef USE_LOCAL_MUTEX
    OwnedLockGuard lock(_localMutex, 4);
#endif

    if (_composition && !IsCompositionWindow(_handle))
        _handle = FindCompositionWindow();
    if (_composition && _handle == nullptr)
        return CallGimiNativePresent(_real, SyncInterval, Flags, false, _starRailProviderOutput, [&] {
            return r448Hook.Original([&]{return ObserveStarRailFinalRaw(_starRailProviderOutput ? this : nullptr,_real,_finalRawCreationQueueInput,SyncInterval,Flags,false,[&]{return _real->Present(SyncInterval, Flags);});});
        });

    HRESULT result;

    if ((Flags & DXGI_PRESENT_TEST) == 0)
    {
        result = LocalPresent(_real, SyncInterval, Flags, nullptr, _device, _handle, _uwp, _genshinPresentIdentity, false, nullptr, _finalOutput.get(), _starRailProviderOutput,
                              _starRailProviderOutput ? this : nullptr, _finalRawCreationQueueInput);

        // When Reflex can't be used to limit (and Optical F5Low's low latency does not keep the cap), sleep in present
        if (!State::Instance().reflexLimitsFps && !native::lowlatency::OwnsFrameCap() &&
            State::Instance().activeFgOutput == FGOutput::NoFG &&
            !IdentifyGpu::getPrimaryGpu().usesDxvk && !XellHooks::canLimit())
            FrameLimit::sleep(false);
    }
    else
    {
        result = CallGimiNativePresent(_real, SyncInterval, Flags, false, _starRailProviderOutput, [&] {
            return r448Hook.Original([&]{return ObserveStarRailFinalRaw(_starRailProviderOutput ? this : nullptr,_real,_finalRawCreationQueueInput,SyncInterval,Flags,false,[&]{return _real->Present(SyncInterval, Flags);});});
        });
    }

    return result;
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetBuffer(UINT Buffer, REFIID riid, void** ppSurface)
{
    auto result = _real->GetBuffer(Buffer, riid, ppSurface);
    return result;
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::SetFullscreenState(BOOL Fullscreen, IDXGIOutput* pTarget)
{
    LOG_DEBUG("Fullscreen: {}, pTarget: {:X}, Caller: {}", Fullscreen, (size_t) pTarget,
              Util::WhoIsTheCaller(_ReturnAddress()));

    HRESULT result = S_OK;

    bool ffxLock = false;

    {
#ifdef USE_LOCAL_MUTEX
        // dlssg calls this from present it seems
        // don't try to get a mutex when present owns it while dlssg mod is enabled
        if (!(_localMutex.getOwner() == 4 && State::Instance().activeFgNvngx != FGNvngxReplacement::None))
        {
            OwnedLockGuard lock(_localMutex, 3);
        }
#endif
        if (Config::Instance()->FGUseMutexForSwapchain.value_or_default())
        {

            if (State::Instance().currentFG != nullptr && State::Instance().currentFG->IsActive() &&
                State::Instance().currentFG->Mutex.getOwner() != 3)
            {
                LOG_TRACE("Waiting ffxMutex 3, current: {}", State::Instance().currentFG->Mutex.getOwner());
                State::Instance().currentFG->Mutex.lock(3);
                ffxLock = true;
                LOG_TRACE("Accuired ffxMutex: {}", State::Instance().currentFG->Mutex.getOwner());
            }
            else
            {
                LOG_TRACE("Skipping ffxMutex, owner is already 3");
            }
        }

        State::Instance().realExclusiveFullscreen = Fullscreen;

        result = _real->SetFullscreenState(Fullscreen, pTarget);

        if (result != S_OK)
            LOG_ERROR("result: {:X}", (UINT) result);
        else
            LOG_DEBUG("result: {:X}", result);
    }

    if (ffxLock)
    {
        LOG_TRACE("Releasing ffxMutex: {}", State::Instance().currentFG->Mutex.getOwner());
        State::Instance().currentFG->Mutex.unlockThis(3);
    }

    return result;
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetFullscreenState(BOOL* pFullscreen, IDXGIOutput** ppTarget)
{
    return _real->GetFullscreenState(pFullscreen, ppTarget);
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetDesc(DXGI_SWAP_CHAIN_DESC* pDesc) { return _real->GetDesc(pDesc); }

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::ResizeBuffers(UINT BufferCount, UINT Width, UINT Height,
                                                                DXGI_FORMAT NewFormat, UINT SwapChainFlags)
{
    GenshinTransport::FinishPendingOutput(nullptr,GenshinTransport::OutputFlushReason::Resize);
    HoYoShade::DirectResizeScope directResize(_real, this);
    if (FAILED(directResize.Result())) return directResize.Result();
    framegen::final_output::Endpoint::Resize finalResize(_finalOutput.get());
    if (!finalResize) return DXGI_ERROR_WAS_STILL_DRAWING;
    if (!finalResize.ConsumersSafe(_real)) return E_FAIL;
    // Admission spans every early return, fallback and both real resize calls.
    DlssNrCurrentOutput::Capture::ResizeScope outputCaptureResize(
        DlssNrCurrentOutput::Capture::ResizeScope::Mode::ReserveOnly);
    if (!outputCaptureResize) return DXGI_ERROR_WAS_STILL_DRAWING;
    const auto captureResize=GenshinLegacy::BeforeResize(_real);
    if (FAILED(captureResize)) return captureResize;
    if (!DlssNr::WaitForFinishedPicture())
        return DXGI_ERROR_WAS_STILL_DRAWING;
    // Real Late Reset has now notified any borrowed capture generation. Keep
    // admission reserved until its own exact fence retires and real resize ends.
    if (!outputCaptureResize.ReadyForResize())
        return DXGI_ERROR_WAS_STILL_DRAWING;
    LOG_DEBUG("");

#ifdef USE_LOCAL_MUTEX
    // dlssg calls this from present it seems
    // don't try to get a mutex when present owns it while dlssg mod is enabled
    if (!(_localMutex.getOwner() == 4 && State::Instance().activeFgNvngx != FGNvngxReplacement::None))
    {
        OwnedLockGuard lock(_localMutex, 1);
    }
#endif

    // The menu can change the current FG object during resize; release the exact object we locked.
    IFGFeature_Dx12* lockedFg = nullptr;
    if (State::Instance().currentFG != nullptr && Config::Instance()->FGUseMutexForSwapchain.value_or_default())
    {
        lockedFg = State::Instance().currentFG;
        LOG_TRACE("Waiting ffxMutex 3, current: {}", lockedFg->Mutex.getOwner());
        lockedFg->Mutex.lock(3);
        LOG_TRACE("Accuired ffxMutex: {}", lockedFg->Mutex.getOwner());
    }

    HRESULT result;
    DXGI_SWAP_CHAIN_DESC desc {};
    _real->GetDesc(&desc);

    if (Config::Instance()->FGEnabled.value_or_default())
    {
        State::Instance().fgResetCapturedResources = true;
        State::Instance().fgOnlyUseCapturedResources = false;
        State::Instance().fgChanged = true;
    }

    // Not on a dxvk game whose menu the Vulkan present bridge draws (see Release).
    if (!VkPresentBridge::DxvkThroughVulkan())
        MenuOverlayDx::CleanupRenderTarget(true, _handle);

    State::Instance().scChanged = true;

    if (!_composition && Config::Instance()->OverrideVsync.value_or_default() && !State::Instance().SCExclusiveFullscreen &&
        State::Instance().currentFG == nullptr)
    {
        LOG_DEBUG("Overriding flags");
        SwapChainFlags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

        if (BufferCount < 2)
            BufferCount = 2;
    }

    State::Instance().SCAllowTearing = (SwapChainFlags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) > 0;

    LOG_DEBUG("BufferCount: {0}, Width: {1}, Height: {2}, NewFormat: {3}, SwapChainFlags: {4:X}", BufferCount, Width,
              Height, (UINT) NewFormat, SwapChainFlags);

    WaitForGPUIdle(_device);

    // Release swapchain backbuffers to prevent errors when resizing
    /*

    const bool outputRequiresRelease =
        State::Instance().activeFgOutput == FGOutput::FSRFG || State::Instance().activeFgOutput == FGOutput::XeFG;

    if (outputRequiresRelease && State::Instance().currentFG != nullptr)
    {
        IDXGISwapChain* skSC = nullptr;
        if (_real->QueryInterface(IID_IUnwrappedDXGISwapChain, (void**) &skSC) == S_OK && skSC != nullptr)
        {
            skSC->Release();
            LOG_DEBUG("Found SK swapchain, skip releasing backbuffers of main swapchain");
        }
        else
        {
            LOG_DEBUG("Releasing backbuffers, count: {}", desc.BufferCount);

            for (UINT i = 0; i < desc.BufferCount; i++)
            {
                ID3D12Resource* backBuffer = nullptr;
                auto bbResult = _real->GetBuffer(i, IID_PPV_ARGS(&backBuffer));

                if (bbResult == S_OK)
                {
                    backBuffer->AddRef();

                    auto refCount = backBuffer->Release();
                    refCount = backBuffer->Release();
                    LOG_DEBUG("Current backbuffer {}, RefCount {}", i, refCount);

                    while (refCount > 1 && refCount < 4294967200ul)
                    {
                        refCount = backBuffer->Release();
                        LOG_DEBUG("Releasing backbuffer {}, RefCount {}", i, refCount);
                    }

                    LOG_DEBUG("Backbuffer {}, RefCount {}", i, refCount);
                }
                else
                {
                    LOG_DEBUG("GetBuffer failed for index {}: {:X}", i, (UINT) bbResult);
                    break;
                }
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    */

#ifdef DXGI_DEBUG_ENABLED
    ReportDXGILiveObjects();

#ifdef ENABLE_DEBUG_LAYER_DX12
    ReportD3D12LiveObjects(State::Instance().currentD3D12Device);
#endif
#endif

    if (Config::Instance()->FGDontUseSwapchainBuffers.value_or_default())
    {
        ScopedSkipHeapCapture skipHeapCapture {};

        _lastFlags = SwapChainFlags;
        result = _real->ResizeBuffers(BufferCount, Width, Height, NewFormat, SwapChainFlags);
    }
    else
    {
        _lastFlags = SwapChainFlags;
        result = _real->ResizeBuffers(BufferCount, Width, Height, NewFormat, SwapChainFlags);
    }

    directResize.Complete(result);
    finalResize.Complete(result);
    if (result == DXGI_ERROR_DEVICE_REMOVED && State::Instance().currentD3D12Device != nullptr)
        Util::GetDeviceRemovedReason(State::Instance().currentD3D12Device);

#ifdef DXGI_DEBUG_ENABLED
    if (result != S_OK)
        ReadDxgiInfoQueue();
#endif

    if (result == S_OK && State::Instance().currentFeature == nullptr)
    {
        State::Instance().screenWidth = static_cast<float>(Width);
        State::Instance().screenHeight = static_cast<float>(Height);
        State::Instance().lastMipBias = 100.0f;
        State::Instance().lastMipBiasMax = -100.0f;
    }

    // Crude implementation of EndlesslyFlowering's AutoHDR-ReShade
    // https://github.com/EndlesslyFlowering/AutoHDR-ReShade
    if (Config::Instance()->ForceHDR.value_or_default())
    {
        LOG_INFO("Force HDR on");

        do
        {
            if (_real3 == nullptr)
                break;

            NewFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
            DXGI_COLOR_SPACE_TYPE hdrCS = DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;

            if (Config::Instance()->UseHDR10.value_or_default())
            {
                NewFormat = DXGI_FORMAT_R10G10B10A2_UNORM;
                hdrCS = DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
            }

            if (!Config::Instance()->SkipColorSpace.value_or_default())
            {
                UINT css = 0;

                result = _real3->CheckColorSpaceSupport(hdrCS, &css);

                if (result != S_OK)
                {
                    LOG_ERROR("CheckColorSpaceSupport error: {:X}", (UINT) result);
                    break;
                }

                if (DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT & css)
                {
                    result = _real3->SetColorSpace1(hdrCS);
                    if (SUCCEEDED(result) && _finalOutput) _finalOutput->SetColorSpace(hdrCS);
                    if (SUCCEEDED(result)) DlssNr::FinishedPictureColorSpace(_real3, hdrCS);

                    if (result != S_OK)
                    {
                        LOG_ERROR("SetColorSpace1 error: {:X}", (UINT) result);
                        break;
                    }
                }

                LOG_INFO("HDR format and color space are set");
            }

        } while (false);
    }

    State::Instance().scBuffers.clear();
    UINT bc = BufferCount;
    if (bc == 0 && _real1 != nullptr)
    {
        DXGI_SWAP_CHAIN_DESC1 desc {};

        if (_real1->GetDesc1(&desc) == S_OK)
            bc = desc.BufferCount;
    }

    for (UINT i = 0; i < bc; i++)
    {
        IUnknown* buffer;

        if (_real->GetBuffer(i, IID_PPV_ARGS(&buffer)) == S_OK)
        {
            State::Instance().scBuffers.push_back(buffer);
            buffer->Release();
        }
    }

    LOG_DEBUG("result: {0:X}", (UINT) result);

    if (lockedFg != nullptr)
    {
        LOG_TRACE("Releasing ffxMutex: {}", lockedFg->Mutex.getOwner());
        lockedFg->Mutex.unlockThis(3);
    }

    CheckForHdrOutput();

    return result;
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::ResizeTarget(const DXGI_MODE_DESC* pNewTargetParameters)
{
    return _real->ResizeTarget(pNewTargetParameters);
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetContainingOutput(IDXGIOutput** ppOutput)
{
    return _real->GetContainingOutput(ppOutput);
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetFrameStatistics(DXGI_FRAME_STATISTICS* pStats)
{
    return _real->GetFrameStatistics(pStats);
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetLastPresentCount(UINT* pLastPresentCount)
{
    return _real->GetLastPresentCount(pLastPresentCount);
}

//
HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetDesc1(DXGI_SWAP_CHAIN_DESC1* pDesc)
{
    return _real1->GetDesc1(pDesc);
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pDesc)
{
    return _real1->GetFullscreenDesc(pDesc);
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetHwnd(HWND* pHwnd) { return _real1->GetHwnd(pHwnd); }

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetCoreWindow(REFIID refiid, void** ppUnk)
{
    return _real1->GetCoreWindow(refiid, ppUnk);
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::Present1(UINT SyncInterval, UINT Flags,
                                                           const DXGI_PRESENT_PARAMETERS* pPresentParameters)
{
    GenshinHookCpuTiming::HookScope r448Hook((true)?GenshinHookCpuTiming::Hook::Present1:GenshinHookCpuTiming::Hook::Present);
    GenshinHookCpuTiming::PresentOwnerScope r448Owner(r448Hook);

    GenshinTransport::PresentOutputScope outputSubmission(!(Flags & DXGI_PRESENT_TEST));
    if (_real1 == nullptr)
        return DXGI_ERROR_DEVICE_REMOVED;
    if (HoYoShade::ColorConsumers(_real) != HoYoShade::ConsumerAccess::Safe) return E_FAIL;

#ifdef USE_LOCAL_MUTEX
    OwnedLockGuard lock(_localMutex, 5);
#endif

    if (_composition && !IsCompositionWindow(_handle))
        _handle = FindCompositionWindow();
    if (_composition && _handle == nullptr)
        return CallGimiNativePresent(_real1, SyncInterval, Flags, true, _starRailProviderOutput, [&] {
            return r448Hook.Original([&]{return ObserveStarRailFinalRaw(_starRailProviderOutput ? this : nullptr,_real1,_finalRawCreationQueueInput,SyncInterval,Flags,true,[&]{return _real1->Present1(SyncInterval, Flags, pPresentParameters);});});
        });

    HRESULT result;

    if ((Flags & DXGI_PRESENT_TEST) == 0)
    {
        result = LocalPresent(_real1, SyncInterval, Flags, pPresentParameters, _device, _handle, _uwp, _genshinPresentIdentity, false, nullptr, _finalOutput.get(), _starRailProviderOutput,
                              _starRailProviderOutput ? this : nullptr, _finalRawCreationQueueInput);

        // When Reflex can't be used to limit (and Optical F5Low's low latency does not keep the cap), sleep in present
        if (!State::Instance().reflexLimitsFps && !native::lowlatency::OwnsFrameCap() &&
            State::Instance().activeFgOutput == FGOutput::NoFG &&
            !IdentifyGpu::getPrimaryGpu().usesDxvk && !XellHooks::canLimit())
            FrameLimit::sleep(false);
    }
    else
    {
        result = CallGimiNativePresent(_real1, SyncInterval, Flags, true, _starRailProviderOutput, [&] {
            return r448Hook.Original([&]{return ObserveStarRailFinalRaw(_starRailProviderOutput ? this : nullptr,_real1,_finalRawCreationQueueInput,SyncInterval,Flags,true,[&]{return _real1->Present1(SyncInterval, Flags, pPresentParameters);});});
        });
    }

    return result;
}

BOOL STDMETHODCALLTYPE WrappedIDXGISwapChain4::IsTemporaryMonoSupported(void)
{
    return _real1->IsTemporaryMonoSupported();
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetRestrictToOutput(IDXGIOutput** ppRestrictToOutput)
{
    return _real1->GetRestrictToOutput(ppRestrictToOutput);
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::SetBackgroundColor(const DXGI_RGBA* pColor)
{
    return _real1->SetBackgroundColor(pColor);
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetBackgroundColor(DXGI_RGBA* pColor)
{
    return _real1->GetBackgroundColor(pColor);
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::SetRotation(DXGI_MODE_ROTATION Rotation)
{
    return _real1->SetRotation(Rotation);
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetRotation(DXGI_MODE_ROTATION* pRotation)
{
    return _real1->GetRotation(pRotation);
}

//
HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::SetSourceSize(UINT Width, UINT Height)
{
    return _real2->SetSourceSize(Width, Height);
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetSourceSize(UINT* pWidth, UINT* pHeight)
{
    return _real2->GetSourceSize(pWidth, pHeight);
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::SetMaximumFrameLatency(UINT MaxLatency)
{
    const auto caller = _ReturnAddress();
    const auto result = _real2->SetMaximumFrameLatency(MaxLatency);
    if (!GenshinHookCpuTiming::Enabled()) return result;
    static const bool starRail = _wcsicmp(Util::ExePath().filename().c_str(), L"StarRail.exe") == 0;
    if (!starRail) return result;

    // Observe this existing physical setter only. SDK calls that bypass this
    // wrapper are outside coverage; never take the source-owned provider lock.
    // TLS bounds output to two rows/second/thread, including alternating calls.
    struct Observation
    {
        const void* receiver = nullptr;
        const void* target = nullptr;
        const void* caller = nullptr;
        int id = 0;
        UINT request = 0, minimum = UINT_MAX, maximum = 0;
        HRESULT result = S_OK;
        ULONGLONG lastRow = 0;
        uint64_t calls = 0, changes = 0, failedCalls = 0;
        bool observed = false, emitted = false;
    };
    static thread_local Observation previous;
    const bool changed = !previous.observed || previous.receiver != this || previous.id != _id ||
        previous.target != _real2 || previous.caller != caller || previous.request != MaxLatency ||
        previous.result != result;
    ++previous.calls;
    previous.changes += changed;
    previous.failedCalls += FAILED(result) ? 1 : 0;
    previous.minimum = (std::min)(previous.minimum, MaxLatency);
    previous.maximum = (std::max)(previous.maximum, MaxLatency);
    previous.observed = true;
    previous.receiver = this;
    previous.id = _id;
    previous.target = _real2;
    previous.caller = caller;
    previous.request = MaxLatency;
    previous.result = result;
    const auto now = GetTickCount64();
    if (!previous.changes || (previous.emitted && now - previous.lastRow < 500)) return result;

    HMODULE callerModule = nullptr;
    char callerPath[MAX_PATH] {};
    const bool haveModule = GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(caller), &callerModule) != FALSE;
    const auto pathSize = haveModule ? GetModuleFileNameA(callerModule, callerPath, MAX_PATH) : 0;
    LOG_INFO("R789 StarRail frame-latency setter: pid={} thread={} receiver={} wrapper_id={} target={} request={} hresult=0x{:08X} caller={} caller_module={} caller_rva=0x{:X} caller_path={} calls_since_row={} changes_since_row={} failed_calls_since_row={} request_min={} request_max={} source_frame=UNKNOWN sdk_private_setters_covered=UNKNOWN intermediate_order=UNKNOWN",
        GetCurrentProcessId(), GetCurrentThreadId(), (uintptr_t)this, _id, (uintptr_t)_real2, MaxLatency,
        (uint32_t)result, (uintptr_t)caller, (uintptr_t)callerModule,
        haveModule ? (uintptr_t)caller - (uintptr_t)callerModule : 0,
        pathSize && pathSize < MAX_PATH ? callerPath : "UNKNOWN", previous.calls, previous.changes,
        previous.failedCalls, previous.minimum, previous.maximum);
    previous.emitted = true;
    previous.lastRow = now;
    previous.calls = previous.changes = previous.failedCalls = 0;
    previous.minimum = UINT_MAX;
    previous.maximum = 0;
    return result;
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetMaximumFrameLatency(UINT* pMaxLatency)
{
    return _real2->GetMaximumFrameLatency(pMaxLatency);
}

HANDLE STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetFrameLatencyWaitableObject(void)
{
    return _real2->GetFrameLatencyWaitableObject();
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::SetMatrixTransform(const DXGI_MATRIX_3X2_F* pMatrix)
{
    return _real2->SetMatrixTransform(pMatrix);
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetMatrixTransform(DXGI_MATRIX_3X2_F* pMatrix)
{
    return _real2->GetMatrixTransform(pMatrix);
}

UINT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetCurrentBackBufferIndex(void)
{
    auto index = _real3->GetCurrentBackBufferIndex();
    // LOG_TRACE("index: {}", index);
    return index;
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE ColorSpace,
                                                                         UINT* pColorSpaceSupport)
{
    return _real3->CheckColorSpaceSupport(ColorSpace, pColorSpaceSupport);
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::SetColorSpace1(DXGI_COLOR_SPACE_TYPE ColorSpace)
{
    auto result = _real3->SetColorSpace1(ColorSpace);
    if (SUCCEEDED(result) && _finalOutput) _finalOutput->SetColorSpace(ColorSpace);

    if (SUCCEEDED(result))
    {
        UpdateOutputColorSpace(ColorSpace);

        CheckForHdrOutput();

        LOG_INFO("Output HDR Active: {}", State::Instance().hdrOutputActive);

        MenuOverlayDx::ApplyThemeStyle();
    }

    // What one unit of the buffer means, which is the question the white point is really asking.
    //
    // Two of these encodings are absolute. PQ (ST.2084) puts 1.0 at 10,000 nits by definition, and
    // scRGB -- linear, Rec.709 primaries -- puts 1.0 at 80 nits. In either the divisor this pass
    // wants is arithmetic rather than a guess or a reading: paper white in nits over the unit. The
    // rest are relative and say nothing about scale.
    //
    // Logged rather than used, for now. Whether a game that reports one of these actually honours it
    // is the thing worth knowing before anything is built on it.
    const char* meaning = "relative -- no scale to be had";
    const char* name = "other";

    switch (ColorSpace)
    {
    case DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020:
        name = "PQ / ST.2084 (HDR10)";
        meaning = "absolute: 1.0 = 10000 nits, so 203-nit paper white = 0.0203";
        break;
    case DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709:
        name = "scRGB (linear, Rec.709)";
        meaning = "absolute: 1.0 = 80 nits, so 203-nit paper white = 2.5375";
        break;
    case DXGI_COLOR_SPACE_YCBCR_FULL_GHLG_TOPLEFT_P2020:
        name = "HLG";
        break;
    case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P2020:
        name = "Rec.2020, gamma 2.2";
        break;
    case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709:
        name = "sRGB (SDR)";
        break;
    default:
        break;
    }

    LOG_INFO("DLSS-NR: swapchain colour space {} -- {} ({})", (int) ColorSpace, name, meaning);

    if (SUCCEEDED(result))
        DlssNr::FinishedPictureColorSpace(_real3, ColorSpace);

    return result;
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::ResizeBuffers1(UINT BufferCount, UINT Width, UINT Height,
                                                                 DXGI_FORMAT Format, UINT SwapChainFlags,
                                                                 const UINT* pCreationNodeMask,
                                                                 IUnknown* const* ppPresentQueue)
{
    GenshinTransport::FinishPendingOutput(nullptr,GenshinTransport::OutputFlushReason::Resize);
    HoYoShade::DirectResizeScope directResize(_real, this);
    if (FAILED(directResize.Result())) return directResize.Result();
    framegen::final_output::Endpoint::Resize finalResize(_finalOutput.get());
    if (!finalResize) return DXGI_ERROR_WAS_STILL_DRAWING;
    if (!finalResize.ConsumersSafe(_real)) return E_FAIL;
    // Admission spans every early return, fallback and both real resize calls.
    DlssNrCurrentOutput::Capture::ResizeScope outputCaptureResize(
        DlssNrCurrentOutput::Capture::ResizeScope::Mode::ReserveOnly);
    if (!outputCaptureResize) return DXGI_ERROR_WAS_STILL_DRAWING;
    const auto captureResize=GenshinLegacy::BeforeResize(_real);
    if (FAILED(captureResize)) return captureResize;
    if (!DlssNr::WaitForFinishedPicture())
        return DXGI_ERROR_WAS_STILL_DRAWING;
    // Real Late Reset has now notified any borrowed capture generation. Keep
    // admission reserved until its own exact fence retires and real resize ends.
    if (!outputCaptureResize.ReadyForResize())
        return DXGI_ERROR_WAS_STILL_DRAWING;
    LOG_DEBUG("");

#ifdef USE_LOCAL_MUTEX
    // dlssg calls this from present it seems
    // don't try to get a mutex when present owns it while dlssg mod is enabled
    if (!(_localMutex.getOwner() == 4 && State::Instance().activeFgNvngx != FGNvngxReplacement::None))
    {
        OwnedLockGuard lock(_localMutex, 2);
    }
#endif

    if (ppPresentQueue != nullptr && *ppPresentQueue != nullptr)
    {
        auto state = &State::Instance();
        state->currentCommandQueue = (ID3D12CommandQueue*) *ppPresentQueue;
        _device = state->currentCommandQueue;
        FGHooks::RegisterMenuOutput(_real, _device, _handle);
    }

    // currentFG can be null during a menu-driven transition.
    IFGFeature_Dx12* lockedFg = nullptr;
    if (State::Instance().activeFgOutput == FGOutput::FSRFG && State::Instance().currentFG != nullptr &&
        Config::Instance()->FGUseMutexForSwapchain.value_or_default())
    {
        lockedFg = State::Instance().currentFG;
        LOG_TRACE("Waiting ffxMutex 3, current: {}", lockedFg->Mutex.getOwner());
        lockedFg->Mutex.lock(3);
        LOG_TRACE("Accuired ffxMutex: {}", lockedFg->Mutex.getOwner());
    }

    HRESULT result = E_FAIL;
    DXGI_SWAP_CHAIN_DESC desc {};
    _real->GetDesc(&desc);

    if (Config::Instance()->FGEnabled.value_or_default())
    {
        State::Instance().fgResetCapturedResources = true;
        State::Instance().fgOnlyUseCapturedResources = false;
        State::Instance().fgChanged = true;
    }

    // Not on a dxvk game whose menu the Vulkan present bridge draws (see Release).
    if (!VkPresentBridge::DxvkThroughVulkan())
        MenuOverlayDx::CleanupRenderTarget(true, _handle);

    State::Instance().scChanged = true;

    if (!_composition && Config::Instance()->OverrideVsync.value_or_default() && !State::Instance().SCExclusiveFullscreen &&
        State::Instance().currentFG == nullptr)
    {
        LOG_DEBUG("Overriding flags");
        SwapChainFlags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

        if (BufferCount < 2)
            BufferCount = 2;
    }

    State::Instance().SCAllowTearing = (SwapChainFlags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) > 0;

    LOG_DEBUG("BufferCount: {}, Width: {}, Height: {}, NewFormat: {}, SwapChainFlags: {:X}", BufferCount, Width, Height,
              (UINT) Format, SwapChainFlags);

    WaitForGPUIdle(_device);

    // Release swapchain backbuffers to prevent errors when resizing
    const bool isUsingOptiFgFeature =
        State::Instance().activeFgOutput == FGOutput::FSRFG || State::Instance().activeFgOutput == FGOutput::XeFG;

    if (isUsingOptiFgFeature && State::Instance().currentFG != nullptr)
    {
        IDXGISwapChain* skSC = nullptr;
        if (_real->QueryInterface(IID_IUnwrappedDXGISwapChain, (void**) &skSC) == S_OK && skSC != nullptr)
        {
            skSC->Release();
            LOG_DEBUG(
                "Found SK swapchain, skip releasing backbuffersand using ResizeBuffers instead of ResizeBuffers1");

#ifdef DXGI_DEBUG_ENABLED
            ReportDXGILiveObjects();

#ifdef ENABLE_DEBUG_LAYER_DX12
            ReportD3D12LiveObjects(State::Instance().currentD3D12Device);
#endif
#endif

            if (Config::Instance()->FGDontUseSwapchainBuffers.value_or_default())
            {
                ScopedSkipHeapCapture skipHeapCapture {};

                _lastFlags = SwapChainFlags;
                result = _real3->ResizeBuffers(BufferCount, Width, Height, Format, SwapChainFlags);
                directResize.Complete(result);
            }
            else
            {
                _lastFlags = SwapChainFlags;
                result = _real3->ResizeBuffers(BufferCount, Width, Height, Format, SwapChainFlags);
                directResize.Complete(result);
            }
        }
        /*
        else
        {
            LOG_DEBUG("Releasing backbuffers, count: {}", desc.BufferCount);

            for (UINT i = 0; i < desc.BufferCount; i++)
            {
                ID3D12Resource* backBuffer = nullptr;
                auto bbResult = _real->GetBuffer(i, IID_PPV_ARGS(&backBuffer));

                if (bbResult == S_OK)
                {
                    backBuffer->AddRef();

                    auto refCount = backBuffer->Release();
                    refCount = backBuffer->Release();
                    LOG_DEBUG("Current backbuffer {}, RefCount {}", i, refCount);

                    while (refCount > 1 && refCount < 4294967200ul)
                    {
                        refCount = backBuffer->Release();
                        LOG_DEBUG("Releasing backbuffer {}, RefCount {}", i, refCount);
                    }

                    LOG_DEBUG("Backbuffer {}, RefCount {}", i, refCount);
                }
                else
                {
                    LOG_DEBUG("GetBuffer failed for index {}: {:X}", i, (UINT) bbResult);
                    break;
                }
            }
        }
        */

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

#ifdef DXGI_DEBUG_ENABLED
    ReportDXGILiveObjects();

#ifdef ENABLE_DEBUG_LAYER_DX12
    ReportD3D12LiveObjects(State::Instance().currentD3D12Device);
#endif
#endif

    if (Config::Instance()->FGDontUseSwapchainBuffers.value_or_default())
    {
        ScopedSkipHeapCapture skipHeapCapture {};

        _lastFlags = SwapChainFlags;
        result = _real3->ResizeBuffers1(BufferCount, Width, Height, Format, SwapChainFlags, pCreationNodeMask,
                                        ppPresentQueue);
    }
    else
    {
        _lastFlags = SwapChainFlags;
        result = _real3->ResizeBuffers1(BufferCount, Width, Height, Format, SwapChainFlags, pCreationNodeMask,
                                        ppPresentQueue);
    }

    directResize.Complete(result);
    finalResize.Complete(result, BufferCount, pCreationNodeMask, ppPresentQueue, true);
    if (result == DXGI_ERROR_DEVICE_REMOVED && State::Instance().currentD3D12Device != nullptr)
        Util::GetDeviceRemovedReason(State::Instance().currentD3D12Device);

#ifdef DXGI_DEBUG_ENABLED
    if (result != S_OK)
        ReadDxgiInfoQueue();
#endif

    if (result == S_OK && State::Instance().currentFeature == nullptr)
    {
        State::Instance().screenWidth = static_cast<float>(Width);
        State::Instance().screenHeight = static_cast<float>(Height);
        State::Instance().lastMipBias = 100.0f;
        State::Instance().lastMipBiasMax = -100.0f;
    }

    // Crude implementation of EndlesslyFlowering's AutoHDR-ReShade
    // https://github.com/EndlesslyFlowering/AutoHDR-ReShade
    if (Config::Instance()->ForceHDR.value_or_default())
    {
        LOG_INFO("Force HDR on");

        do
        {
            Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            DXGI_COLOR_SPACE_TYPE hdrCS = DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;

            if (Config::Instance()->UseHDR10.value_or_default())
            {
                Format = DXGI_FORMAT_R10G10B10A2_UNORM;
                hdrCS = DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
            }

            if (!Config::Instance()->SkipColorSpace.value_or_default())
            {
                UINT css = 0;

                auto result = _real3->CheckColorSpaceSupport(hdrCS, &css);

                if (result != S_OK)
                {
                    LOG_ERROR("CheckColorSpaceSupport error: {:X}", (UINT) result);
                    break;
                }

                if (DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT & css)
                {
                    result = _real3->SetColorSpace1(hdrCS);
                    if (SUCCEEDED(result) && _finalOutput) _finalOutput->SetColorSpace(hdrCS);
                    if (SUCCEEDED(result)) DlssNr::FinishedPictureColorSpace(_real3, hdrCS);

                    if (result != S_OK)
                    {
                        LOG_ERROR("SetColorSpace1 error: {:X}", (UINT) result);
                        break;
                    }
                }

                LOG_INFO("HDR format and color space are set");
            }

        } while (false);
    }

    State::Instance().scBuffers.clear();
    UINT bc = BufferCount;
    if (bc == 0 && _real1 != nullptr)
    {
        DXGI_SWAP_CHAIN_DESC1 desc {};

        if (_real1->GetDesc1(&desc) == S_OK)
            bc = desc.BufferCount;
    }

    for (UINT i = 0; i < bc; i++)
    {
        IUnknown* buffer;

        if (_real->GetBuffer(i, IID_PPV_ARGS(&buffer)) == S_OK)
        {
            State::Instance().scBuffers.push_back(buffer);
            buffer->Release();
        }
    }

    LOG_DEBUG("result: {0:X}", (UINT) result);

    if (lockedFg != nullptr)
    {
        LOG_TRACE("Releasing ffxMutex: {}", lockedFg->Mutex.getOwner());
        lockedFg->Mutex.unlockThis(3);
    }

    CheckForHdrOutput();

    return result;
}

//
HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::SetHDRMetaData(DXGI_HDR_METADATA_TYPE Type, UINT Size,
                                                                 void* pMetaData)
{
    return _real4->SetHDRMetaData(Type, Size, pMetaData);
}

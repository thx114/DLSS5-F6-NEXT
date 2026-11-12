#include "pch.h"
#include <atomic>
#include <climits>
#include <cstdint>
#include <cstring>
#include <array>
#include <format>
#include "menu_overlay_base.h"
#include "menu_overlay_dx.h"
#include "MenuHotpathContext.h"
#include "PreviewOutputCache.h"
#include <wrapped/wrapped_swapchain.h>

#include <Util.h>
#include <Logger.h>
#include <Config.h>
#include <resource_tracking/GenericDepth_Dx12.h>
#include <resource_tracking/GenericDepth_Dx11.h>
#include <native/NativeDriverDx12.h>
#include <native/NativeDriverDx11.h>
#include <genshin/GenshinDepthProvider.h>
#include <runtime/F6InputPolicy.h>
#include <runtime/GuideCapture.h>

#include <imgui/imgui_impl_dx11.h>
#include <imgui/imgui_impl_dx12.h>
#include <imgui/imgui_impl_win32.h>

// menu
// DXGI allows up to 16 buffers; XeFG with many generated frames (6X) has more than 8
static int const NUM_BACK_BUFFERS = 16;
static int const SRV_HEAP_SIZE = 64;
static bool _dx11Device = false;
static bool _dx12Device = false;
// Changes only when the existing owner actually initializes a backend.
static UINT64 g_menuBackendGeneration = 0;

// for dx11
static ID3D11Device* g_pd3dDevice = nullptr;
static ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
static ID3D11RenderTargetView* g_pd3dRenderTarget = nullptr;

// for dx12
static ID3D12Device* g_pd3dDeviceParam = nullptr;
static ID3D12DescriptorHeap* g_pd3dRtvDescHeap = nullptr;
static ID3D12DescriptorHeap* g_pd3dSrvDescHeap = nullptr;
static DescriptorHeapAllocator g_pd3dSrvDescHeapAlloc;
static ID3D12CommandQueue* g_pd3dCommandQueue = nullptr;
// Borrowed alias of the list currently being recorded, used by resource hooks
// to exclude menu draws. Ownership lives in g_menuFrameLists below.
static ID3D12GraphicsCommandList* g_pd3dCommandList = nullptr;
static ID3D12CommandAllocator* g_commandAllocators[NUM_BACK_BUFFERS] = {};
static ID3D12Resource* g_mainRenderTargetResource[NUM_BACK_BUFFERS] = {};
static D3D12_CPU_DESCRIPTOR_HANDLE g_mainRenderTargetDescriptor[NUM_BACK_BUFFERS] = {};

static thread_local MenuOverlayDx::PresentResult* g_presentResult = nullptr;

// Host elapsed spans around existing CPU calls. They include scheduler/driver
// blocking, not GPU time or scheduled thread CPU. No GPU queries are issued.
enum class MenuCpuStage : unsigned { UiBuild, ImGuiRender, SlotSelect, SlotWait, Record, Execute, Signal, Count };
static constexpr unsigned MenuCpuStageCount = static_cast<unsigned>(MenuCpuStage::Count);
static LONGLONG MenuCpuTick() noexcept
{
    const DWORD error = GetLastError();
    LARGE_INTEGER value {};
    const bool valid = QueryPerformanceCounter(&value) != FALSE;
    SetLastError(error);
    return valid ? value.QuadPart : 0;
}
struct MenuCpuMetric
{
    UINT64 Ticks = 0, Maximum = 0, Samples = 0;
    void Add(UINT64 ticks) noexcept
    {
        Ticks += ticks;
        if (ticks > Maximum) Maximum = ticks;
        ++Samples;
    }
    void Merge(const MenuCpuMetric& other) noexcept
    {
        Ticks += other.Ticks;
        if (other.Maximum > Maximum) Maximum = other.Maximum;
        Samples += other.Samples;
    }
};
struct MenuCpuFrame;
static thread_local MenuCpuFrame* g_menuCpuFrame = nullptr;
struct MenuCpuFrame final : MenuHotpaths::FrameContext
{
    MenuHotpaths::FrameScope ContextScope;
    MenuCpuFrame* Previous;
    std::array<MenuCpuMetric, MenuCpuStageCount> Stages {};
    bool PhysicalOutput, NoUi = false, NoDraws = false, DrawReceipt = false;
    bool SlotUnavailable = false, Submitted = false, SubmitFailed = false;
    UINT64 Saturated = 0, WaitAttempts = 0, WaitCalls = 0, WaitTimeouts = 0, WaitFailures = 0, TimingFailures = 0;

    MenuCpuFrame(MenuHotpaths::Backend path, const void* device, UINT64 outputGeneration, bool physicalOutput)
        : MenuHotpaths::FrameContext { path, reinterpret_cast<std::uintptr_t>(device),
                                      g_menuBackendGeneration, outputGeneration },
          ContextScope(*this), Previous(g_menuCpuFrame), PhysicalOutput(physicalOutput)
    { g_menuCpuFrame = this; }
    ~MenuCpuFrame();
    MenuCpuFrame(const MenuCpuFrame&) = delete;
    MenuCpuFrame& operator=(const MenuCpuFrame&) = delete;
};

class MenuCpuSpan final
{
    MenuCpuFrame* Frame;
    MenuCpuStage Stage;
    LONGLONG Start;

  public:
    explicit MenuCpuSpan(MenuCpuStage stage) noexcept
        : Frame(g_menuCpuFrame), Stage(stage), Start(Frame ? MenuCpuTick() : 0) {}
    void Finish() noexcept
    {
        if (Frame == nullptr) return;
        const LONGLONG end = MenuCpuTick();
        if (Start > 0 && end >= Start)
            Frame->Stages[static_cast<unsigned>(Stage)].Add(static_cast<UINT64>(end - Start));
        else
            ++Frame->TimingFailures;
        Frame = nullptr;
    }
    ~MenuCpuSpan() { Finish(); }
    MenuCpuSpan(const MenuCpuSpan&) = delete;
    MenuCpuSpan& operator=(const MenuCpuSpan&) = delete;
};

// Fixed-size TLS buckets belong to the actual render thread. No lock, shared
// counters or retained frame/context pointers; no allocation until a log window.
struct MenuCpuWindow
{
    struct Bucket
    {
        std::array<MenuCpuMetric, MenuCpuStageCount> Stages {};
        UINT64 Frames = 0, NoUi = 0, NoDraws = 0, DrawReceipts = 0;
        UINT64 SlotUnavailable = 0, Submitted = 0, SubmitFailed = 0;
        UINT64 Saturated = 0, WaitAttempts = 0, WaitCalls = 0, WaitTimeouts = 0, WaitFailures = 0, TimingFailures = 0;
    };
    // DX11, DX12 ordinary, DX12 physical output, each split by four demand bits.
    Bucket Buckets[3][16] {};
    ULONGLONG Start = GetTickCount64();
    double MsPerTick = 0.0;

    MenuCpuWindow()
    {
        LARGE_INTEGER frequency {};
        if (QueryPerformanceFrequency(&frequency) && frequency.QuadPart > 0)
            MsPerTick = 1000.0 / static_cast<double>(frequency.QuadPart);
    }
    void Add(const MenuCpuFrame& frame)
    {
        const unsigned path = frame.Path == MenuHotpaths::Backend::Dx11 ? 0 : (frame.PhysicalOutput ? 2 : 1);
        const unsigned mask = (frame.MenuVisible ? 1u : 0u) | (frame.FpsOverlay ? 2u : 0u) |
                              (frame.NotificationDemand ? 4u : 0u) | (frame.OtherOverlayDemand ? 8u : 0u);
        auto& bucket = Buckets[path][mask];
        ++bucket.Frames;
        bucket.NoUi += frame.NoUi;
        bucket.NoDraws += frame.NoDraws;
        bucket.DrawReceipts += frame.DrawReceipt;
        bucket.SlotUnavailable += frame.SlotUnavailable;
        bucket.Submitted += frame.Submitted;
        bucket.SubmitFailed += frame.SubmitFailed;
        bucket.Saturated += frame.Saturated;
        bucket.WaitAttempts += frame.WaitAttempts;
        bucket.WaitCalls += frame.WaitCalls;
        bucket.WaitTimeouts += frame.WaitTimeouts;
        bucket.WaitFailures += frame.WaitFailures;
        bucket.TimingFailures += frame.TimingFailures;
        for (unsigned i = 0; i < MenuCpuStageCount; ++i) bucket.Stages[i].Merge(frame.Stages[i]);
        const ULONGLONG now = GetTickCount64();
        if (now - Start < 1000) return;

        static constexpr const char* names[] = { "ui_build", "imgui_render", "slot_select", "slot_wait",
                                                 "command_record", "execute", "signal" };
        for (unsigned p = 0; p < 3; ++p)
            for (unsigned m = 0; m < 16; ++m)
            {
                const auto& b = Buckets[p][m];
                if (b.Frames == 0) continue;
                std::string phases;
                for (unsigned i = 0; i < MenuCpuStageCount; ++i)
                {
                    const auto& metric = b.Stages[i];
                    const double total = metric.Ticks * MsPerTick;
                    phases += std::format(" {}_cpu_ms_total={:.3f} {}_cpu_ms_avg={:.3f} {}_cpu_ms_max={:.3f} {}_n={}",
                        names[i], total, names[i], metric.Samples ? total / metric.Samples : 0.0,
                        names[i], metric.Maximum * MsPerTick, names[i], metric.Samples);
                }
                LOG_INFO("Menu CPU stages: pid={} tid={} window_ms={} path={} physical_output={} menu_visible={} fps_overlay={} notification_demand={} other_overlay_demand={} frames={} no_ui={} no_draws={} draw_receipts={} slot_unavailable={} slot_saturated_frames={} slot_wait_attempts={} slot_wait_calls={} slot_wait_timeouts={} slot_wait_failures={} submitted={} submit_failed={} timing_ready={} timing_failures={} clock=qpc_host_elapsed slot_wait_is_subset_of_slot_select=true{}",
                    GetCurrentProcessId(), GetCurrentThreadId(), now - Start, p == 0 ? "DX11" : "DX12", p == 2,
                    (m & 1) != 0, (m & 2) != 0, (m & 4) != 0, (m & 8) != 0, b.Frames, b.NoUi, b.NoDraws,
                    b.DrawReceipts, b.SlotUnavailable, b.Saturated, b.WaitAttempts, b.WaitCalls, b.WaitTimeouts, b.WaitFailures,
                    b.Submitted, b.SubmitFailed, MsPerTick > 0.0, b.TimingFailures, phases);
            }
        for (auto& byPath : Buckets)
            for (auto& b : byPath) b = {};
        Start = GetTickCount64(); // Formatting/logging is outside all measured phases.
    }
};

MenuCpuFrame::~MenuCpuFrame()
{
    const DWORD error = GetLastError();
    g_menuCpuFrame = Previous;
    static thread_local MenuCpuWindow window;
    try
    {
        window.Add(*this);
    }
    catch (...)
    {
        // Optional formatting/logger allocation must not unwind a Present or
        // interrupt the caller's existing COM/resource cleanup on an error path.
        for (auto& byPath : window.Buckets)
            for (auto& bucket : byPath) bucket = {};
        window.Start = GetTickCount64();
    }
    SetLastError(error);
}

static void NoteOverlayWrite(UINT index)
{
    if (g_presentResult != nullptr)
    {
        g_presentResult->OverlayWritten = true;
        if (index != UINT_MAX) g_presentResult->BackbufferIndex = index;
    }
}
static bool HasBackbufferDraws(const ImDrawData* data)
{
    if (data == nullptr || data->DisplaySize.x <= 0 || data->DisplaySize.y <= 0 ||
        data->FramebufferScale.x <= 0 || data->FramebufferScale.y <= 0)
        return false;
    const ImVec2 limit(data->DisplayPos.x + data->DisplaySize.x,
                       data->DisplayPos.y + data->DisplaySize.y);
    for (const ImDrawList* list : data->CmdLists)
        for (const ImDrawCmd& command : list->CmdBuffer)
        {
            // CPU callbacks are not proof of a target draw. Immutable output
            // rejects arbitrary callbacks before reaching this recording helper.
            if (command.UserCallback != nullptr)
                continue;
            if (command.ElemCount != 0 && command.ClipRect.x < limit.x && command.ClipRect.y < limit.y &&
                command.ClipRect.z > data->DisplayPos.x && command.ClipRect.w > data->DisplayPos.y &&
                command.ClipRect.x < command.ClipRect.z && command.ClipRect.y < command.ClipRect.w)
                return true;
        }
    return false;
}

// Both ImGui backends return void and may bail out on allocation/Map failure.
// A temporary CPU-only callback after the original draw commands proves the
// backend reached its draw loop. It is removed before snapshot reuse; no custom
// GPU command, persistent callback, extra frame, or draw-data lifetime escapes.
template <typename Render>
static bool RenderWithBackbufferReceipt(ImDrawData* data, Render&& render)
{
    if (data == nullptr)
        return false;
    if (!HasBackbufferDraws(data) || data->CmdListsCount == 0)
    {
        render(data);
        return false;
    }
    bool reachedDrawLoop = false;
    ImDrawCmd receipt {};
    receipt.UserCallback = [](const ImDrawList*, const ImDrawCmd* command)
    { *static_cast<bool*>(command->UserCallbackData) = true; };
    receipt.UserCallbackData = &reachedDrawLoop;
    ImDrawList* lastList = data->CmdLists[data->CmdListsCount - 1];
    // Render is synchronous CPU command recording. Appending preserves every
    // existing draw command and neither backend records the callback for replay.
    lastList->CmdBuffer.push_back(receipt);
    struct ReceiptRemoval
    {
        ImDrawList* List;
        ~ReceiptRemoval() { List->CmdBuffer.pop_back(); }
    } removal { lastList };
    render(data);
    return reachedDrawLoop;
}

// OVERLAY_GPU_COMPLETION_BEGIN
// One queue fence tracks exact per-slot last use. Reset/map requires the chosen
// slot's completion; shared resources and teardown require the whole generation.
// Explicit cleanup may drain only this fence with a bounded timeout.
struct OverlayGpuCompletion
{
    enum class State { Ready, Busy, Unknown, Removed };
    ID3D12Device* Device = nullptr;
    ID3D12CommandQueue* Queue = nullptr;
    ID3D12Fence* Fence = nullptr;
    UINT64 Submitted = 0;
    UINT64 Next = 1;
    bool CompletionUnknown = false;
    HANDLE CompletionEvent = nullptr;
    UINT64 EventValue = 0;
    // Dedicated to saturation recovery: never re-arm a whole-generation drain event.
    HANDLE SlotEvent = nullptr;
    UINT64 SlotEventValue = 0;
    HANDLE SlotBudgetTimer = nullptr;
    bool SlotBudgetTimerUnavailable = false;

    State Poll() const
    {
        if (Device == nullptr)
            return State::Ready;
        if (FAILED(Device->GetDeviceRemovedReason()))
            return State::Removed;
        if (CompletionUnknown || Fence == nullptr)
            return State::Unknown;
        if (Submitted == 0)
            return State::Ready;
        const UINT64 completed = Fence->GetCompletedValue();
        if (completed == UINT64_MAX)
            return FAILED(Device->GetDeviceRemovedReason()) ? State::Removed : State::Unknown;
        return completed >= Submitted ? State::Ready : State::Busy;
    }

    bool Bind(ID3D12Device* device, ID3D12CommandQueue* queue)
    {
        if (Device != nullptr)
        {
            const auto state = Poll();
            return Device == device && Queue == queue && (state == State::Ready || state == State::Busy);
        }
        if (device == nullptr || queue == nullptr || FAILED(device->GetDeviceRemovedReason()))
            return false;
        ID3D12Fence* fence = nullptr;
        const HRESULT hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
        if (FAILED(hr) || fence == nullptr)
        {
            if (fence != nullptr)
                fence->Release();
            LOG_ERROR("Menu GPU completion fence creation failed: {:X}", (unsigned long)hr);
            return false;
        }
        device->AddRef();
        queue->AddRef();
        Device = device;
        Queue = queue;
        Fence = fence;
        return true;
    }

    bool Submit(ID3D12CommandList* list, UINT writtenBackbuffer)
    {
        if (Poll() != State::Ready || Queue == nullptr || Next == UINT64_MAX)
            return false;
        // Execute is void. Set unknown first, so even a failed Signal cannot
        // authorize Reset, backend writes, cleanup, or a replacement generation.
        CompletionUnknown = true;
        ID3D12CommandList* lists[] = { list };
        const auto previewReceipt = GuideCapture::Executing(Queue, 1, lists);
        Queue->ExecuteCommandLists(1, lists);
        GuideCapture::Submitted(Queue, 1, lists, previewReceipt);
        if (writtenBackbuffer != UINT_MAX) NoteOverlayWrite(writtenBackbuffer);
        const UINT64 value = Next;
        const HRESULT hr = Queue->Signal(Fence, value);
        if (FAILED(hr))
        {
            LOG_ERROR("Menu GPU Signal failed; retain one overlay generation: {:X}", (unsigned long)hr);
            return false;
        }
        Submitted = value;
        Next = value + 1;
        CompletionUnknown = false;
        return true;
    }

    // A closed, immutable list may be replayed while OTHER lists are pending.
    // The caller must prove this particular list's previous execution retired.
    bool SubmitImmutable(ID3D12CommandList* list, UINT64& lastUse, UINT writtenBackbuffer)
    {
        const State state = Poll();
        if ((state != State::Ready && state != State::Busy) || Queue == nullptr || Next == UINT64_MAX)
            return false;
        // Check this slot again immediately before execution. Other slots can
        // remain pending, while failed Signal freezes the whole generation.
        const UINT64 completed = Fence->GetCompletedValue();
        if (completed == UINT64_MAX || completed < lastUse)
            return false;
        CompletionUnknown = true;
        ID3D12CommandList* lists[] = { list };
        {
            MenuCpuSpan span(MenuCpuStage::Execute);
            const auto previewReceipt = GuideCapture::Executing(Queue, 1, lists);
            Queue->ExecuteCommandLists(1, lists);
            GuideCapture::Submitted(Queue, 1, lists, previewReceipt);
        }
        if (writtenBackbuffer != UINT_MAX) NoteOverlayWrite(writtenBackbuffer);
        const UINT64 value = Next;
        HRESULT signalResult;
        {
            MenuCpuSpan span(MenuCpuStage::Signal);
            signalResult = Queue->Signal(Fence, value);
        }
        if (FAILED(signalResult))
        {
            LOG_ERROR("Menu slot Signal failed; pending generation retained");
            return false;
        }
        lastUse = Submitted = value;
        Next = value + 1;
        CompletionUnknown = false;
        return true;
    }

    bool CanRetire() const
    {
        const State state = Poll();
        return state == State::Ready || state == State::Removed;
    }

    bool WaitForCompletion(DWORD timeoutMs, bool logTimeout = true)
    {
        const State initial = Poll();
        if (initial == State::Ready || initial == State::Removed)
            return true;
        if (initial != State::Busy)
            return false; // Unknown Signal completion cannot authorize a wait/value.
        if (CompletionEvent == nullptr)
            CompletionEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (CompletionEvent == nullptr)
            return false;
        if (EventValue != Submitted)
        {
            if (!ResetEvent(CompletionEvent))
                return false;
            const HRESULT hr = Fence->SetEventOnCompletion(Submitted, CompletionEvent);
            if (FAILED(hr))
                return CanRetire();
            EventValue = Submitted;
        }
        const DWORD wait = WaitForSingleObject(CompletionEvent, timeoutMs);
        if (wait != WAIT_OBJECT_0)
        {
            if (logTimeout) LOG_WARN("Menu exact-fence wait incomplete: wait={} error={}", wait,
                     wait == WAIT_FAILED ? GetLastError() : 0);
            return CanRetire();
        }
        return CanRetire(); // An event wake alone never proves completion.
    }

    bool WaitForSlotFence(UINT64 value, DWORD timeoutMs, DWORD* waitResult = nullptr)
    {
        if (waitResult != nullptr) *waitResult = WAIT_FAILED;
        if (Poll() != State::Busy || value == 0 || value > Submitted)
            return false;
        if (Fence->GetCompletedValue() >= value)
        {
            if (waitResult != nullptr) *waitResult = WAIT_OBJECT_0;
            return true;
        }
        if (SlotEvent == nullptr)
            SlotEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (SlotEvent == nullptr)
            return false;
        // Ordinary event timeouts round a 2 ms budget to about 15 ms on some
        // systems. No process-wide timer-resolution change or busy spin is used.
        if (SlotBudgetTimer == nullptr && !SlotBudgetTimerUnavailable)
        {
            SlotBudgetTimer = CreateWaitableTimerExW(nullptr, nullptr,
                CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_MODIFY_STATE | SYNCHRONIZE);
            SlotBudgetTimerUnavailable = SlotBudgetTimer == nullptr;
        }
        if (SlotBudgetTimer == nullptr)
            return false;
        if (SlotEventValue != value)
        {
            // Old registrations must have completed before changing target.
            // A timeout retains the same registration for the next output.
            if (SlotEventValue != 0 && Fence->GetCompletedValue() < SlotEventValue)
                return false;
            if (!ResetEvent(SlotEvent) || FAILED(Fence->SetEventOnCompletion(value, SlotEvent)))
                return false;
            SlotEventValue = value;
        }
        LARGE_INTEGER due {};
        due.QuadPart = -static_cast<LONGLONG>(timeoutMs) * 10000;
        if (!SetWaitableTimer(SlotBudgetTimer, &due, 0, nullptr, nullptr, FALSE))
            return false;
        HANDLE events[] = { SlotEvent, SlotBudgetTimer };
        // The high-resolution timer normally ends the budget first. The finite
        // timeout is a second safety exit, never an indefinite GPU wait.
        DWORD wait;
        {
            // Only this OS call is an actual wait. Earlier event setup, a poll
            // that is already ready, or timer failure must not count as one.
            if (g_menuCpuFrame != nullptr) ++g_menuCpuFrame->WaitCalls;
            MenuCpuSpan span(MenuCpuStage::SlotWait);
            wait = WaitForMultipleObjects(2, events, FALSE, timeoutMs);
        }
        if (g_menuCpuFrame != nullptr)
        {
            if (wait == WAIT_TIMEOUT || wait == WAIT_OBJECT_0 + 1) ++g_menuCpuFrame->WaitTimeouts;
            if (wait == WAIT_FAILED) ++g_menuCpuFrame->WaitFailures;
        }
        CancelWaitableTimer(SlotBudgetTimer);
        if (waitResult != nullptr)
            *waitResult = wait == WAIT_OBJECT_0 + 1 ? WAIT_TIMEOUT : wait;
        // Timeout, spurious wake, device removal and failed Signal never prove reuse.
        const State state = Poll();
        return (state == State::Ready || state == State::Busy) && Fence->GetCompletedValue() >= value;
    }

    bool ReleaseCompleted()
    {
        if (!CanRetire())
            return false;
        if (CompletionEvent != nullptr) CloseHandle(CompletionEvent);
        CompletionEvent = nullptr;
        EventValue = 0;
        if (SlotEvent != nullptr) CloseHandle(SlotEvent);
        SlotEvent = nullptr;
        SlotEventValue = 0;
        if (SlotBudgetTimer != nullptr) CloseHandle(SlotBudgetTimer);
        SlotBudgetTimer = nullptr;
        SlotBudgetTimerUnavailable = false;
        if (Fence != nullptr) Fence->Release();
        if (Queue != nullptr) Queue->Release();
        if (Device != nullptr) Device->Release();
        Fence = nullptr;
        Queue = nullptr;
        Device = nullptr;
        Submitted = 0;
        Next = 1;
        CompletionUnknown = false;
        return true;
    }
};
static OverlayGpuCompletion g_overlayGpuCompletion;
// Allocator, closed command list and ImGui VB/IB slot share one exact fence.
// These are output slots, independent of swapchain backbuffer indices and FG.
static ID3D12GraphicsCommandList* g_menuFrameLists[NUM_BACK_BUFFERS] = {};
static std::array<bool, NUM_BACK_BUFFERS> g_menuPreviewReaders {};
// Like failed-Signal menu generations, retain uncertain GPU storage through
// process teardown. Only explicit exact-fence cleanup may release its texture.
static auto& g_menuPreviewOutput = *new MenuPreview::OutputCache;

static UINT64 g_menuFrameLastUse[NUM_BACK_BUFFERS] = {};
static UINT g_menuFrameNext = 0;
static bool g_menuTextureDrainPending = false;

static UINT TrySelectMenuFrameSlot(bool* saturated = nullptr)
{
    const auto state = g_overlayGpuCompletion.Poll();
    if ((state != OverlayGpuCompletion::State::Ready && state != OverlayGpuCompletion::State::Busy) ||
        g_overlayGpuCompletion.Fence == nullptr)
        return UINT_MAX;
    const UINT64 completed = g_overlayGpuCompletion.Fence->GetCompletedValue();
    if (completed == UINT64_MAX)
        return UINT_MAX;
    UINT busy = 0;
    for (UINT offset = 0; offset < NUM_BACK_BUFFERS; ++offset)
    {
        const UINT slot = (g_menuFrameNext + offset) % NUM_BACK_BUFFERS;
        if (g_menuFrameLastUse[slot] <= completed)
        {
            if (g_presentResult != nullptr)
            {
                g_presentResult->ReplaySlot = slot;
                g_presentResult->BusySlots = busy;
            }
            return slot;
        }
        ++busy;
    }
    if (g_presentResult != nullptr) g_presentResult->BusySlots = busy;
    if (saturated != nullptr) *saturated = true;
    return UINT_MAX;
}

// Saturation is the only place where output pacing may wait. Two milliseconds
// is a recovery budget, not a promise of GPU completion or scheduler wake time.
// Waiting for the oldest exact use releases one slot instead of draining all slots.
static UINT SelectMenuFrameSlot(bool allowWait = true)
{
    bool saturated = false;
    UINT slot = TrySelectMenuFrameSlot(&saturated);
    if (saturated && g_menuCpuFrame != nullptr) ++g_menuCpuFrame->Saturated;
    if (slot != UINT_MAX || !allowWait ||
        g_overlayGpuCompletion.Poll() != OverlayGpuCompletion::State::Busy)
        return slot;
    UINT64 oldest = UINT64_MAX;
    for (UINT i = 0; i < NUM_BACK_BUFFERS; ++i)
        if (g_menuFrameLastUse[i] < oldest) oldest = g_menuFrameLastUse[i];
    if (oldest == 0 || oldest == UINT64_MAX)
        return UINT_MAX;

    if (g_presentResult != nullptr) g_presentResult->WaitAttempted = true;
    if (g_menuCpuFrame != nullptr) ++g_menuCpuFrame->WaitAttempts;
    LARGE_INTEGER before {}, after {}, frequency {};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&before);
    DWORD waitResult = WAIT_FAILED;
    g_overlayGpuCompletion.WaitForSlotFence(oldest, 2, &waitResult);
    QueryPerformanceCounter(&after);
    // Re-poll even after timeout/failure: completion can advance at that boundary.
    slot = TrySelectMenuFrameSlot();
    const UINT64 elapsedUs = frequency.QuadPart > 0
        ? static_cast<UINT64>((after.QuadPart - before.QuadPart) * 1000000 / frequency.QuadPart) : 0;
    static UINT64 waits = 0, recovered = 0, incomplete = 0, timeouts = 0, failures = 0, totalUs = 0, maxUs = 0;
    ++waits;
    if (slot != UINT_MAX) ++recovered; else ++incomplete;
    if (waitResult == WAIT_TIMEOUT) ++timeouts;
    if (waitResult == WAIT_FAILED) ++failures;
    totalUs += elapsedUs;
    if (elapsedUs > maxUs) maxUs = elapsedUs;
    if (waits <= 8 || waits % 180 == 0)
        LOG_INFO("Menu slot backpressure: waits={} recovered={} incomplete={} timeouts={} failures={} budgetMs=2 target={} completed={} submitted={} lastUs={} totalUs={} maxUs={}",
                 waits, recovered, incomplete, timeouts, failures, oldest, g_overlayGpuCompletion.Fence->GetCompletedValue(),
                 g_overlayGpuCompletion.Submitted, elapsedUs, totalUs, maxUs);
    return slot;
}

// Physical route metadata survives descriptor retirement; only a completed
// generation cleanup releases it.
static IDXGISwapChain3* g_outputMenuSwapchain = nullptr;
static DXGI_SWAP_CHAIN_DESC g_outputMenuDesc = {};
// Present may visit temporary child swapchains. They must not retire or mutate
// the live menu generation merely because their route differs from its owner.
static IUnknown* g_outputMenuOwnerSwapchain = nullptr;
static IUnknown* g_outputMenuOwnerQueue = nullptr;
static UINT64 g_outputMenuGeneration = 1;
static UINT64 g_outputMenuSnapshotGeneration = 0;
static UINT64 g_outputMenuGenerationSkips = 0;

static bool OutputMenuGenerationMatches(IDXGISwapChain* swapchain, IUnknown* queue, bool bind)
{
    if (g_outputMenuOwnerSwapchain == nullptr && !bind)
        return true;
    if (swapchain == nullptr || queue == nullptr || g_outputMenuGeneration == UINT64_MAX)
        return false;

    IUnknown* swapchainIdentity = nullptr;
    IUnknown* queueIdentity = nullptr;
    const HRESULT swapchainResult = swapchain->QueryInterface(IID_PPV_ARGS(&swapchainIdentity));
    const HRESULT queueResult = queue->QueryInterface(IID_PPV_ARGS(&queueIdentity));
    bool matches = false;
    if (SUCCEEDED(swapchainResult) && swapchainIdentity != nullptr &&
        SUCCEEDED(queueResult) && queueIdentity != nullptr)
    {
        if (g_outputMenuOwnerSwapchain == nullptr && g_outputMenuOwnerQueue == nullptr && bind)
        {
            // Keep canonical COM identities alive until the exact GPU fence retires.
            g_outputMenuOwnerSwapchain = swapchainIdentity;
            g_outputMenuOwnerQueue = queueIdentity;
            return true;
        }
        matches = swapchainIdentity == g_outputMenuOwnerSwapchain && queueIdentity == g_outputMenuOwnerQueue;
    }
    SAFE_RELEASE(swapchainIdentity);
    SAFE_RELEASE(queueIdentity);
    if (!matches)
    {
        ++g_outputMenuGenerationSkips;
        if (g_outputMenuGenerationSkips <= 3 || g_outputMenuGenerationSkips % 600 == 0)
            LOG_INFO("Menu generation gate: generation={} skipped={} foreign swapchain/queue",
                     g_outputMenuGeneration, g_outputMenuGenerationSkips);
    }
    return matches;
}

static void RetireOutputMenuGeneration()
{
    // Only CleanupRenderTargetDx12 calls this, after CanRetire proved completion.
    SAFE_RELEASE(g_outputMenuOwnerSwapchain);
    SAFE_RELEASE(g_outputMenuOwnerQueue);
    if (g_outputMenuGeneration != UINT64_MAX)
        ++g_outputMenuGeneration;
    g_outputMenuSnapshotGeneration = 0;
}

enum class OutputMenuReplay { Submitted, Unavailable, Route };
static void ClearOutputMenuSnapshot()
{
    SAFE_RELEASE(g_outputMenuSwapchain);
    g_outputMenuDesc = {};
    g_outputMenuSnapshotGeneration = 0;
}

static OutputMenuReplay OutputMenuRoute(IDXGISwapChain* swapchain, IUnknown* queue, UINT* matchingIndex = nullptr)
{
    auto reject = [](const char* reason, OutputMenuReplay result = OutputMenuReplay::Route)
    {
        if (g_presentResult != nullptr) g_presentResult->RouteReason = reason;
        return result;
    };
    IDXGISwapChain3* current = nullptr;
    if (g_outputMenuSwapchain == nullptr)
        return reject("snapshot-unavailable", OutputMenuReplay::Unavailable);
    if (g_outputMenuSnapshotGeneration != g_outputMenuGeneration)
        return reject("snapshot-generation");
    if (queue != g_overlayGpuCompletion.Queue)
        return reject("queue-identity");
    if (FAILED(swapchain->QueryInterface(IID_PPV_ARGS(&current))))
        return reject("swapchain3-query");
    DXGI_SWAP_CHAIN_DESC desc {};
    const UINT index = current->GetCurrentBackBufferIndex();
    if (g_presentResult != nullptr) g_presentResult->BackbufferIndex = index;
    ID3D12Resource* buffer = nullptr;
    const char* reason = "matched";
    if (current != g_outputMenuSwapchain) reason = "swapchain-identity";
    else if (FAILED(current->GetDesc(&desc))) reason = "description-query";
    else if (desc.BufferCount != g_outputMenuDesc.BufferCount ||
             desc.BufferDesc.Width != g_outputMenuDesc.BufferDesc.Width ||
             desc.BufferDesc.Height != g_outputMenuDesc.BufferDesc.Height ||
             desc.BufferDesc.Format != g_outputMenuDesc.BufferDesc.Format) reason = "description-changed";
    else if (index >= NUM_BACK_BUFFERS) reason = "backbuffer-index";
    else if (FAILED(current->GetBuffer(index, IID_PPV_ARGS(&buffer))) ||
             buffer != g_mainRenderTargetResource[index]) reason = "backbuffer-resource";
    if (buffer != nullptr) buffer->Release();
    current->Release();
    if (g_presentResult != nullptr) g_presentResult->RouteReason = reason;
    const bool matches = strcmp(reason, "matched") == 0;
    if (matches && matchingIndex != nullptr) *matchingIndex = index;
    return matches ? OutputMenuReplay::Submitted : OutputMenuReplay::Route;
}
static std::atomic_flag g_overlayGpuAccess = ATOMIC_FLAG_INIT;
static std::atomic<bool> g_overlayCleanupPending { false };
static std::atomic<bool> g_overlayClearQueuePending { false };
static thread_local unsigned g_overlayGpuAccessDepth = 0;
struct OverlayGpuAccess
{
    bool Acquired;
    explicit OverlayGpuAccess(bool allowNested = false)
        : Acquired(g_overlayGpuAccessDepth != 0 ? allowNested :
                   !g_overlayGpuAccess.test_and_set(std::memory_order_acquire))
    {
        if (Acquired) ++g_overlayGpuAccessDepth;
    }
    ~OverlayGpuAccess()
    {
        if (Acquired && --g_overlayGpuAccessDepth == 0)
            g_overlayGpuAccess.clear(std::memory_order_release);
    }
};
struct DeferredOverlayDescriptor
{
    D3D12_CPU_DESCRIPTOR_HANDLE Cpu;
    D3D12_GPU_DESCRIPTOR_HANDLE Gpu;
};
static DeferredOverlayDescriptor g_deferredOverlayDescriptors[SRV_HEAP_SIZE] = {};
static unsigned g_deferredOverlayDescriptorCount = 0;
static void RetireOverlayDescriptors()
{
    if (!g_overlayGpuCompletion.CanRetire())
        return;
    for (unsigned i = 0; i < g_deferredOverlayDescriptorCount; ++i)
        g_pd3dSrvDescHeapAlloc.Free(g_deferredOverlayDescriptors[i].Cpu, g_deferredOverlayDescriptors[i].Gpu);
    g_deferredOverlayDescriptorCount = 0;
}
// OVERLAY_GPU_COMPLETION_END


// current command queue for dx12 swapchain
static IUnknown* currentSCCommandQueue = nullptr;

// status
static bool _isInited = false;
static bool _d3d12Captured = false;

// for showing
static bool _showRenderImGuiDebugOnce = true;

static IID streamlineRiid {};
static bool CheckForRealObject(std::string functionName, IUnknown* pObject, IUnknown** ppRealObject)
{
    if (streamlineRiid.Data1 == 0)
    {
        auto iidResult = IIDFromString(L"{ADEC44E2-61F0-45C3-AD9F-1B37379284FF}", &streamlineRiid);

        if (iidResult != S_OK)
            return false;
    }

    auto qResult = pObject->QueryInterface(streamlineRiid, (void**) ppRealObject);

    if (qResult == S_OK && *ppRealObject != nullptr)
    {
        LOG_INFO("{} Streamline proxy found!", functionName);
        (*ppRealObject)->Release();
        return true;
    }

    return false;
}

static int GetCorrectDXGIFormat(int eCurrentFormat)
{
    switch (eCurrentFormat)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    }

    return eCurrentFormat;
}

static void CreateRenderTargetDx12(ID3D12Device* device, IDXGISwapChain* pSwapChain)
{
    LOG_FUNC();

    DXGI_SWAP_CHAIN_DESC sd;
    HRESULT hr = pSwapChain->GetDesc(&sd);

    if (hr != S_OK)
    {
        LOG_ERROR("pSwapChain->GetDesc: {0:X}", (unsigned long) hr);
        return;
    }

    for (UINT i = 0; i < sd.BufferCount; ++i)
    {
        // The descriptors are made with ImGui's D3D12 backend (RenderImGui_DX12); with another backend already holding
        // ImGui they stay empty, and a view created at address 0 crashes in the driver
        if (i >= NUM_BACK_BUFFERS || g_mainRenderTargetDescriptor[i].ptr == 0)
        {
            static bool warned = false;

            if (!warned)
            {
                warned = true;
                LOG_WARN("The menu's D3D12 render target descriptors are not set up (another ImGui backend is active): "
                         "no menu on this swapchain");
            }

            return;
        }

        ID3D12Resource* pBackBuffer = nullptr;
        auto result = pSwapChain->GetBuffer(i, IID_PPV_ARGS(&pBackBuffer));

        if (result != S_OK)
        {
            LOG_ERROR("pSwapChain->GetBuffer: {:X}", (unsigned long) result);
            return;
        }

        if (pBackBuffer != nullptr)
        {
            D3D12_RENDER_TARGET_VIEW_DESC desc = {};
            desc.Format = static_cast<DXGI_FORMAT>(GetCorrectDXGIFormat(sd.BufferDesc.Format));
            desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;

            device->CreateRenderTargetView(pBackBuffer, &desc, g_mainRenderTargetDescriptor[i]);
            g_mainRenderTargetResource[i] = pBackBuffer;
        }
    }

    LOG_INFO("done!");
}

static void CleanupRenderTargetDx12(bool clearQueue, bool allowDrain = false)
{
    if ((!_dx12Device && g_overlayGpuCompletion.Device == nullptr) || State::Instance().isShuttingDown)
        return;

    g_overlayCleanupPending.store(true, std::memory_order_release);
    if (clearQueue)
        g_overlayClearQueuePending.store(true, std::memory_order_release);
    if (!g_overlayGpuCompletion.CanRetire() && allowDrain)
    {
        const ULONGLONG start = GetTickCount64();
        const bool drained = g_overlayGpuCompletion.WaitForCompletion(500);
        LOG_INFO("Menu exact-fence cleanup drain: completed={} elapsedMs={} value={}", drained,
                 GetTickCount64() - start, g_overlayGpuCompletion.Submitted);
    }
    if (!g_overlayGpuCompletion.CanRetire())
    {
        // Keep the single existing generation, including swapchain buffer refs.
        // ResizeBuffers can fail until completion and a later caller retry; this
        // void API must not pretend cleanup/resize has succeeded.
        static bool warned = false;
        if (!warned)
        {
            warned = true;
            LOG_WARN("Menu cleanup deferred: GPU completion unknown/pending; ResizeBuffers may require retry");
        }
        return;
    }
    // Fence completion ends GPU use; successful Reset separately ends replay
    // of each exact preview reader before this renderer generation is released.
    for (UINT slot = 0; slot < NUM_BACK_BUFFERS; ++slot)
    {
        if (!g_menuPreviewReaders[slot]) continue;
        if (!g_menuFrameLists[slot] || !g_commandAllocators[slot] ||
            FAILED(g_commandAllocators[slot]->Reset()) ||
            FAILED(g_menuFrameLists[slot]->Reset(g_commandAllocators[slot], nullptr)))
            return; // Keep this generation and retry through the current entry.
        GuideCapture::Reset(g_menuFrameLists[slot]);
        g_menuFrameLists[slot]->Close();
        g_menuPreviewReaders[slot] = false;
    }
    const bool pendingClearQueue = g_overlayClearQueuePending.exchange(false, std::memory_order_acq_rel);
    clearQueue = clearQueue || pendingClearQueue;
    g_overlayCleanupPending.store(false, std::memory_order_release);

    ClearOutputMenuSnapshot();
    g_menuPreviewOutput.Retire(); // CanRetire and reader Reset proved this generation is no longer executable.
    g_menuTextureDrainPending = false;
    g_pd3dCommandList = nullptr;
    for (UINT slot = 0; slot < NUM_BACK_BUFFERS; ++slot)
    {
        SAFE_RELEASE(g_menuFrameLists[slot]);
        g_menuFrameLastUse[slot] = 0;
    }
    g_menuFrameNext = 0;
    RetireOutputMenuGeneration();

    for (UINT i = 0; i < NUM_BACK_BUFFERS; ++i)
    {
        SAFE_RELEASE(g_mainRenderTargetResource[i]);
    }

    LOG_TRACE("clearQueue: {}", clearQueue);

    if (clearQueue)
    {
        if (MenuOverlayBase::IsInited() && g_pd3dDeviceParam != nullptr && g_pd3dSrvDescHeap != nullptr &&
            ImGui::GetIO().BackendRendererUserData)
        {
            // std::this_thread::sleep_for(std::chrono::milliseconds(500));
            ImGui_ImplDX12_Shutdown(false);
        }

        SAFE_RELEASE(g_pd3dRtvDescHeap);
        SAFE_RELEASE(g_pd3dSrvDescHeap);

        for (UINT i = 0; i < NUM_BACK_BUFFERS; ++i)
        {
            SAFE_RELEASE(g_commandAllocators[i]);
        }

        g_pd3dCommandList = nullptr; // Borrowed frame-slot alias; released below.

        if (g_pd3dCommandQueue != nullptr)
            g_pd3dCommandQueue = nullptr;

        g_deferredOverlayDescriptorCount = 0;
        g_pd3dSrvDescHeapAlloc.Destroy();
        g_overlayGpuCompletion.ReleaseCompleted();
        g_pd3dDeviceParam = nullptr;
        currentSCCommandQueue = nullptr;

        // SAFE_RELEASE(g_pd3dDeviceParam);

        _dx12Device = false;
        _isInited = false;
    }
}

static void CreateRenderTargetDx11(IDXGISwapChain* pSwapChain)
{
    ID3D11Texture2D* pBackBuffer = NULL;
    pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));

    if (pBackBuffer)
    {
        DXGI_SWAP_CHAIN_DESC sd;
        pSwapChain->GetDesc(&sd);

        D3D11_RENDER_TARGET_VIEW_DESC desc = {};
        desc.Format = static_cast<DXGI_FORMAT>(GetCorrectDXGIFormat(sd.BufferDesc.Format));
        desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;

        g_pd3dDevice->CreateRenderTargetView(pBackBuffer, &desc, &g_pd3dRenderTarget);
        pBackBuffer->Release();
    }
}

static void CleanupRenderTargetDx11(bool shutDown)
{
    if (!_isInited || !_dx11Device || State::Instance().isShuttingDown)
        return;

    if (!shutDown)
        LOG_FUNC();

    SAFE_RELEASE(g_pd3dRenderTarget);

    if (g_pd3dDevice != nullptr)
        g_pd3dDevice = nullptr;

    _dx11Device = false;
    _isInited = false;
}

static void RenderImGui_DX11(IDXGISwapChain* pSwapChain)
{
    bool drawMenu = false;

    do
    {
        if (!MenuOverlayBase::IsInited())
            break;

        // Draw only when menu activated
        // if (!MenuOverlayBase::IsVisible())
        //    break;

        if (!_dx11Device || g_pd3dDevice == nullptr)
            break;

        drawMenu = true;

    } while (false);

    if (!drawMenu)
    {
        MenuOverlayBase::HideMenu();
        return;
    }

    LOG_FUNC();

    // The motion step reads the finished picture before the menu is drawn onto it (does nothing unless it is on).
    NativeMotionDx11::OnPresent(pSwapChain, g_pd3dDevice);

    ImGuiIO& io = ImGui::GetIO();
    (void) io;

    if (io.BackendRendererUserData == nullptr)
    {
        if (pSwapChain->GetDevice(IID_PPV_ARGS(&g_pd3dDevice)) == S_OK)
        {
            g_pd3dDevice->Release();
            g_pd3dDevice->GetImmediateContext(&g_pd3dDeviceContext);
            g_pd3dDeviceContext->Release();
            ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);
        }
    }

    if (_isInited)
    {
        if (!g_pd3dRenderTarget)
            CreateRenderTargetDx11(pSwapChain);

        if (ImGui::GetCurrentContext() && g_pd3dRenderTarget)
        {
            MenuCpuFrame cpu(MenuHotpaths::Backend::Dx11, g_pd3dDevice, g_outputMenuGeneration, false);
            ImGui_ImplDX11_NewFrame();
            // RenderMenu owns the single platform input sample and core frame.

            // A real DX11 backend owns both the SRV and its last GPU read.
            // The provider admits only Genshin and the exact producer context.
            if constexpr (F6InputPolicy::FsrBridgeEnabled)
                GenshinDepthProvider::BeginPreviewDraw(g_pd3dDeviceContext);
            struct RawDepthPreviewDraw
            {
                ID3D11DeviceContext* context;
                bool recorded = false;
                ~RawDepthPreviewDraw()
                {
                    if constexpr (F6InputPolicy::FsrBridgeEnabled)
                        GenshinDepthProvider::EndPreviewDraw(context, recorded);
                }
            } rawDepthPreview { g_pd3dDeviceContext };
            bool drawUi;
            {
                MenuCpuSpan span(MenuCpuStage::UiBuild);
                drawUi = MenuOverlayBase::RenderMenu();
            }
            if (drawUi)
            {
                {
                    MenuCpuSpan span(MenuCpuStage::ImGuiRender);
                    ImGui::Render();
                }

                MenuCpuSpan record(MenuCpuStage::Record);
                g_pd3dDeviceContext->OMSetRenderTargets(1, &g_pd3dRenderTarget, NULL);
                rawDepthPreview.recorded = true;
                const bool written = RenderWithBackbufferReceipt(ImGui::GetDrawData(),
                    [](ImDrawData* data) { ImGui_ImplDX11_RenderDrawData(data); });
                record.Finish();
                cpu.DrawReceipt = written;
                if (written)
                    NoteOverlayWrite(UINT_MAX);
                if (g_presentResult != nullptr) g_presentResult->Decision = "dx11-rendered";
            }
            else
                cpu.NoUi = true;
        }
    }
}

static void RenderImGui_DX12(IDXGISwapChain* pSwapChainPlain, bool physicalOutputMenu,
                             bool resumedGenshinContext, const bool sealedPreviewOutput)
{
    bool drawMenu = false;
    IDXGISwapChain3* pSwapChain = nullptr;

    do
    {
        if (pSwapChainPlain->QueryInterface(IID_PPV_ARGS(&pSwapChain)) != S_OK || pSwapChain == nullptr)
            return;

        if (!MenuOverlayBase::IsInited())
            break;

        // Draw only when menu activated
        // if (!MenuOverlayBase::IsVisible())
        //    break;

        if (!_dx12Device || currentSCCommandQueue == nullptr || g_pd3dDeviceParam == nullptr)
            break;

        drawMenu = true;

    } while (false);

    if (!drawMenu)
    {
        MenuOverlayBase::HideMenu();
        auto releaseResult = pSwapChain->Release();
        return;
    }

    LOG_FUNC();

    // Get device from swapchain
    ID3D12Device* device = g_pd3dDeviceParam;
    if (!g_overlayGpuCompletion.Bind(device, (ID3D12CommandQueue*)currentSCCommandQueue))
    {
        pSwapChain->Release();
        return;
    }

    ImGuiIO& io = ImGui::GetIO();
    (void) io;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;

    // Generate ImGui resources
    if (!io.BackendRendererUserData && currentSCCommandQueue != nullptr)
    {
        LOG_DEBUG("ImGui::GetIO().BackendRendererUserData == nullptr");

        HRESULT result;

        {
            D3D12_DESCRIPTOR_HEAP_DESC desc = {};
            desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            desc.NumDescriptors = NUM_BACK_BUFFERS;
            desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
            desc.NodeMask = 1;

            {
                ScopedSkipHeapCapture skipHeapCapture {};
                result = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&g_pd3dRtvDescHeap));
            }

            if (result != S_OK)
            {
                LOG_ERROR("CreateDescriptorHeap(g_pd3dRtvDescHeap): {0:X}", (unsigned long) result);
                MenuOverlayBase::HideMenu();
                CleanupRenderTargetDx12(true);
                pSwapChain->Release();
                return;
            }

            SIZE_T rtvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = g_pd3dRtvDescHeap->GetCPUDescriptorHandleForHeapStart();

            for (UINT i = 0; i < NUM_BACK_BUFFERS; ++i)
            {
                g_mainRenderTargetDescriptor[i] = rtvHandle;
                rtvHandle.ptr += rtvDescriptorSize;
            }
        }

        {
            D3D12_DESCRIPTOR_HEAP_DESC desc = {};
            desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            desc.NumDescriptors = SRV_HEAP_SIZE;
            desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

            {
                ScopedSkipHeapCapture skipHeapCapture {};
                result = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&g_pd3dSrvDescHeap));
            }

            if (result != S_OK)
            {
                LOG_ERROR("CreateDescriptorHeap(g_pd3dSrvDescHeap): {0:X}", (unsigned long) result);
                MenuOverlayBase::HideMenu();
                CleanupRenderTargetDx12(true);
                pSwapChain->Release();
                return;
            }

            g_pd3dSrvDescHeapAlloc.Create(device, g_pd3dSrvDescHeap);
        }

        for (UINT i = 0; i < NUM_BACK_BUFFERS; ++i)
        {
            result =
                device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_commandAllocators[i]));

            if (result != S_OK)
            {
                LOG_ERROR("CreateCommandAllocator[{0}]: {1:X}", i, (unsigned long) result);
                MenuOverlayBase::HideMenu();
                CleanupRenderTargetDx12(true);
                pSwapChain->Release();
                return;
            }
        }

        DXGI_SWAP_CHAIN_DESC scDesc;
        pSwapChain->GetDesc(&scDesc);

        ImGui_ImplDX12_InitInfo initInfo {};
        initInfo.Device = device;
        initInfo.CommandQueue = (ID3D12CommandQueue*) currentSCCommandQueue;
        initInfo.NumFramesInFlight = NUM_BACK_BUFFERS;
        initInfo.RTVFormat = scDesc.BufferDesc.Format;
        initInfo.DSVFormat = DXGI_FORMAT_UNKNOWN;
        initInfo.SrvDescriptorHeap = g_pd3dSrvDescHeap;
        initInfo.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* out_cpu_handle,
                                           D3D12_GPU_DESCRIPTOR_HANDLE* out_gpu_handle)
        { return g_pd3dSrvDescHeapAlloc.Alloc(out_cpu_handle, out_gpu_handle); };
        initInfo.SrvDescriptorFreeFn =
            [](ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle, D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle)
        { return g_pd3dSrvDescHeapAlloc.Free(cpu_handle, gpu_handle); };

        if (!ImGui_ImplDX12_Init(&initInfo))
        {
            g_presentResult->Decision = "backend-init-failed";
            CleanupRenderTargetDx12(true);
            pSwapChain->Release();
            return;
        }
        if (!resumedGenshinContext)
        {
            ImGui_ImplDX12_NewFrame(); // No menu work has been submitted in this generation.
            pSwapChain->Release();
            return;
        }
        // An authenticated parent/child renderer handoff keeps its CPU context.
        // Build the fresh target and draw this output, rather than dropping the
        // first two child frames on every brief native fallback.
    }

    if (_isInited && currentSCCommandQueue != nullptr)
    {
        if (!g_mainRenderTargetResource[0] || g_outputMenuSwapchain == nullptr)
        {
            if (!g_mainRenderTargetResource[0]) CreateRenderTargetDx12(device, pSwapChain);
            if (g_mainRenderTargetResource[0] != nullptr && SUCCEEDED(pSwapChain->GetDesc(&g_outputMenuDesc)))
            {
                pSwapChain->AddRef();
                g_outputMenuSwapchain = pSwapChain;
                g_outputMenuSnapshotGeneration = g_outputMenuGeneration;
            }
            g_presentResult->Decision = "frame-target-init";
            if (!resumedGenshinContext || !g_mainRenderTargetResource[0] || !g_outputMenuSwapchain)
            {
                pSwapChain->Release();
                return;
            }
        }
        // CPU frame/input processing does not map or reset any GPU slot. Keep
        // it running even when this output eventually exhausts the wait budget.
        // Shared texture updates retain their whole-generation completion gate.
        MenuCpuFrame cpu(MenuHotpaths::Backend::Dx12, device, g_outputMenuGeneration, physicalOutputMenu);
        ImGui_ImplDX12_NewFrame();
        bool drawUi;
        {
            MenuCpuSpan span(MenuCpuStage::UiBuild);
            drawUi = MenuOverlayBase::RenderMenu();
        }
        // Close the replay lifetime of completed preview reads even after Off
        // or a mode change. Without this, a hidden menu could pin all three
        // snapshot slots forever and never acquire a new mode to drive Reset.
        const bool pendingPreviewReaders = std::any_of(g_menuPreviewReaders.begin(),g_menuPreviewReaders.end(),
            [](bool pending){return pending;});
        const auto previewQueueState = pendingPreviewReaders ? g_overlayGpuCompletion.Poll() : OverlayGpuCompletion::State::Unknown;
        if (previewQueueState == OverlayGpuCompletion::State::Ready ||
            previewQueueState == OverlayGpuCompletion::State::Busy)
        {
            const auto completed = g_overlayGpuCompletion.Fence->GetCompletedValue();
            if (completed != UINT64_MAX)
                for (UINT i = 0; i < NUM_BACK_BUFFERS; ++i)
                    if (g_menuPreviewReaders[i] && completed >= g_menuFrameLastUse[i] &&
                        g_menuFrameLists[i] && g_commandAllocators[i] &&
                        SUCCEEDED(g_commandAllocators[i]->Reset()) &&
                        SUCCEEDED(g_menuFrameLists[i]->Reset(g_commandAllocators[i], nullptr)))
                    {
                        GuideCapture::Reset(g_menuFrameLists[i]);
                        g_menuFrameLists[i]->Close();
                        g_menuPreviewReaders[i] = false;
                    }
        }
        GuideCapture::PreviewSnapshot previewSnapshot;
        g_menuPreviewOutput.Select(GuideCapture::GetPreviewDemand(), sealedPreviewOutput);
        const bool hasPreview = sealedPreviewOutput && GuideCapture::TryAcquireCompleted(previewSnapshot);
        if (hasPreview) g_menuPreviewOutput.NeedsRefresh(previewSnapshot);
        const bool previewDemand = hasPreview || g_menuPreviewOutput.HasImage();
        if(!sealedPreviewOutput&&GuideCapture::PreviewActive())
            GuideCapture::PreviewUnavailable("Preview awaits authenticated physical output after SDK input seal");
        if (!drawUi && !previewDemand)
        {
            cpu.NoUi = true;
            g_presentResult->Decision = "frame-no-ui-demand";
            pSwapChain->Release();
            return;
        }
        if (drawUi)
        {
            MenuCpuSpan span(MenuCpuStage::ImGuiRender);
            ImGui::Render();
        }
        ImDrawData* drawData = drawUi ? ImGui::GetDrawData() : nullptr;
        bool sharedTexturesNeedUpdate = false;
        if (drawData != nullptr && drawData->Textures != nullptr)
            for (ImTextureData* texture : *drawData->Textures)
                if (texture->Status == ImTextureStatus_WantCreate || texture->Status == ImTextureStatus_WantUpdates ||
                    (texture->Status == ImTextureStatus_WantDestroy && texture->UnusedFrames >= NUM_BACK_BUFFERS))
                    sharedTexturesNeedUpdate = true;
        if (sharedTexturesNeedUpdate)
        {
            if (!g_overlayGpuCompletion.CanRetire())
            {
                // Freeze new submissions until this exact generation retires;
                // never update/free a shared font texture while any slot reads it.
                g_menuTextureDrainPending = true;
                g_presentResult->Decision = "frame-texture-drain";
                pSwapChain->Release();
                return;
            }
            for (ImTextureData* texture : *drawData->Textures)
                if (texture->Status == ImTextureStatus_WantCreate || texture->Status == ImTextureStatus_WantUpdates ||
                    (texture->Status == ImTextureStatus_WantDestroy && texture->UnusedFrames >= NUM_BACK_BUFFERS))
                    ImGui_ImplDX12_UpdateTexture(texture);
        }
        if (!previewDemand && !HasBackbufferDraws(drawData))
        {
            cpu.NoDraws = true;
            g_presentResult->Decision = "frame-no-draws";
            pSwapChain->Release();
            return;
        }
        UINT frameSlot;
        {
            MenuCpuSpan span(MenuCpuStage::SlotSelect);
            // Preview acquisition must not disable the existing bounded recovery
            // for FPS/menu draws. Cached output needs no additional reader slot.
            frameSlot = SelectMenuFrameSlot();
        }
        if (frameSlot == UINT_MAX)
        {
            cpu.SlotUnavailable = true;
            g_presentResult->Decision = "frame-slots-busy";
            pSwapChain->Release();
            return;
        }
        const UINT backbuffer = pSwapChain->GetCurrentBackBufferIndex();
        if (backbuffer >= NUM_BACK_BUFFERS || g_mainRenderTargetResource[backbuffer] == nullptr ||
            g_commandAllocators[frameSlot] == nullptr)
        {
            g_presentResult->Decision = "frame-target-invalid";
            pSwapChain->Release();
            return;
        }
        ID3D12CommandAllocator* allocator = g_commandAllocators[frameSlot];
        MenuCpuSpan record(MenuCpuStage::Record);
        if (FAILED(allocator->Reset()))
        {
            record.Finish();
            g_presentResult->Decision = "frame-allocator-reset-failed";
            pSwapChain->Release();
            return;
        }
        const HRESULT opened = g_menuFrameLists[frameSlot] != nullptr ?
            g_menuFrameLists[frameSlot]->Reset(allocator, nullptr) :
            device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr,
                                      IID_PPV_ARGS(&g_menuFrameLists[frameSlot]));
        if (FAILED(opened))
        {
            record.Finish();
            g_presentResult->Decision = "frame-list-open-failed";
            pSwapChain->Release();
            return;
        }
        auto* list = g_menuFrameLists[frameSlot];
        GuideCapture::Reset(list); // actual successful Reset above, never CPU completion
        g_pd3dCommandList = list; // Exclude only this actual menu list in draw/resource hooks.
        // Preserve the ordinary preview's former whole-generation completion
        // boundary. Physical outputs previously never recorded this shared copy.
        if (!physicalOutputMenu && g_overlayGpuCompletion.Poll() == OverlayGpuCompletion::State::Ready)
            GenericDepthDx12::RecordPreviewCopy(list);
        D3D12_RESOURCE_BARRIER barrier {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = g_mainRenderTargetResource[backbuffer];
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        list->ResourceBarrier(1, &barrier);
        list->OMSetRenderTargets(1, &g_mainRenderTargetDescriptor[backbuffer], FALSE, nullptr);
        // Independent physical-output pass; normal NR/SDK targets stay intact.
        // The fullscreen snapshot is drawn first, with the menu above it.
        bool previewReaderRecorded = false;
        const bool previewDrawn = previewDemand && g_menuPreviewOutput.Record(device, list,
            g_mainRenderTargetResource[backbuffer], hasPreview ? &previewSnapshot : nullptr, previewReaderRecorded);
        if (previewReaderRecorded) g_menuPreviewReaders[frameSlot] = true;
        list->SetDescriptorHeaps(1, &g_pd3dSrvDescHeap);
        bool slotAccepted = true;
        const bool uiWritten = drawUi && RenderWithBackbufferReceipt(drawData, [&](ImDrawData* data)
        { slotAccepted = ImGui_ImplDX12_RenderDrawDataForSlot(data, list, frameSlot); });
        const bool written = previewDrawn || uiWritten;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        list->ResourceBarrier(1, &barrier);
        const HRESULT closed = list->Close();
        g_pd3dCommandList = nullptr;
        record.Finish();
        cpu.DrawReceipt = written;
        if (FAILED(closed) || !slotAccepted)
        {
            g_menuPreviewOutput.DiscardRecording();
            g_presentResult->Decision = "frame-record-failed";
            pSwapChain->Release();
            return;
        }
        // Fresh recording uses exactly the same exact-last-use proof as closed
        // immutable submission; other slots may remain in flight on this queue.
        const bool submitted = g_overlayGpuCompletion.SubmitImmutable(list, g_menuFrameLastUse[frameSlot],
                                                                     written ? backbuffer : UINT_MAX);
        cpu.Submitted = submitted;
        cpu.SubmitFailed = !submitted;
        g_presentResult->Decision = submitted ? "frame-slot-submitted" : "frame-slot-submit-failed";
        if (submitted) {
            if (previewDrawn) g_menuPreviewOutput.NoteSubmitted();
            g_menuFrameNext = (frameSlot + 1) % NUM_BACK_BUFFERS;
        }
        else g_menuPreviewOutput.DiscardRecording();
    }

    pSwapChain->Release();
}

ID3D12GraphicsCommandList* MenuOverlayDx::MenuCommandList() { return g_pd3dCommandList; }

ID3D12DescriptorHeap* MenuOverlayDx::SrvHeap() { return g_pd3dSrvDescHeap; }

bool MenuOverlayDx::AllocSrv(D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu)
{
    OverlayGpuAccess access(true);
    if (!access.Acquired || g_pd3dSrvDescHeap == nullptr)
        return false;
    RetireOverlayDescriptors();
    if (g_pd3dSrvDescHeapAlloc.FreeIndices.Size <= 0)
        return false;

    g_pd3dSrvDescHeapAlloc.Alloc(cpu, gpu);
    return true;
}

void MenuOverlayDx::FreeSrv(D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE gpu)
{
    OverlayGpuAccess access(true);
    if (!access.Acquired || g_pd3dSrvDescHeap == nullptr)
        return; // Keep this slot allocated rather than race/reuse a live descriptor.
    if (g_overlayGpuCompletion.CanRetire())
    {
        RetireOverlayDescriptors();
        g_pd3dSrvDescHeapAlloc.Free(cpu, gpu);
        return;
    }
    for (unsigned i = 0; i < g_deferredOverlayDescriptorCount; ++i)
        if (g_deferredOverlayDescriptors[i].Cpu.ptr == cpu.ptr)
            return;
    if (g_deferredOverlayDescriptorCount < SRV_HEAP_SIZE)
        g_deferredOverlayDescriptors[g_deferredOverlayDescriptorCount++] = { cpu, gpu };
    // Fixed capacity: retain any excess slot until full heap teardown, never grow.

}

void MenuOverlayDx::CleanupRenderTarget(bool clearQueue, HWND hWnd)
{
    LOG_FUNC();
    g_overlayCleanupPending.store(true, std::memory_order_release);
    if (clearQueue)
        g_overlayClearQueuePending.store(true, std::memory_order_release);
    OverlayGpuAccess access;
    if (!access.Acquired)
        return;
    if (_dx11Device)
    {
        g_overlayCleanupPending.store(false, std::memory_order_release);
        g_overlayClearQueuePending.store(false, std::memory_order_release);
    }

    auto fg = State::Instance().currentFG;
    if (fg != nullptr && fg->FrameGenerationContext() != nullptr && fg->IsActive())
    {
        State::Instance().fgChanged = true;
        fg->UpdateTarget();
        fg->Deactivate();
    }

    if (_dx11Device)
        CleanupRenderTargetDx11(false);
    else
        CleanupRenderTargetDx12(clearQueue, true);
}

void MenuOverlayDx::Present(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags,
                            const DXGI_PRESENT_PARAMETERS* pPresentParameters, IUnknown* pDevice, HWND hWnd, bool isUWP,
                            bool waitForOutputMenu, PresentResult* result)
{
    if (result != nullptr)
    {
        *result = {};
        result->ImmutableOutput = waitForOutputMenu;
        result->Decision = "invalid-or-test";
    }
    if (pSwapChain == nullptr || pDevice == nullptr || (Flags & DXGI_PRESENT_TEST) != 0)
        return;
    // Capture route permission before any menu callbacks can edit the FG
    // request. A selected FG source remains excluded even when requested Off;
    // authenticated physical-output callers supply waitForOutputMenu themselves.
    const bool sealedPreviewOutput = waitForOutputMenu || State::Instance().activeFgOutput == FGOutput::NoFG;
    // D3D12's finder closes its frame with the native input step (NativeMotionDx12::OnPresent / OnFGPresent). Under
    // Dx11wDx12SC the D3D11 finder's frame is closed by NativeMotionDx11::OnFGPresent instead: frame generation's present
    // still reaches here, and a second close in the same frame sees no draws and resets every candidate's warm-up.
    if (State::Instance().swapchainInteropApi == SwapchainInteropApi::None)
        GenericDepthDx11::OnPresent(pSwapChain);

    if (!Config::Instance()->OverlayMenu.value_or_default())
    {
        if (result != nullptr) result->Decision = "overlay-disabled";
        // Normally this step runs inside RenderImGui_DX11, as part of the overlay's own device capture; with the
        // overlay off that capture never happens, so grab the device just for this call -- it does nothing unless
        // native input (no frame generation) is actually on.
        ID3D11Device* device = nullptr;
        if (pDevice->QueryInterface(IID_PPV_ARGS(&device)) == S_OK)
        {
            NativeMotionDx11::OnPresent(pSwapChain, device);
            device->Release();
        }

        MenuOverlayBase::Present();
        return;
    }

    OverlayGpuAccess access;
    if (!access.Acquired)
    {
        if (result != nullptr) result->Decision = "access-busy";
        return; // Only menu work is skipped; the caller continues FG/NR/Present.
    }
    // Captured before releasing OverlayGpuAccess; never read mutable generation
    // state from the wrapper after another Present/cleanup may have acquired it.
    struct PresentCapture
    {
        PresentResult Local;
        PresentResult* Output;
        PresentResult* Previous;
        IDXGISwapChain* Swapchain;
        IUnknown* IncomingQueue;
        const DXGI_PRESENT_PARAMETERS* Parameters;
        PresentCapture(PresentResult* output, IDXGISwapChain* swapchain, IUnknown* incomingQueue,
                       const DXGI_PRESENT_PARAMETERS* parameters, bool immutable)
            : Output(output != nullptr ? output : &Local), Previous(g_presentResult),
              Swapchain(swapchain), IncomingQueue(incomingQueue), Parameters(parameters)
        {
            g_presentResult = Output;
            Output->Decision = "prepare";
            Output->ImmutableOutput = immutable;
        }
        ~PresentCapture()
        {
            Output->Generation = g_outputMenuGeneration;
            Output->SnapshotGeneration = g_outputMenuSnapshotGeneration;
            Output->Epoch = UINT64_MAX; // Fresh output slots have no replay epoch.
            Output->Queue = g_overlayGpuCompletion.Queue;
            Output->CompletionState = static_cast<int>(g_overlayGpuCompletion.Poll());
            Output->SubmittedFence = g_overlayGpuCompletion.Submitted;
            Output->CompletedFence = g_overlayGpuCompletion.Fence != nullptr
                ? g_overlayGpuCompletion.Fence->GetCompletedValue() : 0;
            if (Output->ReplaySlot < NUM_BACK_BUFFERS)
                Output->BackbufferFence = g_menuFrameLastUse[Output->ReplaySlot];
            Output->DrainFence = g_menuTextureDrainPending || g_deferredOverlayDescriptorCount != 0
                ? g_overlayGpuCompletion.Submitted : 0;
            Output->DrainPending = g_menuTextureDrainPending || g_deferredOverlayDescriptorCount != 0;
            // Other present owners can ignore the optional return value. Retain
            // bounded submission evidence for those routes as well.
            if (Output == &Local)
            {
                static std::atomic<UINT64> calls { 0 };
                static thread_local const char* lastDecision = nullptr;
                static thread_local bool lastWritten = false;
                static thread_local ULONGLONG lastAt = 0;
                const UINT64 count = ++calls;
                const ULONGLONG now = GetTickCount64();
                const bool changed = lastDecision != Output->Decision || lastWritten != Output->OverlayWritten;
                if (count <= 12 || count % 180 == 0 || (changed && now - lastAt >= 250))
                {
                    lastDecision = Output->Decision;
                    lastWritten = Output->OverlayWritten;
                    lastAt = now;
                    LOG_INFO("Overlay direct-owner evidence: pid={} tick={} calls={} sc={:X} incomingQueue={:X} queue={:X} decision={} overlayWritten={} immutable={} index={} slot={} busySlots={} route={} refresh={} generation={}/{} epoch={} completion={} fence={}/{} bufferFence={} drain={}:{} wait={} present1={} dirty={} scroll={}/{}",
                             GetCurrentProcessId(), now, count, (size_t)Swapchain, (size_t)IncomingQueue,
                             (size_t)Output->Queue, Output->Decision, Output->OverlayWritten, Output->ImmutableOutput,
                             Output->BackbufferIndex, Output->ReplaySlot, Output->BusySlots, Output->RouteReason,
                             Output->RefreshReason, Output->Generation, Output->SnapshotGeneration, Output->Epoch,
                             Output->CompletionState, Output->CompletedFence, Output->SubmittedFence,
                             Output->BackbufferFence, Output->DrainPending, Output->DrainFence, Output->WaitAttempted,
                             Parameters != nullptr, Parameters != nullptr ? Parameters->DirtyRectsCount : 0,
                             Parameters != nullptr && Parameters->pScrollRect != nullptr,
                             Parameters != nullptr && Parameters->pScrollOffset != nullptr);
                }
            }
            g_presentResult = Previous;
        }
    } capture(result, pSwapChain, pDevice, pPresentParameters, waitForOutputMenu);
    IDXGISwapChain3* indexed = nullptr;
    if (SUCCEEDED(pSwapChain->QueryInterface(IID_PPV_ARGS(&indexed))))
    {
        g_presentResult->BackbufferIndex = indexed->GetCurrentBackBufferIndex();
        indexed->Release();
    }
    // A resize/final-release cleanup may have deferred on the exact GPU fence
    // or the access guard. Let a later Present finish only that existing request,
    // without waiting, even if the old owner no longer presents. Otherwise the
    // identity gate would permanently strand its replacement behind the old owner.
    if (g_overlayCleanupPending.load(std::memory_order_acquire))
    {
        if (!_dx12Device && g_overlayGpuCompletion.Device == nullptr)
        {
            g_overlayCleanupPending.store(false, std::memory_order_release);
            g_overlayClearQueuePending.store(false, std::memory_order_release);
        }
        else
            CleanupRenderTargetDx12(false);
        if (g_overlayCleanupPending.load(std::memory_order_acquire))
            return;
    }
    // Identity is checked before initiating drains, queue changes or window
    // teardown. A foreign child cannot itself request generation retirement.
    if (!OutputMenuGenerationMatches(pSwapChain, pDevice, false))
    {
        // Only an authenticated native-only fallback may retire its own retained
        // child menu generation. Ordinary parent Presents keep the child's owner.
        ComPtr<IDXGISwapChain> childOwner;
        const HWND childWindow = MenuOverlayBase::Handle();
        const bool ownFallback = GenshinNativeParentFrame::OwnsMenuFallback(pSwapChain, pDevice) &&
            g_outputMenuOwnerSwapchain && childWindow && hWnd &&
            GetAncestor(childWindow, GA_ROOT) == hWnd &&
            SUCCEEDED(g_outputMenuOwnerSwapchain->QueryInterface(IID_PPV_ARGS(&childOwner))) &&
            GenshinNativeParentFrame::IsChildOutput(childOwner.Get(), childWindow);
        if (ownFallback)
        {
            // Nonblocking existing exact-fence cleanup. Never deactivate a provider here.
            CleanupRenderTargetDx12(true);
            static uint64_t fallbackReports = 0;
            if (++fallbackReports <= 8 || fallbackReports % 600 == 0)
                LOG_INFO("R827 phase15 menu fallback: pid={} retired={} pending={} generation={}",
                    GetCurrentProcessId(),g_outputMenuOwnerSwapchain==nullptr,
                    g_overlayCleanupPending.load(std::memory_order_acquire),g_outputMenuGeneration);
            if (g_overlayCleanupPending.load(std::memory_order_acquire))
            {
                g_presentResult->Decision = "genshin-menu-fallback-drain";
                return;
            }
        }
        if (!ownFallback || !OutputMenuGenerationMatches(pSwapChain, pDevice, false))
        {
            g_presentResult->RouteReason = "generation-owner";
            g_presentResult->Decision = "generation-route";
            return;
        }
    }
    auto completion = g_overlayGpuCompletion.Poll();
    IUnknown* selectedQueue = pDevice;
    if (!CheckForRealObject(__FUNCTION__, pDevice, &selectedQueue))
        selectedQueue = pDevice;
    // Both ordinary and physical-FG outputs use independent frame slots. Only
    // shared resource/lifecycle changes require whole-generation quiescence.
    if (completion == OverlayGpuCompletion::State::Removed)
    {
        CleanupRenderTargetDx12(true);
        g_presentResult->Decision = "frame-device-removed";
        return;
    }
    if (completion == OverlayGpuCompletion::State::Unknown)
    {
        g_presentResult->Decision = "frame-completion-unknown";
        return;
    }
    const bool routeChanged = g_outputMenuSwapchain != nullptr &&
        (MenuOverlayBase::Handle() != hWnd ||
         OutputMenuRoute(pSwapChain, selectedQueue) != OutputMenuReplay::Submitted);
    if (routeChanged)
    {
        // This request freezes submissions immediately and never transplants an
        // in-flight fence/list/texture generation onto another physical route.
        CleanupRenderTargetDx12(true);
        g_presentResult->Decision = "frame-route-retire";
        if (g_overlayCleanupPending.load(std::memory_order_acquire))
            return;
    }
    if (g_menuTextureDrainPending || g_deferredOverlayDescriptorCount != 0)
    {
        if (!g_overlayGpuCompletion.CanRetire())
        {
            g_presentResult->Decision = "frame-shared-resource-drain";
            return; // No new submissions can move the pending shared-resource target.
        }
        g_menuTextureDrainPending = false;
    }
    RetireOverlayDescriptors();

    ID3D12CommandQueue* cq = nullptr;
    ID3D11Device* device = nullptr;
    ID3D12Device* device12 = nullptr;

    // try to obtain directx objects and find the path
    if (pDevice->QueryInterface(IID_PPV_ARGS(&device)) == S_OK)
    {
        if (!_dx11Device)
            LOG_DEBUG("D3D11Device captured");

    }
    else if (pDevice->QueryInterface(IID_PPV_ARGS(&cq)) == S_OK)
    {
        if (!_dx12Device)
            LOG_DEBUG("D3D12CommandQueue captured");

        IUnknown* selectedQueue = nullptr;
        if (!CheckForRealObject(__FUNCTION__, pDevice, &selectedQueue))
            selectedQueue = pDevice;
        if (g_overlayGpuCompletion.Queue != nullptr && g_overlayGpuCompletion.Queue != selectedQueue)
            CleanupRenderTargetDx12(true); // Old completion was proved above; never transplant its fence.
        if (g_overlayCleanupPending.load(std::memory_order_acquire))
        {
            SAFE_RELEASE(cq);
            g_presentResult->Decision = "backend-queue-drain";
            return;
        }

        if (((ID3D12CommandQueue*) selectedQueue)->GetDevice(IID_PPV_ARGS(&device12)) == S_OK)
        {
            if (!_dx12Device)
                LOG_DEBUG("D3D12Device captured");

        }
    }

    const bool incomingDx11 = device != nullptr;
    const bool incomingDx12 = cq != nullptr && device12 != nullptr;
    if (!incomingDx11 && !incomingDx12)
    {
        SAFE_RELEASE(cq);
        SAFE_RELEASE(device12);
        g_presentResult->Decision = "backend-device-unavailable";
        return;
    }
    const bool backendChanged = incomingDx11 ? _dx12Device : _dx11Device;
    if (backendChanged && _dx11Device)
    {
        // Retire the actual outgoing renderer before destroying its ImGui context.
        if (MenuOverlayBase::IsInited() && ImGui::GetIO().BackendRendererUserData)
        {
            const char* renderer = ImGui::GetIO().BackendRendererName;
            if (!renderer || std::strcmp(renderer, "imgui_impl_dx11") != 0)
            {
                SAFE_RELEASE(cq); SAFE_RELEASE(device); SAFE_RELEASE(device12);
                g_presentResult->Decision = "backend-renderer-owner";
                return;
            }
            ImGui_ImplDX11_Shutdown(false);
        }
        CleanupRenderTargetDx11(false);
        _dx11Device = false;
        g_pd3dDevice = nullptr;
        g_pd3dDeviceContext = nullptr;
        _isInited = false;
    }

    // Retain only CPU menu state across the authenticated Genshin parent/child
    // pair. The actual outgoing renderer and its GPU resources still retire
    // through the existing exact-completion path above/below.
    bool resumedGenshinContext = false;
    // A pending native-fallback drain can finish at the next ordinary native
    // Present, after its fallback flag has left scope. OwnsNative authenticates
    // CPU context reuse on that same parent; generation retirement above still
    // requires OwnsMenuFallback, and Retarget checks the exact owned window pair.
    const bool authenticatedGenshinRoute = !isUWP &&
        (GenshinNativeParentFrame::OwnsNative(pSwapChain, pDevice) ||
         GenshinNativeParentFrame::IsChildOutput(pSwapChain, hWnd));
    // Incoming API identity, not stale backend flags, selects initialization.
    if (backendChanged || MenuOverlayBase::Handle() != hWnd)
    {
        LOG_DEBUG("Handle changed {:X} -> {:X}", (size_t) MenuOverlayBase::Handle(), (size_t) hWnd);

        if (_dx12Device || g_overlayGpuCompletion.Device != nullptr)
            CleanupRenderTargetDx12(true); // Preserve exact-completion retirement before context destruction.
        if (g_overlayCleanupPending.load(std::memory_order_acquire))
        {
            SAFE_RELEASE(cq); SAFE_RELEASE(device); SAFE_RELEASE(device12);
            g_presentResult->Decision = "backend-context-drain";
            return;
        }
        currentSCCommandQueue = incomingDx12 ? selectedQueue : nullptr;
        if (authenticatedGenshinRoute && MenuOverlayBase::IsInited())
            resumedGenshinContext = MenuOverlayBase::RetargetGenshinWindow(hWnd);
        if (!resumedGenshinContext)
        {
            if (MenuOverlayBase::IsInited())
                MenuOverlayBase::Shutdown();
            MenuOverlayBase::Init(hWnd, isUWP);
        }

        _isInited = false;
    }

    // Init
    if (!_isInited)
    {
        if (incomingDx11)
        {
            CleanupRenderTargetDx11(false);
            g_pd3dDevice = device;
            CreateRenderTargetDx11(pSwapChain);
            _dx11Device = true;
            _dx12Device = false;
            MenuOverlayBase::Dx11Ready();
            _isInited = MenuOverlayBase::IsInited() && g_pd3dRenderTarget != nullptr;
        }
        else if (incomingDx12)
        {
            CleanupRenderTargetDx12(true);
            if (g_overlayCleanupPending.load(std::memory_order_acquire))
            {
                if (cq != nullptr) cq->Release();
                if (device12 != nullptr) device12->Release();
                return;
            }
            currentSCCommandQueue = selectedQueue;
            g_pd3dCommandQueue = cq;
            g_pd3dDeviceParam = device12;

            MenuOverlayBase::Dx12Ready();
            _dx11Device = false;
            _dx12Device = true;
            _isInited = MenuOverlayBase::IsInited() && currentSCCommandQueue != nullptr &&
                g_pd3dDeviceParam != nullptr;
        }
        if (_isInited) ++g_menuBackendGeneration;
        static uint64_t backendInitReports = 0;
        if (++backendInitReports <= 8 || backendInitReports % 600 == 0)
        LOG_INFO("R827 phase15 menu backend: pid={} incoming={} dx11={} dx12={} initialized={} device11={} device12={} queue={} backend_changed={} hwnd={:X}",
            GetCurrentProcessId(),incomingDx11?"DX11":"DX12",_dx11Device,_dx12Device,_isInited,
            g_pd3dDevice!=nullptr,g_pd3dDeviceParam!=nullptr,currentSCCommandQueue!=nullptr,backendChanged,(size_t)hWnd);
    }

    if (!_isInited)
    {
        SAFE_RELEASE(cq); SAFE_RELEASE(device); SAFE_RELEASE(device12);
        g_presentResult->Decision = "backend-not-initialized";
        return;
    }
    if (_dx12Device && !OutputMenuGenerationMatches(pSwapChain, pDevice, true))
    {
        SAFE_RELEASE(cq);
        SAFE_RELEASE(device);
        SAFE_RELEASE(device12);
        return;
    }

    {
        ScopedSkipHeapCapture skipHeapCapture {};

        // Render menu
        if (_dx11Device)
            RenderImGui_DX11(pSwapChain);
        else if (_dx12Device)
            RenderImGui_DX12(pSwapChain, waitForOutputMenu, resumedGenshinContext, sealedPreviewOutput);
    }

    // release used objects
    if (cq != nullptr)
        cq->Release();

    if (device != nullptr)
        device->Release();

    if (device12 != nullptr)
        device12->Release();
}

void MenuOverlayDx::ApplyThemeStyle() { MenuOverlayBase::ApplyThemeStyle(); }

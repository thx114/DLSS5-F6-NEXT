#include <pch.h>

#include <functional>
#include <atomic>
#include <array>
#include <vector>

#include "IFeature_Dx12.h"
#include "State.h"
#include <resource_tracking/GenericDepth_Dx12.h>
#include <integration/HoYoShade.h>
#include <native/VirtualUpscalerDriver.h>
#include <genshin/GenshinSrReceipt.h>
#include <genshin/GenshinCpuStageTiming.h>

namespace GenshinSrRecordingCpu
{
namespace
{
struct Counter
{
    uint64_t calls = 0, ticks = 0, minTicks = 0, maxTicks = 0;
    uint64_t failures = 0, timeouts = 0, aborted = 0;
};
struct Window
{
    const void* feature = nullptr;
    uint64_t epoch = 0, qpcFailures = 0;
    int64_t start = 0, frequency = 0;
    std::array<Counter, static_cast<unsigned>(Stage::Count)> stages {};
};
// No shared mutable counters, locks, heap containers or GPU queries. Feature
// switches discard a partial window; epoch prevents nested scopes mixing owners.
thread_local Window window;
int64_t Tick() noexcept
{
    const auto savedError = GetLastError();
    LARGE_INTEGER now {};
    const bool ok = QueryPerformanceCounter(&now) != FALSE;
    SetLastError(savedError);
    return ok ? now.QuadPart : 0;
}
}
Scope::Scope(const void* feature, Stage stage, bool enabled) noexcept : feature_(feature), stage_(stage)
{
    if (!GENSHIN_CPU_STAGE_TIMING_ENABLED || !enabled || !feature || static_cast<unsigned>(stage) >= static_cast<unsigned>(Stage::Count))
        return;
    const auto savedError = GetLastError();
    if (!window.frequency)
    {
        LARGE_INTEGER frequency {};
        if (QueryPerformanceFrequency(&frequency))
            window.frequency = frequency.QuadPart;
    }
    SetLastError(savedError);
    if (window.frequency <= 0)
        return;
    start_ = Tick();
    if (!start_) { ++window.qpcFailures; return; }
    if (window.feature != feature)
    {
        window.feature = feature;
        ++window.epoch;
        window.stages = {};
        window.qpcFailures = 0;
        window.start = start_;
    }
    epoch_ = window.epoch;
    active_ = true;
}
void Scope::Complete(HRESULT result, bool aborted) noexcept
{
    if (!active_)
        return;
    active_ = false;
    if (window.feature != feature_ || window.epoch != epoch_)
        return;
    const auto finish = Tick();
    if (!finish || finish < start_) { ++window.qpcFailures; return; }
    auto& counter = window.stages[static_cast<unsigned>(stage_)];
    const auto ticks = static_cast<uint64_t>(finish - start_);
    if (!counter.calls || ticks < counter.minTicks) counter.minTicks = ticks;
    ++counter.calls;
    counter.ticks += ticks;
    counter.maxTicks = std::max(counter.maxTicks, ticks);
    counter.failures += FAILED(result);
    counter.timeouts += result == HRESULT_FROM_WIN32(WAIT_TIMEOUT);
    counter.aborted += aborted;
}
void Report(const void* feature) noexcept
{
    if (!GENSHIN_CPU_STAGE_TIMING_ENABLED || !feature || window.feature != feature || !window.start || window.frequency <= 0)
        return;
    const auto finish = Tick();
    if (!finish || finish < window.start) { ++window.qpcFailures; return; }
    if (finish - window.start < window.frequency)
        return;
    const auto snapshot = window;
    window.start = finish;
    window.stages = {};
    window.qpcFailures = 0;
    static constexpr const char* names[] = { "recording_wait_global", "provider_record", "ngx_evaluate",
        "transport_execute_signal", "feature_signal", "execute_signal_inclusive" };
    const double msPerTick = 1000.0 / static_cast<double>(snapshot.frequency);
    const auto savedError = GetLastError();
    try
    {
        // Called after the measured scopes, at most once per elapsed second. Stages
        // are inclusive and can straddle windows; never sum them as frame time.
        for (unsigned i = 0; i < static_cast<unsigned>(Stage::Count); ++i)
        {
            const auto& c = snapshot.stages[i];
            LOG_INFO("R067 SR CPU: pid={} thread={} feature={} window_start_qpc={} window_end_qpc={} "
                     "window_ms={:.3f} stage={} n={} cpu_ms_total={:.3f} cpu_ms_min={:.3f} "
                     "cpu_ms_avg={:.3f} cpu_ms_max={:.3f} failures={} timeouts={} aborted={} "
                     "qpc_failures={} host_elapsed=true gpu_time=false fps=false wait_policy=global",
                GetCurrentProcessId(), GetCurrentThreadId(), reinterpret_cast<uint64_t>(snapshot.feature),
                snapshot.start, finish, (finish - snapshot.start) * msPerTick, names[i], c.calls,
                c.ticks * msPerTick, c.minTicks * msPerTick,
                c.calls ? c.ticks * msPerTick / c.calls : 0.0, c.maxTicks * msPerTick,
                c.failures, c.timeouts, c.aborted, snapshot.qpcFailures);
        }
    }
    catch (...) {} // Diagnostics must not change provider/retirement behavior.
    SetLastError(savedError);
}
}

void IFeature_Dx12::ResourceBarrier(ID3D12GraphicsCommandList* InCommandList, ID3D12Resource* InResource,
                                    D3D12_RESOURCE_STATES InBeforeState, D3D12_RESOURCE_STATES InAfterState) const
{
    if (InBeforeState == InAfterState)
        return;

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = InResource;
    barrier.Transition.StateBefore = InBeforeState;
    barrier.Transition.StateAfter = InAfterState;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    InCommandList->ResourceBarrier(1, &barrier);
}

bool IFeature_Dx12::Init(ID3D12Device* InDevice, ID3D12GraphicsCommandList* InCommandList,
                         NVSDK_NGX_Parameter* InParameters)
{
    Device = InDevice;

    auto result = InitInternal(InCommandList, InParameters);

    if (result)
    {
        if (!Config::Instance()->OverlayMenu.value_or_default())
        {
            std::lock_guard lock(ImguiMutex);

            if (Imgui == nullptr)
                Imgui = std::make_unique<Menu_Dx12>(Util::GetProcessWindow(), InDevice);
        }

        OutputScaler = std::make_unique<OS_Dx12>("Output Scaling", InDevice, (TargetWidth() < DisplayWidth()));
        RCAS = std::make_unique<RCAS_Dx12>("RCAS", InDevice);
        Bias = std::make_unique<Bias_Dx12>("Bias", InDevice); // TODO: not needed on DLSS/DLSSD
        Magnifier = std::make_unique<Magnifier_Dx12>("Magnifier", InDevice);

        UpscalerTime = std::make_unique<GpuTime_Dx12>(InDevice);
    }

    return result;
}

bool IFeature_Dx12::Evaluate(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters)
{
    const EvaluateContext context { this, InCommandList, std::nullopt, false };
    return EvaluateWithContext(InCommandList, InParameters, context);
}

bool IFeature_Dx12::EvaluateForRecordingSlot(ID3D12GraphicsCommandList* InCommandList,
                                            NVSDK_NGX_Parameter* InParameters, UINT recordingSlot, bool isolatePassResources)
{
    const EvaluateContext context { this, InCommandList, recordingSlot, isolatePassResources };
    return EvaluateWithContext(InCommandList, InParameters, context);
}

bool IFeature_Dx12::CanIsolateRecordingResources() const
{
    // DLSS records directly into the supplied list; it does not use Bias or
    // the other backends' Opt upload/descriptor helpers. The DX12 inline menu
    // still has its own two-frame mutable resources, so it retains global wait.
    return GetUpscalerType() == Upscaler::DLSS && Config::Instance()->OverlayMenu.value_or_default() &&
           RCAS && OutputScaler && Magnifier && UpscalerTime;
}

void IFeature_Dx12::FinishRecordingSlotResources(UINT recordingSlot, bool executed) noexcept
{
    if (RCAS) RCAS->FinishRecordingSlot(recordingSlot, executed);
    if (OutputScaler) OutputScaler->FinishRecordingSlot(recordingSlot, executed);
    if (Magnifier) Magnifier->FinishRecordingSlot(recordingSlot, executed);
}

bool IFeature_Dx12::EvaluateWithContext(ID3D12GraphicsCommandList* InCommandList,
                                       NVSDK_NGX_Parameter* InParameters, const EvaluateContext& context)
{
    // Validate before any parameter, resource, descriptor or upload mutation.
    // The synchronous pass lambdas borrow this stack context only until return;
    // scope exit/unwinding discards it without installing any ambient slot state.
    if (InCommandList == nullptr || InParameters == nullptr || context.feature != this ||
        context.commandList != InCommandList ||
        (context.recordingSlot && *context.recordingSlot >= RCAS_Dx12::RecordingSlotCount) ||
        (context.isolatePassResources && (!context.recordingSlot || !CanIsolateRecordingResources())))
        return false;

    static_assert(RCAS_Dx12::RecordingSlotCount == OS_Dx12::RecordingSlotCount &&
                  RCAS_Dx12::RecordingSlotCount == Magnifier_Dx12::RecordingSlotCount &&
                  RCAS_Dx12::RecordingSlotCount == GpuTime_Dx12::RecordingSlotCount);
    const std::optional<UINT> passSlot = context.isolatePassResources ? context.recordingSlot : std::nullopt;

    // Bare NGX Evaluate supplies a recording list, not permission to submit
    // it. The bridge's explicit recording slot and NativeProducer's virtual
    // consumer have their own real owner seams. Leave all other recordings
    // unsupported for BeforeSR; never insert an immediate input read here.
    if (HoYoShade::Selected(HoYoShade::Stage::BeforeSr) &&
        !context.recordingSlot && !native::IsVirtualUpscalerFeature(this))
    {
        static std::atomic<bool> reported{false};
        if (!reported.exchange(true, std::memory_order_relaxed))
            LOG_WARN("HoYoShade BeforeSR unsupported: external DX12 Evaluate recording has no producer submit authority; selected SR continues");
    }

    // The depth finder only runs while the game is not calling an upscaler.
    GenericDepthDx12::NoteUpscalerCall();

    if (!IsInited())
    {
        LOG_ERROR("Not inited!");
        return false;
    }

    if (passSlot)
    {
        RCAS->BeginRecordingSlot(*passSlot);
        OutputScaler->BeginRecordingSlot(*passSlot);
        Magnifier->BeginRecordingSlot(*passSlot);
    }

    if (Config::Instance()->OverrideSharpness.value_or_default())
        _sharpness = Config::Instance()->Sharpness.value_or_default();
    else
        _sharpness = GetSharpness(InParameters);

    if (_sharpness > 1.0f)
        _sharpness = 1.0f;

    // Those upcalers don't have their own sharpness so always need to use RCAS when sharpness is set
    auto upscaler = GetUpscalerType();
    bool useRcas = upscaler == Upscaler::XeSS ||
                   (upscaler == Upscaler::DLSS && Version() >= feature_version(2, 5, 1)) || upscaler == Upscaler::DLSSD;

    if (!useRcas)
        useRcas = Config::Instance()->RcasEnabled.value_or_default();

    if (_sharpness == 0.0f)
        useRcas = false;

    // Need RCAS for MAS
    if (!useRcas && (Config::Instance()->MotionSharpnessEnabled.value_or_default() &&
                     Config::Instance()->MotionSharpness.value_or_default() > 0.0f))
    {
        useRcas = true;
    }

    if (!RCAS->IsInit())
        useRcas = false;

    bool useOutputScaling =
        Config::Instance()->OutputScalingEnabled.value_or_default() && (LowResMV() || RenderWidth() == DisplayWidth());

    if (!OutputScaler->IsInit())
        useOutputScaling = false;

    ID3D12Resource* paramOutput = nullptr;
    ID3D12Resource* paramMotion = nullptr;
    ID3D12Resource* paramDepth = nullptr;

    InParameters->Get(NVSDK_NGX_Parameter_Output, &paramOutput);
    InParameters->Get(NVSDK_NGX_Parameter_MotionVectors, &paramMotion);
    InParameters->Get(NVSDK_NGX_Parameter_Depth, &paramDepth);

    // Order is important as that's the order of shader dispatch
    std::vector<ShaderPass> pipeline;
    bool outputGridPasses=true;
    bool passSetupFailed = false;

    if (useOutputScaling)
    {
        pipeline.push_back(
            { // Setup
              [&](ID3D12Resource* nextOutput) -> ID3D12Resource*
              {
                  if (OutputScaler->CreateBufferResource(Device, nextOutput, TargetWidth(), TargetHeight(),
                                                         D3D12_RESOURCE_STATE_UNORDERED_ACCESS, passSlot))
                  {
                      OutputScaler->SetBufferState(InCommandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, passSlot);
                      return OutputScaler->Buffer(passSlot);
                  }
                  if (passSlot) passSetupFailed = true;
                  return nullptr;
              },

              // Dispatch
              [&](ID3D12Resource* input, ID3D12Resource* output) -> bool
              {
                  LOG_DEBUG("Scaling output...");
                  OutputScaler->SetBufferState(InCommandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, passSlot);

                  if (!OutputScaler->Dispatch(InCommandList, input, output, passSlot))
                  {
                      if (!passSlot)
                      {
                          Config::Instance()->OutputScalingEnabled.set_volatile_value(false);
                          State::Instance().changeBackend[Handle()->Id] = true;
                      }
                      return false;
                  }
                  return true;
              } });
    }

    _actualSharpness = _sharpness;
    if (useRcas)
    {
        pipeline.push_back(
            { // Setup
              [&](ID3D12Resource* nextOutput) -> ID3D12Resource*
              {
                  // Disable any built-in sharpness shaders
                  InParameters->Set(NVSDK_NGX_Parameter_Sharpness, 0.0f);
                  _sharpness = 0.0f;

                  if (RCAS->CreateBufferResource(Device, nextOutput, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, passSlot))
                  {
                      RCAS->SetBufferState(InCommandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, passSlot);
                      return RCAS->Buffer(passSlot);
                  }
                  if (passSlot) passSetupFailed = true;
                  return nullptr;
              },

              // Dispatch
              [&](ID3D12Resource* input, ID3D12Resource* output) -> bool
              {
                  if (!RCAS->CanRender(passSlot) || !paramMotion || !paramOutput) {
                      outputGridPasses=false;
                      return !context.isolatePassResources;
                  }

                  RCAS->SetBufferState(InCommandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, passSlot);

                  RcasConstants rcasConstants {};

                  rcasConstants.Sharpness = _actualSharpness.value_or(_sharpness);
                  rcasConstants.DepthIsLinear = DepthLinear();
                  rcasConstants.DepthIsReversed = DepthInverted();
                  rcasConstants.IsHdr = IsHdr();

                  // Restore value
                  _sharpness = _actualSharpness.value_or(_sharpness);
                  _actualSharpness.reset();

                  InParameters->Get(NVSDK_NGX_Parameter_MV_Scale_X, &rcasConstants.MvScaleX);
                  InParameters->Get(NVSDK_NGX_Parameter_MV_Scale_Y, &rcasConstants.MvScaleY);

                  float nearPlane = 0.0f;
                  float farPlane = 0.0f;

                  // We need camera near and far for DLSSD
                  // We passthrough those values from the DLSSG params onto the upscaler's params
                  if (InParameters->Get("DLSSG.CameraNear", &nearPlane) == NVSDK_NGX_Result_Success &&
                      InParameters->Get("DLSSG.CameraFar", &farPlane) == NVSDK_NGX_Result_Success)
                  {
                      rcasConstants.CameraNear = nearPlane;
                      rcasConstants.CameraFar = farPlane;
                  }
                  else
                  {
                      rcasConstants.CameraNear = Config::Instance()->FsrCameraNear.value_or_default();
                      rcasConstants.CameraFar = Config::Instance()->FsrCameraFar.value_or_default();
                  }

                  if (!RCAS->Dispatch(InCommandList, input, paramMotion, rcasConstants, output, paramDepth,
                                      context.recordingSlot))
                  {
                      if (!passSlot) Config::Instance()->RcasEnabled.set_volatile_value(false);
                      return false;
                  }
                  return true;
              } });
    }

    const bool outputGridMagnifier=Magnifier->ShouldRun();
    if (outputGridMagnifier)
    {
        pipeline.push_back(
            { // Setup
              [&](ID3D12Resource* nextOutput) -> ID3D12Resource*
              {
                  if (Magnifier->CreateBufferResource(Device, nextOutput, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, passSlot))
                  {
                      Magnifier->SetBufferState(InCommandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, passSlot);
                      return Magnifier->Buffer(passSlot);
                  }
                  if (passSlot) passSetupFailed = true;

                  return nullptr;
              },

              // Dispatch
              [&](ID3D12Resource* input, ID3D12Resource* output) -> bool
              {
                  if (!Magnifier->CanRender(passSlot) || !paramMotion || !paramOutput)
                      return !context.isolatePassResources;

                  Magnifier->SetBufferState(InCommandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, passSlot);

                  return Magnifier->Dispatch(InCommandList, input, output, passSlot);
              } });
    }

    // Iterate BACKWARDS to establish where each shader needs to pull its input from
    ID3D12Resource* currentTarget = paramOutput;
    for (auto it = pipeline.rbegin(); it != pipeline.rend(); ++it)
    {
        ID3D12Resource* requiredInput = it->Setup(currentTarget);
        if (requiredInput)
        {
            it->outputBuffer = currentTarget;
            it->inputBuffer = requiredInput;
            currentTarget = requiredInput; // Shift the target back for the next previous stage
        }
    }

    if (passSetupFailed) return false;

    // Upscaler will write to the first active shader, or just output
    InParameters->Set(NVSDK_NGX_Parameter_Output, currentTarget);
    struct RestoreOutput
    {
        NVSDK_NGX_Parameter* params;
        ID3D12Resource* output;
        ~RestoreOutput() { params->Set(NVSDK_NGX_Parameter_Output, output); }
    } restoreOutput { InParameters, paramOutput };

    struct OutputGridPipeline {
        GenshinSrReceipt::Scope* scope=nullptr;bool complete=false;
        ~OutputGridPipeline(){if(scope)scope->EndOutputPipeline(complete);}
    } outputGridPipeline;
    if(auto* scope=GenshinSrReceipt::active){
        // RCAS preserves pixel coordinates and extent. Scaling/magnification
        // have no admitted output-grid contract in this implementation.
        if(scope->BeginOutputPipeline(InCommandList,InParameters,currentTarget,paramOutput,
            !useOutputScaling && !outputGridMagnifier))outputGridPipeline.scope=scope;
    }
    UpscalerTime->Start(InCommandList, context.recordingSlot);

    auto evalResult = EvaluateInternal(InCommandList, InParameters);

    UpscalerTime->End(InCommandList);

    if (!evalResult)
        return false;

    // Iterate FORWARDS to execute the shaders in the defined order
    for (auto& pass : pipeline)
    {
        if (pass.inputBuffer && pass.outputBuffer)
        {
            if (!pass.Dispatch(pass.inputBuffer, pass.outputBuffer))
            {
                // Legacy paths retain their existing fallback. A private-slot
                // failure cannot leave the output on an unfinished intermediate
                // or silently disable a user-selected pass.
                return !context.isolatePassResources;
            }
        }
    }

    // A concurrently changed menu choice must not reach its unisolated
    // uploads/descriptors after a slot-only wait. The Bridge discards this
    // unexecuted list and returns to its global-wait path on the next call.
    if (context.isolatePassResources && !CanIsolateRecordingResources()) return false;

    // The actual inline branch is forbidden on isolated recordings even if
    // OverlayMenu changes after the check above. The next call acquires the
    // global dependency for that newly selected route.
    // imgui
    if (!context.isolatePassResources && !Config::Instance()->OverlayMenu.value_or_default() && _frameCount > 30)
    {
        std::lock_guard lock(ImguiMutex);

        if (Imgui != nullptr && Imgui.get() != nullptr)
        {
            if (Imgui->IsHandleDifferent())
            {
                Imgui.reset();
            }
            else
                Imgui->Render(InCommandList, paramOutput);
        }
        else
        {
            if (Imgui == nullptr || Imgui.get() == nullptr)
                Imgui = std::make_unique<Menu_Dx12>(GetForegroundWindow(), Device);
        }
    }

    InParameters->Set(NVSDK_NGX_Parameter_Output, paramOutput);

    outputGridPipeline.complete=evalResult && outputGridPasses;
    return evalResult;
}

std::optional<double> IFeature_Dx12::ReadUpscalerTime(void* commandQueueVoid)
{
    ID3D12CommandQueue* commandQueue = (ID3D12CommandQueue*) commandQueueVoid;

    lastUpscalerTime = UpscalerTime->ReadGpuTime(commandQueue);
    lastRcasTime = RCAS->ReadGpuTime(commandQueue);
    lastOutputScalingTime = OutputScaler->ReadGpuTime(commandQueue);

    return sumOpts(lastUpscalerTime, lastRcasTime, lastOutputScalingTime);
}

void IFeature_Dx12::ReadDetailedGpuTimes(void* commandQueueVoid, std::vector<DetailedGpuTime>& detailedGpuTimes)
{
    ID3D12CommandQueue* commandQueue = (ID3D12CommandQueue*) commandQueueVoid;

    detailedGpuTimes.clear();

    // Do not call ReadGpuTime twice for shaders
    if (lastUpscalerTime)
        detailedGpuTimes.emplace_back(DetailedGpuTime { ShortName(), lastUpscalerTime.value(), true });

    if (lastRcasTime)
        detailedGpuTimes.emplace_back(DetailedGpuTime { RCAS->Name(), lastRcasTime.value(), true });

    if (lastOutputScalingTime)
        detailedGpuTimes.emplace_back(DetailedGpuTime { OutputScaler->Name(), lastOutputScalingTime.value(), true });

    auto magnifierTime = Magnifier->ReadGpuTime(commandQueue);

    if (magnifierTime)
        detailedGpuTimes.emplace_back(DetailedGpuTime { Magnifier->Name(), magnifierTime.value(), false });
}

IFeature_Dx12::IFeature_Dx12(unsigned int InHandleId, NVSDK_NGX_Parameter* InParameters) {}

IFeature_Dx12::~IFeature_Dx12()
{
    if (State::Instance().isShuttingDown)
    {
        // Returning alone still runs unique_ptr destructors under the loader lock.
        OutputScaler.release();
        RCAS.release();
        Bias.release();
        Magnifier.release();
        UpscalerTime.release();
        return;
    }

    {
        std::lock_guard lock(ImguiMutex);
        Imgui.reset();
    }

    OutputScaler.reset();
    RCAS.reset();
    Bias.reset();
}

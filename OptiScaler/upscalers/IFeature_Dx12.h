#pragma once
#include <d3d12.h>
#include <cstdint>
#include <optional>
#include "IFeature.h"

#include "SysUtils.h"
#include <Util.h>
#include <menu/menu_dx12.h>
#include <shaders/output_scaling/OS_Dx12.h>
#include <shaders/rcas/RCAS_Dx12.h>
#include <shaders/bias/Bias_Dx12.h>
#include <shaders/magnifier/Magnifier_Dx12.h>
#include <gpu_time/GpuTime_Dx12.h>

// Observation only: fixed thread-local counters, never consulted by admission.
// These are host elapsed spans, including driver blocking/scheduling, not GPU time.
namespace GenshinSrRecordingCpu
{
enum class Stage : unsigned
{
    RecordingWait, ProviderRecord, NgxEvaluate, TransportExecuteSignal, FeatureSignal, ExecuteSignal, Count
};
void Report(const void* feature) noexcept;
struct ReportOnExit final
{
    const void* feature = nullptr;
    ~ReportOnExit() noexcept { Report(feature); }
};
class Scope final
{
    const void* feature_ = nullptr;
    Stage stage_;
    uint64_t epoch_ = 0;
    int64_t start_ = 0;
    bool active_ = false;
    void Complete(HRESULT result, bool aborted) noexcept;

  public:
    Scope(const void* feature, Stage stage, bool enabled) noexcept;
    ~Scope() noexcept { Complete(E_ABORT, true); }
    HRESULT Finish(HRESULT result) noexcept
    {
        Complete(result, false);
        return result;
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};
}

class IFeature_Dx12 : public virtual IFeature
{
  private:
    // Immutable, call-local context: no member/TLS slot can survive a failed
    // Evaluate, exception, early return or a nested un-slotted call.
    struct EvaluateContext
    {
        const IFeature_Dx12* feature;
        ID3D12GraphicsCommandList* commandList;
        std::optional<UINT> recordingSlot;
        bool isolatePassResources;
    };
    bool EvaluateWithContext(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters,
                             const EvaluateContext& context);

    struct ShaderPass
    {
        // Requests the target buffer it needs to write to. Returns the buffer the PREVIOUS stage must write to
        std::function<ID3D12Resource*(ID3D12Resource* nextOutput)> Setup;

        // Runs the shader
        std::function<bool(ID3D12Resource* input, ID3D12Resource* output)> Dispatch;

        // Internal state tracked by the pipeline setup loop
        ID3D12Resource* inputBuffer = nullptr;
        ID3D12Resource* outputBuffer = nullptr;
    };

  protected:
    ID3D12Device* Device = nullptr;
    static inline std::unique_ptr<Menu_Dx12> Imgui = nullptr;
    // Imgui is shared by every feature, and a replaced feature is destroyed on another thread (Util::DelayedDestroy): its
    // destructor resetting Imgui raced the render thread drawing with it, and two features dropped together reset it at
    // once (RE2 crash on a backend switch with the old overlay). Recursive: a feature destroyed while the menu is drawn.
    static inline std::recursive_mutex ImguiMutex;
    std::unique_ptr<OS_Dx12> OutputScaler = nullptr;
    std::unique_ptr<RCAS_Dx12> RCAS = nullptr;
    std::unique_ptr<Bias_Dx12> Bias = nullptr;
    std::unique_ptr<Magnifier_Dx12> Magnifier = nullptr;

    std::unique_ptr<GpuTime_Dx12> UpscalerTime = nullptr;

    void ResourceBarrier(ID3D12GraphicsCommandList* InCommandList, ID3D12Resource* InResource,
                         D3D12_RESOURCE_STATES InBeforeState, D3D12_RESOURCE_STATES InAfterState) const;

    virtual bool InitInternal(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters) = 0;
    virtual bool EvaluateInternal(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters) = 0;

  public:
    bool Init(ID3D12Device* InDevice, ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters);
    bool Evaluate(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters);
    // Internal Bridge entry; the caller retains ownership of slot/fence admission.
    // Backend virtual EvaluateInternal implementations remain unchanged.
    bool EvaluateForRecordingSlot(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters,
                                  UINT recordingSlot, bool isolatePassResources = false);
    // Only the selected DLSS path without the shared inline menu is covered.
    // The Bridge separately excludes inline NR and owns the fence wait.
    bool CanIsolateRecordingResources() const;
    void FinishRecordingSlotResources(UINT recordingSlot, bool executed) noexcept;

    API Api() const override { return API::DX12; }
    std::optional<double> ReadUpscalerTime(void* commandQueue) override;
    void ReadDetailedGpuTimes(void* commandQueue, std::vector<DetailedGpuTime>& detailedGpuTimes) override;

    IFeature_Dx12(unsigned int InHandleId, NVSDK_NGX_Parameter* InParameters);

    ~IFeature_Dx12();
};

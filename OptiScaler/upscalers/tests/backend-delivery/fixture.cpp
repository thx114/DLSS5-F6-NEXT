// Production control flow with fake SDK/factory calls and real WARP identities.
// This fixture does not certify vendor execution, queue retirement or games.
#define NOMINMAX
#define LOG_WARN(...) ((void)0)
#include <d3d11.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <nvsdk_ngx_params.h>
#include <upscalers/FeatureEvaluation.h>
#include <upscalers/BackendSelection.h>
#include <inputs/NVNGX_DLSS.h>
#include <upscalers/FeatureTransition.h>
#include <chrono>
#include <thread>
#include <functional>
#include <map>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <mutex>
using Microsoft::WRL::ComPtr;
#define LOG_ERROR(...) ((void)0)
#define LOG_INFO(...) ((void)0)
#define LOG_TRACE(...) ((void)0)
unsigned checks = 0;
void Check(bool ok, const char* reason)
{
    ++checks;
    if (!ok) { std::fprintf(stderr, "FAIL %s\n", reason); std::exit(1); }
}
struct Parameters final : NVSDK_NGX_Parameter
{
    ID3D11Resource* output11 = nullptr;
    ID3D12Resource* output12 = nullptr;
    bool refuseOutput = false;
    void Set(const char*, ID3D11Resource* p) override { if (!refuseOutput) output11 = p; }
    void Set(const char*, ID3D12Resource* p) override { if (!refuseOutput) output12 = p; }
    NVSDK_NGX_Result Get(const char*, ID3D11Resource** p) const override { *p = output11; return NVSDK_NGX_Result_Success; }
    NVSDK_NGX_Result Get(const char*, ID3D12Resource** p) const override { *p = output12; return NVSDK_NGX_Result_Success; }
#define UNUSED_PARAMETER(T) \
    void Set(const char*, T) override {} \
    NVSDK_NGX_Result Get(const char*, T*) const override { return NVSDK_NGX_Result_Fail; }
    UNUSED_PARAMETER(unsigned long long)
    UNUSED_PARAMETER(float)
    UNUSED_PARAMETER(double)
    UNUSED_PARAMETER(unsigned)
    UNUSED_PARAMETER(int)
    UNUSED_PARAMETER(void*)
#undef UNUSED_PARAMETER
    void Reset() override {}
};
using NVNGX_Parameters = Parameters;
struct Option { bool value = false; bool value_or_default() const { return value; } };
struct Config
{
    std::optional<Upscaler> Dx11Upscaler, Dx12Upscaler, VulkanUpscaler;
    std::optional<bool> DisableReactiveMask;
    std::optional<float> DlssReactiveMaskBias;
    Option Dx11DelayedInit, OverlayMenu;
    static Config* Instance() { static Config config; return &config; }
};
struct Menu
{
    bool IsHandleDifferent() { return false; }
    template<class... Args> void Render(Args...) {}
    template<class... Args> explicit Menu(Args...) {}
};
using Menu_Dx11 = Menu;
using Menu_Dx12 = Menu;
namespace Util
{
void* GetProcessWindow() { return nullptr; }
template<class T> void DelayedDestroy(std::unique_ptr<T> owner) { owner.reset(); }
}
struct RestoreGuard { void Restore() {} };
struct Timing { template<class T> void End(T*) {} template<class... T> void Start(T...) {} };
struct Pass
{
    void* inputBuffer = reinterpret_cast<void*>(1);
    void* outputBuffer = reinterpret_cast<void*>(2);
    std::function<bool(void*, void*)> Dispatch;
};
struct Delivery
{
    FeatureEvaluation nextEvaluation = FeatureEvaluation::OutputRecorded;
    bool failPass = false, outputChainComplete = true, outputGridPasses = true;
    unsigned dispatches = 0, _frameCount = 0;
    std::unique_ptr<Menu> Imgui;
    std::recursive_mutex ImguiMutex;
    ID3D11Device* Device = nullptr;
    Timing timer;
    Timing* UpscalerTime = &timer;
    bool CanIsolateRecordingResources() const { return true; }
    template<class... Args> FeatureEvaluation EvaluateInternal(Args...) { return nextEvaluation; }
    std::vector<Pass> Pipeline()
    {
        return {Pass{reinterpret_cast<void*>(1), reinterpret_cast<void*>(2),
            [&](void*, void*) { ++dispatches; return !failPass; }}};
    }
    bool Dx11(ID3D11DeviceContext* InDeviceContext, Parameters* InParameters,
              ID3D11Resource* paramOutput, ID3D11Resource** orderedOutput)
    {
        *orderedOutput = nullptr;
        bool result = true;
        auto pipeline = Pipeline();
        [[maybe_unused]] RestoreGuard restoreColor, savedState;
        [[maybe_unused]] ComPtr<ID3D11ShaderResourceView> restoreSRVs[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT]{};
        [[maybe_unused]] ComPtr<ID3D11SamplerState> restoreSamplerStates[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT]{};
        [[maybe_unused]] ComPtr<ID3D11Buffer> restoreCBVs[D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT]{};
        [[maybe_unused]] ComPtr<ID3D11UnorderedAccessView> restoreUAVs[D3D11_1_UAV_SLOT_COUNT]{};
        [[maybe_unused]] ID3D11RenderTargetView* rawRTVs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
        [[maybe_unused]] ComPtr<ID3D11DepthStencilView> restoreDSV;
        [[maybe_unused]] const UINT uavCount = D3D11_PS_CS_UAV_REGISTER_COUNT;
        (void)restoreColor; (void)savedState;
        InParameters->Set(NVSDK_NGX_Parameter_Output, paramOutput);
#include "Dx11DeliveryProduction.inl"
    }
    struct Context { ID3D12Resource** recordedOutput; bool isolatePassResources = false; std::optional<unsigned> recordingSlot; };
    bool Dx12(ID3D12GraphicsCommandList* InCommandList, Parameters* InParameters,
              ID3D12Resource* paramOutput, ID3D12Resource** recordedOutput)
    {
        *recordedOutput = nullptr;
        Context context{recordedOutput};
        auto pipeline = Pipeline();
        auto* currentTarget = paramOutput;
#include "Dx12DeliveryProduction.inl"
    }
};
enum class FGInput { None, Upscaler };
struct IFeature_Dx11
{
    Upscaler backend;
    bool initialized = false, initializeSucceeds = true;
    explicit IFeature_Dx11(Upscaler target) : backend(target) {}
    bool ModuleLoaded() const { return true; }
    Upscaler GetUpscalerType() const { return backend; }
    unsigned GetFeatureFlags() const { return 0; }
    unsigned RenderWidth() const { return 640; }
    unsigned RenderHeight() const { return 360; }
    unsigned DisplayWidth() const { return 1280; }
    unsigned DisplayHeight() const { return 720; }
    int PerfQualityValue() const { return 2; }
};
struct State
{
    API api = API::DX11;
    Upscaler newBackend = Upscaler::Reset;
    void* currentFG = nullptr;
    FGInput activeFgInput = FGInput::None;
    bool fgChanged = false, clearCapturedHudlesses = false;
    IFeature_Dx11* currentFeature = nullptr;
    std::map<unsigned, bool> changeBackend;
    static State& Instance() { static State state; return state; }
};
namespace native { bool OpticalFgNrEnabled() { return false; } }
namespace IdentifyGpu { struct Gpu { bool dlssCapable = true; }; Gpu getPrimaryGpu() { return {}; } }
namespace NVNGXProxy { bool IsDx11Inited() { return true; } }
namespace taa::source_output
{
struct Source { unsigned handle; uintptr_t context, parameters; bool Valid() const { return true; } };
Source* current = nullptr;
}
struct FeatureLifecycleTrace { FeatureLifecycleTrace(const char*, unsigned) {} };
enum class ImGuiToastType { Warning };
namespace ImGui
{
struct Notification { ImGuiToastType type; int duration; const char* message; };
void InsertNotification(Notification) {}
}
NVSDK_NGX_Parameter* GetNGXParameters(API, bool) { return new Parameters; }
struct FeatureProvider_Dx11
{
    static inline bool nextInitSucceeds = true;
    static Upscaler ResolveBackend(std::optional<Upscaler> requested, bool taaInput,
        std::optional<Upscaler> dx12, bool capable, bool ready)
    { return BackendSelection::ResolveDx11(requested, taaInput, dx12, capable, ready); }
#include "Dx11FallbackProduction.inl"
    static bool GetFeature(Upscaler target, unsigned, NVSDK_NGX_Parameter*,
        std::unique_ptr<IFeature_Dx11>* feature, bool)
    {
        *feature = std::make_unique<IFeature_Dx11>(target);
        (*feature)->initializeSucceeds = nextInitSucceeds;
        nextInitSucceeds = true;
        return true;
    }
    static bool InitializeFeature(IFeature_Dx11* feature, ID3D11Device*, ID3D11DeviceContext*, NVSDK_NGX_Parameter*)
    { return feature->initialized = feature->initializeSucceeds; }
    static void ForgetFeature(IFeature_Dx11*) {}
    static bool ChangeFeature(ID3D11Device*, ID3D11DeviceContext*, unsigned,
        NVSDK_NGX_Parameter*, ContextData<IFeature_Dx11>*, bool);
};
#include "Dx11RebuildProduction.inl"

namespace MenuText
{
bool clicked = false;
bool Button(const char*) { return clicked; }
}
void RegisterCreatedHandle(unsigned handleId)
{
#include "Dx11CreateRegistrationProduction.inl"
}
void ApplyMenu(Upscaler requested, Upscaler currentBackend, bool clicked = true)
{
    auto* config = Config::Instance();
    auto& state = State::Instance();
    state.newBackend = requested;
    MenuText::clicked = clicked;
    (void)currentBackend;
#include "Dx11MenuApplyProduction.inl"
}

int main()
{
    auto& config = *Config::Instance();
    config.OverlayMenu.value = true;
    ComPtr<ID3D11Device> device, other;
    ComPtr<ID3D11DeviceContext> context, otherContext;
    Check(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context)), "WARP source");
    Check(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&other,nullptr,&otherContext)), "WARP other device");
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = desc.Height = desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    ComPtr<ID3D11Texture2D> texture;
    Check(SUCCEEDED(device->CreateTexture2D(&desc,nullptr,&texture)), "WARP output texture");
    auto* output12 = reinterpret_cast<ID3D12Resource*>(uintptr_t{0x1234});
    for (auto evaluation : {FeatureEvaluation::Failed, FeatureEvaluation::NoOutput, FeatureEvaluation::OutputRecorded})
    {
        Parameters parameters;
        Delivery delivery;
        delivery.nextEvaluation = evaluation;
        delivery.Device = device.Get();
        ID3D11Resource* received11 = reinterpret_cast<ID3D11Resource*>(uintptr_t{1});
        const bool success11 = delivery.Dx11(context.Get(), &parameters, texture.Get(), &received11);
        Check(success11 == (evaluation != FeatureEvaluation::Failed), "DX11 legacy success preserves no-output rebuild");
        Check((received11 == texture.Get()) == (evaluation == FeatureEvaluation::OutputRecorded), "DX11 current receipt only on current output");
        Check(delivery.dispatches == (evaluation == FeatureEvaluation::OutputRecorded ? 1u : 0u), "DX11 does not postprocess stale output");
        delivery.dispatches = 0;
        ID3D12Resource* received12 = reinterpret_cast<ID3D12Resource*>(uintptr_t{1});
        const bool success12 = delivery.Dx12(nullptr, &parameters, output12, &received12);
        Check(success12 == (evaluation != FeatureEvaluation::Failed), "DX12 legacy success preserves no-output rebuild");
        Check((received12 == output12) == (evaluation == FeatureEvaluation::OutputRecorded), "DX12 current receipt only on current output");
        Check(delivery.dispatches == (evaluation == FeatureEvaluation::OutputRecorded ? 1u : 0u), "DX12 does not postprocess stale output");
        Check(parameters.output12 == output12, "DX12 restores caller output on every result");
    }
    Parameters parameters;
    Delivery delivery;
    delivery.Device = device.Get();
    ID3D11Resource* received11 = nullptr;
    ID3D12Resource* received12 = nullptr;
    delivery.failPass = true;
    delivery.Dx11(context.Get(), &parameters, texture.Get(), &received11);
    delivery.Dx12(nullptr, &parameters, output12, &received12);
    Check(!received11 && !received12, "failed final pass cannot certify provider intermediate");
    delivery.failPass = false;
    delivery.outputChainComplete = delivery.outputGridPasses = false;
    delivery.Dx11(context.Get(), &parameters, texture.Get(), &received11);
    delivery.Dx12(nullptr, &parameters, output12, &received12);
    Check(!received11 && !received12, "incomplete final chain cannot certify output");
    delivery.outputChainComplete = delivery.outputGridPasses = true;
    delivery.Dx11(otherContext.Get(), &parameters, texture.Get(), &received11);
    Check(!received11, "another device cannot claim an ordered DX11 output");
    ComPtr<ID3D11DeviceContext> deferred;
    Check(SUCCEEDED(device->CreateDeferredContext(0,&deferred)), "deferred context");
    delivery.Dx11(deferred.Get(), &parameters, texture.Get(), &received11);
    Check(!received11, "unsubmitted deferred context cannot claim immediate output");
    parameters.refuseOutput = true;
    parameters.output11 = nullptr; parameters.output12 = nullptr;
    delivery.Dx11(context.Get(), &parameters, texture.Get(), &received11);
    delivery.Dx12(nullptr, &parameters, output12, &received12);
    Check(!received11 && !received12, "parameter restore mismatch cannot publish output");
    parameters.refuseOutput = false;

    using U = Upscaler;
    for (const auto [source, target] : {std::pair{U::XeSS,U::XeSS_on12}, {U::FSR21,U::FSR21_on12},
        {U::FSR22,U::FSR22_on12}, {U::FFX,U::FFX_on12}, {U::DLSS,U::DLSS_on12}})
    {
        Check(BackendSelection::ResolveDx11({},true,source,true,true)==target, "TAA inherits compatible DX12 preference");
        Check(BackendSelection::ResolveDx11({},false,source,true,true)==U::DLSS, "ordinary NGX retains auto policy");
        Check(BackendSelection::ResolveDx11(U::FSR31,true,source,true,true)==U::FSR31, "explicit DX11 preference wins");
    }
    Check(BackendSelection::ResolveDx11({},true,U::DLSS,false,true)==U::FSR22, "unsupported DLSS cannot grant bridge route");
    Check(!config.Dx11Upscaler, "auto starts unset");
    BackendSelection::Commit(API::DX11,U::XeSS_on12,config);
    BackendSelection::Commit(API::DX12,U::FSR22,config);
    BackendSelection::Commit(API::Vulkan,U::FFX,config);
    BackendSelection::Commit(API::DX11,U::Reset,config);
    Check(config.Dx11Upscaler==U::XeSS_on12 && config.Dx12Upscaler==U::FSR22 && config.VulkanUpscaler==U::FFX, "explicit API requests stay separate");
    auto& state = State::Instance();
    state.newBackend = U::XeSS_on12;
    ContextData<IFeature_Dx11> a, b;
    a.feature = std::make_unique<IFeature_Dx11>(U::DLSS);
    b.feature = std::make_unique<IFeature_Dx11>(U::DLSS);
    const auto step = [&](auto& entry, unsigned id) {
        return FeatureProvider_Dx11::ChangeFeature(device.Get(),context.Get(),id,&parameters,&entry,true);
    };
    Check(step(a,1) && step(b,2), "both contexts retire on the same user target");
    Check(step(a,1) && step(a,1), "first context constructs and initializes");
    Check(state.newBackend==U::Reset && !a.rebuildBackend, "completed context clears its target");
    Check(step(b,2) && b.feature->GetUpscalerType()==U::XeSS_on12 && step(b,2), "second context retains target after shared request clears");
    FeatureProvider_Dx11::nextInitSucceeds = false;
    state.newBackend = U::XeSS_on12;
    Check(step(a,1) && step(a,1) && !step(a,1), "failed vendor initialization requests its own fallback");
    Check(a.rebuildBackend==U::FSR22_on12 && config.Dx11Upscaler==U::XeSS_on12, "fallback preserves user's explicit request");
    state.newBackend = U::DLSS;
    Check(step(a,1) && step(a,1) && a.feature->GetUpscalerType()==U::FSR22_on12 && step(a,1), "unrelated global selection cannot replace an in-progress fallback");
    a.rebuildBackend = U::FSR31;
    const auto revision = a.rebuildRequestRevision;
    ContextData<IFeature_Dx11> moved(std::move(a));
    Check(moved.rebuildBackend==U::FSR31 && !a.rebuildBackend && moved.rebuildRequestRevision==revision, "dense context moves retain target and request revision");
    moved.rebuildBackend.reset();
    BackendSelection::Commit(API::DX11,U::FSR22_on12,config);
    state.newBackend = U::FSR22_on12;
    Check(step(moved,3) && step(b,2) && step(moved,3), "two old rebuilds retain their own recipe");
    BackendSelection::Commit(API::DX11,U::FSR31,config);
    state.newBackend = U::FSR31;
    state.changeBackend[3] = state.changeBackend[2] = true;
    Check(!step(moved,3) && state.changeBackend[3] && state.newBackend==U::FSR31, "old initialization cannot consume newer Apply");
    Check(step(b,2) && !step(b,2) && state.changeBackend[2], "second old context also retains newer Apply");
    Check(step(moved,3) && step(moved,3) && step(moved,3), "first context replays new committed request");
    Check(step(b,2) && step(b,2) && step(b,2), "second context replays new request after shared draft clears");
    Check(moved.feature->GetUpscalerType()==U::FSR31 && b.feature->GetUpscalerType()==U::FSR31, "all handles converge on latest Apply");
    ContextData<IFeature_Dx11> queued;
    queued.feature = std::make_unique<IFeature_Dx11>(U::DLSS);
    queued.rebuildBackend = U::FSR22_on12;
    queued.rebuildRequestRevision = BackendSelection::Dx11Revision();
    BackendSelection::Commit(API::DX11,U::XeSS_on12,config);
    state.newBackend = U::XeSS_on12;
    Check(step(queued,4) && step(queued,4) && queued.feature->GetUpscalerType()==U::XeSS_on12 && step(queued,4), "new Apply supersedes fallback before Retire");
    queued.rebuildBackend = U::FSR22_on12;
    queued.rebuildRequestRevision = BackendSelection::Dx11Revision();
    Check(step(queued,4), "queued fallback starts normally");
    BackendSelection::Commit(API::DX11,U::XeSS_on12,config);
    state.newBackend = U::XeSS_on12;
    Check(step(queued,4) && !step(queued,4) && state.changeBackend[4], "reapplying the same preference still supersedes in-flight fallback");
    Check(step(queued,4) && step(queued,4) && step(queued,4) && queued.feature->GetUpscalerType()==U::XeSS_on12, "same-choice Apply retries requested provider");
    state.newBackend = U::DLSS; // uncommitted combo selection
    Check(step(queued,4) && step(queued,4) && queued.feature->GetUpscalerType()==U::XeSS_on12 && step(queued,4), "unapplied menu draft cannot redirect an internal rebuild");
    state.changeBackend.clear();
    ContextData<IFeature_Dx11> early, idle;
    early.feature = std::make_unique<IFeature_Dx11>(U::DLSS);
    idle.feature = std::make_unique<IFeature_Dx11>(U::DLSS);
    RegisterCreatedHandle(101);
    RegisterCreatedHandle(102);
    Check(state.changeBackend.size()==2 && !state.changeBackend[101] && !state.changeBackend[102], "production creation registers both handles before Evaluate");
    const auto beforeClick = BackendSelection::Dx11Revision();
    const auto beforePreference = config.Dx11Upscaler;
    ApplyMenu(U::FSR31,U::DLSS,false);
    Check(config.Dx11Upscaler==beforePreference && BackendSelection::Dx11Revision()==beforeClick && !state.changeBackend[102], "unpressed production button cannot commit a draft");
    ApplyMenu(U::FSR31,U::DLSS);
    Check(config.Dx11Upscaler==U::FSR31 && state.changeBackend[101] && state.changeBackend[102], "production menu Apply reaches an unevaluated handle");
    Check(step(early,101) && step(early,101) && step(early,101) && early.feature->GetUpscalerType()==U::FSR31, "evaluating handle applies the committed request");
    Check(state.changeBackend[102] && step(idle,102) && step(idle,102) && step(idle,102) && idle.feature->GetUpscalerType()==U::FSR31, "first Evaluate of idle handle applies the pending request");
    ApplyMenu(U::FSR22_on12,U::FSR31);
    Check(step(early,101) && step(early,101) && step(early,101), "one handle reaches the next backend while another is still on the previous backend");
    const auto sameActiveRevision = BackendSelection::Dx11Revision();
    ApplyMenu(U::FSR31,U::FSR31);
    Check(config.Dx11Upscaler==U::FSR31 && BackendSelection::Dx11Revision()==sameActiveRevision+1, "production button commits a selection equal to the currently displayed backend");
    Check(step(early,101) && step(early,101) && step(early,101) && step(idle,102) && step(idle,102) && step(idle,102) && early.feature->GetUpscalerType()==U::FSR31 && idle.feature->GetUpscalerType()==U::FSR31, "mixed handles converge after Apply back to the displayed backend");
    const auto beforeReset = BackendSelection::Dx11Revision();
    ApplyMenu(U::Reset,U::FSR31);
    Check(BackendSelection::Dx11Revision()==beforeReset && !state.changeBackend[101] && !state.changeBackend[102], "production button ignores Reset");
    ApplyMenu(U::FSR31,U::FSR31);
    Check(BackendSelection::Dx11Revision()==beforeReset+1 && state.changeBackend[101] && state.changeBackend[102], "production button can reapply the same choice to retry");
    std::printf("PASS %u production delivery/selection/rebuild checks; WARP identity only, vendor/game untested\n", checks);
}

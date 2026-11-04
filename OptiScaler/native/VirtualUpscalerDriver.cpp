#include "pch.h"
#include <native/OpticalFgNrPolicy.h>

#include "VirtualUpscalerDriver.h"

#include "DepthFinderCore.h"

#include <atomic>
#include <NVNGX_Parameter.h>
#include <State.h>
#include <Config.h>
#include <with_dx12/with_dx12.h>
#include <Util.h>
#include <upscalers/FeatureProvider_Dx12.h>
#include <upscalers/IFeature_Dx12.h>
#include <inputs/FG/Upscaler_Inputs_Dx12.h>
#include <framegen/dlssg/FgDepthContract.h>
#include <framegen/dlssg/DLSSG_Dx12.h>
#include <dlssnr/DlssNr.h>

namespace native
{

namespace
{
// The feature the virtual upscaler is running now, for IsVirtualUpscalerFeature (read by the menu thread).
std::atomic<const void*> g_virtualFeature { nullptr };
// CPU source identity only; minted once per successful virtual feature creation.
std::atomic<uint64_t> g_virtualSourceGeneration { 0 };

// Our feature's handle id: never in the real NGX handle tables (HandleToFeature, Dx12Contexts), so no game call finds it. It is
// in State::changeBackend, so the menu's backend switch reaches it like any other feature.
constexpr UINT kVirtualHandleId = 0x4E524631; // 'NRF1'

struct RetainedFeature
{
    std::shared_ptr<IFeature_Dx12> feature;
};

// Snapshot before prefix Execute. It retains exact objects, not another SR
// pipeline or a guessed GPU-completion delay. NativeProducer owns retirement.
struct BeforeSrResources
{
    std::shared_ptr<IFeature_Dx12> feature;
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    Microsoft::WRL::ComPtr<ID3D12Resource> color, input, output, neutral, depth, motion, distrust;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtv;
};

// Legacy delayed release remains; a BeforeSR frame additionally owns the exact COM resource through its final tail.
struct ReleaseLater
{
    explicit ReleaseLater(ID3D12Resource* resource) : resource(resource) {}
    ~ReleaseLater() { resource->Release(); }
    ReleaseLater(const ReleaseLater&) = delete;
    ReleaseLater& operator=(const ReleaseLater&) = delete;
    ID3D12Resource* resource;
};

void ReleaseTexture(ID3D12Resource*& texture)
{
    if (texture == nullptr)
        return;

    Util::DelayedDestroy(std::make_unique<ReleaseLater>(texture));
    texture = nullptr;
}

void Transition(ID3D12GraphicsCommandList* cmd, ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                D3D12_RESOURCE_STATES after)
{
    if (before == after)
        return;

    D3D12_RESOURCE_BARRIER b {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = resource;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    cmd->ResourceBarrier(1, &b);
}

// UAVs cannot be sRGB; the bytes are the same, so the copy in and out keeps the picture exact.
DXGI_FORMAT UavFormat(DXGI_FORMAT view)
{
    switch (view)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8X8_UNORM;
    default:
        return view;
    }
}

bool IsUnrealGame()
{
    const auto& state = State::Instance();
    return state.NVNGX_Engine == NVSDK_NGX_ENGINE_TYPE_UNREAL || state.gameEngine == GameEngineType::Unreal ||
           state.gameQuirks & GameQuirk::ForceUnrealEngine;
}

// The FSR and XeSS backends take each input in the state its *ResourceBarrier setting names (and hand it back in it), and in
// an Unreal game set colour and motion to RENDER_TARGET and UNORDERED_ACCESS on their first Evaluate. Setting those here,
// before UpscaleStart, makes frame generation's copies (which read the same settings) agree with the backend from the first
// frame. DLSS takes everything in its working state.
bool UsesBarrierSettings(Upscaler backend) { return backend != Upscaler::DLSS && backend != Upscaler::DLSSD; }

D3D12_RESOURCE_STATES SettingOr(const CustomOptional<int32_t, NoDefault>& setting, D3D12_RESOURCE_STATES working)
{
    return setting.has_value() ? (D3D12_RESOURCE_STATES) setting.value() : working;
}

Upscaler WantedBackend()
{
    if (OpticalFgNrEnabled()) return Upscaler::Optical;
    const auto& cfg = *Config::Instance();
    const Upscaler wanted = cfg.Dx12Upscaler.has_value() ? cfg.Dx12Upscaler.value() : Upscaler::FFX;

    // Ray Reconstruction needs inputs we do not have; the config stores it as DLSS anyway.
    return wanted == Upscaler::DLSSD ? Upscaler::DLSS : wanted;
}
} // namespace

class OpticalInputOwner final : public IFeature_Dx12
{
    bool InitInternal(ID3D12GraphicsCommandList*, NVSDK_NGX_Parameter*) override { return true; }
    bool EvaluateInternal(ID3D12GraphicsCommandList*, NVSDK_NGX_Parameter*) override { return false; }
public:
    OpticalInputOwner(unsigned int id, NVSDK_NGX_Parameter* params, ID3D12Device* device)
        : IFeature(id, params), IFeature_Dx12(id, params)
    {
        SetInitParameters(params); Device = device; _moduleLoaded = true; SetInit(true);
    }
    bool IsWithDx12() override { return false; }
    feature_version Version() override { return {}; }
    Upscaler GetUpscalerType() const override { return Upscaler::Optical; }
    // Metadata-only owner: no SR Init/Evaluate, hence no SR GPU query/shader objects.
    // Present/menu timing must not enter IFeature_Dx12's resource-owning readers.
    std::optional<double> ReadUpscalerTime(void*) override { return std::nullopt; }
    void ReadDetailedGpuTimes(void*, std::vector<DetailedGpuTime>& times) override { times.clear(); }
    void Advance() { ++_frameCount; }
};

VirtualUpscalerDriver::VirtualUpscalerDriver() = default;

bool IsVirtualUpscalerFeature(const void* feature) { return feature != nullptr && g_virtualFeature.load() == feature; }

VirtualUpscalerDriver::~VirtualUpscalerDriver()
{
    Release();
    ReleaseTexture(_output);
    ReleaseTexture(_input);
    ReleaseTexture(_neutralDepth);

    if (_rtvHeap != nullptr)
        _rtvHeap->Release();

    delete _params;
}

void VirtualUpscalerDriver::DropFeature(bool destroyFgContext)
{
    auto& state = State::Instance();

    // What NVSDK_NGX_D3D12_ReleaseFeature (destroyFgContext) and FeatureProvider_Dx12::ChangeFeature (not) do for a game's
    // feature.
    if (state.currentFG != nullptr && state.activeFgInput == FGInput::Upscaler)
    {
        state.fgChanged = true;
        state.clearCapturedHudlesses = true;

        if (destroyFgContext)
            state.currentFG->DestroyFGContext();
    }

    UpscalerInputsDx12::Reset();

    if (state.currentFeature == _feature.get())
        state.currentFeature = nullptr;

    if (g_virtualFeature.load() == _feature.get())
        g_virtualFeature = nullptr;

    Util::DelayedDestroy(std::make_unique<RetainedFeature>(RetainedFeature{std::move(_feature)}));
}

void VirtualUpscalerDriver::Release()
{
    if (_feature == nullptr)
        return;

    _paused = false;

    LOG_INFO("Virtual upscaler: releasing the {} backend", _backendName);
    DropFeature(true);
    State::Instance().changeBackend.erase(kVirtualHandleId);
    _backendName.clear();
}

void VirtualUpscalerDriver::Pause()
{
    if (_feature == nullptr || _paused)
        return;

    _paused = true;

    auto& state = State::Instance();

    if (state.currentFeature == _feature.get())
        state.currentFeature = nullptr;

    LOG_INFO("Virtual upscaler: paused, the game is calling its own upscaler (the {} backend stays)", _backendName);
}

bool VirtualUpscalerDriver::CreateFeature(ID3D12GraphicsCommandList* cmd, const Key& key, Upscaler backend)
{
    _params->Set(NVSDK_NGX_Parameter_Width, key.width);
    _params->Set(NVSDK_NGX_Parameter_Height, key.height);
    _params->Set(NVSDK_NGX_Parameter_OutWidth, key.width);
    _params->Set(NVSDK_NGX_Parameter_OutHeight, key.height);
    _params->Set(NVSDK_NGX_Parameter_PerfQualityValue, 1);

    // Not jittered and not low-res: the guides are output-sized, unjittered motion. AutoExposure because there is no exposure
    // texture to give: without both, FFXFeatureDx12::EvaluateInternal returns true having dispatched nothing.
    unsigned int flags = NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;

    if (key.depthReversed)
        flags |= NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;

    if (key.hdr)
        flags |= NVSDK_NGX_DLSS_Feature_Flags_IsHDR;

    _params->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, flags);
    if (backend == Upscaler::Optical)
    {
        _feature = std::make_unique<OpticalInputOwner>(kVirtualHandleId, _params, _device);
        g_virtualFeature = _feature.get();
        UpscalerInputsDx12::Init(_device);
        LOG_INFO("Optical FG/NR: persistent final-picture input owner ready; no SR backend created");
        return true;
    }


    // As TryCreateOptiFeature does: the menu and some backends read the API of the feature being run from here.
    State::Instance().api = API::DX12;

    std::unique_ptr<IFeature_Dx12> feature;

    if (!FeatureProvider_Dx12::GetFeature(backend, kVirtualHandleId, _params, &feature) || feature == nullptr)
    {
        _error = std::format("could not create the {} backend", UpscalerDisplayName(backend));
        LOG_WARN("Virtual upscaler: {}", _error);
        return false;
    }

    if (!feature->Init(_device, cmd, _params))
    {
        _error = std::format("the {} backend failed to initialise", feature->Name());
        LOG_WARN("Virtual upscaler: {}", _error);
        Util::DelayedDestroy(std::move(feature));
        return false;
    }

    _feature = std::move(feature);
    g_virtualFeature = _feature.get();
    return true;
}

bool VirtualUpscalerDriver::EnsureFeature(ID3D12GraphicsCommandList* cmd, const Key& key, bool rebuild, bool realDepth)
{
    if (_feature != nullptr && !rebuild)
    {
        const bool flagsOnly = key.width == _featureKey.width && key.height == _featureKey.height &&
                               key.format == _featureKey.format && key.backend == _featureKey.backend;

        // Built before the game's depth came, the direction was a guess: the first real depth corrects it at once
        if (!_settle.Now(_featureKey, key, flagsOnly, !_featureRealDepth && realDepth))
        {
            // A guess that was right: from now on a flip is a real one, and waits
            if (realDepth && key == _featureKey)
                _featureRealDepth = true;

            return true;
        }
    }

    // Tried and failed for exactly this: not again every frame.
    if (_feature == nullptr && !rebuild && _failed && key == _failedKey)
        return false;

    const bool replacing = _feature != nullptr;

    if (replacing)
    {
        LOG_INFO("Virtual upscaler: rebuilding the {} backend", _backendName);
        DropFeature(false);
    }

    const auto backend = (Upscaler) key.backend;

    // Like the real path's fallback when a chosen backend will not initialise; FFX needs nothing from the game.
    if (!CreateFeature(cmd, key, backend) && (backend == Upscaler::FFX || !CreateFeature(cmd, key, Upscaler::FFX)))
    {
        _failed = true;
        _failedKey = key;
        return false;
    }

    // Run/EnsureFeature share the caller's driver lock. Publish a value snapshot
    // to NR; NR never retains or dereferences the driver/feature through it.
    _nrSourceGeneration = g_virtualSourceGeneration.fetch_add(1, std::memory_order_relaxed) + 1;
    _featureRealDepth = realDepth;
    _failed = false;
    auto& state = State::Instance();

    // GetFeature recorded what it built in Dx12Upscaler, which WantedBackend reads: the key matches it from the next frame on.
    _featureKey = key;
    _featureKey.backend = (int) WantedBackend();
    _backendName = _feature->Name();
    _error.clear();

    state.changeBackend[kVirtualHandleId] = false;

    if (replacing && state.currentFG != nullptr && state.activeFgInput == FGInput::Upscaler)
        state.currentFG->UpdateTarget();

    LOG_INFO("Virtual upscaler: {} ready, {}x{}, depth reversed {}, HDR {}", _backendName, key.width, key.height,
             key.depthReversed, key.hdr);
    return true;
}

bool VirtualUpscalerDriver::EnsureTexture(ID3D12Resource*& texture, Key& made, const Key& key,
                                          D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES restState, const char* what)
{
    if (texture != nullptr && made.width == key.width && made.height == key.height && made.format == key.format)
        return true;

    ReleaseTexture(texture);

    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = key.width;
    desc.Height = key.height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = key.format;
    desc.SampleDesc.Count = 1;
    desc.Flags = flags;

    const HRESULT hr = _device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, restState, nullptr,
                                                        IID_PPV_ARGS(&texture));

    if (FAILED(hr))
    {
        texture = nullptr;
        _error = std::format("creating the {}x{} {} texture (format {}) failed: {:X}", key.width, key.height, what,
                             (int) key.format, (UINT) hr);
        LOG_WARN("Virtual upscaler: {}", _error);
        return false;
    }

    made = key;
    return true;
}

ID3D12Resource* VirtualUpscalerDriver::NeutralDepth(ID3D12GraphicsCommandList* cmd, const Key& key)
{
    constexpr auto kRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    Key wanted = key;
    wanted.format = DXGI_FORMAT_R32_FLOAT;
    ID3D12Resource* before = _neutralDepth;

    if (!EnsureTexture(_neutralDepth, _neutralKey, wanted, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                       D3D12_RESOURCE_STATE_RENDER_TARGET, "neutral depth"))
        return nullptr;

    if (_neutralDepth != before)
        _neutralFilled = false;

    if (_neutralFilled && _neutralReversed == key.depthReversed)
        return _neutralDepth;

    if (_rtvHeap == nullptr)
    {
        D3D12_DESCRIPTOR_HEAP_DESC heap {};
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heap.NumDescriptors = 1;

        if (FAILED(_device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&_rtvHeap))))
        {
            _rtvHeap = nullptr;
            _error = "creating the neutral depth's descriptor heap failed";
            return nullptr;
        }
    }

    if (_neutralFilled)
        Transition(cmd, _neutralDepth, kRead, D3D12_RESOURCE_STATE_RENDER_TARGET);

    const auto rtv = _rtvHeap->GetCPUDescriptorHandleForHeapStart();
    _device->CreateRenderTargetView(_neutralDepth, nullptr, rtv);
    const float farDepth[4] = { key.depthReversed ? 0.0f : 1.0f, 0.0f, 0.0f, 0.0f };
    cmd->ClearRenderTargetView(rtv, farDepth, 0, nullptr);
    Transition(cmd, _neutralDepth, D3D12_RESOURCE_STATE_RENDER_TARGET, kRead);

    _neutralFilled = true;
    _neutralReversed = key.depthReversed;
    return _neutralDepth;
}

bool VirtualUpscalerDriver::RunOptical(ID3D12GraphicsCommandList* cmd, const NativeFrame& frame)
{
    auto* queue = WithDx12::GetD3D12CommandQueue();
    if (!queue || !frame.color || !frame.motion || !_feature) return false;
    const auto desc = frame.color->GetDesc();
    const auto space = frame.space == ColorSpace::ScRgb ? DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709 :
                       frame.space == ColorSpace::Pq ? DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 :
                       DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    const bool nrRan = DlssNr::ApplyNativeInput(queue, cmd, frame.color, frame.depth, frame.motion,
        frame.historyDistrust, frame.depthReversed, frame.reset, space, frame.pictureState,
        State::Instance().frameCount, true);
    auto* depth = frame.depth ? frame.depth : NeutralDepth(cmd, _featureKey);
    if (!depth) return false;
    _params->Set(NVSDK_NGX_Parameter_Color, frame.color);
    _params->Set(NVSDK_NGX_Parameter_Output, frame.color);
    _params->Set(NVSDK_NGX_Parameter_Depth, depth);
    _params->Set(NVSDK_NGX_Parameter_MotionVectors, frame.motion);
    _params->Set(NVSDK_NGX_Parameter_Jitter_Offset_X, 0.0f);
    _params->Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, 0.0f);
    _params->Set(NVSDK_NGX_Parameter_MV_Scale_X, 1.0f);
    _params->Set(NVSDK_NGX_Parameter_MV_Scale_Y, 1.0f);
    _params->Set(NVSDK_NGX_Parameter_Reset, frame.reset ? 1u : 0u);
    static_cast<OpticalInputOwner*>(_feature.get())->Advance();
    State::Instance().currentFeature = _feature.get();
    State::Instance().detailedGpuTimes.clear(); // the optical owner has no SR timing rows
    UpscalerInputsDx12::UpscaleStart(cmd, _params, _feature.get());
    UpscalerInputsDx12::UpscaleEnd(cmd, _params, _feature.get());
    if (++_evaluations % 300 == 1)
        LOG_INFO("Optical FG/NR: epoch {}, {}x{}, NR {}, realDepth {}, owner {}; game DLSS independent",
                 State::Instance().frameCount, desc.Width, desc.Height, nrRan, frame.depth != nullptr, kVirtualHandleId);
    return true;
}

bool VirtualUpscalerDriver::BeforeSrInputMatches(ID3D12Resource* color, const Key& key) const
{
    ID3D12Resource* actual = nullptr;
    if (_params->Get(NVSDK_NGX_Parameter_Color, &actual) != NVSDK_NGX_Result_Success || actual != color) return false;
    unsigned width = 0, height = 0;
    if (_params->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, &width) != NVSDK_NGX_Result_Success ||
        _params->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, &height) != NVSDK_NGX_Result_Success)
    {
        // This owner creates a full native-size synthetic table. Prove the
        // same fallback used by IFeature::GetRenderResolution without its
        // diagnostic/logging or jitter-history side effects.
        unsigned outWidth = 0, outHeight = 0;
        if (_params->Get(NVSDK_NGX_Parameter_Width, &width) != NVSDK_NGX_Result_Success ||
            _params->Get(NVSDK_NGX_Parameter_Height, &height) != NVSDK_NGX_Result_Success ||
            _params->Get(NVSDK_NGX_Parameter_OutWidth, &outWidth) != NVSDK_NGX_Result_Success ||
            _params->Get(NVSDK_NGX_Parameter_OutHeight, &outHeight) != NVSDK_NGX_Result_Success ||
            outWidth != key.width || outHeight != key.height) return false;
    }
    if (width != key.width || height != key.height) return false;
    const auto backend = _feature->GetUpscalerType();
    if (backend == Upscaler::DLSS || backend == Upscaler::DLSSD || backend == Upscaler::XeSS)
    {
        unsigned x = 0, y = 0; // absent optional base means origin zero
        _params->Get(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X, &x);
        _params->Get(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y, &y);
        if (x || y) return false;
    }
    return true;
}

bool VirtualUpscalerDriver::Run(ID3D12GraphicsCommandList* cmd, const NativeFrame& frame)
{
    if (cmd == nullptr || frame.color == nullptr || frame.motion == nullptr)
        return false;

    if (_device == nullptr)
    {
        Microsoft::WRL::ComPtr<ID3D12Device> device;

        if (FAILED(cmd->GetDevice(IID_PPV_ARGS(&device))))
        {
            _error = "could not get the D3D12 device from the command list";
            return false;
        }

        _device = device.Get(); // not kept alive by us: lives as long as the game's device
    }

    if (_params == nullptr)
        _params = new NVNGX_Parameters(API::DX12, false);

    const D3D12_RESOURCE_DESC colorDesc = frame.color->GetDesc();

    Key key;
    key.width = (uint32_t) colorDesc.Width;
    key.height = colorDesc.Height;
    key.format = UavFormat(frame.colorFormat != DXGI_FORMAT_UNKNOWN ? frame.colorFormat : colorDesc.Format);
    // A frame with no depth keeps the convention the backend was built with, so it is not rebuilt for it.
    key.depthReversed = frame.depth != nullptr || _feature == nullptr ? frame.depthReversed : _featureKey.depthReversed;
    key.hdr = frame.space == ColorSpace::ScRgb; // PQ is a perceptual encoding, as sRGB is: passed as it comes
    key.backend = (int) WantedBackend();

    auto& state = State::Instance();
    auto& cfg = *Config::Instance();

    // Our own call, for all of it: GetFeature and Evaluate both tell the depth finder a game is calling an upscaler.
    SyntheticUpscalerCallScope guard;

    // A backend switch from the menu, or a backend asking to be rebuilt (FeatureProvider_Dx12::ChangeFeature's job for a game's
    // feature).
    bool rebuild = false;

    if (const auto change = state.changeBackend.find(kVirtualHandleId);
        !OpticalFgNrEnabled() && _feature != nullptr && change != state.changeBackend.end() && change->second)
    {
        if (state.newBackend != Upscaler::Reset)
        {
            key.backend = (int) (state.newBackend == Upscaler::DLSSD ? Upscaler::DLSS : state.newBackend);
            state.newBackend = Upscaler::Reset;
        }

        change->second = false;
        rebuild = true;
    }

    if (!EnsureFeature(cmd, key, rebuild, frame.depth != nullptr))
        return false;

    if (OpticalFgNrEnabled()) return RunOptical(cmd, frame);

    const bool copyIn = colorDesc.Format != key.format;

    if (!EnsureTexture(_output, _outputKey, key, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS, "output"))
        return false;

    if (copyIn && !EnsureTexture(_input, _inputKey, key, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, "input"))
        return false;

    // SR can use its compatibility all-far input. That allocation is never
    // evidence of real scene depth and cannot authorize ZZZ's own FG producer.
    const bool zzzFgProducer=dlssg::depth_contract::Game()==GameRouting::Identity::ZenlessZoneZero &&
        state.activeFgInput==FGInput::Upscaler && state.activeFgOutput==FGOutput::DLSSG;
    // NR continues receiving the original depth (including null) as before.
    ID3D12Resource* depth = frame.depth != nullptr ? frame.depth : NeutralDepth(cmd, key);

    if (depth == nullptr)
        return false;

    // Frame generation's Upscaler input only has a device once NVSDK_NGX_D3D12_Init gave it one, which a game with no upscaler
    // never calls.
    UpscalerInputsDx12::Init(_device);

    const auto backend = _feature->GetUpscalerType();
    const bool usesSettings = UsesBarrierSettings(backend);

    // The picture is a render target in a D3D12 Unreal game; a D3D11 game's shared picture is not (no
    // ALLOW_RENDER_TARGET), and moving it to that state makes the list invalid.
    if (usesSettings && IsUnrealGame())
    {
        // Set either way: the FSR backends add the render-target state themselves for an Unreal game when it is unset.
        if (!cfg.ColorResourceBarrier.has_value())
            cfg.ColorResourceBarrier.set_volatile_value((colorDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) != 0
                                                            ? D3D12_RESOURCE_STATE_RENDER_TARGET
                                                            : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        if (!cfg.MVResourceBarrier.has_value())
            cfg.MVResourceBarrier.set_volatile_value(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    constexpr auto kRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    constexpr auto kWrite = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    // Frame generation's copies (UpscaleStart/UpscaleEnd) read the settings whatever the backend.
    const auto fgMotion = SettingOr(cfg.MVResourceBarrier, kRead);
    const auto fgDepth = SettingOr(cfg.DepthResourceBarrier, kRead);
    const auto fgOutput = SettingOr(cfg.OutputResourceBarrier, kWrite);

    const auto inColor = usesSettings ? SettingOr(cfg.ColorResourceBarrier, kRead) : kRead;
    const auto inMotion = usesSettings ? fgMotion : kRead;
    const auto inDepth = usesSettings ? fgDepth : kRead;
    const auto inOutput = usesSettings ? fgOutput : kWrite;

    // The colour the backend reads: the picture itself, or a typed copy of it. The picture stays in COPY_SOURCE until the
    // result is copied back over it.
    ID3D12Resource* color = frame.color;
    D3D12_RESOURCE_STATES pictureNow = frame.pictureState;

    if (copyIn)
    {
        Transition(cmd, frame.color, pictureNow, D3D12_RESOURCE_STATE_COPY_SOURCE);
        pictureNow = D3D12_RESOURCE_STATE_COPY_SOURCE;
        cmd->CopyResource(_input, frame.color);
        Transition(cmd, _input, D3D12_RESOURCE_STATE_COPY_DEST, inColor);
        color = _input;
    }
    else
    {
        Transition(cmd, frame.color, pictureNow, inColor);
        pictureNow = inColor;
    }

    _params->Set(NVSDK_NGX_Parameter_Color, color);
    _params->Set(NVSDK_NGX_Parameter_Depth, frame.depth);
    _params->Set(NVSDK_NGX_Parameter_MotionVectors, frame.motion);
    _params->Set(NVSDK_NGX_Parameter_Output, _output);
    _params->Set(NVSDK_NGX_Parameter_Jitter_Offset_X, 0.0f);
    _params->Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, 0.0f);
    _params->Set(NVSDK_NGX_Parameter_MV_Scale_X, 1.0f);
    _params->Set(NVSDK_NGX_Parameter_MV_Scale_Y, 1.0f);
    _params->Set(NVSDK_NGX_Parameter_Reset, frame.reset ? 1u : 0u);
    _params->Set(NVSDK_NGX_Parameter_Sharpness, 0.0f);
    _params->Set(NVSDK_NGX_Parameter_DLSS_Pre_Exposure, 1.0f);

    // From here the resources are where a game's would be at its call: in the states the backend takes them in.
    Transition(cmd, frame.motion, kRead, inMotion);
    Transition(cmd, depth, kRead, inDepth);
    Transition(cmd, _output, kWrite, inOutput);

    bool beforeSrFiltered = false;
    if (frame.beforeSr_ && BeforeSrInputMatches(color, key))
    {
        auto resources = std::make_shared<BeforeSrResources>();
        resources->feature = _feature; resources->device = _device;
        resources->color = color; resources->input = _input; resources->output = _output;
        resources->neutral = _neutralDepth; resources->depth = frame.depth;
        resources->motion = frame.motion; resources->distrust = frame.historyDistrust; resources->rtv = _rtvHeap;
        // cmd is replaced only by the real NativeProducer owner after it has
        // executed/signalled this exact typed-copy/input-preparation prefix,
        // run the one core's filter and reset its owned continuation list.
        const auto filtered = frame.beforeSr_(cmd, color, key.format, inColor, resources);
        if (!filtered.continued) return false;
        beforeSrFiltered = filtered.filtered;
    }

    // DLSS-NR hangs off NVSDK_NGX_D3D12_EvaluateFeature around the upscaler, not off the feature: before it (Run before SR,
    // or the finished-picture capture of depth and motion) and after it (over Output).
    DlssNr::EvaluateBeforeUpscale(cmd, _params, nullptr, 0, false, { 0, this, _nrSourceGeneration });
    _params->Set(NVSDK_NGX_Parameter_Depth, depth);

    Transition(cmd, frame.motion, inMotion, fgMotion);
    Transition(cmd, depth, inDepth, fgDepth);
    Transition(cmd, _output, inOutput, fgOutput);

    // Back from a pause (the game called its own upscaler for a moment): frame generation's input changed, as when a
    // game switches features, which it takes as a short pause, not a new context
    if (_paused)
    {
        _paused = false;

        if (state.currentFG != nullptr && state.activeFgInput == FGInput::Upscaler)
        {
            state.fgChanged = true;
            state.clearCapturedHudlesses = true;
        }

        LOG_INFO("Virtual upscaler: resumed with the {} backend", _backendName);
    }

    state.currentFeature = _feature.get();

    if (!zzzFgProducer)
    {
        UpscalerInputsDx12::UpscaleStart(cmd, _params, _feature.get());
        UpscalerInputsDx12::UpscaleEnd(cmd, _params, _feature.get());
    }

    Transition(cmd, frame.motion, fgMotion, inMotion);
    Transition(cmd, depth, fgDepth, inDepth);
    Transition(cmd, _output, fgOutput, inOutput);

    bool evaluated = false;
    {
        ScopedSkipHeapCapture skip {};
        // NR and FG callbacks above must not silently redirect the actual SR
        // away from the exact filtered resource/region. The default route
        // incurs no additional parameter getter calls.
        if (!beforeSrFiltered || BeforeSrInputMatches(color, key))
            evaluated = _feature->Evaluate(cmd, _params);
        else
        {
            _error = "BeforeSR consumer color or region changed after the owned filter";
            LOG_WARN("Virtual upscaler: {}; selected SR skipped", _error);
        }
    }

    _params->Set(NVSDK_NGX_Parameter_Depth, frame.depth);

    if (evaluated)
        DlssNr::EvaluateAfterUpscale(cmd, _params);

    // Only ZZZ's own selected-SR producer changes: publish this actual source
    // after successful SR, with its real depth. The neutral compatibility input
    // stays confined to SR. Original Genshin/StarRail/optical routes are separate.
    if (zzzFgProducer && (!evaluated || !frame.depth))
    {
        if (auto* own=dynamic_cast<DLSSG_Dx12*>(state.currentFG))
            own->RejectUpscalerGuideSource(!evaluated?"zzz-selected-sr-failed":"zzz-no-real-source-depth");
    }
    if (zzzFgProducer && evaluated && frame.depth)
    {
        Transition(cmd,frame.motion,inMotion,fgMotion);
        Transition(cmd,frame.depth,inDepth,fgDepth);
        Transition(cmd,_output,inOutput,fgOutput);
        UpscalerInputsDx12::UpscaleStart(cmd,_params,_feature.get());
        UpscalerInputsDx12::UpscaleEnd(cmd,_params,_feature.get());
        Transition(cmd,frame.motion,fgMotion,inMotion);
        Transition(cmd,frame.depth,fgDepth,inDepth);
        Transition(cmd,_output,fgOutput,inOutput);
    }

    if (_evaluations < 8 || _evaluations % 300 == 0)
        LOG_INFO("Virtual upscaler: Evaluate {} -> {}, {} {}x{}, colour format {}{}, reset {}, depth reversed {}",
                 _evaluations, evaluated, _backendName, key.width, key.height, (int) key.format,
                 copyIn ? " (copied)" : "", frame.reset, _featureKey.depthReversed);

    ++_evaluations;

    // The backend hands its inputs back in the states it took them in.
    Transition(cmd, frame.motion, inMotion, kRead);
    Transition(cmd, depth, inDepth, kRead);

    if (copyIn)
        Transition(cmd, _input, inColor, D3D12_RESOURCE_STATE_COPY_DEST);

    if (evaluated)
    {
        Transition(cmd, _output, inOutput, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Transition(cmd, frame.color, pictureNow, D3D12_RESOURCE_STATE_COPY_DEST);
        cmd->CopyResource(frame.color, _output);
        Transition(cmd, frame.color, D3D12_RESOURCE_STATE_COPY_DEST, frame.pictureState);
        Transition(cmd, _output, D3D12_RESOURCE_STATE_COPY_SOURCE, kWrite);
    }
    else
    {
        Transition(cmd, frame.color, pictureNow, frame.pictureState);
        Transition(cmd, _output, inOutput, kWrite);
    }

    return evaluated;
}

} // namespace native

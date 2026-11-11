// SPDX-License-Identifier: GPL-3.0-or-later
// Production HLSL on WARP: CPU code supplies inputs and production handoff plans,
// never a replacement reprojection, gain, or composition implementation.
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#pragma warning(push)
#pragma warning(disable : 4324) // Production constants occupy a 256-byte CB slot.
#include <shaders/dlssnr/DlssNr_DetailReuseConstants.h>
#pragma warning(pop)
#include <dlssnr/DlssNrTemporalHandoff.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
using Microsoft::WRL::ComPtr;
using Constants = DlssNrDetailReuseConstants;
using Handoff = DlssNrTemporalDetail::Handoff;
using Clock = std::chrono::steady_clock;
constexpr UINT kWidth = 16, kHeight = 8;
constexpr UINT kProbeX = 8, kProbeY = 4;
constexpr float kEpsilon = 1e-5f;

struct Pixel
{
    float r = 0, g = 0, b = 0, a = 0;
};
static_assert(sizeof(Pixel) == 4 * sizeof(float));

struct Image
{
    UINT width, height;
    std::vector<Pixel> pixels;

    Image(Pixel value = {}, UINT w = kWidth, UINT h = kHeight)
        : width(w), height(h), pixels(static_cast<size_t>(w) * h, value)
    {
    }

    Pixel& At(UINT x, UINT y) { return pixels.at(static_cast<size_t>(y) * width + x); }
    const Pixel& At(UINT x, UINT y) const { return pixels.at(static_cast<size_t>(y) * width + x); }
    const Pixel& Probe() const { return At(kProbeX, kProbeY); }
};
using Inputs = std::array<Image, 6>;

bool Near(float a, float b)
{
    return std::isfinite(a) && std::isfinite(b) && std::abs(a - b) <= kEpsilon;
}

bool Near(Pixel a, Pixel b)
{
    return Near(a.r, b.r) && Near(a.g, b.g) && Near(a.b, b.b) && Near(a.a, b.a);
}

class Checks
{
public:
    void Expect(bool ok, const std::string& name)
    {
        ++count;
        if (!ok)
        {
            ++failures;
            std::cout << "FAIL: " << name << std::endl;
        }
    }

    void PixelEquals(Pixel actual, Pixel expected, const std::string& name)
    {
        Expect(Near(actual, expected), name);
        if (!Near(actual, expected))
            std::cout << "  actual=" << actual.r << ',' << actual.g << ',' << actual.b << ',' << actual.a
                      << " expected=" << expected.r << ',' << expected.g << ',' << expected.b << ',' << expected.a
                      << std::endl;
    }

    void ImageEquals(const Image& actual, const Image& expected, const std::string& name)
    {
        if (actual.width != expected.width || actual.height != expected.height)
        {
            Expect(false, name + ": dimensions");
            return;
        }
        for (UINT y = 0; y < actual.height; ++y)
            for (UINT x = 0; x < actual.width; ++x)
                if (!Near(actual.At(x, y), expected.At(x, y)))
                {
                    PixelEquals(actual.At(x, y), expected.At(x, y),
                                name + " at " + std::to_string(x) + "," + std::to_string(y));
                    return;
                }
        Expect(true, name);
    }

    int Finish() const
    {
        std::cout << "NR history production shader WARP: " << count << " checks, " << failures
                  << " failures" << std::endl;
        return failures ? 1 : 0;
    }

private:
    unsigned count = 0, failures = 0;
};

void RequireHr(HRESULT result, const char* operation)
{
    if (FAILED(result))
    {
        std::ostringstream message;
        message << operation << " failed: HRESULT 0x" << std::hex << static_cast<unsigned long>(result);
        throw std::runtime_error(message.str());
    }
}

struct DispatchResult
{
    Image primary;
    Image colourDepth;
};

class Fixture
{
public:
    Fixture()
    {
        D3D_FEATURE_LEVEL level {};
        RequireHr(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                                   D3D11_SDK_VERSION, &device, &level, &context), "Create WARP device");
        if (level < D3D_FEATURE_LEVEL_11_0)
            throw std::runtime_error("WARP feature level 11.0 is required");

        // The runner compiles an absolute source filename. Derive the production
        // shader from it so running the executable does not depend on cwd.
        const auto shaderPath = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() /
                                "shaders/dlssnr/precompile/dlssnr_detail_reuse.hlsl";
        std::cout << "Compiling production HLSL once: " << shaderPath << std::endl;
        ComPtr<ID3DBlob> code, errors;
        const auto result = D3DCompileFromFile(shaderPath.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
                                             "CSMain", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
        if (errors)
            std::cerr.write(static_cast<const char*>(errors->GetBufferPointer()),
                            static_cast<std::streamsize>(errors->GetBufferSize()));
        RequireHr(result, "Compile production HLSL");
        RequireHr(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader),
                  "Create production compute shader");

        D3D11_SAMPLER_DESC desc {};
        desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        desc.AddressU = desc.AddressV = desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        desc.MaxLOD = D3D11_FLOAT32_MAX;
        RequireHr(device->CreateSamplerState(&desc, &sampler), "Create linear clamp sampler");
    }

    DispatchResult Run(const Inputs& inputs, const Constants& constants)
    {
        std::array<ComPtr<ID3D11Texture2D>, 6> textures;
        std::array<ComPtr<ID3D11ShaderResourceView>, 6> views;
        std::array<ID3D11ShaderResourceView*, 6> rawViews {};
        for (size_t i = 0; i < inputs.size(); ++i)
        {
            const auto& input = inputs[i];
            auto desc = TextureDesc(input.width, input.height);
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA data {};
            data.pSysMem = input.pixels.data();
            data.SysMemPitch = input.width * static_cast<UINT>(sizeof(Pixel));
            RequireHr(device->CreateTexture2D(&desc, &data, &textures[i]), "Upload input texture");
            RequireHr(device->CreateShaderResourceView(textures[i].Get(), nullptr, &views[i]), "Create input SRV");
            rawViews[i] = views[i].Get();
        }

        auto desc = TextureDesc(constants.WorkWidth, constants.WorkHeight);
        desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        std::array<ComPtr<ID3D11Texture2D>, 2> outputs;
        std::array<ComPtr<ID3D11UnorderedAccessView>, 2> uavs;
        std::array<ID3D11UnorderedAccessView*, 2> rawUavs {};
        const float clear[4] {};
        for (size_t i = 0; i < outputs.size(); ++i)
        {
            RequireHr(device->CreateTexture2D(&desc, nullptr, &outputs[i]), "Create output texture");
            RequireHr(device->CreateUnorderedAccessView(outputs[i].Get(), nullptr, &uavs[i]), "Create output UAV");
            context->ClearUnorderedAccessViewFloat(uavs[i].Get(), clear);
            rawUavs[i] = uavs[i].Get();
        }

        D3D11_BUFFER_DESC bufferDesc {};
        bufferDesc.ByteWidth = static_cast<UINT>(sizeof(constants));
        bufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA bufferData {};
        bufferData.pSysMem = &constants;
        ComPtr<ID3D11Buffer> buffer;
        RequireHr(device->CreateBuffer(&bufferDesc, &bufferData, &buffer), "Create constants buffer");
        auto* rawBuffer = buffer.Get();
        auto* rawSampler = sampler.Get();
        context->CSSetShader(shader.Get(), nullptr, 0);
        context->CSSetConstantBuffers(0, 1, &rawBuffer);
        context->CSSetSamplers(0, 1, &rawSampler);
        context->CSSetShaderResources(0, static_cast<UINT>(rawViews.size()), rawViews.data());
        context->CSSetUnorderedAccessViews(0, static_cast<UINT>(rawUavs.size()), rawUavs.data(), nullptr);
        context->Dispatch((constants.WorkWidth + 7) / 8, (constants.WorkHeight + 7) / 8, 1);
        context->ClearState();

        DispatchResult result;
        result.primary = Readback(outputs[0].Get());
        if (constants.Mode == DlssNrDetailReuse_HandoffCompose)
            result.colourDepth = Readback(outputs[1].Get());
        return result;
    }

private:
    static D3D11_TEXTURE2D_DESC TextureDesc(UINT width, UINT height)
    {
        D3D11_TEXTURE2D_DESC desc {};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        desc.SampleDesc.Count = 1;
        return desc;
    }

    Image Readback(ID3D11Texture2D* output)
    {
        D3D11_TEXTURE2D_DESC desc {};
        output->GetDesc(&desc);
        desc.BindFlags = 0;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        RequireHr(device->CreateTexture2D(&desc, nullptr, &staging), "Create readback texture");
        context->CopyResource(staging.Get(), output);
        D3D11_MAPPED_SUBRESOURCE mapped {};
        // Blocking Map observes real WARP output rather than a CPU reference.
        RequireHr(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Map WARP output");
        Image result({}, desc.Width, desc.Height);
        for (UINT y = 0; y < desc.Height; ++y)
            std::memcpy(result.pixels.data() + static_cast<size_t>(y) * desc.Width,
                        static_cast<const char*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch,
                        static_cast<size_t>(desc.Width) * sizeof(Pixel));
        context->Unmap(staging.Get(), 0);
        return result;
    }

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11ComputeShader> shader;
    ComPtr<ID3D11SamplerState> sampler;
};

Constants EstimateConstants(bool lightEdit = false)
{
    Constants c {};
    c.Mode = DlssNrDetailReuse_Estimate;
    c.WorkWidth = c.MotionWidth = c.DepthWidth = kWidth;
    c.WorkHeight = c.MotionHeight = c.DepthHeight = kHeight;
    c.DepthInverted = 1;
    c.MvScaleX = c.MvScaleY = 1;
    c.DepthTolerance = 0.05f;
    c.ClipGamma = kDlssNrDetailReuseClipGamma;
    c.ClipFalloff = kDlssNrDetailReuseClipFalloff;
    c.SigmaFloor = kDlssNrDetailReuseSigmaFloor;
    c.TemporalMode = 1;
    c.MinimumColourTrust = 0.1f;
    c.TemporalHandoff = lightEdit ? 1u : 0u;
    c.TemporalContent = lightEdit ? 1u : 0u;
    // Disable motion rejection only in isolated candidate/depth tests.
    return c;
}

Inputs EstimateInputs()
{
    return {Image({0.5f, 0.5f, 0.5f, 1}), Image({0.2f, 0.2f, 0.2f, 1}),
            Image({0.5f, 0.5f, 0.5f, 0.8f}), Image(), Image({0.8f, 0, 0, 0}), Image()};
}

void SetMotion(Image& motion, float x, float y)
{
    for (auto& pixel : motion.pixels) pixel = {x, y, 0, 0};
}

struct Direction
{
    const char* name;
    int x, y;
};
constexpr std::array<Direction, 4> kDirections {{{"+X", 1, 0}, {"-X", -1, 0}, {"+Y", 0, 1}, {"-Y", 0, -1}}};

Pixel Target(const Image& image, Direction direction)
{
    return image.At(static_cast<UINT>(static_cast<int>(kProbeX) + direction.x),
                    static_cast<UINT>(static_cast<int>(kProbeY) + direction.y));
}

void TestSurfaceEstimates(Fixture& gpu, Checks& checks)
{
    for (bool lightEdit : {false, true})
    {
        const std::string prefix = lightEdit ? "light-edit estimate: " : "residual estimate: ";
        auto c = EstimateConstants(lightEdit);
        auto input = EstimateInputs();
        input[4].At(kProbeX, kProbeY).r = 0.2f;
        input[3].At(kProbeX, kProbeY).r = 2;
        input[3].At(kProbeX - 1, kProbeY).r = -2;
        checks.Expect(gpu.Run(input, c).primary.Probe().a == 0, prefix + "background cannot borrow foreground depth");
        if (!lightEdit)
        {
            auto legacy = c;
            legacy.TemporalMode = 0;
            checks.Expect(gpu.Run(input, legacy).primary.Probe().a > 0.99f, "legacy foreground donor unchanged");
        }

        input = EstimateInputs();
        SetMotion(input[3], 0.5f, 0);
        input[2].At(kProbeX + 1, kProbeY).a = 0.2f;
        checks.Expect(gpu.Run(input, c).primary.Probe().a == 0, prefix + "mixed bilinear surfaces rejected");
        c.DepthInverted = 0;
        for (auto& depth : input[4].pixels) depth.r = 1 - depth.r;
        checks.Expect(gpu.Run(input, c).primary.Probe().a == 0, prefix + "standard Z mixed surfaces rejected");

        c = EstimateConstants(lightEdit);
        input = EstimateInputs();
        SetMotion(input[3], 0.5f, 0);
        checks.PixelEquals(gpu.Run(input, c).primary.Probe(), {0.2f, 0.2f, 0.2f, 1},
                           prefix + "same plane subpixel detail survives");
        SetMotion(input[3], 0, 0);
        input[2].At(kProbeX + 1, kProbeY).a = 0.2f;
        checks.Expect(gpu.Run(input, c).primary.Probe().a > 0.99f, prefix + "zero-weight surface ignored");

        input = EstimateInputs();
        SetMotion(input[3], 0.5f, 0);
        input[4] = Image({0.5f, 0, 0, 0});
        input[2].At(kProbeX, kProbeY).a = 0.2f;
        checks.Expect(gpu.Run(input, c).primary.Probe().a == 0, prefix + "0.5 inside [0.2,0.8] is not a surface");

        input = EstimateInputs();
        SetMotion(input[3], 1.0f / 32, 1.0f / 64);
        input[2].At(kProbeX + 1, kProbeY + 1).a = 0.2f;
        // Both axis fractions are representable by the texture filter. Their
        // nonzero diagonal weight 1/2048 falls below the old 1e-3 cutoff.
        checks.Expect(gpu.Run(input, c).primary.Probe().a == 0, prefix + "small nonzero diagonal weight rejected");

        for (const auto direction : kDirections)
        {
            input = EstimateInputs();
            input[1] = Image({0, 0, 0, 1});
            const UINT x = static_cast<UINT>(static_cast<int>(kProbeX) + direction.x);
            const UINT y = static_cast<UINT>(static_cast<int>(kProbeY) + direction.y);
            input[1].At(x, y) = {0.3f, 0.4f, 0.5f, 1};
            SetMotion(input[3], static_cast<float>(direction.x), static_cast<float>(direction.y));
            checks.PixelEquals(gpu.Run(input, c).primary.Probe(), Target(input[1], direction),
                               prefix + direction.name + " moves actual saved pixels");
        }

        input = EstimateInputs();
        input[4].At(kProbeX - 1, kProbeY).r = 0.82f;
        input[3].At(kProbeX - 1, kProbeY).r = -1;
        input[1].At(kProbeX - 1, kProbeY) = {0.4f, 0.4f, 0.4f, 1};
        checks.PixelEquals(gpu.Run(input, c).primary.Probe(), input[1].Probe(), prefix + "native tie retains own motion");
        if (!lightEdit)
        {
            auto legacy = c;
            legacy.TemporalMode = 0;
            checks.PixelEquals(gpu.Run(input, legacy).primary.Probe(), input[1].At(kProbeX - 1, kProbeY),
                               "legacy tie still prefers near motion");
        }

        input = EstimateInputs();
        input[4].At(kProbeX - 1, kProbeY).r = 0.83f;
        input[3].At(kProbeX, kProbeY).r = 2;
        input[3].At(kProbeX - 1, kProbeY).r = -2;
        input[2].At(kProbeX + 2, kProbeY).a = 0.2f;
        input[2].At(kProbeX - 2, kProbeY).a = 0.86f;
        checks.Expect(gpu.Run(input, c).primary.Probe().a == 0,
                      prefix + "compatible donor still validates history against recipient depth");
    }
}

#pragma warning(push)
#pragma warning(disable : 4324) // Fixture embeds the production aligned constant buffer.
struct Frame
{
    Constants constants = EstimateConstants(true);
    Image input {Pixel{0.5f, 0.5f, 0.5f, 1}};
    Image fresh {Pixel{0.55f, 0.55f, 0.55f, 0.73f}};
    Image motion;
    Image depth {Pixel{0.8f, 0, 0, 0}};

    Frame()
    {
        constants.MotionReject = kDlssNrDetailReuseMotionReject;
        // Fixed legitimate strength isolates shader behavior; cohort deadlines
        // come exclusively from the production Handoff policy below.
        constants.Steady = constants.TemporalHistoryWeight = 0.75f;
    }
};
#pragma warning(pop)

struct History
{
    std::array<Image, 2> cohort;
    Image colourDepth;
};

struct FeedbackResult
{
    History history;
    std::array<Image, 2> estimate;
    Image shown;
};

Constants PlannedConstants(const Frame& frame, const Handoff::Plan& plan)
{
    auto c = frame.constants;
    c.TemporalHandoffResetMask = plan.resetMask;
    c.TemporalHandoffIncoming = plan.incoming;
    c.TemporalHandoffBlend = plan.blend;
    return c;
}

Image Capture(Fixture& gpu, const Frame& frame, const Image& estimate, const Image& previousInput,
              const Handoff::Plan& plan, bool reset)
{
    auto c = PlannedConstants(frame, plan);
    c.Mode = DlssNrDetailReuse_HandoffCapture;
    c.TemporalFrameKind = reset ? 2u : 0u;
    return gpu.Run({frame.input, frame.fresh, estimate, frame.motion, frame.depth, previousInput}, c).primary;
}

DispatchResult Compose(Fixture& gpu, const Frame& frame, const History& history, const Handoff::Plan& plan)
{
    auto c = PlannedConstants(frame, plan);
    c.Mode = DlssNrDetailReuse_HandoffCompose;
    return gpu.Run({frame.input, history.cohort[0], history.cohort[1], frame.fresh, frame.depth, Image()}, c);
}

FeedbackResult RunFeedback(Fixture& gpu, const Frame& frame, const History& previous, const Handoff::Plan& plan)
{
    // AfterModelHandoff bindings/order: estimates read the common previous raw
    // input/depth; captures read the SAME genuine fresh answer independently.
    // Displayed composition is never used as a cohort's input or fresh answer.
    FeedbackResult result;
    for (unsigned slot = 0; slot < 2; ++slot)
    {
        const bool reset = (plan.resetMask & (1u << slot)) != 0;
        if (!reset)
        {
            auto c = PlannedConstants(frame, plan);
            c.Mode = DlssNrDetailReuse_Estimate;
            result.estimate[slot] = gpu.Run({frame.input, previous.cohort[slot], previous.colourDepth,
                                              frame.motion, frame.depth, Image()}, c).primary;
        }
        result.history.cohort[slot] = Capture(gpu, frame, result.estimate[slot], previous.colourDepth, plan, reset);
    }
    const auto composed = Compose(gpu, frame, result.history, plan);
    result.shown = composed.primary;
    result.history.colourDepth = composed.colourDepth;
    return result;
}

Handoff::Plan ContinuingPlan()
{
    Handoff policy;
    const auto epoch = Clock::time_point{} + std::chrono::seconds(1);
    policy.Next(epoch, 1000, false);
    policy.Captured(epoch);
    return policy.Next(epoch + std::chrono::milliseconds(100), 1000, true);
}

History Seed(Fixture& gpu, const Frame& frame)
{
    Handoff policy;
    const auto epoch = Clock::time_point{} + std::chrono::seconds(1);
    return RunFeedback(gpu, frame, History(), policy.Next(epoch, 1000, false)).history;
}

void ExpectContribution(Checks& checks, Pixel shown, Pixel fresh, const std::string& name)
{
    checks.Expect(std::isfinite(shown.r) && std::isfinite(shown.g) && std::isfinite(shown.b) &&
                      shown.r > fresh.r + 1e-4f && shown.g > fresh.g + 1e-4f && shown.b > fresh.b + 1e-4f &&
                      shown.r < 0.59f && shown.g < 0.59f && shown.b < 0.59f && Near(shown.a, fresh.a), name);
}

void CheckRejectedFeedback(Fixture& gpu, Checks& checks, const Frame& frame, const History& previous,
                           const std::string& name)
{
    const auto plan = ContinuingPlan();
    const auto result = RunFeedback(gpu, frame, previous, plan);
    const auto freshLatent = Capture(gpu, frame, Image(), previous.colourDepth, plan, true);
    for (unsigned slot = 0; slot < 2; ++slot)
    {
        const auto label = name + " cohort " + std::to_string(slot);
        checks.Expect(result.estimate[slot].Probe().a == 0, label + " rejects historical surface");
        checks.PixelEquals(result.history.cohort[slot].Probe(), freshLatent.Probe(), label + " captures genuine fresh edit");
    }
    checks.PixelEquals(result.shown.Probe(), frame.fresh.Probe(), name + " preserves fresh NR RGB and alpha");
}

void TestFullFeedback(Fixture& gpu, Checks& checks)
{
    Frame old;
    old.fresh = Image({0.56f, 0.56f, 0.56f, 0.73f});
    const auto seed = Seed(gpu, old);
    Frame current;
    const auto positive = RunFeedback(gpu, current, seed, ContinuingPlan());
    checks.Expect(positive.estimate[0].Probe().a > 0.99f && positive.estimate[1].Probe().a > 0.99f,
                  "full light-edit feedback retains valid same-surface estimates");
    ExpectContribution(checks, positive.shown.Probe(), current.fresh.Probe(), "full light-edit feedback changes real displayed pixels");
    checks.PixelEquals(positive.history.colourDepth.Probe(), {0.5f, 0.5f, 0.5f, 0.8f},
                       "compose saves raw input/depth, not fresh or displayed answer");

    auto previous = seed;
    SetMotion(current.motion, 0.5f, 0);
    previous.colourDepth.At(kProbeX + 1, kProbeY).a = 0.2f;
    CheckRejectedFeedback(gpu, checks, current, previous, "mixed bilinear footprint");
    current.constants.DepthInverted = 0;
    current.depth = Image({0.2f, 0, 0, 0});
    CheckRejectedFeedback(gpu, checks, current, previous, "standard Z mixed footprint");

    current = Frame();
    current.depth = Image({0.5f, 0, 0, 0});
    SetMotion(current.motion, 0.5f, 0);
    previous = seed;
    for (auto& p : previous.colourDepth.pixels) p.a = 0.5f;
    previous.colourDepth.At(kProbeX, kProbeY).a = 0.2f;
    previous.colourDepth.At(kProbeX + 1, kProbeY).a = 0.8f;
    CheckRejectedFeedback(gpu, checks, current, previous, "0.5 inside mixed [0.2,0.8] is not a surface");

    current = Frame();
    SetMotion(current.motion, 1.0f / 32, 1.0f / 64);
    previous = seed;
    previous.colourDepth.At(kProbeX + 1, kProbeY + 1).a = 0.2f;
    CheckRejectedFeedback(gpu, checks, current, previous, "nonzero 1/2048 diagonal weight");

    current = Frame();
    current.constants.DepthWidth = current.constants.DepthHeight = 0;
    current.depth = current.input; // Production null-guide descriptor stand-in.
    CheckRejectedFeedback(gpu, checks, current, seed, "missing current depth");
    const auto noDepth = RunFeedback(gpu, current, seed, ContinuingPlan());
    checks.Expect(noDepth.history.colourDepth.Probe().a == -1, "missing depth remains missing in saved guides");

    current = Frame();
    previous = seed;
    previous.colourDepth.At(kProbeX, kProbeY).a = -1;
    CheckRejectedFeedback(gpu, checks, current, previous, "missing saved depth");
    const float nan = std::numeric_limits<float>::quiet_NaN();
    previous = seed;
    previous.colourDepth.At(kProbeX, kProbeY).a = nan;
    CheckRejectedFeedback(gpu, checks, current, previous, "NaN saved depth");
    current.depth.At(kProbeX, kProbeY).r = nan;
    CheckRejectedFeedback(gpu, checks, current, seed, "NaN current depth");
}

void TestFloorDirectionsAndGuideRegions(Fixture& gpu, Checks& checks)
{
    Frame floor;
    for (UINT y = 0; y < kHeight; ++y)
        for (UINT x = 0; x < kWidth; ++x)
        {
            const float value = 0.45f + static_cast<float>(x) * 0.004f + static_cast<float>(y) * 0.003f;
            floor.input.At(x, y) = {value, value, value, 1};
            floor.fresh.At(x, y) = {value + 0.03f, value + 0.03f, value + 0.03f, 0.73f};
        }
    const auto previous = Seed(gpu, floor);
    FeedbackResult positiveX;
    Frame positiveXFrame;
    for (const auto direction : kDirections)
    {
        Frame now;
        SetMotion(now.motion, static_cast<float>(direction.x), static_cast<float>(direction.y));
        for (UINT y = 0; y < kHeight; ++y)
            for (UINT x = 0; x < kWidth; ++x)
            {
                const auto sx = static_cast<UINT>(std::clamp(static_cast<int>(x) + direction.x, 0, static_cast<int>(kWidth) - 1));
                const auto sy = static_cast<UINT>(std::clamp(static_cast<int>(y) + direction.y, 0, static_cast<int>(kHeight) - 1));
                const auto source = floor.input.At(sx, sy);
                now.input.At(x, y) = source;
                now.fresh.At(x, y) = {source.r + 0.02f, source.g + 0.02f, source.b + 0.02f, 0.73f};
            }
        const auto result = RunFeedback(gpu, now, previous, ContinuingPlan());
        const std::string name = std::string("textured same-plane floor ") + direction.name;
        checks.PixelEquals(result.estimate[0].Probe(), Target(previous.cohort[0], direction), name + " reprojects actual latent");
        ExpectContribution(checks, result.shown.Probe(), now.fresh.Probe(), name + " retains valid historical contribution");
        if (direction.x == 1)
        {
            positiveX = result;
            positiveXFrame = now;
        }
    }

    Frame atlas = positiveXFrame;
    auto& c = atlas.constants;
    c.MotionWidth = c.DepthWidth = kWidth / 2;
    c.MotionHeight = c.DepthHeight = kHeight / 2;
    c.MotionBaseX = 3;
    c.MotionBaseY = 2;
    c.DepthBaseX = 4;
    c.DepthBaseY = 1;
    c.MvScaleX = -2;
    c.MvScaleY = 4;
    c.NativeMotionPhaseUvX = 1.0f / 8;
    c.NativeMotionPhaseUvY = -1.0f / 4;
    c.NativeDepthPhaseUvX = -1.0f / 8;
    c.NativeDepthPhaseUvY = 1.0f / 4;
    c.NativeMotionJitterCancelUvX = 1.0f / 16;
    c.NativeMotionJitterCancelUvY = 1.0f / 4;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    atlas.motion = Image({nan, nan, 0, 0}, 14, 10);
    atlas.depth = Image({nan, 0, 0, 0}, 16, 8);
    for (UINT y = 0; y < c.MotionHeight; ++y)
        for (UINT x = 0; x < c.MotionWidth; ++x)
        {
            atlas.motion.At(c.MotionBaseX + x, c.MotionBaseY + y) = {-0.5f, 0.25f, 0, 0};
            atlas.depth.At(c.DepthBaseX + x, c.DepthBaseY + y) = {0.8f, 0, 0, 0};
        }
    // Unphased addresses are decoys, so the check exercises phase, not just size.
    atlas.motion.At(c.MotionBaseX + 4, c.MotionBaseY + 2) = {100, 100, 0, 0};
    atlas.depth.At(c.DepthBaseX + 4, c.DepthBaseY + 2) = {0.2f, 0, 0, 0};
    const auto result = RunFeedback(gpu, atlas, previous, ContinuingPlan());
    checks.PixelEquals(result.estimate[0].Probe(), positiveX.estimate[0].Probe(),
                       "offset half-size guides, signed scales, phases and jitter cancel keep correct floor sample");
    checks.PixelEquals(result.shown.Probe(), positiveX.shown.Probe(), "guide mapping preserves real feedback pixels");
    checks.PixelEquals(result.history.colourDepth.Probe(), {atlas.input.Probe().r, atlas.input.Probe().g,
                                                           atlas.input.Probe().b, 0.8f}, "compose saves phase-correct depth");
}

void TestMovingResidualEdges(Fixture& gpu, Checks& checks)
{
    // Genshin selects residual content (TemporalContent=0), unlike the light-edit
    // fixtures above. A colour variance box alone accepts a displaced thin edge.
    for (const auto direction : kDirections)
    {
        Frame before;
        before.constants.TemporalContent = 0;
        const bool horizontal = direction.x != 0;
        for (UINT y = 0; y < kHeight; ++y)
            for (UINT x = 0; x < kWidth; ++x)
            {
                const bool edge = horizontal ? x == kProbeX : y == kProbeY;
                const float c = edge ? 0.8f : 0.2f;
                before.input.At(x,y) = {c,c,c,1};
                before.fresh.At(x,y) = {c + (edge ? 0.1f : 0.0f),c,c,0.73f};
            }
        const auto seed = Seed(gpu,before);
        Frame now = before;
        for (UINT y = 0; y < kHeight; ++y)
            for (UINT x = 0; x < kWidth; ++x)
            {
                const UINT sx = static_cast<UINT>(std::clamp(int(x)+direction.x,0,int(kWidth)-1));
                const UINT sy = static_cast<UINT>(std::clamp(int(y)+direction.y,0,int(kHeight)-1));
                now.input.At(x,y) = before.input.At(sx,sy);
                now.fresh.At(x,y) = now.input.At(x,y);
                now.fresh.At(x,y).a = 0.73f;
            }
        const auto wrong = RunFeedback(gpu,now,seed,ContinuingPlan());
        checks.Expect(wrong.estimate[0].Probe().a == 0,
            std::string("residual moving edge rejects spatially stale motion ") + direction.name);
        checks.PixelEquals(wrong.shown.Probe(),now.fresh.Probe(),
            std::string("residual edge leaves no stale fence copy ") + direction.name);
        SetMotion(now.motion,float(direction.x),float(direction.y));
        const auto correct = RunFeedback(gpu,now,seed,ContinuingPlan());
        checks.Expect(correct.estimate[0].Probe().a > 0.99f,
            std::string("residual translated structure remains usable ") + direction.name);
        const UINT edgeX = static_cast<UINT>(int(kProbeX) - direction.x);
        const UINT edgeY = static_cast<UINT>(int(kProbeY) - direction.y);
        // A one-pixel fence is outside this fixture's 1.25-sigma colour
        // box even with exact correspondence: expected trust is 1.25+1-sqrt(2).
        // Keep that legitimate attenuation and require its measured contribution.
        checks.Expect(std::abs(correct.estimate[0].At(edgeX, edgeY).a -
                               (kDlssNrDetailReuseClipGamma + 1.0f - std::sqrt(2.0f))) < 1e-4f,
            std::string("residual remains usable at the moved fence itself ") + direction.name);
        checks.Expect(correct.shown.At(edgeX, edgeY).r > now.fresh.At(edgeX, edgeY).r + 1e-4f &&
                      Near(correct.shown.At(edgeX, edgeY).a, now.fresh.At(edgeX, edgeY).a),
            std::string("retained residual follows the real moved fence ") + direction.name);
    }
}

void TestProductionCohortBoundaries(Fixture& gpu, Checks& checks)
{
    Handoff policy;
    const auto epoch = Clock::time_point{} + std::chrono::seconds(1);
    Frame old;
    old.fresh = Image({0.56f, 0.56f, 0.56f, 0.73f});
    auto result = RunFeedback(gpu, old, History(), policy.Next(epoch, 1000, false));
    policy.Captured(epoch);
    checks.ImageEquals(result.shown, old.fresh, "initial reset preserves genuine NR answer");

    Frame now;
    unsigned resetSlots = 0;
    for (const int ageMs : {250, 499, 500, 501, 750, 999, 1000, 1001})
    {
        const auto time = epoch + std::chrono::milliseconds(ageMs);
        const auto plan = policy.Next(time, 1000, true);
        const auto previous = result.history;
        result = RunFeedback(gpu, now, previous, plan);
        const std::string name = "production 1s handoff at " + std::to_string(ageMs) + "ms";
        if (ageMs == 500 || ageMs == 1000)
        {
            resetSlots |= plan.resetMask;
            checks.Expect(plan.resetMask == (1u << plan.incoming) && plan.blend == 0,
                          name + " reseeds only the zero-weight incoming cohort");
            const auto reset = plan.incoming;
            const auto outgoing = reset ^ 1u;
            Image poisoned({2, 2, 2, 1});
            const auto reseeded = Capture(gpu, now, poisoned, previous.colourDepth, plan, true);
            checks.ImageEquals(result.history.cohort[reset], reseeded, name + " reset capture ignores old estimate");

            auto poisonedPrevious = previous;
            poisonedPrevious.cohort[reset] = poisoned;
            const auto replay = RunFeedback(gpu, now, poisonedPrevious, plan);
            checks.ImageEquals(replay.shown, result.shown, name + " expired cohort cannot affect displayed pixels");
            checks.ImageEquals(replay.history.cohort[reset], result.history.cohort[reset],
                               name + " expired cohort cannot renew itself");

            auto outgoingOnly = result.history;
            outgoingOnly.cohort[reset] = result.history.cohort[outgoing];
            checks.ImageEquals(result.shown, Compose(gpu, now, outgoingOnly, plan).primary,
                               name + " display equals independently updated outgoing cohort");
            if (ageMs == 500)
                ExpectContribution(checks, result.shown.Probe(), now.fresh.Probe(), name + " valid outgoing history survives reset");
            else
                checks.ImageEquals(result.shown, now.fresh, name + " both original dependencies have expired");
        }
        else
        {
            checks.Expect(plan.resetMask == 0, name + " does not invent additional reset deadlines");
            if (ageMs == 750)
            {
                auto changed = previous;
                for (auto& p : changed.cohort[0].pixels)
                {
                    p.r += 0.05f;
                    p.g += 0.05f;
                    p.b += 0.05f;
                }
                const auto independent = RunFeedback(gpu, now, changed, plan);
                checks.ImageEquals(independent.history.cohort[1], result.history.cohort[1],
                                   name + " changing A cannot contaminate B capture");
                checks.Expect(std::abs(independent.shown.Probe().r - result.shown.Probe().r) > 1e-4f,
                              name + " both displayed cohorts have observable pixel weight");
            }
        }
        policy.Captured(time);
    }
    checks.Expect(resetSlots == 3, "real pixel chain covers both production cohort reset boundaries");
}

void TestResidualFenceAcrossLifetime(Fixture& gpu, Checks& checks)
{
    Handoff policy;
    History history;
    const auto epoch = Clock::time_point{} + std::chrono::seconds(1);
    int previousX = 3;
    bool contributed = false;
    for (int frame = 0; frame <= 24; ++frame)
    {
        const int x = 3 + (frame % 16 < 8 ? frame % 8 : 8 - frame % 8);
        Frame current;
        current.constants.TemporalContent = 0;
        current.input = Image({0.2f,0.2f,0.2f,1});
        current.fresh = Image({0.2f,0.2f,0.2f,0.73f});
        for (UINT y = 0; y < kHeight; ++y)
        {
            current.input.At(x,y) = {0.8f,0.8f,0.8f,1};
            current.fresh.At(x,y) = {frame == 0 ? 0.9f : 0.82f,0.8f,0.8f,0.73f};
        }
        SetMotion(current.motion,float(previousX-x),0);
        const auto time = epoch + std::chrono::milliseconds(frame*50);
        const auto plan = policy.Next(time,1000,frame != 0);
        auto result = RunFeedback(gpu,current,history,plan);
        for (UINT q = 0; q < kWidth; ++q)
            if (int(q) != x)
                checks.PixelEquals(result.shown.At(q,kProbeY),current.fresh.At(q,kProbeY),
                    "1s residual fence has no detached trail at frame " + std::to_string(frame));
        contributed = contributed || result.shown.At(x,kProbeY).r > current.fresh.At(x,kProbeY).r + 1e-4f;
        checks.Expect(result.estimate[0].At(x,kProbeY).a > 0.0f || frame == 0 ||
                      (plan.resetMask & 1u) != 0,
                      "correct moving residual edge remains admitted");
        history = result.history;
        policy.Captured(time);
        previousX = x;
    }
    checks.Expect(contributed,"moving fence retains nonzero historical contribution without shortening lifetime");
}
} // namespace

int main()
{
    try
    {
        Fixture gpu;
        Checks checks;
        TestSurfaceEstimates(gpu, checks);
        TestFullFeedback(gpu, checks);
        TestFloorDirectionsAndGuideRegions(gpu, checks);
        TestMovingResidualEdges(gpu, checks);
        TestProductionCohortBoundaries(gpu, checks);
        TestResidualFenceAcrossLifetime(gpu, checks);
        return checks.Finish();
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << std::endl;
        return 2;
    }
}

// SPDX-License-Identifier: GPL-3.0-or-later
// Production NR Look HLSL on WARP. No embedded bytecode generation or DLL build.
// Analytic grayscale checks plus real layer/final dispatches and legacy-final comparison.
#define NOMINMAX
#include <d3d11.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#pragma warning(push)
#pragma warning(disable : 4324) // Production constants occupy a 256-byte CB slot.
#include <shaders/dlssnr/DlssNr_Look.h>
#pragma warning(pop)
#include <fstream>
#include <iterator>
#include <memory>
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
using Constants = DlssNrConstants;
using Clock = std::chrono::steady_clock;
constexpr UINT kWidth = 24, kHeight = 16;
constexpr UINT kProbeX = 8, kProbeY = 4;
constexpr float kEpsilon = 8e-5f;

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
    return std::isfinite(a) && std::isfinite(b) && std::abs(a - b) <= kEpsilon * std::max({1.0f, std::abs(a), std::abs(b)});
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

    void ImageExact(const Image& actual, const Image& expected, const std::string& name)
    {
        Expect(actual.width == expected.width && actual.height == expected.height &&
            actual.pixels.size() == expected.pixels.size() &&
            std::memcmp(actual.pixels.data(), expected.pixels.data(), actual.pixels.size() * sizeof(Pixel)) == 0, name);
    }

    int Finish() const
    {
        std::cout << "NR Look production shader WARP: " << count << " checks, " << failures
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
    Fixture(const std::string& source = {})
    {
        D3D_FEATURE_LEVEL level {};
        RequireHr(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                                   D3D11_SDK_VERSION, &device, &level, &context), "Create WARP device");
        if (level < D3D_FEATURE_LEVEL_11_0)
            throw std::runtime_error("WARP feature level 11.0 is required");

        // The runner compiles an absolute source filename. Derive the production
        // shader from it so running the executable does not depend on cwd.
        const auto shaderPath = std::filesystem::path(__FILE__).parent_path().parent_path() / "precompile/dlssnr.hlsl";
        std::cout << "Compiling production HLSL once: " << shaderPath << std::endl;
        std::string text = source;
        if (text.empty())
        {
            std::ifstream file(shaderPath, std::ios::binary);
            if (!file) throw std::runtime_error("Cannot read production HLSL");
            text.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        }
        // Specialize only the uniform dispatch mode AFTER its cbuffer
        // declaration. All production branch bodies/resources remain intact.
        // The unspecialized multi-mode CS faults this Windows WARP build on
        // first dispatch; these are the same mode-14 and resolve pixel paths.
        for (unsigned mode : {1u, 14u})
        {
            auto specialized = text;
            const auto anchor = specialized.find("float SkinColourWeight(");
            if (anchor == std::string::npos) throw std::runtime_error("Production mode anchor missing");
            specialized.insert(anchor, "#define gMode " + std::to_string(mode) + "u\n");
            ComPtr<ID3DBlob> code, errors;
            const auto pathText = shaderPath.string();
            const auto result = D3DCompile(specialized.data(), specialized.size(), pathText.c_str(), nullptr,
                D3D_COMPILE_STANDARD_FILE_INCLUDE, "CSMain", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
            if (FAILED(result) && errors)
                std::cerr.write(static_cast<const char*>(errors->GetBufferPointer()),
                                static_cast<std::streamsize>(errors->GetBufferSize()));
            RequireHr(result, "Compile production HLSL");
            auto& shader = mode == 14 ? layerShader : resolveShader;
            RequireHr(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader),
                      "Create production compute shader");
            std::cout << "Production mode " << mode << " ready (" << code->GetBufferSize() << " bytes)" << std::endl;
        }

        D3D11_SAMPLER_DESC desc {};
        desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        desc.AddressU = desc.AddressV = desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        desc.MaxLOD = D3D11_FLOAT32_MAX;
        RequireHr(device->CreateSamplerState(&desc, &sampler), "Create linear clamp sampler");
    }

    DispatchResult Run(const Inputs& inputs, const Constants& constants)
    {
        if (!reportedFirstDispatch)
        {
            std::cout << "First production dispatch, mode=" << constants.Mode << std::endl;
            reportedFirstDispatch = true;
        }
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

        auto desc = TextureDesc(constants.Width, constants.Height);
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
        context->CSSetShader(constants.Mode == 14 ? layerShader.Get() : resolveShader.Get(), nullptr, 0);
        context->CSSetConstantBuffers(0, 1, &rawBuffer);
        context->CSSetSamplers(0, 1, &rawSampler);
        context->CSSetShaderResources(0, static_cast<UINT>(rawViews.size()), rawViews.data());
        context->CSSetUnorderedAccessViews(0, static_cast<UINT>(rawUavs.size()), rawUavs.data(), nullptr);
        context->Dispatch((constants.Width + 7) / 8, (constants.Height + 7) / 8, 1);
        context->ClearState();

        DispatchResult result;
        result.primary = Readback(outputs[0].Get());
        if (constants.Mode == DlssNrMode_Encode)
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
    ComPtr<ID3D11ComputeShader> layerShader, resolveShader;
    ComPtr<ID3D11SamplerState> sampler;
    bool reportedFirstDispatch = false;
};

// Only storage/standard transfer functions live on the CPU. Look, composition,
// gamut compression, halo and protection execute the actual production CSMain.
double SrgbLight(double x)
{
    x = std::clamp(x, 0.0, 1.0);
    return x < 0.04045 ? x / 12.92 : std::pow((x + 0.055) / 1.055, 2.4);
}
double SrgbCode(double x)
{
    x = std::clamp(x, 0.0, 1.0);
    return x < 0.0031308 ? x * 12.92 : 1.055 * std::pow(x, 1.0 / 2.4) - 0.055;
}
double Uncurve(double y, unsigned mode)
{
    if (mode == 2) return y / std::sqrt(std::max(1.0 - y * y, 1e-8));
    if (mode == 4 && y > 0.75)
    {
        const double u = (y - 0.75) / 0.25;
        return 0.75 + 0.25 * u / std::sqrt(std::max(1.0 - u * u, 1e-8));
    }
    const double signal = SrgbCode(y);
    if (mode == 5)
        return (signal <= 0.5 ? signal * signal / 3.0
            : (std::exp((signal - 0.55991073) / 0.17883277) + 0.28466892) / 12.0) / 0.26496256;
    if (mode == 6)
    {
        const double p = std::pow(signal, 32.0 / 2523.0);
        return std::pow(std::max(p - 3424.0 / 4096.0, 0.0) /
            std::max(2413.0 / 128.0 - 2392.0 / 128.0 * p, 1e-6), 16384.0 / 2610.0) / 0.0203;
    }
    return y;
}
Pixel Working(Pixel p, unsigned mode, bool passthrough)
{
    if (passthrough || mode == 7) return p;
    Pixel out {static_cast<float>(SrgbLight(p.r)), static_cast<float>(SrgbLight(p.g)),
               static_cast<float>(SrgbLight(p.b)), p.a};
    const double peak = std::max({out.r, out.g, out.b});
    if (peak > 1e-6)
    {
        const float scale = static_cast<float>(Uncurve(peak, mode) / peak);
        out.r *= scale; out.g *= scale; out.b *= scale;
    }
    return out;
}
double Storage(double working, unsigned mode, bool passthrough)
{
    if (passthrough || mode == 7) return working;
    if (mode == 2) working /= std::sqrt(working * working + 1.0);
    if (mode == 4 && working > 0.75)
    {
        const double excess = (working - 0.75) / 0.25;
        working = 0.75 + 0.25 * excess / std::sqrt(excess * excess + 1.0);
    }
    if (mode == 5)
    {
        const double e = working * 0.26496256;
        return e <= 1.0 / 12.0 ? std::sqrt(3.0 * e)
            : 0.17883277 * std::log(12.0 * e - 0.28466892) + 0.55991073;
    }
    if (mode == 6)
    {
        const double y = std::pow(working * 0.0203, 2610.0 / 16384.0);
        return std::pow((3424.0 / 4096.0 + 2413.0 / 128.0 * y) /
            (1.0 + 2392.0 / 128.0 * y), 2523.0 / 32.0);
    }
    return SrgbCode(working);
}

Constants Settings(unsigned mode, bool passthrough)
{
    Constants c {};
    c.Width = kWidth; c.Height = kHeight; c.WhitePoint = 1;
    c.Passthrough = passthrough ? 1u : 0u; c.ReversibleMode = mode;
    c.TransferStrength = c.ColourStrength = c.ModelWorkScale = 1;
    c.MaxRatio = 8; c.ApplyModel = 1; c.DebugScale = 1;
    DlssNr::LayerLook::Profile p;
    p.enabled = true;
    for (unsigned i = 0; i < p.values.size(); ++i)
        p.values[i] = DlssNr::LayerLook::Parameters[i].neutral;
    DlssNrLook::Fill(c, p);
    return c;
}
Inputs LayerInputs(const Image& base, const Image& answer, const Image& mask = Image())
{
    return {answer, base, Image(), Image(), mask, Image()};
}
Inputs FinalInputs(const Image& base, const Image& answer, const Constants& c)
{
    Image original = base;
    for (auto& p : original.pixels) p = Working(p, c.ReversibleMode, c.Passthrough != 0);
    return {base, answer, original, Image(), Image(), Image()};
}
// Compile the actual output descriptor ownership and resolve bindings, then
// run production HLSL. Source permissions must never stand in for output flags.
bool ProductionResolveUav(bool sourceUav, bool separateInput, bool outputUav, bool cropColor)
{
    struct Resource {
        bool uav;
        D3D12_RESOURCE_DESC GetDesc() const {
            D3D12_RESOURCE_DESC desc {};
            desc.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
            return desc;
        }
    } source {sourceUav}, destination {outputUav};
    auto* output = separateInput ? &destination : &source;
#include "colour-destination.inl"
    return targetSupportsUav;
}
void CurrentSourceResolve(Fixture& f, Checks& checks)
{
    for (unsigned path = 0; path < 4; ++path) for (bool hdr : {false, true})
        for (unsigned frame = 0; frame < 3; ++frame) for (unsigned variant = 0; variant < 3; ++variant)
        {
            const bool separateInput = path < 2;
            const bool sourceUav = path != 0 && path != 3;
            const bool outputUav = separateInput || sourceUav;
            const Image current({0.15f + 0.1f * frame, 0.28f, 0.19f, 0.37f});
            const Image answer({0.22f + 0.1f * frame, 0.32f, 0.23f, 0.37f});
            const Image stale({0.83f, 0.06f + 0.1f * frame, 0.77f, 0.91f});
            auto resolveParams = Settings(hdr ? 2u : 0u, !hdr);
            resolveParams.Mode = DlssNrMode_Resolve;
            if (variant == 0) resolveParams.ApplyModel = 0;
            if (variant == 2) resolveParams.LookStrength = 0.5f;
            auto inputs = FinalInputs(current, answer, resolveParams);
            // The fixture's default game encoding is linear. Its model proxy
            // is encoded separately, so raw game colour equals saved working colour.
            const Image gameCurrent = inputs[2];
            const struct { const Image* hdrCopy; } g_nr {&inputs[2]};
            const Image* target = separateInput ? &stale : &gameCurrent;
            const bool targetSupportsUav = ProductionResolveUav(sourceUav, separateInput, outputUav, false);
#include "colour-bindings.inl"
            checks.Expect(resolveTarget == (outputUav ? target : g_nr.hdrCopy) &&
                          resolveOriginal == (outputUav ? g_nr.hdrCopy : target) &&
                          resolveParams.OriginalIsGameColour == (outputUav ? 0u : 1u),
                          "resolve binds the actual destination and this source's saved original");
            auto expectedParams = resolveParams; expectedParams.OriginalIsGameColour = 0;
            const auto expected = f.Run(FinalInputs(current, answer, expectedParams), expectedParams).primary;
            inputs[2] = *resolveOriginal;
            checks.ImageEquals(f.Run(inputs, resolveParams).primary, expected,
                "no previous private colour in final resolve source=" + std::to_string(frame) +
                " path=" + std::to_string(path) + " hdr=" + std::to_string(hdr) +
                " variant=" + std::to_string(variant));
        }
    checks.Expect(!ProductionResolveUav(false, false, false, false), "in-place non-UAV still uses legal copy-back");
    checks.Expect(ProductionResolveUav(false, false, false, true), "cropped private surface is writable");
}
Image Layer(Fixture& f, const Image& base, const Image& answer, const Constants& settings)
{
    auto c = settings;
    c.Mode = DlssNrMode_LayerLook;
    return f.Run(LayerInputs(base, answer), c).primary;
}
Image Decode(const Image& image, const Constants& c)
{
    Image out = image;
    for (auto& p : out.pixels) p = Working(p, c.ReversibleMode, c.Passthrough != 0);
    return out;
}
Image Pattern(bool answer)
{
    Image out;
    for (UINT y = 0; y < kHeight; ++y)
        for (UINT x = 0; x < kWidth; ++x)
        {
            const float v = ((x / 3 + y / 2) % 2 == 0) ? 0.28f : 0.58f;
            out.At(x,y) = answer ? Pixel{v * 1.05f, v * 0.86f, v * 1.10f, 0.37f}
                                : Pixel{v, v * 0.95f, v * 0.90f, 0.37f};
        }
    return out;
}
void AnalyticAndEndpoints(Fixture& f, Checks& checks)
{
    for (bool passthrough : {false, true}) for (unsigned mode = 0; mode < 8; ++mode)
    {
        auto c = Settings(mode, passthrough);
        const std::string tag = "mode=" + std::to_string(mode) + " passthrough=" + std::to_string(passthrough);
        Image base({0.25f,0.25f,0.25f,0.37f}), answer({0.5f,0.5f,0.5f,0.37f});
        checks.ImageExact(Layer(f, base, answer, c), answer, "neutral exact " + tag);
        c.LookMode = 0;
        checks.ImageExact(Layer(f, base, answer, c), answer, "disabled exact " + tag);
        c.LookMode = 1; c.LookStrength = 0;
        checks.ImageExact(Layer(f, base, answer, c), base, "zero exact " + tag);
        c.LookStrength = 0.5f;
        const double lo = Working(base.Probe(), mode, passthrough).r;
        const double hi = Working(answer.Probe(), mode, passthrough).r;
        const float expected = static_cast<float>(Storage(std::sqrt(lo * hi), mode, passthrough));
        checks.ImageEquals(Layer(f, base, answer, c), Image({expected,expected,expected,0.37f}),
                           "half strength is geometric mean in final working domain " + tag);
        c.LookStrength = 1; c.LookMaxBrighten = 0.7f;
        const double ev = std::log2(hi / lo);
        const double bounded = 0.7 * (1.0 - std::exp(-ev / 0.7));
        const float capped = static_cast<float>(Storage(lo * std::exp2(bounded), mode, passthrough));
        checks.ImageEquals(Layer(f, base, answer, c), Image({capped,capped,capped,0.37f}),
                           "soft EV ceiling in final working domain " + tag);
        c.LookMaxBrighten = 0; c.LookMaxDarken = 0.7f;
        const double dark = -0.7 * (1.0 - std::exp(-ev / 0.7));
        const float floored = static_cast<float>(Storage(hi * std::exp2(dark), mode, passthrough));
        checks.ImageEquals(Layer(f, answer, base, c), Image({floored,floored,floored,0.37f}),
                           "soft darkening EV floor " + tag);
        c.LookMaxDarken = 0; c.LookStrength = 0.6f;
        checks.ImageEquals(Layer(f, base, base, c), base, "zero residual stays zero " + tag);
        // Hybrid Replace's inverse is identity below 0.75 LIGHT. These stored
        // values cross that knee, so an sRGB-only fix cannot pass by accident.
        const Image highlightBase({0.91f,0.91f,0.91f,0.37f});
        const Image highlightAnswer({0.96f,0.96f,0.96f,0.37f});
        c.LookStrength = 0.5f;
        const double highLo = Working(highlightBase.Probe(), mode, passthrough).r;
        const double highHi = Working(highlightAnswer.Probe(), mode, passthrough).r;
        const float highExpected = static_cast<float>(Storage(std::sqrt(highLo * highHi), mode, passthrough));
        checks.ImageEquals(Layer(f, highlightBase, highlightAnswer, c),
            Image({highExpected,highExpected,highExpected,0.37f}), "highlight inverse/forward pair " + tag);
        auto domain = Settings(7, !passthrough);
        DlssNrLook::FillWorkingDomain(domain, c);
        checks.Expect(domain.Passthrough == c.Passthrough && domain.ReversibleMode == mode &&
                      domain.LookStrength == 1, "caller copies only domain " + tag);
    }
    auto c = Settings(2, true);
    const Image base({0.7f,0.4f,0.2f,0.37f}), wide({1.3f,0.6f,0.2f,0.37f});
    checks.ImageExact(Layer(f, base, wide, c), wide, "passthrough neutral does not cube-clamp HDR-like values");
    c.LookStrength = 0.5f;
    const auto out = Layer(f, base, wide, c);
    checks.Expect(out.Probe().r > 0.7f && out.Probe().a == 0.37f, "passthrough nonneutral retains alpha and signal");
    c = Settings(4, false);
    Image extreme({0.9999f,0.9998f,0.9f,0.37f});
    checks.ImageExact(Layer(f, base, extreme, c), extreme, "neutral avoids Replace inverse pole round trip");
    c.LookStrength = 0;
    checks.ImageExact(Layer(f, base, extreme, c), base, "zero avoids Replace inverse pole round trip");
}

// Compare to the final compositor itself, with its original deliberately set
// to the current layer base in the final working domain. Other composition
// controls remain at identity; no CPU implementation of Look is the oracle.
void LayerFinalParity(Fixture& f, Checks& checks)
{
    const auto base = Pattern(false), answer = Pattern(true);
    for (bool passthrough : {false, true}) for (unsigned mode = 0; mode < 8; ++mode)
        for (unsigned variant = 0; variant < 6; ++variant)
        {
            auto c = Settings(mode, passthrough);
            if (variant == 0) c.LookStrength = 0.6f;
            if (variant == 1) {c.LookColour = 0.4f; c.LookHue = 1.2f; c.LookMaxColour = 0.4f;}
            if (variant == 2) {c.LookTone = 0.5f; c.LookDetail = 1.3f; c.LookDetailRadius = 2.3f;}
            if (variant == 3) {c.LookHalo = 0.7f; c.LookHaloBrightSide = 0.6f; c.LookDetailRadius = 3.2f;}
            if (variant == 4) {c.LookShadows = 0.6f; c.LookMidtones = 1.2f; c.LookHighlights = 0.8f;}
            if (variant == 5) {c.LookMaxBrighten = 0.7f; c.LookMaxDarken = 0.8f; c.LookBrighten = 0.9f;}
            const auto layer = Decode(Layer(f, base, answer, c), c);
            c.Mode = DlssNrMode_Resolve;
            const auto final = f.Run(FinalInputs(base, answer, c), c).primary;
            checks.ImageEquals(layer, final, "layer/final working-domain parity " + std::to_string(mode) +
                "/" + std::to_string(passthrough) + "/" + std::to_string(variant));
        }
    // Exercise the existing mode-14 mask branch with an explicitly supplied
    // same-size fixture texture. This is no authorization for a runtime mask.
    auto c = Settings(2, false); c.Mode = DlssNrMode_LayerLook; c.LookStrength = 0;
    c.LayerLookUiMask = 1;
    checks.ImageEquals(f.Run(LayerInputs(base, answer, Image({1,0,0,0})), c).primary, answer,
                       "explicit protected pixels preserve pre-Look answer");
    checks.ImageEquals(f.Run(LayerInputs(base, answer, Image()), c).primary, base,
                       "unprotected pixels apply zero strength");
}

void FinalCompatibility(Fixture& current, Fixture& baseline, Checks& checks)
{
    const auto base = Pattern(false), answer = Pattern(true);
    for (bool passthrough : {false,true}) for (unsigned mode = 0; mode < 8; ++mode)
        for (unsigned variant = 0; variant < 14; ++variant)
        {
            auto c = Settings(mode, passthrough); c.Mode = DlssNrMode_Resolve;
            c.LookStrength = 0.7f; c.LookTone = 0.6f; c.LookDetail = 1.3f;
            c.LookHalo = 0.4f; c.LookHaloBrightSide = 0.5f; c.LookDetailRadius = 2.7f;
            c.LookColour = 0.6f; c.LookHue = 1.2f; c.LookMaxBrighten = 0.8f; c.LookMaxColour = 0.5f;
            if (variant == 1) c.LookMode = 0;
            if (variant == 2) c.LookStrength = 0;
            if (variant == 3) c.TransferStrength = 0.3f;
            if (variant == 4) c.ColourStrength = 0;
            if (variant == 5) {c.SkinProtection = 1; c.SkinDetail = 0.4f; c.SkinColour = 0.3f;
                              c.EnvironmentDetail = 0.8f; c.EnvironmentColour = 0.6f;}
            if (variant == 6) {c.CompareMode = 1; c.CompareZoom = 1.2f;}
            if (variant == 7) {c.CompareMode = 2; c.CompareSplit = 0.4f;}
            if (variant == 8) c.ApplyModel = 0;
            if (variant == 9) c.DebugView = 2;
            if (variant == 10) {c.Transfer = 1; c.ModelWorkScale = 0.5f; c.ReplaceDetailStrength = 0.5f;}
            if (variant == 11) {c.WhitePoint = 2.3f; c.MaxRatio = 1.1f;}
            if (variant == 12) {c.InputEncoding = 3; c.OriginalIsGameColour = 1;}
            if (variant == 13) {c.InputEncoding = 4; c.OriginalIsGameColour = 1;}
            auto inputs = FinalInputs(base, answer, c);
            checks.ImageEquals(current.Run(inputs, c).primary, baseline.Run(inputs, c).primary,
                "unchanged legacy final resolve " + std::to_string(mode) + "/" +
                std::to_string(passthrough) + "/" + std::to_string(variant));
        }
}
std::string ReadSource(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("Cannot read shader: " + path.string());
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
} // namespace

int main(int argc, char** argv)
{
    try
    {
        Checks checks;
        const bool negative = argc > 1 && std::string(argv[1]) == "--negative-control";
        std::string source;
        if (negative)
        {
            source = ReadSource(std::filesystem::path(__FILE__).parent_path().parent_path() / "precompile/dlssnr.hlsl");
            const auto start = source.find("        float3 workingBase = LayerLookToWorking(base);");
            const auto end = source.find("        return;", start);
            if (start == std::string::npos || end == std::string::npos)
                throw std::runtime_error("Negative-control production layer anchors missing");
            source.replace(start, end-start,
                "        float3 looked = ApplyLookEdit(base, restored, uv, 1.0, true);\n"
                "        gTarget[id.xy] = float4(CubeScaleResidual(base, SanitizeFinite3(looked, base)), raw.a);\n");
            std::cout << "NEGATIVE CONTROL: old encoded-domain layer math restored in production CSMain\n";
        }
        Fixture current(source);
        AnalyticAndEndpoints(current, checks);
        if (!negative)
        {
            LayerFinalParity(current, checks);
            CurrentSourceResolve(current, checks);
            if (argc > 1)
            {
                Fixture baseline(ReadSource(argv[1]));
                FinalCompatibility(current, baseline, checks);
            }
        }
        return checks.Finish();
    }
    catch (const std::exception& e)
    {
        std::cerr << "ERROR: " << e.what() << '\n';
        return 2;
    }
}


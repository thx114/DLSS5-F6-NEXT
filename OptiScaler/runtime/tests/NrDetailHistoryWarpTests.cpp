// SPDX-License-Identifier: GPL-3.0-or-later
// Single-dispatch full-model history on production HLSL/WARP.
// Standalone entry: compile this file with /std:c++20 /EHsc /I OptiScaler
// and link d3d11.lib d3dcompiler.lib. No generated shader or DLL is needed.
// The main session owns compilation; this fixture compiles HLSL in memory only.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#pragma warning(push)
#pragma warning(disable : 4324) // Production constants occupy a 256-byte CB slot.
#include <shaders/dlssnr/DlssNr_DetailReuseConstants.h>
#pragma warning(pop)
#include <d3d11shader.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{
using Microsoft::WRL::ComPtr;
using Constants = DlssNrDetailReuseConstants;
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
using Inputs = std::array<Image, 8>;

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
        std::cout << "NR detail history production shader WARP: " << count << " checks, " << failures
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
    Image secondary;
    Image birthContext;
    Image stability;
    Image adjacentRaw;
};

class Fixture
{
public:
    explicit Fixture(const std::filesystem::path& overrideShader = {})
    {
        D3D_FEATURE_LEVEL level {};
        RequireHr(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                                   D3D11_SDK_VERSION, &device, &level, &context), "Create WARP device");
        if (level < D3D_FEATURE_LEVEL_11_0)
            throw std::runtime_error("WARP feature level 11.0 is required");

        // The runner compiles an absolute source filename. Derive the production
        // shader from it so running the executable does not depend on cwd.
        const auto shaderPath = overrideShader.empty() ?
            std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() /
                "shaders/dlssnr/precompile/dlssnr_detail_reuse.hlsl" : overrideShader;
        std::cout << "Compiling production HLSL once: " << shaderPath << std::endl;
        ComPtr<ID3DBlob> code, errors;
        const auto result = D3DCompileFromFile(shaderPath.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
                                             "CSMain", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
        if (errors)
            std::cerr.write(static_cast<const char*>(errors->GetBufferPointer()),
                            static_cast<std::streamsize>(errors->GetBufferSize()));
        RequireHr(result, "Compile production HLSL");
        // Reflect the actual production CB, not a duplicated CPU mock layout.
        ComPtr<ID3D11ShaderReflection> reflection;
        RequireHr(D3DReflect(code->GetBufferPointer(), code->GetBufferSize(),
                            __uuidof(ID3D11ShaderReflection), &reflection), "Reflect production shader");
        auto* cb = reflection->GetConstantBufferByName("Params");
        const std::array<std::pair<const char*, UINT>, 10> offsets {{
            {"feedbackDeltaSeconds", 196}, {"feedbackDurationSeconds", 200},
            {"feedbackMaxAgeSeconds", 204}, {"feedbackStrength", 208},
            {"feedbackHistoryValid", 212}, {"feedbackRasterOffsetUvX", 216},
            {"feedbackRasterOffsetUvY", 220}, {"feedbackStabilitySeconds", 224},
            {"feedbackStabilityFloor", 228}, {"feedbackStabilityClamp", 232}
        }};
        for (const auto& [name, offset] : offsets)
        {
            D3D11_SHADER_VARIABLE_DESC variable {};
            RequireHr(cb->GetVariableByName(name)->GetDesc(&variable), "Reflect feedback constant");
            if (variable.StartOffset != offset)
                throw std::runtime_error(std::string("Production CB offset mismatch: ") + name);
        }
        RequireHr(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader),
                  "Create production compute shader");

        D3D11_SAMPLER_DESC desc {};
        desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        desc.AddressU = desc.AddressV = desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        desc.MaxLOD = D3D11_FLOAT32_MAX;
        RequireHr(device->CreateSamplerState(&desc, &sampler), "Create linear clamp sampler");
    }

    DispatchResult Run(const Inputs& inputs, const Constants& constants, bool productionFormats=false)
    {
        std::array<ComPtr<ID3D11Texture2D>, std::tuple_size_v<Inputs>> textures;
        std::array<ComPtr<ID3D11ShaderResourceView>, std::tuple_size_v<Inputs>> views;
        std::array<ID3D11ShaderResourceView*, std::tuple_size_v<Inputs>> rawViews {};
        for (size_t i = 0; i < inputs.size(); ++i)
        {
            const auto& input = inputs[i];
            if (input.width == 0 || input.height == 0) continue; // Actually unbound SRV, not a black substitute.
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
        std::array<ComPtr<ID3D11Texture2D>, 5> outputs;
        std::array<ComPtr<ID3D11UnorderedAccessView>, 5> uavs;
        std::array<ID3D11UnorderedAccessView*, 5> rawUavs {};
        const float clear[4] {};
        for (size_t i = 0; i < outputs.size(); ++i)
        {
            desc.Format = productionFormats && i != 2 && i != 4
                ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R32G32B32A32_FLOAT;
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
        result.secondary = Readback(outputs[1].Get());
        result.birthContext = Readback(outputs[2].Get());
        result.stability = Readback(outputs[3].Get());
        result.adjacentRaw = Readback(outputs[4].Get());
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
        {
            const auto* row=static_cast<const char*>(mapped.pData)+static_cast<size_t>(y)*mapped.RowPitch;
            if(desc.Format==DXGI_FORMAT_R16G16B16A16_FLOAT)
            {
                const auto* values=reinterpret_cast<const uint16_t*>(row);
                for(UINT x=0;x<desc.Width;++x)
                {
                    float decoded[4];
                    for(unsigned channel=0;channel<4;++channel)
                    {
                        const uint16_t bits=values[4*x+channel];
                        const unsigned exponent=(bits>>10)&31, fraction=bits&1023;
                        const float value=exponent==0?std::ldexp(float(fraction),-24):
                            exponent==31?(fraction?std::numeric_limits<float>::quiet_NaN():
                                                  std::numeric_limits<float>::infinity()):
                            std::ldexp(1.0f+float(fraction)/1024.0f,int(exponent)-15);
                        decoded[channel]=(bits&0x8000)?-value:value;
                    }
                    result.At(x,y)={decoded[0],decoded[1],decoded[2],decoded[3]};
                }
            }
            else std::memcpy(result.pixels.data()+static_cast<size_t>(y)*desc.Width,row,
                             static_cast<size_t>(desc.Width)*sizeof(Pixel));
        }
        context->Unmap(staging.Get(), 0);
        return result;
    }

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11ComputeShader> shader;
    ComPtr<ID3D11SamplerState> sampler;
};

Constants FeedbackConstants()
{
    Constants c {};
    c.Mode = DlssNrDetailReuse_FeedbackResolve;
    c.TemporalMode = 2;
    c.WorkWidth = c.MotionWidth = c.DepthWidth = kWidth;
    c.WorkHeight = c.MotionHeight = c.DepthHeight = kHeight;
    c.DepthInverted = 1;
    c.MvScaleX = c.MvScaleY = 1;
    c.DepthTolerance = 0.05f;
    c.SigmaFloor = 0.01f;
    c.FeedbackDeltaSeconds = 0.05f;
    c.FeedbackDurationSeconds = 0.8f;
    c.FeedbackMaxAgeSeconds = 1.5f;
    c.FeedbackStrength = 0.75f;
    c.FeedbackHistoryValid = 1;
    return c;
}

Inputs FlatInputs()
{
    return {Image({0.4f,0.5f,0.6f,1}), Image({0.42f,0.52f,0.62f,0.73f}),
            Image({0.035f,0.01f,-0.015f,0.2f}), Image({0.4f,0.5f,0.6f,0.4f}),
            Image({0,0,0,0}), Image({0.4f,0,0,0}), Image({},0,0), Image({0.4f,0.5f,0.6f,0})};
}

void Fill(Image& image, Pixel value) { std::fill(image.pixels.begin(), image.pixels.end(), value); }

void Motion(Inputs& inputs, float x, float y) { Fill(inputs[4], {x,y,0,0}); }

void ExpectFresh(Fixture& gpu, Checks& checks, const Inputs& inputs, const Constants& c,
                 const std::string& name, UINT x = kProbeX, UINT y = kProbeY)
{
    const auto result = gpu.Run(inputs, c);
    checks.PixelEquals(result.primary.At(x,y), inputs[1].At(x,y), name + " fresh answer");
    const auto fresh=inputs[1].At(x,y), proxy=inputs[0].At(x,y);
    checks.PixelEquals(result.secondary.At(x,y),{fresh.r-proxy.r,fresh.g-proxy.g,fresh.b-proxy.b,0},
                       name + " stores only fresh residual with new ancestry at zero");
}

void TestWeightsAndLanding(Fixture& gpu, Checks& checks)
{
    auto c = FeedbackConstants();
    auto inputs = FlatInputs();
    for (const float duration : {0.1f, 0.8f, 4.0f})
        for (const float strength : {0.25f, 0.75f, 1.0f, 3.0f})
        {
            c.FeedbackDurationSeconds = duration;
            c.FeedbackStrength = strength;
            auto output = gpu.Run(inputs,c);
            const float age = 0.2f+c.FeedbackDeltaSeconds;
            const float w = std::min(std::clamp(strength,0.0f,1.0f), std::exp(-c.FeedbackDeltaSeconds/duration))*
                            (1-age/c.FeedbackMaxAgeSeconds);
            const Pixel expected {0.4f+0.02f*(1-w)+0.035f*w, 0.5f+0.02f*(1-w)+0.01f*w,
                                  0.6f+0.02f*(1-w)-0.015f*w,0.73f};
            checks.PixelEquals(output.primary.Probe(),expected,"signed vector mean uses source delta and remaining birth lifetime");
            const float meanWeight=std::exp(-c.FeedbackDeltaSeconds/duration);
            checks.PixelEquals(output.secondary.Probe(),{0.02f*(1-meanWeight)+0.035f*meanWeight,
                0.02f*(1-meanWeight)+0.01f*meanWeight,0.02f*(1-meanWeight)-0.015f*meanWeight,0.25f},
                "history stores a symmetric observation mean with independent original birth age");
            checks.Expect(Near(output.secondary.Probe().a,0.25f),"fresh mix does not reset old birth age");
            checks.Expect(std::abs(output.primary.Probe().r-inputs[1].Probe().r)>1e-4f,
                          "accepted history makes measurable nonzero contribution");
        }
    c = FeedbackConstants();
    Fill(inputs[2],{500,-300,700,0.2f});
    const auto limited = gpu.Run(inputs,c);
    const float w = std::min(c.FeedbackStrength,std::exp(-c.FeedbackDeltaSeconds/c.FeedbackDurationSeconds))*
                    (1-0.25f/c.FeedbackMaxAgeSeconds);
    checks.PixelEquals(limited.primary.Probe(),{0.4f+0.02f*(1-w)+0.05f*w,
        0.5f+0.02f*(1-w)-0.03f*w,0.6f+0.02f*(1-w)+0.07f*w,0.73f},
        "large historical RGB is bounded by current signal and fresh residual support");
    inputs[1] = inputs[0];
    checks.PixelEquals(gpu.Run(inputs,c).primary.Probe(),{0.4f+0.05f*w,0.5f-0.03f*w,0.6f+0.07f*w,1},
                       "zero fresh residual does not allow huge history to exceed the verified signal budget");

    inputs=FlatInputs();
    const float matched=gpu.Run(inputs,c).primary.Probe().r;
    Fill(inputs[3],{0.42f,0.5f,0.6f,0.4f});
    const float colourFaded=gpu.Run(inputs,c).primary.Probe().r;
    checks.Expect(colourFaded>inputs[1].Probe().r && colourFaded<matched,
                  "small raw proxy colour mismatch smoothly reduces contribution");

    // Local amplitude may bound a landing but must never supply history RGB.
    inputs = FlatInputs();
    Fill(inputs[2],{0,0,0,0.2f});
    const auto noDonor=gpu.Run(inputs,c).primary.Probe();
    inputs[2].At(kProbeX-1,kProbeY) = {100,100,100,0.2f};
    const auto own = gpu.Run(inputs,c);
    checks.PixelEquals(own.primary.Probe(),noDonor,
                       "no neighbour residual donor filling");
}

void TestFirstFrameAndInvalidValues(Fixture& gpu, Checks& checks)
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    auto c = FeedbackConstants();
    auto inputs = FlatInputs();
    for (UINT slot = 2; slot < 6; ++slot) inputs[slot] = Image({},0,0);
    c.FeedbackHistoryValid = 0;
    checks.ImageEquals(gpu.Run(inputs,c).primary,inputs[1],"first invalid frame with truly unbound t2-t5");
    c.FeedbackHistoryValid = 1;
    c.FeedbackDurationSeconds = 0;
    checks.ImageEquals(gpu.Run(inputs,c).primary,inputs[1],"duration zero means fresh only with unbound history");
    c = FeedbackConstants();
    c.FeedbackDeltaSeconds = c.FeedbackMaxAgeSeconds;
    checks.ImageEquals(gpu.Run(inputs,c).primary,inputs[1],"frame time exhausts lifetime before accessing history");

    for (UINT slot = 2; slot < 6; ++slot)
    {
        inputs = FlatInputs();
        inputs[slot] = Image({},0,0);
        ExpectFresh(gpu,checks,inputs,FeedbackConstants(),"missing resource " + std::to_string(slot));
    }
    for (UINT item = 0; item < 27; ++item)
    {
        inputs = FlatInputs();
        c = FeedbackConstants();
        switch (item)
        {
        case 0: inputs[2].At(kProbeX,kProbeY).r=nan; break;
        case 1: inputs[2].At(kProbeX,kProbeY).a=nan; break;
        case 2: inputs[2].At(kProbeX,kProbeY).a=-0.1f; break;
        case 3: inputs[3].At(kProbeX,kProbeY).g=nan; break;
        case 4: inputs[3].At(kProbeX,kProbeY).a=-1; break;
        case 5: inputs[3].At(kProbeX,kProbeY).a=inf; break;
        case 6: inputs[4].At(kProbeX,kProbeY).r=nan; break;
        case 7: inputs[5].At(kProbeX,kProbeY).r=nan; break;
        case 8: inputs[5].At(kProbeX,kProbeY).r=-1; break;
        case 9: inputs[5].At(kProbeX,kProbeY).r=1.1f; break;
        case 10: c.DepthWidth=0; break;
        case 11: c.MotionHeight=0; break;
        case 12: c.FeedbackDeltaSeconds=nan; break;
        case 13: c.FeedbackDeltaSeconds=-1; break;
        case 14: c.FeedbackDurationSeconds=inf; break;
        case 15: c.FeedbackStrength=nan; break;
        case 16: c.FeedbackMaxAgeSeconds=0; break;
        // Production optimization must not erase finite gates on uniforms used
        // in reciprocal/exp paths. Case 14 (+Inf duration) caught this on WARP.
        case 17: c.FeedbackDurationSeconds=nan; break;
        case 18: c.FeedbackDurationSeconds=-inf; break;
        case 19: c.FeedbackDeltaSeconds=inf; break;
        case 20: c.FeedbackMaxAgeSeconds=inf; break;
        case 21: c.FeedbackStrength=inf; break;
        case 22: c.FeedbackRasterOffsetUvX=inf; break;
        case 23: c.FeedbackRasterOffsetUvY=nan; break;
        case 24: c.MvScaleX=inf; break;
        case 25: c.DepthTolerance=inf; break;
        case 26: c.NativeDepthPhaseUvY=nan; break;
        }
        ExpectFresh(gpu,checks,inputs,c,"invalid history/guide/constants " + std::to_string(item));
    }
    for (UINT slot : {0u,1u})
        for (UINT channel = 0; channel < 4; ++channel)
        {
            inputs = FlatInputs();
            auto& bad = inputs[slot].At(kProbeX,kProbeY);
            if (channel==0) bad.r=nan;
            if (channel==1) bad.g=nan;
            if (channel==2) bad.b=nan;
            if (channel==3) bad.a=nan;
            const auto result = gpu.Run(inputs,FeedbackConstants());
            const auto p = result.primary.Probe();
            checks.Expect(std::isfinite(p.r)&&std::isfinite(p.g)&&std::isfinite(p.b)&&std::isfinite(p.a),
                          "invalid proxy/fresh including alpha produces finite answer");
            if (slot==0)
                checks.PixelEquals(p,inputs[1].Probe(),"invalid proxy safely returns finite fresh");
            else
                checks.PixelEquals(p,{0.4f,0.5f,0.6f,channel==3?1.0f:0.73f},
                                   "invalid fresh safely uses finite current proxy");
            checks.PixelEquals(result.secondary.Probe(),{},"invalid source cannot seed old RGB or age");
        }
}

void TestStrictFootprintAndDepth(Fixture& gpu, Checks& checks)
{
    auto c = FeedbackConstants();
    auto inputs = FlatInputs();
    Motion(inputs,0.5f,0.5f);
    for (UINT tap = 0; tap < 4; ++tap)
    {
        const UINT x=kProbeX+(tap&1u), y=kProbeY+((tap>>1u)&1u);
        for (UINT reason = 0; reason < 4; ++reason)
        {
            inputs = FlatInputs();
            Motion(inputs,0.5f,0.5f);
            if (reason==0) inputs[3].At(x,y).a=0.8f;
            if (reason==1) inputs[2].At(x,y).a=c.FeedbackMaxAgeSeconds-c.FeedbackDeltaSeconds;
            if (reason==2) inputs[2].At(x,y).b=std::numeric_limits<float>::quiet_NaN();
            if (reason==3) inputs[3].At(x,y).a=-1;
            ExpectFresh(gpu,checks,inputs,c,"positive tap " + std::to_string(tap) + " veto " + std::to_string(reason));
        }
    }
    inputs = FlatInputs();
    Motion(inputs,0.5f,0.5f);
    for (UINT tap=0; tap<4; ++tap)
    {
        inputs[3].At(kProbeX+(tap&1u),kProbeY+((tap>>1u)&1u)).a = (tap&1u) ? 0.6f : 0.2f;
        inputs[2].At(kProbeX+(tap&1u),kProbeY+((tap>>1u)&1u)).a = 0.1f*float(tap+1);
    }
    ExpectFresh(gpu,checks,inputs,c,"bilinear depth mean is compatible but each surface is incompatible");
    Fill(inputs[3],{0.4f,0.5f,0.6f,0.4f});
    const auto mixedBirth=gpu.Run(inputs,c);
    checks.Expect(Near(mixedBirth.secondary.Probe().a,0.45f),
                  "valid fractional births use oldest age instead of resetting or rejuvenating");
    Fill(inputs[2],{0.035f,0.01f,-0.015f,0.4f});
    const auto aged=gpu.Run(inputs,c);
    checks.Expect(Near(aged.secondary.Probe().a,0.45f),"coherent fractional footprint keeps its birth age");
    const float oldestWeight=std::min(c.FeedbackStrength,std::exp(-c.FeedbackDeltaSeconds/c.FeedbackDurationSeconds))*
                             (1-0.45f/c.FeedbackMaxAgeSeconds);
    checks.PixelEquals(aged.primary.Probe(),{0.4f+0.02f*(1-oldestWeight)+0.035f*oldestWeight,
        0.5f+0.02f*(1-oldestWeight)+0.01f*oldestWeight,0.6f+0.02f*(1-oldestWeight)-0.015f*oldestWeight,0.73f},
        "remaining-life ratio uses oldest positive tap, not mean age");
    checks.Expect(aged.primary.Probe().r>inputs[1].Probe().r+0.001f,"four valid fractional taps really contribute");

    inputs=FlatInputs();
    inputs[2].At(kProbeX+1,kProbeY).a=-1;
    inputs[3].At(kProbeX+1,kProbeY).a=-1;
    checks.Expect(gpu.Run(inputs,c).secondary.Probe().a>0,"zero-weight history tap is not an authority");
    for (auto [x,y,dx,dy] : {std::array<float,4>{0,4,-0.25f,0}, {15,4,0.25f,0},
                             {8,0,0,-0.25f}, {8,7,0,0.25f}})
    {
        inputs=FlatInputs();
        Motion(inputs,dx,dy);
        ExpectFresh(gpu,checks,inputs,c,"positive bilinear tap outside image",static_cast<UINT>(x),static_cast<UINT>(y));
    }
    // Raw normal and reversed depth must agree after canonicalization, including sky.
    for (float canonical : {0.0f,0.4f,1.0f})
        for (UINT inverted : {0u,1u})
        {
            inputs=FlatInputs();
            c=FeedbackConstants();
            c.DepthInverted=inverted;
            Fill(inputs[3],{0.4f,0.5f,0.6f,canonical});
            Fill(inputs[5],{inverted?canonical:1-canonical,0,0,0});
            checks.Expect(gpu.Run(inputs,c).secondary.Probe().a>0,"normal/reversed depth and far-zero sky accepted");
            Fill(inputs[5],{inverted?0.9f:0.1f,0,0,0});
            Fill(inputs[3],{0.4f,0.5f,0.6f,0.2f});
            ExpectFresh(gpu,checks,inputs,c,"foreground occlusion rejects background history");
        }
    // Depth confidence fades before the hard footprint veto.
    inputs=FlatInputs();
    c=FeedbackConstants();
    const float full=gpu.Run(inputs,c).primary.Probe().r;
    Fill(inputs[3],{0.4f,0.5f,0.6f,0.416f});
    const float faded=gpu.Run(inputs,c).primary.Probe().r;
    checks.Expect(faded>inputs[1].Probe().r && faded<full,"partial depth confidence scales real contribution");
}

Image Pattern(int sx=0,int sy=0)
{
    Image image;
    for (UINT y=0; y<kHeight; ++y)
        for (UINT x=0; x<kWidth; ++x)
        {
            const int xx=int(x)+sx, yy=int(y)+sy;
            const float r=((xx%5+5)%5)==2 ? 0.85f : 0.2f;
            const float g=((yy%4+4)%4)==1 ? 0.8f : 0.3f;
            const float b=((xx+2*yy)%7+7)%7 < 2 ? 0.75f : 0.15f;
            image.At(x,y)={r,g,b,1};
        }
    return image;
}

void FreshFromProxy(Inputs& inputs, float r=0.02f)
{
    inputs[1]=inputs[0];
    for (auto& p:inputs[1].pixels) { p.r+=r; p.g+=0.02f; p.b+=0.02f; p.a=0.73f; }
}

void PreviousFromProxy(Inputs& inputs, const Image& proxy)
{
    inputs[3]=proxy;
    for (auto& p:inputs[3].pixels) p.a=0.4f;
}

Image RedFence(UINT coordinate, bool horizontal=false)
{
    Image proxy({0.2f,0.35f,0.45f,1});
    for (UINT i=0;i<(horizontal?kWidth:kHeight);++i)
        proxy.At(horizontal?i:coordinate,horizontal?coordinate:i).r=0.8f;
    return proxy;
}

void NoFreshResidual(Inputs& inputs)
{
    inputs[1]=inputs[0];
    for (auto& p:inputs[1].pixels) p.a=0.73f;
}

void TestVanishingFreshOnMovingEdge(Fixture& gpu, Checks& checks)
{
    for (const auto direction:{std::array<int,2>{1,0},{-1,0},{0,1},{0,-1}})
    {
        const int dx=direction[0],dy=direction[1];
        const bool horizontal=dy!=0;
        const UINT currentCoordinate=horizontal?kProbeY:kProbeX;
        const UINT previousCoordinate=UINT(int(currentCoordinate)+(horizontal?dy:dx));
        auto c=FeedbackConstants();
        auto inputs=FlatInputs();
        inputs[0]=RedFence(currentCoordinate,horizontal);
        NoFreshResidual(inputs);
        PreviousFromProxy(inputs,RedFence(previousCoordinate,horizontal));
        Fill(inputs[2],{0,0,0,0.2f});
        for (UINT i=0;i<(horizontal?kWidth:kHeight);++i)
            inputs[2].At(horizontal?i:previousCoordinate,horizontal?previousCoordinate:i)={0.08f,0.3f,-0.3f,0.2f};
        Motion(inputs,float(dx),float(dy));
        const float w=std::min(c.FeedbackStrength,std::exp(-c.FeedbackDeltaSeconds/c.FeedbackDurationSeconds))*
                      (1-0.25f/c.FeedbackMaxAgeSeconds);
        const auto carried=gpu.Run(inputs,c);
        const float scale=0.04375f/0.3f;
        checks.PixelEquals(carried.primary.Probe(),{0.8f+0.08f*scale*w,0.35f+0.3f*scale*w,0.45f-0.3f*scale*w,0.73f},
                           "moving raw fence retains historical detail when all fresh residuals vanish");
        const float meanWeight=std::exp(-c.FeedbackDeltaSeconds/c.FeedbackDurationSeconds);
        checks.PixelEquals(carried.secondary.Probe(),{0.08f*scale*meanWeight,0.3f*scale*meanWeight,-0.3f*scale*meanWeight,0.25f},
                           "vector landing updates its mean without renewing the original birth age");
        checks.Expect(carried.primary.Probe().r-inputs[1].Probe().r>0.005f,
                      "zero-fresh valid edge has meaningful nonzero carry");
        auto freshOnly=c;
        freshOnly.FeedbackDurationSeconds=0;
        ExpectFresh(gpu,checks,inputs,freshOnly,"duration zero still forbids carry on a raw-supported moving edge");
        // Huge prior RGB cannot enlarge the signal/raw-contrast envelope.
        auto huge=inputs;
        Fill(huge[2],{100,100,-100,0.2f});
        checks.PixelEquals(gpu.Run(huge,c).primary.Probe(),{0.8f+0.04375f*w,0.35f+0.04375f*w,0.45f-0.04375f*w,0.73f},
                           "current raw contrast bounds landing without using any old RGB range");
        Motion(inputs,0,0);
        ExpectFresh(gpu,checks,inputs,c,"zero-fresh shifted fence with wrong motion still fails strict structure");
        Motion(inputs,float(dx),float(dy));
        inputs[4].At(kProbeX,kProbeY)={0,0,0,0};
        ExpectFresh(gpu,checks,inputs,c,"zero-fresh fence cannot borrow a nearby correct motion donor");
    }
    auto inputs=FlatInputs();
    auto c=FeedbackConstants();
    NoFreshResidual(inputs);
    Fill(inputs[2],{0.08f,0.3f,-0.3f,0.2f});
    Motion(inputs,1,0);
    const float w=std::min(c.FeedbackStrength,std::exp(-c.FeedbackDeltaSeconds/c.FeedbackDurationSeconds))*
                  (1-0.25f/c.FeedbackMaxAgeSeconds);
    checks.PixelEquals(gpu.Run(inputs,c).primary.Probe(),{0.4f+(0.08f*0.0625f/0.3f)*w,0.5f+0.0625f*w,0.6f-0.0625f*w,0.73f},
                       "verified flat surface retains bounded detail when fresh residual is zero");
}

void TestContinuousExpiryFade(Fixture& gpu, Checks& checks)
{
    auto c=FeedbackConstants();
    c.FeedbackDurationSeconds=4;
    c.FeedbackMaxAgeSeconds=1;
    c.FeedbackStrength=1;
    c.FeedbackDeltaSeconds=1.0f/16;
    constexpr float seed=0.08f;
    Image previousResidual,previousProxy;
    UINT previousX=6;
    float previousCarry=seed,expectedAge=0;
    bool nonzeroCarry=false,nearExpiry=false,expired=false;
    // All rows of the same moving fence are seeded together, exercising the
    // same-birth case that otherwise hard-drops as a maxAge-period cohort.
    // With unit confidence and no clipping, the ratios telescope analytically:
    // seed * exp(-elapsed/duration) * max(1-elapsed/maxAge,0).
    for (UINT frame=0;frame<=36;++frame)
    {
        const UINT x=6+frame%3;
        auto inputs=FlatInputs();
        inputs[0]=RedFence(x);
        NoFreshResidual(inputs);
        if (frame==0)
            for (UINT y=0;y<kHeight;++y) inputs[1].At(x,y).r+=seed;
        Motion(inputs,float(int(previousX)-int(x)),0);
        c.FeedbackHistoryValid=frame!=0;
        inputs[2]=frame?previousResidual:Image({},0,0);
        inputs[3]=frame?previousProxy:Image({},0,0);
        const auto output=gpu.Run(inputs,c);
        const float elapsed=float(frame)*c.FeedbackDeltaSeconds;
        const float expected=seed*std::exp(-elapsed/c.FeedbackDurationSeconds)*
                             std::max(1-elapsed/c.FeedbackMaxAgeSeconds,0.0f);
        const float carry=output.primary.At(x,kProbeY).r-inputs[0].At(x,kProbeY).r;
        checks.Expect(Near(carry,expected),"same-age moving edge residual follows continuous remaining-life fade");
        checks.PixelEquals(output.primary.At(x,kProbeY),{0.8f+expected,0.35f,0.45f,0.73f},
                           "fused moving fence agrees with the analytic expiry fade");
        const bool expiresNow=frame && expectedAge+c.FeedbackDeltaSeconds>=c.FeedbackMaxAgeSeconds;
        expectedAge=frame==0||expiresNow?0:expectedAge+c.FeedbackDeltaSeconds;
        checks.Expect(Near(output.secondary.At(x,kProbeY).a,expectedAge),
                      "remaining-life fade never refreshes a valid ancestry age");
        if (frame)
        {
            const float maxStep=seed*c.FeedbackDeltaSeconds*(1+1/c.FeedbackDurationSeconds)+kEpsilon;
            checks.Expect(carry>=-kEpsilon && carry<=previousCarry+kEpsilon && previousCarry-carry<=maxStep,
                          "every step including expiry is a small monotonic fade, never a seed-sized drop");
            if (frame==1) nonzeroCarry=carry>0.05f;
        }
        if (frame==15) nearExpiry=carry>0 && carry<0.005f;
        if (frame>=16)
        {
            expired=true;
            checks.Expect(Near(carry,0),"expired vanished-fresh seed never revives across further lifetime cycles");
        }
        for (UINT y=0;y<kHeight;++y)
            checks.Expect(Near(output.primary.At(x,y).r-inputs[0].At(x,y).r,expected),"same-age seed rows fade together continuously");
        for (UINT q=0;q<kWidth;++q)
            if (q!=x)
                checks.PixelEquals(output.primary.At(q,kProbeY),inputs[1].At(q,kProbeY),
                                   "zero-fresh moving edge fade leaves no detached background trail");
        previousCarry=carry;
        previousResidual=output.secondary;
        previousProxy=output.birthContext;
        previousX=x;
    }
    checks.Expect(nonzeroCarry && nearExpiry && expired,
                  "continuous expiry case covers meaningful zero-fresh carry, pre-expiry fade and actual expiry");
}

void TestMotionStructureAndJitter(Fixture& gpu, Checks& checks)
{
    for (const auto direction : {std::array<int,2>{1,0},{-1,0},{0,1},{0,-1}})
    {
        const int dx=direction[0],dy=direction[1];
        auto inputs=FlatInputs();
        auto c=FeedbackConstants();
        const auto previous=Pattern();
        inputs[0]=Pattern(dx,dy);
        FreshFromProxy(inputs);
        PreviousFromProxy(inputs,previous);
        Motion(inputs,float(dx),float(dy));
        auto accepted=gpu.Run(inputs,c);
        checks.Expect(accepted.secondary.Probe().a>0,"correct actual motion carries the raw proxy structure");
        checks.Expect(std::abs(accepted.primary.Probe().r-inputs[1].Probe().r)>1e-3f,"correct motion carries nonzero residual");
        Motion(inputs,0,0);
        ExpectFresh(gpu,checks,inputs,c,"same-depth wrong motion rejects raw thin-edge structure");
        // Even a nearby correct vector cannot become a donor for the bad centre.
        Motion(inputs,float(dx),float(dy));
        inputs[4].At(kProbeX,kProbeY)={0,0,0,0};
        ExpectFresh(gpu,checks,inputs,c,"nearby correct motion cannot rescue a bad centre vector");
    }
    // Same centre colour, opposite local thin-edge arrangement. A centre-only
    // colour test (and a variance box around this edge) would admit it.
    auto inputs=FlatInputs();
    auto c=FeedbackConstants();
    inputs[0].At(kProbeX-1,kProbeY)={0.9f,0.5f,0.6f,1};
    inputs[3].At(kProbeX+1,kProbeY)={0.9f,0.5f,0.6f,0.4f};
    FreshFromProxy(inputs);
    ExpectFresh(gpu,checks,inputs,c,"five-point structure rejects same-centre same-depth shifted fence");

    // Uniform fast motion survives: only spatial differences in the field penalize it.
    inputs=FlatInputs();
    Motion(inputs,4,0);
    const auto coherent=gpu.Run(inputs,c);
    checks.Expect(coherent.secondary.Probe().a>0,"coherent large object translation is not hard-rejected");
    inputs[4].At(kProbeX-1,kProbeY)={-4,0,0,0};
    const auto discontinuous=gpu.Run(inputs,c);
    checks.Expect(discontinuous.primary.Probe().r>inputs[1].Probe().r &&
                  discontinuous.primary.Probe().r<coherent.primary.Probe().r,
                  "motion discontinuity attenuates without borrowing or hard velocity rejection");

    // Actual subpixel previous-current jitter in each axis. The raw proxy is
    // an analytic plane, so its bilinear interpolation has an exact value.
    // Both jitters are inside [-0.5,0.5]; using either alone is incorrect.
    for (const auto d : {std::array<int,2>{1,0},{-1,0},{0,1},{0,-1}})
    {
        inputs=FlatInputs();
        c=FeedbackConstants();
        Image previous;
        const float dx=0.5f*float(d[0]),dy=0.5f*float(d[1]);
        for (UINT y=0;y<kHeight;++y)
            for (UINT x=0;x<kWidth;++x)
            {
                previous.At(x,y)={0.1f+0.1f*float(x),0.1f+0.1f*float(y),0.2f,1};
                inputs[0].At(x,y)={0.1f+0.1f*(float(x)+dx),0.1f+0.1f*(float(y)+dy),0.2f,1};
            }
        FreshFromProxy(inputs);
        PreviousFromProxy(inputs,previous);
        const float currentJx=-0.25f*float(d[0]),currentJy=-0.25f*float(d[1]);
        const float previousJx=0.25f*float(d[0]),previousJy=0.25f*float(d[1]);
        c.FeedbackRasterOffsetUvX=(previousJx-currentJx)/kWidth;
        c.FeedbackRasterOffsetUvY=(previousJy-currentJy)/kHeight;
        checks.Expect(gpu.Run(inputs,c).secondary.Probe().a>0,"preSR published Jprev-Jcur accepts X/Y jitter");
        c.FeedbackRasterOffsetUvX=-c.FeedbackRasterOffsetUvX;
        c.FeedbackRasterOffsetUvY=-c.FeedbackRasterOffsetUvY;
        ExpectFresh(gpu,checks,inputs,c,"current-minus-previous jitter sign is a negative control");
    }
    // No legacy jitter cancel and no second vector-Y negation is permitted.
    inputs=FlatInputs();
    c=FeedbackConstants();
    inputs[0]=Pattern(0,1);
    FreshFromProxy(inputs);
    PreviousFromProxy(inputs,Pattern());
    Motion(inputs,0,1);
    c.NativeMotionJitterCancelUvY=2.0f/kHeight;
    c.SceneGuideFlags=4;
    checks.Expect(gpu.Run(inputs,c).secondary.Probe().a>0,"already-oriented Y ignores legacy double transforms");
    Motion(inputs,0,-1);
    ExpectFresh(gpu,checks,inputs,c,"double Y negate fails structure");
    // Motion to an older frame would also look plausible on a static depth plane.
    Motion(inputs,0,2);
    ExpectFresh(gpu,checks,inputs,c,"cross-frame displacement fails current-to-published-previous identity");
}

void TestGuideRectsAndCapture(Fixture& gpu, Checks& checks)
{
    auto c=FeedbackConstants();
    auto inputs=FlatInputs();
    inputs[0]=Pattern(1,-1);
    FreshFromProxy(inputs);
    PreviousFromProxy(inputs,Pattern());
    c.MotionWidth=8; c.MotionHeight=4; c.MotionBaseX=3; c.MotionBaseY=2;
    c.DepthWidth=12; c.DepthHeight=6; c.DepthBaseX=2; c.DepthBaseY=1;
    c.MvScaleX=2; c.MvScaleY=-4;
    inputs[4]=Image({999,999,0,0},15,9);
    inputs[5]=Image({-1,0,0,0},17,10);
    for (UINT y=0;y<c.MotionHeight;++y)
        for (UINT x=0;x<c.MotionWidth;++x)
            inputs[4].At(x+c.MotionBaseX,y+c.MotionBaseY)={0.25f,0.125f,0,0};
    for (UINT y=0;y<c.DepthHeight;++y)
        for (UINT x=0;x<c.DepthWidth;++x)
            inputs[5].At(x+c.DepthBaseX,y+c.DepthBaseY)={0.4f,0,0,0};
    checks.Expect(gpu.Run(inputs,c).secondary.Probe().a>0,
                  "nonzero guide rects and asymmetric signed scales use guide dimensions");
    c.MotionBaseX=100;
    ExpectFresh(gpu,checks,inputs,c,"out-of-resource guide subrect fails closed");

    // postSR phases locate real guides at Jcur/render size, independently of the
    // zero raster offset. All unused guide texels are poison, not fallback data.
    c=FeedbackConstants();
    inputs=FlatInputs();
    inputs[0]=Pattern(1,0);
    FreshFromProxy(inputs);
    PreviousFromProxy(inputs,Pattern());
    c.NativeMotionPhaseUvX=1.0f/kWidth;
    c.NativeMotionPhaseUvY=-1.0f/kHeight;
    c.NativeDepthPhaseUvX=1.0f/kWidth;
    c.NativeDepthPhaseUvY=-1.0f/kHeight;
    Fill(inputs[4],{std::numeric_limits<float>::quiet_NaN(),0,0,0});
    Fill(inputs[5],{-1,0,0,0});
    // The centre and its four cross neighbours need actual motion samples.
    for (const auto d:{std::array<int,2>{0,0},{1,0},{-1,0},{0,1},{0,-1}})
        inputs[4].At(UINT(int(kProbeX)+1+d[0]),UINT(int(kProbeY)-1+d[1]))={1,0,0,0};
    inputs[5].At(kProbeX+1,kProbeY-1)={0.4f,0,0,0};
    checks.Expect(gpu.Run(inputs,c).secondary.Probe().a>0,"postSR uses Jcur/R guide phase with zero raster offset");
    c.FeedbackRasterOffsetUvX=2.0f/kWidth;
    ExpectFresh(gpu,checks,inputs,c,"postSR double jitter offset is a negative control");
    c.FeedbackRasterOffsetUvX=0;
    inputs[4].At(kProbeX+1,kProbeY)={0,0,0,0};
    auto flipped=inputs[4];
    for (UINT y=0;y<kHeight;++y)
        for (UINT x=0;x<kWidth;++x)
            flipped.At(x,y)=inputs[4].At(x,kHeight-1-y);
    inputs[4]=flipped;
    ExpectFresh(gpu,checks,inputs,c,"second guide row flip reads the wrong actual motion row");

    c=FeedbackConstants();
    c.FeedbackHistoryValid=0;
    for (UINT inverted:{0u,1u})
    {
        c.DepthInverted=inverted;
        inputs=FlatInputs();
        inputs[5]=Image({inverted?0.4f:0.6f,0,0,0});
        for (UINT slot=2;slot<5;++slot) inputs[slot]=Image({},0,0);
        const auto captured=gpu.Run(inputs,c);
        checks.PixelEquals(captured.birthContext.Probe(),{0.4f,0.5f,0.6f,0.4f},"capture canonical far-zero depth and raw RGB");
    }
    c.DepthInverted=1;
    inputs=FlatInputs();
    inputs[5]=Image({0.4f,0,0,0});
    for (float bad:{-1.0f,1.1f,std::numeric_limits<float>::quiet_NaN()})
    {
        inputs[5].At(kProbeX,kProbeY).r=bad;
        checks.PixelEquals(gpu.Run(inputs,c).birthContext.Probe(),{0.4f,0.5f,0.6f,-1},"capture invalid raw Z has explicit sentinel");
    }
    inputs[5]=Image({0.4f,0,0,0});
    inputs[0].At(kProbeX,kProbeY).a=std::numeric_limits<float>::quiet_NaN();
    checks.PixelEquals(gpu.Run(inputs,c).birthContext.Probe(),{0.4f,0.5f,0.6f,-1},"capture rejects nonfinite proxy alpha");
    inputs=FlatInputs();
    inputs[5]=Image({},0,0);
    checks.PixelEquals(gpu.Run(inputs,c).birthContext.Probe(),{0.4f,0.5f,0.6f,-1},"capture missing depth is invalid, not sky");
    // Capture obeys the same nonzero depth rect and phase as resolve.
    inputs=FlatInputs();
    inputs[5]=Image({-1,0,0,0},24,12);
    c.DepthBaseX=2;c.DepthBaseY=1;
    c.NativeDepthPhaseUvX=1.0f/kWidth;c.NativeDepthPhaseUvY=-1.0f/kHeight;
    inputs[5].At(kProbeX+3,kProbeY)={0.4f,0,0,0};
    checks.PixelEquals(gpu.Run(inputs,c).birthContext.Probe(),{0.4f,0.5f,0.6f,0.4f},"capture phase plus depth base maps actual guide");
}

void TestLifetimeAndMovingFence(Fixture& gpu, Checks& checks)
{
    auto c=FeedbackConstants();
    auto inputs=FlatInputs();
    c.FeedbackMaxAgeSeconds=1.25f;
    c.FeedbackDurationSeconds=0.2f;
    c.FeedbackStrength=1;
    Image residual({0.035f,0.01f,-0.015f,0.0f});
    float age=0;
    // Duration is only a decay time. It must not reset a cohort every 0.2s.
    for (UINT frame=0;frame<60;++frame)
    {
        c.FeedbackDeltaSeconds=frame%2 ? 0.07f : 0.03f;
        const bool expire=age+c.FeedbackDeltaSeconds>=c.FeedbackMaxAgeSeconds;
        inputs[2]=residual;
        const auto result=gpu.Run(inputs,c);
        age=expire?0:age+c.FeedbackDeltaSeconds;
        checks.Expect(Near(result.secondary.Probe().a,age),"per-pixel actual time across multiple decay durations");
        if (expire)
            checks.PixelEquals(result.primary.Probe(),inputs[1].Probe(),"expiry takes only fresh, without resurrection");
        residual=result.secondary;
    }
    // Different per-pixel births expire independently, never a full-screen cohort.
    inputs=FlatInputs();c=FeedbackConstants();
    inputs[2].At(kProbeX,kProbeY).a=c.FeedbackMaxAgeSeconds;
    const auto staggered=gpu.Run(inputs,c);
    checks.Expect(Near(staggered.secondary.Probe().a,0) && staggered.secondary.At(kProbeX+1,kProbeY).a>0,
                  "only expired pixel drops history");

    c=FeedbackConstants();
    c.FeedbackDurationSeconds=1;
    c.FeedbackMaxAgeSeconds=1;
    c.FeedbackStrength=0.9f;
    Image previousResidual,previousProxy;
    int previousX=3;
    float expectedAge=0;
    bool contribution=false,afterExpiry=false;
    for (UINT frame=0;frame<50;++frame)
    {
        const int x=3+int(frame%8);
        inputs=FlatInputs();
        Fill(inputs[0],{0.2f,0.2f,0.2f,1});
        for (UINT y=0;y<kHeight;++y) inputs[0].At(UINT(x),y)={0.8f,0.8f,0.8f,1};
        inputs[1]=inputs[0];
        for (UINT y=0;y<kHeight;++y) inputs[1].At(UINT(x),y).r+=frame==0?0.035f:0.02f;
        Motion(inputs,float(previousX-x),0);
        c.FeedbackHistoryValid=frame!=0;
        c.FeedbackDeltaSeconds=0.05f;
        if (frame)
        {
            inputs[2]=previousResidual;
            inputs[3]=previousProxy;
        }
        else
        {
            inputs[2]=Image({},0,0);
            inputs[3]=Image({},0,0);
        }
        const auto result=gpu.Run(inputs,c);
        const bool expired=frame && expectedAge+c.FeedbackDeltaSeconds>=c.FeedbackMaxAgeSeconds;
        expectedAge=frame==0||expired?0:expectedAge+c.FeedbackDeltaSeconds;
        checks.Expect(Near(result.secondary.At(UINT(x),kProbeY).a,expectedAge),"moving fence lifetime follows its own pixel ancestry");
        for (UINT q=0;q<kWidth;++q)
            if (int(q)!=x)
                checks.PixelEquals(result.primary.At(q,kProbeY),inputs[1].At(q,kProbeY),
                                   "moving fence leaves no detached residual after >1 second");
        if (frame==1)
            contribution=result.primary.At(UINT(x),kProbeY).r>inputs[1].At(UINT(x),kProbeY).r+0.001f;
        if (expired)
        {
            afterExpiry=true;
            checks.PixelEquals(result.primary.At(UINT(x),kProbeY),inputs[1].At(UINT(x),kProbeY),"moving expired fence cannot resurrect old appearance");
        }
        if (afterExpiry)
            checks.PixelEquals(result.primary.At(UINT(x),kProbeY),inputs[1].At(UINT(x),kProbeY),"old distinctive fence residual stays gone after expiry");
        previousResidual=result.secondary;
        previousProxy=result.birthContext;
        previousX=x;
    }
    checks.Expect(contribution,"moving fence has meaningful valid carry before expiry");
    checks.Expect(afterExpiry,"moving fence test exceeds actual 1 second maxAge");
}
void TestPresentationAnchors(Fixture& gpu, Checks& checks)
{
    auto c=FeedbackConstants();
    c.TemporalMode=2;
    auto inputs=FlatInputs();
    const auto answer=gpu.Run(inputs,c);
    const float meanWeight=std::exp(-c.FeedbackDeltaSeconds/c.FeedbackDurationSeconds);
    checks.PixelEquals(answer.secondary.Probe(),{0.02f*(1-meanWeight)+0.035f*meanWeight,
        0.02f*(1-meanWeight)+0.01f*meanWeight,0.02f*(1-meanWeight)-0.015f*meanWeight,0.25f},
        "presentation stores the observation mean at the oldest carried birth age");
    const auto displayed=answer.primary.Probe();
    checks.Expect(std::abs((displayed.r-inputs[0].Probe().r)-answer.secondary.Probe().r)>1e-4f,
                  "displayed contribution never becomes next model/history input");
    for (unsigned field=0;field<3;++field) {
        auto invalid=inputs;
        if(field==0) Motion(invalid,0,3);
        if(field==1) Fill(invalid[5],{0.9f,0,0,0});
        if(field==2) invalid[0].At(kProbeX-1,kProbeY).r=0.95f;
        ExpectFresh(gpu,checks,invalid,c,"presentation rejects wrong depth/motion/structure");
    }
    c.FeedbackDurationSeconds=1; c.FeedbackMaxAgeSeconds=1; c.FeedbackStrength=.9f;
    Image previousResidual,previousProxy;
    int previousX=3;
    bool carried=false,expired=false;
    for (UINT frame=0;frame<50;++frame) {
        const int x=3+int(frame%8);
        inputs=FlatInputs(); Fill(inputs[0],{.2f,.2f,.2f,1});
        for(UINT y=0;y<kHeight;++y) inputs[0].At(UINT(x),y)={.8f,.8f,.8f,1};
        inputs[1]=inputs[0];
        for(UINT y=0;y<kHeight;++y) inputs[1].At(UINT(x),y).r+=frame==0?.035f:.02f;
        Motion(inputs,float(previousX-x),0);
        c.FeedbackHistoryValid=frame!=0; c.FeedbackDeltaSeconds=.05f;
        if(frame) {inputs[2]=previousResidual; inputs[3]=previousProxy;}
        else {inputs[2]=Image({},0,0); inputs[3]=Image({},0,0);}
        const auto result=gpu.Run(inputs,c);
        for(UINT q=0;q<kWidth;++q) if(int(q)!=x)
            checks.PixelEquals(result.primary.At(q,kProbeY),inputs[1].At(q,kProbeY),
                               "presentation moving fence never leaves detached anchors");
        if(frame==1) carried=result.primary.At(UINT(x),kProbeY).r>inputs[1].At(UINT(x),kProbeY).r+.001f;
        if(frame>21) {
            expired=true;
            checks.PixelEquals(result.primary.At(UINT(x),kProbeY),inputs[1].At(UINT(x),kProbeY),
                               "expired original fence cannot return through fused-history feedback");
        }
        previousResidual=result.secondary;
        previousProxy=result.birthContext; previousX=x;
    }
    checks.Expect(carried && expired,"presentation anchor has nonzero carry and exceeds one-second lifetime");
}
void TestBirthReferenceDrift(Fixture& gpu, Checks& checks)
{
    auto c=FeedbackConstants();
    c.FeedbackDurationSeconds=0.911f;
    c.FeedbackMaxAgeSeconds=1.529f;
    c.FeedbackStrength=0.95f;
    c.SigmaFloor=0.017f;
    Image previousResidual, previousContext;
    bool resetObserved=false;
    for (UINT frame=0;frame<25;++frame)
    {
        auto inputs=FlatInputs();
        const float raw=0.4f+0.006f*float(frame);
        Fill(inputs[0],{raw,raw,raw,1});
        inputs[1]=inputs[0];
        for(auto& pixel:inputs[1].pixels) pixel.r+=frame==0?0.02f:0.005f;
        c.FeedbackHistoryValid=frame!=0;
        inputs[2]=frame?previousResidual:Image({},0,0);
        inputs[3]=frame?previousContext:Image({},0,0);
        const auto result=gpu.Run(inputs,c);
        if(frame && result.secondary.Probe().a==0) resetObserved=true;
        if(frame==1)
        {
            checks.Expect(result.primary.Probe().r>raw,"compatible original detail has nonzero early contribution");
            checks.Expect(Near(result.birthContext.Probe().r,0.4f),"retained residual keeps its birth colour");
        }
        if(frame>=12)
        {
            checks.Expect(Near(result.secondary.Probe().r,0.005f),"gradual appearance drift cannot carry an unrelated old edit");
            checks.PixelEquals(result.primary.Probe(),inputs[1].Probe(),"rejected old edit never reappears after gradual drift");
        }
        previousResidual=result.secondary;
        previousContext=result.birthContext;
    }
    checks.Expect(resetObserved,"birth mismatch actually resets the history before its lifetime expiry");
}

void TestFractionalSilhouette(Fixture& gpu, Checks& checks)
{
    auto c=FeedbackConstants();
    auto inputs=FlatInputs();
    // The averaged fingerprint is a perfect match, but each actual tap names
    // a different surface. Depth alone cannot prove ownership on a flat plane.
    Motion(inputs,0.5f,0);
    for(UINT y=0;y<kHeight;++y)
        for(UINT x=0;x<kWidth;++x)
            inputs[3].At(x,y)={(x&1u)?0.2f:0.6f,0.5f,0.6f,0.4f};
    ExpectFresh(gpu,checks,inputs,c,"opposite birth fingerprints cannot hide behind a matching bilinear average");
    // Low-resolution real guides, asymmetric units, nonzero rects, fractional
    // displacement and jitter. Four coherent taps must still carry real detail.
    inputs=FlatInputs();
    c.MotionWidth=8;c.MotionHeight=4;c.MotionBaseX=2;c.MotionBaseY=1;
    c.DepthWidth=8;c.DepthHeight=4;c.DepthBaseX=1;c.DepthBaseY=2;
    c.MvScaleX=2;c.MvScaleY=-3;
    c.NativeMotionPhaseUvX=0.25f/8;c.NativeDepthPhaseUvX=0.25f/8;
    c.NativeMotionPhaseUvY=-0.25f/4;c.NativeDepthPhaseUvY=-0.25f/4;
    c.FeedbackRasterOffsetUvX=-0.25f/kWidth;
    c.FeedbackRasterOffsetUvY=0.25f/kHeight;
    inputs[4]=Image({0.125f,-1.0f/12,0,0},12,8);
    inputs[5]=Image({0.4f,0,0,0},12,8);
    const auto result=gpu.Run(inputs,c);
    checks.Expect(result.secondary.Probe().a>0 && result.primary.Probe().r>inputs[1].Probe().r,
                  "fractional low-resolution guides and jitter preserve valid history contribution");
    inputs[2].At(kProbeX+1,kProbeY).a=-1;
    ExpectFresh(gpu,checks,inputs,c,"newly uncovered invalid history cannot join a character edge");
}

void TestSubpixelTexturedCarry(Fixture& gpu, Checks& checks)
{
    auto c=FeedbackConstants();
    c.SigmaFloor=0.017f;
    auto inputs=FlatInputs();
    Image birth;
    for(UINT y=0;y<kHeight;++y)
        for(UINT x=0;x<kWidth;++x)
        {
            const float v=std::clamp(0.5f+0.3f*(float(x)-float(kProbeX)),0.05f,0.95f);
            birth.At(x,y)={v,v,v,0.4f};
        }
    // The current raw picture is EXACTLY the correctly reprojected, filtered
    // previous picture. Individual texels differ at an edge; that is not a
    // colour mismatch of the reprojected surface.
    for(UINT y=0;y<kHeight;++y)
        for(UINT x=0;x<kWidth;++x)
        {
            const float v=0.5f*(birth.At(x,y).r+birth.At(std::min(x+1,kWidth-1),y).r);
            inputs[0].At(x,y)={v,v,v,1};
        }
    inputs[3]=birth;
    FreshFromProxy(inputs,0.005f);
    Fill(inputs[2],{0.035f,0.025f,0.025f,0.2f});
    Motion(inputs,0.5f,0);
    const auto result=gpu.Run(inputs,c);
    checks.Expect(result.primary.Probe().r>inputs[1].Probe().r+0.01f,
                  "correct fractional textured reprojection has nonzero detail carry");
    checks.Expect(result.secondary.Probe().a>0,
                  "filtered edge correspondence does not restart history at zero age");
    Fill(inputs[5],{0.9f,0,0,0});
    ExpectFresh(gpu,checks,inputs,c,"fractional texture still rejects a different depth surface");
}

void TestStableSurfaceWithVanishingFresh(Fixture& gpu, Checks& checks)
{
    auto c=FeedbackConstants();
    auto inputs=FlatInputs();
    NoFreshResidual(inputs);
    Fill(inputs[2],{0.035f,0.025f,-0.015f,0.2f});
    const auto result=gpu.Run(inputs,c);
    checks.Expect(result.primary.Probe().r>inputs[1].Probe().r+0.01f,
                  "stable verified surface keeps detail after fresh model residual vanishes");
    checks.Expect(result.secondary.Probe().a>0,
                  "retained detail keeps its original age when fresh detail vanishes");
    Fill(inputs[5],{0.9f,0,0,0});
    ExpectFresh(gpu,checks,inputs,c,"vanishing fresh detail cannot bypass disocclusion depth rejection");
}

void TestContinuousFractionalBirths(Fixture& gpu, Checks& checks)
{
    for(const bool productionFormats:{false,true})
    {
        auto c=FeedbackConstants();
        c.FeedbackDurationSeconds=0.820f;c.FeedbackMaxAgeSeconds=2.346f;c.FeedbackStrength=0.94f;
        auto inputs=FlatInputs();NoFreshResidual(inputs);
        for(UINT y=0;y<kHeight;++y)for(UINT x=0;x<kWidth;++x)
            inputs[2].At(x,y)={0.025f,0.02f,-0.015f,0.1f*float((x+y)%3)};
        unsigned sustained=0;float priorAge=0;
        for(unsigned frame=0;frame<45;++frame)
        {
            Motion(inputs,frame%2?-0.25f:0.25f,frame%2?0.125f:-0.125f);
            const auto result=gpu.Run(inputs,c,productionFormats);
            const auto answer=result.primary.Probe();
            const float age=result.secondary.Probe().a;
            if(frame>=5&&frame<20)
            {
                if(answer.r>inputs[1].Probe().r+0.001f)++sustained;
                checks.Expect(age>=priorAge&&age>0.2f,
                              "fractional footprint preserves oldest birth age across continuous sources");
            }
            priorAge=age;
            inputs[2]=result.secondary;inputs[3]=result.birthContext;
        }
        checks.Expect(sustained==15,productionFormats?
            "production FP16 history continuously retains detail after fresh residual disappears":
            "mixed valid births retain detail continuously instead of a one-frame pulse");
    }
}

void TestSignedResidualMean(Fixture& gpu, Checks& checks)
{
    auto c=FeedbackConstants();
    for (const float old:{0.0f,0.005f,-0.03f})
    {
        auto inputs=FlatInputs();
        FreshFromProxy(inputs,0.08f);
        Fill(inputs[2],{old,0,0,0.2f});
        const auto output=gpu.Run(inputs,c);
        const auto answer=output.primary.Probe();
        checks.Expect(answer.r>=inputs[0].Probe().r+old-kEpsilon&&answer.r<=inputs[1].Probe().r+kEpsilon&&
            std::isfinite(answer.g)&&std::isfinite(answer.b),"signed mean stays in the current and historical residual convex hull");
        auto mirrored=inputs;
        for(size_t index=0;index<mirrored[1].pixels.size();++index){
            auto& fresh=mirrored[1].pixels[index];const auto raw=mirrored[0].pixels[index];
            fresh.r=2*raw.r-fresh.r;fresh.g=2*raw.g-fresh.g;fresh.b=2*raw.b-fresh.b;
            auto& history=mirrored[2].pixels[index];history.r=-history.r;history.g=-history.g;history.b=-history.b;
        }
        const auto negative=gpu.Run(mirrored,c).primary.Probe();
        checks.PixelEquals({answer.r+negative.r,answer.g+negative.g,answer.b+negative.b,1},
            {2*inputs[0].Probe().r,2*inputs[0].Probe().g,2*inputs[0].Probe().b,1},
            "positive and negative model edits have exactly mirrored temporal response");
    }
    auto inputs=FlatInputs();
    FreshFromProxy(inputs,0.08f);
    Fill(inputs[2],{0,0,0,0.2f});
    const auto seeded=gpu.Run(inputs,c);
    inputs[2]=seeded.secondary;inputs[3]=seeded.birthContext;
    NoFreshResidual(inputs);
    checks.Expect(gpu.Run(inputs,c).primary.Probe().r>inputs[0].Probe().r+0.002f,
                  "a new model observation enters the estimator and survives the following source");
    for (const bool fp16 : {false,true})
    {
        auto mixed=FlatInputs();NoFreshResidual(mixed);
        for(auto& pixel:mixed[1].pixels)pixel.g+=0.08f;
        Fill(mixed[2],{0.035f,0,0,0.2f});
        const auto replacement=gpu.Run(mixed,c,fp16);
        checks.Expect(replacement.primary.Probe().r>mixed[1].Probe().r+0.01f,
                      "old red detail may still contribute on the replacement display");
        checks.Expect(replacement.primary.Probe().g>mixed[0].Probe().g+0.01f&&replacement.primary.Probe().g<mixed[1].Probe().g,
                      "fresh green detail enters the same symmetric vector estimator as red");
        checks.Expect(std::abs(replacement.secondary.Probe().a-0.25f)<0.001f&&
                      replacement.secondary.Probe().r>0.03f&&replacement.secondary.Probe().g>0.003f,
                      "mixed-channel observation mean retains its shared oldest birth age");
        mixed[2]=replacement.secondary;mixed[3]=replacement.birthContext;NoFreshResidual(mixed);
        const auto carried=gpu.Run(mixed,c,fp16);
        checks.Expect(carried.primary.Probe().g>mixed[0].Probe().g+0.002f,
                      "new green detail survives the next source without selecting channel maxima");
        checks.Expect(std::abs(carried.secondary.Probe().a-0.30f)<0.001f&&
                      carried.primary.Probe().r>mixed[0].Probe().r+0.01f,
                      "old red detail keeps advancing its original age alongside the new green channel");
    }
}

void TestCameraDeformation(Fixture& gpu, Checks& checks)
{
    // Camera roll and a non-uniform affine displacement. Each neighbour has
    // its OWN actual vector: translating the whole patch by the centre vector
    // gives the wrong surface despite a perfectly correct centre correspondence.
    for (const bool roll:{false,true})
    {
        auto inputs=FlatInputs();
        auto c=FeedbackConstants();
        const auto proxy=[](float x,float y) {
            return Pixel{0.12f+0.042f*x,0.14f+0.055f*y,0.22f+0.011f*x+0.017f*y,1};
        };
        for(UINT y=0;y<kHeight;++y) for(UINT x=0;x<kWidth;++x)
        {
            const float dx=float(x)-kProbeX,dy=float(y)-kProbeY;
            const float px=kProbeX+(roll?-dy:0.5f*dx+0.25f*dy);
            const float py=kProbeY+(roll?dx:0.5f*dy);
            inputs[0].At(x,y)=proxy(px,py);
            inputs[3].At(x,y)=proxy(float(x),float(y));
            inputs[3].At(x,y).a=0.4f+0.002f*(float(x)-kProbeX)+0.001f*(float(y)-kProbeY);
            inputs[5].At(x,y)={0.4f+0.002f*(px-kProbeX)+0.001f*(py-kProbeY),0,0,0};
            inputs[4].At(x,y)={px-float(x),py-float(y),0,0};
        }
        NoFreshResidual(inputs);
        Fill(inputs[2],{0.025f,0.02f,0.01f,0.1f});
        const auto output=gpu.Run(inputs,c);
        checks.Expect(output.primary.Probe().r>inputs[1].Probe().r+0.003f,
                      "camera rotation/affine motion retains verified same-surface detail with per-pixel correspondence");
        checks.Expect(output.secondary.Probe().a>0,
                      "camera deformation does not reset the entire valid patch");
        // A wrong centre may not be rescued by four correct neighbour vectors.
        inputs[4].At(kProbeX,kProbeY)={3,0,0,0};
        ExpectFresh(gpu,checks,inputs,c,"deformed patch cannot donate a neighbour correspondence to a wrong centre");
        inputs[4].At(kProbeX,kProbeY)={0,0,0,0};
        Fill(inputs[5],{0.9f,0,0,0});
        ExpectFresh(gpu,checks,inputs,c,"camera deformation still rejects incompatible geometry");
    }
}

void TestInspectorMatchesActualEvaluation(Fixture& gpu, Checks& checks)
{
    auto c=FeedbackConstants();
    const auto original=FlatInputs();
    const auto actual=gpu.Run(original,c);
    for(unsigned test=0;test<8;++test)
    {
        auto inputs=original;
        auto debug=c;debug.Mode=DlssNrDetailReuse_FeedbackInspect;debug.DebugView=1;
        Pixel colour{0.1f,0.9f,0.2f,1};
        switch(test) {
        case 1: debug.FeedbackHistoryValid=0;colour={.22f,.22f,.22f,1};break;
        case 2: Fill(inputs[2],{.02f,.02f,.02f,.2f});colour={.1f,.25f,.75f,1};break;
        case 3: Motion(inputs,100,0);colour={1,.05f,.05f,1};break;
        case 4: Fill(inputs[5],{.9f,0,0,0});colour={1,.35f,0,1};break;
        case 5: Fill(inputs[2],{.03f,0,0,c.FeedbackMaxAgeSeconds});colour={1,.9f,0,1};break;
        case 6: Motion(inputs,std::numeric_limits<float>::quiet_NaN(),0);colour={1,0,1,1};break;
        case 7: inputs[0].At(kProbeX-1,kProbeY).r=.95f;colour={0,.85f,.9f,1};break;
        }
        const auto inspected=gpu.Run(inputs,debug);
        checks.PixelEquals(inspected.primary.Probe(),colour,"inspector shows the actual evaluator's per-pixel rejection/contribution");
        checks.ImageEquals(inspected.secondary,Image{},"inspector never writes residual history");
        checks.ImageEquals(inspected.birthContext,Image{},"inspector never writes birth/depth context");
    }
    auto debug=c;debug.Mode=DlssNrDetailReuse_FeedbackInspect;debug.DebugView=4;
    auto expected=original[1].Probe();expected.a=1;
    checks.PixelEquals(gpu.Run(original,debug).primary.Probe(),expected,"inspector fresh image is same-source actual model answer");
    debug.DebugView=5;expected=actual.primary.Probe();expected.a=1;
    checks.PixelEquals(gpu.Run(original,debug).primary.Probe(),expected,"inspector fused image equals production resolve pixels");
    debug.DebugView=2;
    const float age=actual.secondary.Probe().a/c.FeedbackMaxAgeSeconds;
    checks.PixelEquals(gpu.Run(original,debug).primary.Probe(),{age,1-age,.1f,1},"inspector age uses original ancestry, not CPU frame age");
}

void TestTunableThresholds(Fixture& gpu, Checks& checks)
{
    auto inputs=FlatInputs();NoFreshResidual(inputs);
    auto c=FeedbackConstants();c.DepthTolerance=.01f;
    Fill(inputs[5],{.45f,0,0,0});
    ExpectFresh(gpu,checks,inputs,c,"tight adjustable depth rejects disagreement");
    c.DepthTolerance=1;
    checks.Expect(gpu.Run(inputs,c).primary.Probe().r>inputs[1].Probe().r+.001f,
                  "depth slider one bypasses pixel depth disagreement");
    inputs=FlatInputs();NoFreshResidual(inputs);
    for(auto& pixel:inputs[0].pixels)pixel.r+=.1f;
    NoFreshResidual(inputs);c=FeedbackConstants();c.ClipGamma=.03f;
    ExpectFresh(gpu,checks,inputs,c,"tight adjustable birth-colour threshold");
    c.ClipGamma=.4f;
    checks.Expect(gpu.Run(inputs,c).primary.Probe().r>inputs[1].Probe().r+.001f,
                  "colour slider widens the actual production acceptance limit");
    inputs=FlatInputs();NoFreshResidual(inputs);
    inputs[4].At(kProbeX,kProbeY)={1.1f,0,0,0};c=FeedbackConstants();c.MotionReject=.5f;
    ExpectFresh(gpu,checks,inputs,c,"tight adjustable motion curvature threshold");
    c.MotionReject=4;
    checks.Expect(gpu.Run(inputs,c).primary.Probe().r>inputs[1].Probe().r+.001f,
                  "motion slider widens actual production curvature tolerance");
}

// Three frames: the long ancestry may still carry F0's edit after F1,
// but the short filter must compare against F1, not the ancestry's birth.
void TestStabilizerUsesAdjacentRaw(Fixture& gpu, Checks& checks)
{
    auto c = FeedbackConstants();
    c.ClipGamma = .125f;
    c.FeedbackStabilitySeconds = .2f;
    c.FeedbackStabilityFloor = 1.f;
    c.FeedbackStabilityClamp = .015625f;
    auto in = FlatInputs();
    const auto frame = [&](float raw, float fresh, bool valid) {
        Fill(in[0], {raw,raw,raw,1});
        Fill(in[1], {fresh,fresh,fresh,.75f});
        c.FeedbackHistoryValid = valid;
        return gpu.Run(in, c, true);
    };
    const auto advance = [&](const DispatchResult& previous) {
        in[2] = previous.secondary;
        in[3] = previous.birthContext;
        in[6] = previous.stability;
        in[7] = previous.adjacentRaw;
    };
    in[2] = in[3] = in[6] = in[7] = Image({}, 0, 0);
    advance(frame(.25f, .28125f, false));
    for(const float raw:{.265625f,.28125f,.296875f,.3125f}){
        const auto stepped=frame(raw,raw+.03125f,true);
        checks.Expect(Near(stepped.birthContext.Probe().r,.25f)&&stepped.secondary.Probe().a>0,
            "small adjacent raw changes preserve the distinct long birth");
        checks.Expect(Near(stepped.adjacentRaw.Probe().r,raw),
            "adjacent source stores current raw instead of the birth or displayed answer");
        advance(stepped);
    }
    Fill(in[2],{.03125f,.03125f,.03125f,.2f});
    Fill(in[3],{.25f,.25f,.25f,.4f});
    Fill(in[6],{.03125f,.03125f,.03125f,.1f});
    Fill(in[7],{.3125f,.3125f,.3125f,.0009765625f});
    const auto same = frame(.3125f, .296875f, true);
    checks.Expect(same.primary.Probe().r>.296875f+.002f && same.stability.Probe().a > .02f,
        "short stability follows adjacent raw even when the long birth differs");
    const auto cut = frame(.25f, .234375f, true);
    checks.Expect(Near(cut.primary.Probe().r, .234375f) && Near(cut.stability.Probe().a, 0),
        "return to birth colour does not hide an adjacent-frame appearance change");
}

void TestIndependentStabilizer(Fixture& gpu, Checks& checks)
{
    auto inputs=FlatInputs();
    auto c=FeedbackConstants();c.FeedbackDurationSeconds=0;c.FeedbackStrength=0;
    c.FeedbackStabilitySeconds=.2f;c.FeedbackStabilityFloor=.8f;c.FeedbackStabilityClamp=.08f;
    inputs[6]=Image({-.02f,-.02f,-.02f,0});
    Fill(inputs[2],{-.02f,-.02f,-.02f,.2f});
    const float nan=std::numeric_limits<float>::quiet_NaN();
    for(unsigned test=0;test<2;++test)
    {
        auto sample=inputs;
        if(test==1)Fill(sample[5],{.416f,0,0,0}); // Valid surface, softly reduced depth confidence.
        const auto result=gpu.Run(sample,c,true);
        checks.Expect(result.primary.Probe().r<sample[1].Probe().r-.008f,
                      "positive low-trust floor filters NR within valid correspondence "+std::to_string(test));
        checks.Expect(result.primary.Probe().a==sample[1].Probe().a||std::abs(result.primary.Probe().a-sample[1].Probe().a)<.001f,
                      "stabilization preserves current alpha");
        checks.Expect(result.stability.Probe().a>0&&result.stability.Probe().a<c.FeedbackStabilitySeconds,
                      "filtered residual carries bounded mean lag");
        const auto a=sample[1].Probe(),b=sample[0].Probe();
        checks.Expect(result.secondary.Probe().r>=-.0201f&&result.secondary.Probe().r<=a.r-b.r+.0001f&&
            result.adjacentRaw.Probe().a>=0,"responsive mean and raw observation energy remain finite and bounded");
    }
    auto fallback=inputs;Fill(fallback[5],{.416f,0,0,0});Fill(fallback[6],{-.02f,-.02f,-.02f,.17f});c.FeedbackStabilityFloor=0;
    const float noFloor=gpu.Run(fallback,c).primary.Probe().r;
    c.FeedbackStabilityFloor=1;
    const float shortWindowResult=gpu.Run(fallback,c).primary.Probe().r;
    checks.Expect(shortWindowResult<noFloor-.005f,"user floor strengthens low-confidence valid surface stabilization");
    c.FeedbackStabilitySeconds=.8f;
    checks.Expect(gpu.Run(fallback,c).primary.Probe().r<shortWindowResult,
                  "larger user stability time gives stronger temporal smoothing");
    {
        auto settled=inputs;Fill(settled[6],{-.02f,-.02f,-.02f,.3958478f});
        Fill(settled[7],{.4f,.5f,.6f,.0004f});
        auto highRate=c;highRate.FeedbackDeltaSeconds=1.0f/120;
        highRate.Mode=DlssNrDetailReuse_FeedbackInspect;highRate.DebugView=6;
        highRate.FeedbackStabilitySeconds=.4f;const float mediumWindow=gpu.Run(settled,highRate).primary.Probe().r;
        Fill(settled[6],{-.02f,-.02f,-.02f,1.9958362f});
        highRate.FeedbackStabilitySeconds=2;const float longWindow=gpu.Run(settled,highRate).primary.Probe().r;
        checks.Expect(std::abs(mediumWindow-.97938218f)<2e-5f&&std::abs(longWindow-.9958420f)<2e-5f,
                      "settled stability weights retain the exponential time constants at 120 source Hz");
        checks.Expect(mediumWindow>.97f&&longWindow>mediumWindow+.01f,
                      "long stability slider keeps its time semantics at 120 source Hz without a fixed weight plateau");
    }
    c.FeedbackStabilityClamp=.005f;
    checks.Expect(gpu.Run(fallback,c).primary.Probe().r<fallback[1].Probe().r-.005f,
                  "statistical evidence preserves strong denoising despite a tight steady-state envelope");
    c.FeedbackStabilitySeconds=0;
    checks.ImageEquals(gpu.Run(fallback,c).primary,fallback[1],"stability zero is exactly fresh with long carry disabled");
    c.FeedbackStabilitySeconds=.2f;c.FeedbackHistoryValid=0;
    for(unsigned slot=2;slot<7;++slot)fallback[slot]=Image({},0,0);
    checks.ImageEquals(gpu.Run(fallback,c).primary,fallback[1],"first frame never samples unbound stability or guide resources");
    fallback=inputs;c.FeedbackHistoryValid=1;c.FeedbackDeltaSeconds=.2f;
    checks.ImageEquals(gpu.Run(fallback,c).primary,fallback[1],"expired source time does not reuse stabilization");
    c.FeedbackDeltaSeconds=.05f;Fill(fallback[6],{nan,0,0,0});
    checks.ImageEquals(gpu.Run(fallback,c).primary,fallback[1],"nonfinite filtered history cannot contaminate current NR");
    fallback=inputs;c.FeedbackStabilityFloor=1;c.FeedbackStabilityClamp=.05f;
    Fill(fallback[6],{10000,-10000,10000,0});
    const auto bounded=gpu.Run(fallback,c).primary.Probe();
    checks.Expect(std::abs(bounded.r-inputs[1].Probe().r)<=.05001f&&std::abs(bounded.g-inputs[1].Probe().g)<=.05001f,
                  "malformed huge old residual is bounded in both directions");
    // Appearance changes remain immediate even with the maximum user floor.
    for(auto& pixel:fallback[0].pixels)pixel={.9f,.05f,.02f,1};NoFreshResidual(fallback);
    checks.ImageEquals(gpu.Run(fallback,c).primary,fallback[1],"newly revealed surface does not inherit unrelated NR edits");

    // A real moving, asymmetric edit must land at its actual correspondence.
    fallback=inputs;Fill(fallback[6],{0,0,0,0});NoFreshResidual(fallback);
    fallback[6].At(kProbeX-1,kProbeY)={.04f,0,0,0};Motion(fallback,-1,0);
    checks.Expect(gpu.Run(fallback,c).primary.Probe().r>fallback[1].Probe().r+.01f,
                  "independent stabilization follows valid motion instead of stationary screen history");
    // Noise alternates in sign; one-sided stronger-only history cannot solve it.
    fallback=inputs;c.FeedbackStabilitySeconds=.2f;c.FeedbackDeltaSeconds=.016f;
    double inputEnergy=0,outputEnergy=0;
    for(unsigned n=0;n<90;++n)
    {
        const float residual=n%2?.04f:-.04f;
        Fill(fallback[1],{.4f+residual,.5f+residual,.6f+residual,.73f});
        c.FeedbackHistoryValid=n!=0;
        const auto result=gpu.Run(fallback,c,true);
        if(n>15){inputEnergy+=residual*residual;const double d=result.primary.Probe().r-.4;outputEnergy+=d*d;}
        fallback[2]=result.secondary;fallback[3]=result.birthContext;
        fallback[6]=result.stability;fallback[7]=result.adjacentRaw;
        checks.Expect(result.stability.Probe().a<=c.FeedbackStabilitySeconds+.001f,
                      "long noise sequence has bounded lag without periodic full-screen resets");
    }
    checks.Expect(outputEnergy<inputEnergy*.2,"signed NR noise/lighting oscillation energy falls by more than 80 percent in fixture");
    c.Mode=DlssNrDetailReuse_FeedbackInspect;c.DebugView=6;
    const auto inspected=gpu.Run(fallback,c);
    checks.Expect(inspected.primary.Probe().r>.1f,"stability weight inspector uses the same production evaluator");
    checks.ImageEquals(inspected.stability,Image{},"inspector never writes filtered history");
    checks.ImageEquals(inspected.adjacentRaw,Image{},"inspector never writes adjacent source history");
}

void TestStabilizerSurfaceAdmission(Fixture& gpu, Checks& checks)
{
    const float nan=std::numeric_limits<float>::quiet_NaN();
    const float inf=std::numeric_limits<float>::infinity();
    // Match the reported settings, keep long detail enabled, and repeat at the
    // maximum floor. Identical raw RGB deliberately defeats an appearance veto.
    for(const bool fp16:{false,true}) for(const float floor:{.6f,1.0f})
    {
        auto c=FeedbackConstants();
        c.FeedbackDurationSeconds=.177f;c.FeedbackDeltaSeconds=.016f;
        c.FeedbackStabilitySeconds=.4f;c.FeedbackStabilityClamp=.075f;c.FeedbackStabilityFloor=floor;
        for(unsigned test=0;test<34;++test)
        {
            auto inputs=FlatInputs();NoFreshResidual(inputs);
            Fill(inputs[2],{.001f,.001f,.001f,.2f});
            inputs[6]=Image({.06f,-.04f,.03f,.1f});
            auto sample=c;
            const std::string name="surface admission "+std::to_string(test)+
                (fp16?" FP16":" FP32")+" floor="+std::to_string(floor);
            switch(test)
            {
            case 0: Fill(inputs[5],{.9f,0,0,0});break; // Same-colour background vs old grass.
            case 1: Fill(inputs[5],{nan,0,0,0});break;
            case 2: Fill(inputs[5],{inf,0,0,0});break;
            case 3: Fill(inputs[5],{-.1f,0,0,0});break;
            case 4: Fill(inputs[5],{1.1f,0,0,0});break;
            case 5: inputs[5]=Image({},0,0);break;
            case 6: Fill(inputs[3],{.4f,.5f,.6f,nan});break;
            case 7: Fill(inputs[3],{.4f,.5f,.6f,inf});break;
            case 8: Fill(inputs[3],{.4f,.5f,.6f,-1});break;
            case 9: Fill(inputs[3],{.4f,.5f,.6f,1.1f});break;
            case 10: Motion(inputs,nan,0);break;
            case 11: Motion(inputs,0,inf);break;
            case 12: Motion(inputs,-inf,0);break;
            case 13: Motion(inputs,10000,0);break;
            case 14: Motion(inputs,-10000,0);break;
            case 15: Motion(inputs,0,10000);break;
            case 16: inputs[4]=Image({},0,0);break;
            case 17: sample.NativeMotionPhaseUvX=2;break;
            case 18: sample.NativeDepthPhaseUvX=2;break;
            case 19: sample.FeedbackRasterOffsetUvX=2;break;
            case 20: sample.DepthTolerance=nan;break;
            case 21: sample.DepthTolerance=-.05f;break;
            case 22: sample.DepthTolerance=inf;break;
            case 31: sample.FeedbackDurationSeconds=0; inputs[7]=Image({},0,0);break; // Missing paired source.
            case 32: sample.FeedbackDurationSeconds=0; inputs[7]=Image({.4f,.5f,.6f,0},kWidth-1,kHeight);break;
            case 33: sample.FeedbackDurationSeconds=0; inputs[7].At(kProbeX,kProbeY).g=nan;break;
            case 27: case 28: case 29: case 30:
                // Finite, in-bounds centre MV disagrees with its neighbours.
                // Explicit motion rejection must not become a soft floor case.
                inputs[4].At(kProbeX,kProbeY)={1.1f,0,0,0};sample.MotionReject=.5f;
                if(test==28)sample.FeedbackDurationSeconds=0;
                if(test==29)sample.FeedbackStrength=0;
                if(test==30)Fill(inputs[2],{.001f,.001f,.001f,sample.FeedbackMaxAgeSeconds});
                break;
            default:
                // Four-way fractional footprint: ANY positive tap on another
                // surface must veto before blending, even with matching RGB.
                Motion(inputs,.5f,.5f);
                inputs[3].At(kProbeX+((test-23)&1u),kProbeY+((test-23)>>1u)).a=.9f;
                break;
            }
            const auto result=gpu.Run(inputs,sample,fp16);
            const auto fresh=inputs[1].Probe(),proxy=inputs[0].Probe();
            const auto answer=result.primary.Probe(),stable=result.stability.Probe();
            const float epsilon=fp16?.001f:kEpsilon;
            checks.Expect(std::abs(answer.r-fresh.r)<epsilon&&std::abs(answer.g-fresh.g)<epsilon&&
                          std::abs(answer.b-fresh.b)<epsilon&&std::abs(answer.a-fresh.a)<epsilon,
                          name+" rejects old grass rather than filtering same-colour background");
            checks.Expect(stable.a==0&&std::abs(stable.r-(fresh.r-proxy.r))<epsilon&&
                          std::abs(stable.g-(fresh.g-proxy.g))<epsilon&&std::abs(stable.b-(fresh.b-proxy.b))<epsilon,
                          name+" reseeds independent residual and lag from this source");
            checks.Expect(result.secondary.Probe().a==0,name+" cannot renew long-detail ancestry");
            auto inspect=sample;inspect.Mode=DlssNrDetailReuse_FeedbackInspect;inspect.DebugView=6;
            checks.PixelEquals(gpu.Run(inputs,inspect).primary.Probe(),{0,.15f,1,1},name+" inspector has zero stability weight");
        }
        // A weak wrong-surface edit used to pull a detailed fresh model answer
        // down toward the background, even though long detail correctly refused it.
        auto inputs=FlatInputs();FreshFromProxy(inputs,.08f);
        Fill(inputs[2],{.001f,0,0,.2f});inputs[6]=Image({.001f,0,0,.1f});
        Fill(inputs[5],{.9f,0,0,0});
        const auto answer=gpu.Run(inputs,c,fp16).primary.Probe();
        checks.Expect(std::abs(answer.r-inputs[1].Probe().r)<.001f,
                      "NR fresh detail is not diluted by weak history from a different surface");
    }
    // The floor remains a real user control below the hard rejection threshold.
    auto inputs=FlatInputs();NoFreshResidual(inputs);inputs[6]=Image({.04f,0,0,.3f});
    Fill(inputs[2],{0,0,0,.2f});Fill(inputs[3],{.4f,.5f,.6f,.416f});
    auto c=FeedbackConstants();c.FeedbackDurationSeconds=0;c.FeedbackStrength=0;
    c.FeedbackStabilitySeconds=.4f;c.FeedbackStabilityClamp=.075f;
    c.FeedbackStabilityFloor=0;
    const float withoutFloor=gpu.Run(inputs,c).primary.Probe().r;
    c.FeedbackStabilityFloor=.6f;
    const auto admitted=gpu.Run(inputs,c);
    checks.Expect(admitted.primary.Probe().r>withoutFloor+.005f,
                  "valid same-surface low-confidence correspondence retains the user floor");
    checks.Expect(admitted.stability.Probe().a>0,"valid low-confidence surface carries real filtered lag");
}

void TestStrongTemporalNoise(Fixture& gpu, Checks& checks)
{
    for(const bool colourNoise:{false,true})for(const bool longEnabled:{false,true}){
        auto constants=FeedbackConstants();
        constants.FeedbackDurationSeconds=longEnabled?.262f:0.0f;
        constants.FeedbackStrength=longEnabled?.95f:0.0f;
        constants.FeedbackMaxAgeSeconds=.561f;
        constants.FeedbackStabilitySeconds=1.5f;
        constants.FeedbackStabilityFloor=1;
        constants.FeedbackStabilityClamp=.041f;
        constants.FeedbackDeltaSeconds=1.0f/60;
        auto inputs=FlatInputs();
        double inputEnergy=0,outputEnergy=0,mean=0;
        unsigned samples=0;
        for(unsigned frame=0;frame<110;++frame){
            const float noise=frame%2?.12f:-.12f;
            const float greenNoise=colourNoise?-noise*(.2126f+.0722f)/.7152f:noise;
            Fill(inputs[1],{.4f+noise,.5f+greenNoise,.6f+noise,.73f});
            constants.FeedbackHistoryValid=frame!=0;
            const auto result=gpu.Run(inputs,constants,true);
            if(frame>=30){const double residual=result.primary.Probe().r-.4;
                inputEnergy+=noise*noise;outputEnergy+=residual*residual;mean+=residual;++samples;}
            const auto statistic=result.adjacentRaw.Probe();
            checks.Expect(std::isfinite(statistic.a)&&statistic.a>=0&&statistic.a<=.02f,
                "strong NR noise keeps bounded RGB observation energy in FP32");
            inputs[2]=result.secondary;inputs[3]=result.birthContext;
            inputs[6]=result.stability;inputs[7]=result.adjacentRaw;
        }
        std::cout<<"NR noise fixture chroma="<<colourNoise<<" long="<<longEnabled
                 <<" energyRatio="<<outputEnergy/inputEnergy<<" mean="<<mean/samples<<std::endl;
        checks.Expect(outputEnergy<inputEnergy*.04,"large final stability suppresses severe luminance and equal-luma chroma flicker amplitude by over 80 percent");
        checks.Expect(std::abs(mean/samples)<.005,"signed temporal estimator does not retain alternating model peaks as a brightness bias");
        Fill(inputs[0],{.8f,.8f,.8f,1});Fill(inputs[1],{.77f,.77f,.77f,.73f});
        const auto changed=gpu.Run(inputs,constants,true);
        checks.Expect(std::abs(changed.primary.Probe().r-.77f)<.001&&changed.stability.Probe().a==0,
            "a real raw appearance step stays immediate even at large final stability");
    }
}

void TestTemporalWarmup(Fixture& gpu, Checks& checks)
{
    auto constants=FeedbackConstants();
    constants.FeedbackDurationSeconds=constants.FeedbackStrength=0;
    constants.FeedbackDeltaSeconds=1.0f/60;
    constants.FeedbackStabilitySeconds=1.5f;
    constants.FeedbackStabilityFloor=1;
    constants.FeedbackStabilityClamp=.041f;
    auto inputs=FlatInputs();
    constexpr float expectedMean[]{-.125f,0,-.125f/3,0};
    for(unsigned frame=0;frame<4;++frame){
        const float noise=frame%2?.125f:-.125f;
        Fill(inputs[1],{.4f+noise,.5f+noise,.6f+noise,.73f});
        constants.FeedbackHistoryValid=frame!=0;
        const auto result=gpu.Run(inputs,constants);
        checks.Expect(std::abs(result.stability.Probe().r-expectedMean[frame])<2e-5f,
            "temporal warmup removes the first noisy sample bias by a signed arithmetic mean");
        checks.Expect(std::abs(result.stability.Probe().a-float(frame)/120)<2e-5f,
            "warmup mean lag follows source time instead of a fabricated history count");
        inputs[2]=result.secondary;inputs[3]=result.birthContext;
        inputs[6]=result.stability;inputs[7]=result.adjacentRaw;
    }
}

void TestEmptyResidualChannel(Fixture& gpu, Checks& checks)
{
    auto inputs=FlatInputs();
    Fill(inputs[0],{.4f,.5f,0,1});
    Fill(inputs[1],{.4f,.5f,0,.73f});
    Fill(inputs[2],{.04f,0,0,.2f});
    Fill(inputs[3],{.4f,.5f,0,.4f});
    const auto result=gpu.Run(inputs,FeedbackConstants());
    checks.Expect(result.primary.Probe().r>.41f&&Near(result.primary.Probe().g,.5f)&&Near(result.primary.Probe().b,0),
        "a zero residual channel with zero carry budget does not erase valid detail in other RGB channels");
}

void TestModelGuidePhase(Fixture& gpu, Checks& checks)
{
    auto constants=FeedbackConstants();
    constants.Mode=DlssNrDetailReuse_PrepareSceneGuides;
    constants.DepthWidth=constants.MotionWidth=8;
    constants.DepthHeight=constants.MotionHeight=4;
    constants.DepthBaseX=1;constants.DepthBaseY=2;
    constants.MotionBaseX=3;constants.MotionBaseY=1;
    constants.MvScaleX=2;constants.MvScaleY=-3;
    auto inputs=FlatInputs();
    inputs[3]=Image({0,0,0,0},14,10);
    inputs[4]=Image({0,0,0,0},14,10);
    for(UINT sourceY=0;sourceY<4;++sourceY)for(UINT sourceX=0;sourceX<8;++sourceX){
        inputs[3].At(sourceX+3,sourceY+1)={float(sourceX+1)*.125f,-float(sourceY+1)*.125f,0,0};
        inputs[4].At(sourceX+1,sourceY+2)={.125f+float(sourceY*8+sourceX)*.015625f,0,0,0};
    }
    for(const float jitter:{-.5f,.5f})for(unsigned mapping=0;mapping<8;++mapping){
        constants.NativeDepthPhaseUvX=constants.NativeMotionPhaseUvX=jitter/8;
        constants.NativeDepthPhaseUvY=constants.NativeMotionPhaseUvY=-jitter/4;
        constants.SceneGuideFlags=mapping;
        const auto result=gpu.Run(inputs,constants);
        for(UINT outputY=0;outputY<kHeight;++outputY)for(UINT outputX=0;outputX<kWidth;++outputX){
            const float shiftedX=(float(outputX)+.5f)/kWidth+jitter/8;
            const float shiftedY=(float(outputY)+.5f)/kHeight-jitter/4;
            const auto sourceX=UINT(std::clamp(int(std::floor(shiftedX*8)),0,7));
            const auto sourceY=UINT(std::clamp(int(std::floor(shiftedY*4)),0,3));
            const UINT depthY=(mapping&1)?3-sourceY:sourceY;
            const UINT motionY=(mapping&2)?3-sourceY:sourceY;
            const auto sourceDepth=inputs[4].At(sourceX+1,depthY+2).r;
            const auto sourceMotion=inputs[3].At(sourceX+3,motionY+1);
            const bool inside=shiftedX>=0&&shiftedX<1&&shiftedY>=0&&shiftedY<1;
            checks.PixelEquals(result.primary.At(outputX,outputY),
                {inside?sourceMotion.r*2*kWidth/8:65504.f,inside?sourceMotion.g*((mapping&4)?3.f:-3.f)*kHeight/4:65504.f,
                 inside?sourceDepth:-1.f,inside?1.f:0.f},
                "model guide phase precedes independent rect/row normalization and converts motion to work pixels");
            checks.PixelEquals(result.secondary.At(outputX,outputY),{inside?sourceDepth:0.f,0,0,0},
                "model DeviceZ uses the same current jitter phase without depth inversion or history-phase reuse");
        }
    }
}

void TestTaaLowResolutionModelGuides(Fixture& gpu, Checks& checks)
{
    constexpr UINT sourceW=1536,sourceH=960;
    auto constants=FeedbackConstants();
    constants.Mode=DlssNrDetailReuse_PrepareSceneGuides;
    constants.WorkWidth=constants.MotionWidth=constants.DepthWidth=sourceW;
    constants.WorkHeight=constants.MotionHeight=constants.DepthHeight=sourceH;
    constants.ScenePictureWidth=2560;constants.ScenePictureHeight=1600;
    constants.NativeMotionPhaseUvX=constants.NativeDepthPhaseUvX=.25f/sourceW;
    constants.NativeMotionPhaseUvY=constants.NativeDepthPhaseUvY=-.25f/sourceH;
    constants.MvScaleX=constants.MvScaleY=1;
    Inputs inputs{};
    inputs[3]=Image({},sourceW,sourceH);inputs[4]=Image({},sourceW,sourceH);
    for(UINT y=0;y<sourceH;++y)for(UINT x=0;x<sourceW;++x){
        inputs[3].At(x,y)={1.f+float(x%5)*.125f,-2.f-float(y%7)*.125f,0,0};
        inputs[4].At(x,y)={.25f+float(y%11)/32,0,0,0};
    }
    const auto result=gpu.Run(inputs,constants);
    for(UINT y:{0u,1u,479u,958u,959u})for(UINT x:{0u,1u,767u,1534u,1535u}){
        const auto motion=inputs[3].At(x,y);
        const auto depth=inputs[4].At(x,y).r;
        checks.PixelEquals(result.primary.At(x,y),{motion.r,motion.g,depth,1},
            "0.6 TAA source guides reach NR in work pixels with unit scale, not another 0.6 multiplication");
        checks.PixelEquals(result.secondary.At(x,y),{depth,0,0,0},
            "0.6 TAA model keeps current source DeviceZ and its independent jitter address phase");
    }
}
} // namespace

int main(int argc, char** argv)
{
    try
    {
        Fixture gpu(argc==3 && std::string(argv[1])=="--shader" ? std::filesystem::path(argv[2]) : std::filesystem::path{});
        Checks checks;
        TestWeightsAndLanding(gpu,checks);
        TestFirstFrameAndInvalidValues(gpu,checks);
        TestStrictFootprintAndDepth(gpu,checks);
        TestVanishingFreshOnMovingEdge(gpu,checks);
        TestContinuousExpiryFade(gpu,checks);
        TestMotionStructureAndJitter(gpu,checks);
        TestGuideRectsAndCapture(gpu,checks);
        TestLifetimeAndMovingFence(gpu,checks);
        TestPresentationAnchors(gpu,checks);
        TestBirthReferenceDrift(gpu,checks);
        TestFractionalSilhouette(gpu,checks);
        TestSubpixelTexturedCarry(gpu,checks);
        TestStableSurfaceWithVanishingFresh(gpu,checks);
        TestContinuousFractionalBirths(gpu,checks);
        TestSignedResidualMean(gpu,checks);
        TestCameraDeformation(gpu,checks);
        TestInspectorMatchesActualEvaluation(gpu,checks);
        TestTunableThresholds(gpu,checks);
        TestIndependentStabilizer(gpu,checks);
        TestStabilizerUsesAdjacentRaw(gpu,checks);
        TestStabilizerSurfaceAdmission(gpu,checks);
        TestStrongTemporalNoise(gpu,checks);
        TestTemporalWarmup(gpu,checks);
        TestEmptyResidualChannel(gpu,checks);
        TestModelGuidePhase(gpu,checks);
        TestTaaLowResolutionModelGuides(gpu,checks);
        return checks.Finish();
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << std::endl;
        return 2;
    }
}


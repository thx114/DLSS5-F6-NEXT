// Captured pixel programs through the production chain, compared with a
// separate full-resolution draw of the same original bytecode and inputs.
#define NOMINMAX
#include <inputs/taa/TaaConsumerDx11.h>
#include <DirectXPackedVector.h>
#include <d3d11sdklayers.h>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <stdexcept>

using Microsoft::WRL::ComPtr;
namespace {
unsigned checks = 0;
void Check(bool ok, const std::string& message) { ++checks; if (!ok) throw std::runtime_error(message); }
void Hr(HRESULT hr) { Check(SUCCEEDED(hr), "D3D11 API HRESULT=" + std::to_string(hr)); }
struct Pixel { float r, g, b, a; };
struct Texture {
    ComPtr<ID3D11Texture2D> image;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11RenderTargetView> rtv;
};
struct Inputs {
    std::array<Texture, 5> auxiliary;
    ComPtr<ID3D11Buffer> constants;
};
constexpr unsigned kLowW = 160, kLowH = 100, kHighW = 256, kHighH = 160;
constexpr uint64_t kCopy = 0xF3EF7F661CCF4AF8ull;
constexpr std::array<uint64_t, 5> kPrograms {kCopy, 0x12F210414D64E9FAull,
    0xC6C24FFD1D81EA68ull, 0xF53695D73045BEB1ull, 0x46D956B3CEB5978Cull};

std::vector<char> Load(const std::filesystem::path& directory, const char* stage, uint64_t hash) {
    char name[48]; std::snprintf(name, sizeof(name), "%s-%016llX.dxbc", stage, static_cast<unsigned long long>(hash));
    std::ifstream file(directory / name, std::ios::binary);
    Check(bool(file), "missing captured shader " + std::string(name));
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

struct Fixture {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11InfoQueue> debug;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> copy;
    ComPtr<ID3D11InputLayout> layout;
    ComPtr<ID3D11Buffer> vertices, vertexConstants;
    ComPtr<ID3D11DepthStencilState> noDepth;
    ComPtr<ID3D11RasterizerState> noCull;
    std::array<ComPtr<ID3D11SamplerState>, 3> samplers;
    taa::consumer::Program vertexProgram, copyProgram;

    ComPtr<ID3D11Buffer> Buffer(const void* data, UINT size, UINT bind) {
        D3D11_BUFFER_DESC desc {}; desc.ByteWidth = size; desc.BindFlags = bind;
        D3D11_SUBRESOURCE_DATA init {data, 0, 0}; ComPtr<ID3D11Buffer> result;
        Hr(device->CreateBuffer(&desc, &init, &result));
        taa::constants::Initialized(result.Get(), &init);
        return result;
    }
    explicit Fixture(const std::filesystem::path& directory, const std::filesystem::path& procedural = {}) {
        Hr(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_DEBUG,
            nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context));
        Hr(device.As(&debug));
        const auto v = Load(procedural.empty() ? directory : procedural, "VS",
            procedural.empty() ? 0xF1CE86BC0BE8E994ull : 0x98109168E808BC85ull);
        const auto p = Load(directory, "PS", kCopy);
        vertexProgram = taa::consumer::Describe(v.data(), v.size(), taa::consumer::ProgramStage::Vertex);
        copyProgram = taa::consumer::Describe(p.data(), p.size(), taa::consumer::ProgramStage::Pixel);
        Check(vertexProgram.supported && copyProgram.supported, "captured fullscreen/copy declaration");
        Hr(device->CreateVertexShader(v.data(), v.size(), nullptr, &vs));
        Hr(device->CreatePixelShader(p.data(), p.size(), nullptr, &copy));
        if(procedural.empty()) {
        const D3D11_INPUT_ELEMENT_DESC element {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
            D3D11_INPUT_PER_VERTEX_DATA, 0};
        Hr(device->CreateInputLayout(&element, 1, v.data(), v.size(), &layout));
        taa::fullscreen::Remember(layout.Get(), &element, 1);
        const float points[] {-1, -1, 3, -1, -1, 3};
        vertices = Buffer(points, sizeof(points), D3D11_BIND_VERTEX_BUFFER);
        std::array<float, 64> matrix {}; matrix[48] = 1; matrix[53] = -1; matrix[62] = .5f; matrix[63] = 1;
        vertexConstants = Buffer(matrix.data(), sizeof(matrix), D3D11_BIND_CONSTANT_BUFFER);
        }
        D3D11_DEPTH_STENCIL_DESC depth {}; Hr(device->CreateDepthStencilState(&depth, &noDepth));
        D3D11_RASTERIZER_DESC raster {}; raster.FillMode=D3D11_FILL_SOLID;
        raster.CullMode=D3D11_CULL_NONE; raster.DepthClipEnable=TRUE;
        Hr(device->CreateRasterizerState(&raster,&noCull));
        for (unsigned i = 0; i < samplers.size(); ++i) {
            D3D11_SAMPLER_DESC d {}; d.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
            d.AddressU = d.AddressV = d.AddressW = i ? D3D11_TEXTURE_ADDRESS_WRAP : D3D11_TEXTURE_ADDRESS_CLAMP;
            d.MaxLOD = D3D11_FLOAT32_MAX; Hr(device->CreateSamplerState(&d, &samplers[i]));
        }
    }
    Texture Make(unsigned w, unsigned h, DXGI_FORMAT format,
        const std::function<Pixel(unsigned, unsigned)>& value = {}) {
        using namespace DirectX;
        using namespace DirectX::PackedVector;
        const unsigned bytes = format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8u : 4u;
        std::vector<unsigned char> data(static_cast<size_t>(w) * h * bytes);
        if (value) for (unsigned y = 0; y < h; ++y) for (unsigned x = 0; x < w; ++x) {
            const auto p = value(x, y); auto* dst = data.data() + (static_cast<size_t>(y) * w + x) * bytes;
            if (format == DXGI_FORMAT_R11G11B10_FLOAT) {
                XMFLOAT3PK packed; XMStoreFloat3PK(&packed, XMVectorSet(p.r, p.g, p.b, p.a));
                std::memcpy(dst, &packed, sizeof(packed));
            } else if (bytes == 8) {
                const HALF packed[] {XMConvertFloatToHalf(p.r), XMConvertFloatToHalf(p.g),
                    XMConvertFloatToHalf(p.b), XMConvertFloatToHalf(p.a)};
                std::memcpy(dst, packed, sizeof(packed));
            } else {
                const float components[] {p.r, p.g, p.b, p.a};
                for (unsigned c = 0; c < 4; ++c) dst[c] = static_cast<unsigned char>(std::lround(std::clamp(components[c], 0.f, 1.f) * 255));
            }
        }
        D3D11_TEXTURE2D_DESC d {}; d.Width = w; d.Height = h; d.MipLevels = d.ArraySize = d.SampleDesc.Count = 1;
        d.Format = format; d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        D3D11_SUBRESOURCE_DATA init {data.data(), w * bytes, 0}; Texture t;
        Hr(device->CreateTexture2D(&d, value ? &init : nullptr, &t.image));
        Hr(device->CreateShaderResourceView(t.image.Get(), nullptr, &t.srv));
        Hr(device->CreateRenderTargetView(t.image.Get(), nullptr, &t.rtv));
        return t;
    }
    Inputs Resources(uint64_t program, unsigned version, unsigned lensMode, unsigned flip) {
        Inputs out;
        std::array<float, 464 * 4> cb {};
        const auto reg = [&](unsigned r, Pixel p) { cb[r * 4] = p.r; cb[r * 4 + 1] = p.g; cb[r * 4 + 2] = p.b; cb[r * 4 + 3] = p.a; };
        cb[60 * 4 + 1] = 1; reg(70, {1.f / 1024, 1.f / 32, 31, 0});
        cb[138 * 4] = .75f; cb[145 * 4] = version ? .7f : .35f;
        reg(433, {.8f, .85f, .9f, 1.2f}); reg(434, {.5f, .5f, 1, 1.1f});
        cb[426 * 4 + 2] = version ? .35f : .18f;
        reg(427, {.6f, .5f, .4f, 0}); reg(428, {.3f, .3f, .3f, 0}); reg(429, {.1f, .2f, .3f, 0});
        reg(419, {.25f, .4f, .5f, .5f}); reg(420, {.08f, .35f, 1.3f, .7f});
        reg(459, {.18f, .7f, .55f, .08f}); reg(460, {.1f, -.08f, .4f, 27});
        reg(461, {.04f, float(lensMode), 0, .12f}); reg(462, {3, 2, .11f, .07f});
        reg(463, {.1f, -.15f, float(flip), 0});
        out.constants = Buffer(cb.data(), static_cast<UINT>(cb.size() * sizeof(float)), D3D11_BIND_CONSTANT_BUFFER);
        out.auxiliary[0] = Make(80, 50, DXGI_FORMAT_R11G11B10_FLOAT, [=](unsigned x, unsigned y) {
            return Pixel {.02f + .001f * x, .03f + .001f * y, version ? .13f : .06f, 1}; });
        const bool overlay = program == 0x12F210414D64E9FAull;
        const unsigned additive = overlay ? 2 : 1, lut = overlay ? 3 : 2, maximum = overlay ? 4 : 3;
        out.auxiliary[additive] = Make(4, 4, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, [=](unsigned x, unsigned y) {
            return Pixel {.04f + .02f * x, .03f + .02f * y, version ? .16f : .05f, 1}; });
        out.auxiliary[lut] = Make(1024, 32, DXGI_FORMAT_R16G16B16A16_FLOAT, [=](unsigned x, unsigned y) {
            return Pixel {float(x % 32) / 31, float(y) / 31, float(version ? 31 - x / 32 : x / 32) / 31, 1}; });
        out.auxiliary[maximum] = Make(4, 4, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, [=](unsigned x, unsigned y) {
            const float a = (x + y + version) % 2 ? .01f : 1.f; return Pixel {a, a * .8f, a * .6f, 1}; });
        if (overlay) out.auxiliary[1] = Make(47, 29, DXGI_FORMAT_R16G16B16A16_FLOAT, [=](unsigned x, unsigned y) {
            return Pixel {.08f + x * .003f, .12f + y * .004f, version ? .6f : .2f, .2f + x * .01f}; });
        else out.auxiliary[4] = Make(16, 16, DXGI_FORMAT_R16G16B16A16_FLOAT, [=](unsigned x, unsigned y) {
            return Pixel {.04f + x * .003f, .03f + y * .004f, version ? .4f : .1f, .4f}; });
        return out;
    }
    void Bind(ID3D11PixelShader* pixel, ID3D11ShaderResourceView* source, const Inputs& inputs,
        ID3D11RenderTargetView* target, unsigned w, unsigned h) {
        context->ClearState(); context->VSSetShader(vs.Get(), nullptr, 0); context->PSSetShader(pixel, nullptr, 0);
        context->IASetInputLayout(layout.Get()); context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11Buffer* vb = vertices.Get(); UINT stride = 8, offset = 0; context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
        auto* cb = vertexConstants.Get(); context->VSSetConstantBuffers(0, 1, &cb);
        cb = inputs.constants.Get(); context->PSSetConstantBuffers(0, 1, &cb);
        ID3D11ShaderResourceView* views[] {source, inputs.auxiliary[0].srv.Get(), inputs.auxiliary[1].srv.Get(),
            inputs.auxiliary[2].srv.Get(), inputs.auxiliary[3].srv.Get(), inputs.auxiliary[4].srv.Get()};
        context->PSSetShaderResources(0, 6, views);
        ID3D11SamplerState* states[] {samplers[0].Get(), samplers[1].Get(), samplers[2].Get()}; context->PSSetSamplers(0, 3, states);
        context->OMSetRenderTargets(1, &target, nullptr); context->OMSetDepthStencilState(noDepth.Get(), 0);
        D3D11_VIEWPORT vp {0, 0, float(w), float(h), 0, 1}; context->RSSetViewports(1, &vp);
        context->RSSetState(noCull.Get());
    }
    std::vector<unsigned char> Read(Texture& texture) {
        D3D11_TEXTURE2D_DESC d {}; texture.image->GetDesc(&d); d.BindFlags = d.MiscFlags = 0;
        d.Usage = D3D11_USAGE_STAGING; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> stage; Hr(device->CreateTexture2D(&d, nullptr, &stage)); context->ClearState();
        context->CopyResource(stage.Get(), texture.image.Get()); D3D11_MAPPED_SUBRESOURCE map {};
        Hr(context->Map(stage.Get(), 0, D3D11_MAP_READ, 0, &map));
        std::vector<unsigned char> result(static_cast<size_t>(d.Width) * d.Height * 4);
        for (unsigned y = 0; y < d.Height; ++y) std::memcpy(result.data() + static_cast<size_t>(y) * d.Width * 4,
            static_cast<const unsigned char*>(map.pData) + static_cast<size_t>(y) * map.RowPitch, d.Width * 4);
        context->Unmap(stage.Get(), 0); return result;
    }
};
void ProceduralAdmission(Fixture& f, const std::filesystem::path& directory) {
    using namespace taa;
    const auto code=Load(directory,"VS",0x98109168E808BC85ull);
    Check(f.vertexProgram.positionFromVertex &&
        f.vertexProgram.position.source==fullscreen::PositionTransform::Source::VertexIdTriangle,
        "captured VertexID program retains mandatory geometry proof");
    auto changed=code;changed.back()^=1;
    Check(!consumer::Describe(changed.data(),changed.size(),consumer::ProgramStage::Vertex).supported,
        "changed complete VertexID byte identity is not registered");
    const char* similar="float4 main(uint id:SV_VertexID):SV_Position{return float4((id&1)*4-1,(id&2)*2-1,0,1);}";
    ComPtr<ID3DBlob> binary,errors;Hr(D3DCompile(similar,std::strlen(similar),nullptr,nullptr,nullptr,"main","vs_5_0",0,0,&binary,&errors));
    Check(!consumer::Describe(binary->GetBufferPointer(),binary->GetBufferSize(),consumer::ProgramStage::Vertex).supported,
        "unregistered similar procedural shader cannot inherit byte contract");
    auto low=f.Make(kLowW,kLowH,DXGI_FORMAT_R11G11B10_FLOAT);
    auto high=f.Make(kHighW,kHighH,DXGI_FORMAT_R8G8B8A8_UNORM);
    Inputs inputs;
    const auto bind=[&]{f.Bind(f.copy.Get(),low.srv.Get(),inputs,high.rtv.Get(),kHighW,kHighH);};
    bind();
    auto position=f.vertexProgram.position;
    Check(position.BindConstants(f.context.Get()),"VertexID has no position constants");
    const auto geometry=[&](unsigned first){return fullscreen::Geometry(f.context.Get(),false,3,first,0,nullptr,&position);};
    Check(geometry(0),"VertexID full triangle without layout or VB");
    for(unsigned first:{1u,2u,3u})Check(!geometry(first),"actual start vertex changes clip coverage");
    Check(geometry(4),"actual start vertex follows the program's low-bit period");
    Check(!geometry(UINT_MAX),"VertexID addition overflow is rejected");
    Check(!fullscreen::Geometry(f.context.Get(),true,3,0,0,nullptr,&position),"indexed VertexID semantics remain unproven");
    const float irrelevant[]{9,8,7,6,5,4};auto vb=f.Buffer(irrelevant,sizeof(irrelevant),D3D11_BIND_VERTEX_BUFFER);
    ID3D11Buffer* raw=vb.Get();UINT stride=8,offset=0;f.context->IASetVertexBuffers(0,1,&raw,&stride,&offset);
    constants::Written(vb.Get());Check(geometry(0),"unrelated unknown VB does not invalidate VertexID positions");
    const auto admit=[&](consumer::Command command) {consumer::Chain chain(f.context.Get());chain.Reset(low.image.Get());
        return chain.Before(command,f.copyProgram,f.vertexProgram,kLowW,kLowH);};
    for(const auto command:{consumer::Command{false,3,0,0},consumer::Command{false,3,0,2},consumer::Command{false,3,0,1,1}}) {
        bind();Check(!admit(command),"procedural shader retains instance count/base proof");
    }
    bind();Check(admit({false,3}),"physical procedural copy connects current resources");
    bind();D3D11_VIEWPORT partial{0,0,float(kHighW-1),float(kHighH),0,1};f.context->RSSetViewports(1,&partial);
    Check(!admit({false,3}),"procedural consumer rejects partial viewport");
    for(unsigned variant=0;variant<4;++variant) {
        bind();D3D11_RASTERIZER_DESC raster{};raster.FillMode=D3D11_FILL_SOLID;raster.DepthClipEnable=TRUE;
        raster.CullMode=variant<2?D3D11_CULL_BACK:D3D11_CULL_FRONT;raster.FrontCounterClockwise=variant%2;
        ComPtr<ID3D11RasterizerState> state;Hr(f.device->CreateRasterizerState(&raster,&state));f.context->RSSetState(state.Get());
        Check(geometry(0)==(variant==1||variant==2),"real culling and winding remain in procedural proof");
    }
    bind();D3D11_RASTERIZER_DESC raster{};raster.FillMode=D3D11_FILL_WIREFRAME;raster.CullMode=D3D11_CULL_NONE;
    ComPtr<ID3D11RasterizerState> state;Hr(f.device->CreateRasterizerState(&raster,&state));f.context->RSSetState(state.Get());
    Check(!admit({false,3}),"procedural consumer rejects wireframe");
    bind();raster.FillMode=D3D11_FILL_SOLID;raster.ScissorEnable=TRUE;state.Reset();Hr(f.device->CreateRasterizerState(&raster,&state));
    f.context->RSSetState(state.Get());const D3D11_RECT scissor{0,0,LONG(kHighW-1),LONG(kHighH)};f.context->RSSetScissorRects(1,&scissor);
    Check(!admit({false,3}),"procedural consumer rejects partial scissor");
    bind();f.context->OMSetBlendState(nullptr,nullptr,0);Check(!admit({false,3}),"procedural consumer rejects disabled samples");
}
} // namespace

int main(int argc, char** argv) { try {
    Check(argc == 2 || argc == 3, "captured shader directory required");
    Fixture f(argv[1], argc==3 ? std::filesystem::path(argv[2]) : std::filesystem::path());
    if(argc==3)ProceduralAdmission(f,argv[2]);
    for (const auto hash : kPrograms) {
        const auto bytes = Load(argv[1], "PS", hash);
        const auto program = taa::consumer::Describe(bytes.data(), bytes.size(), taa::consumer::ProgramStage::Pixel);
        Check(program.supported && program.hash == hash, "exact pixel declaration");
        ComPtr<ID3D11PixelShader> ps; Hr(f.device->CreatePixelShader(bytes.data(), bytes.size(), nullptr, &ps));
        const unsigned modes = hash == 0xC6C24FFD1D81EA68ull ? 6 : 1;
        for (unsigned mode = 0; mode < modes; ++mode) for (const auto format :
            {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB}) for (unsigned frame = 0; frame < 2; ++frame) {
            const auto source = [=](unsigned x, unsigned y) { return Pixel {.04f + float(x % 31) / 60,
                .07f + float(y % 23) / 48, .1f + float((x / 5 + y / 3 + frame) % 7) / 13, 1}; };
            auto low = f.Make(kLowW, kLowH, DXGI_FORMAT_R11G11B10_FLOAT, source);
            auto high = f.Make(kHighW, kHighH, DXGI_FORMAT_R11G11B10_FLOAT, source);
            auto node = f.Make(kLowW, kLowH, format), oracleNode = f.Make(kHighW, kHighH, format);
            auto actual = f.Make(kHighW, kHighH, DXGI_FORMAT_R8G8B8A8_UNORM);
            auto expected = f.Make(kHighW, kHighH, DXGI_FORMAT_R8G8B8A8_UNORM);
            auto changed = f.Make(kHighW, kHighH, DXGI_FORMAT_R8G8B8A8_UNORM);
            auto captured = f.Resources(hash, frame, mode / 2, mode % 2), oracle = f.Resources(hash, frame, mode / 2, mode % 2);
            auto different = f.Resources(hash, 1 - frame, mode / 2, mode % 2);
            taa::consumer::Chain chain(f.context.Get()); chain.Reset(low.image.Get());
            f.Bind(ps.Get(), low.srv.Get(), captured, node.rtv.Get(), kLowW, kLowH);
            Check(chain.Before({false, 3}, program, f.vertexProgram, kLowW, kLowH), chain.Error());
            f.context->Draw(3, 0); chain.Returned(); f.context->ClearState();
            // Change the same original GPU objects after capture, rather than
            // merely rebinding different objects. Replay must use its snapshots.
            f.context->CopyResource(captured.constants.Get(), different.constants.Get());
            for (unsigned i = 0; i < captured.auxiliary.size(); ++i)
                f.context->CopyResource(captured.auxiliary[i].image.Get(), different.auxiliary[i].image.Get());
            f.Bind(ps.Get(), high.srv.Get(), oracle, oracleNode.rtv.Get(), kHighW, kHighH); f.context->Draw(3, 0);
            f.Bind(f.copy.Get(), oracleNode.srv.Get(), oracle, expected.rtv.Get(), kHighW, kHighH); f.context->Draw(3, 0);
            f.Bind(ps.Get(), high.srv.Get(), different, oracleNode.rtv.Get(), kHighW, kHighH); f.context->Draw(3, 0);
            f.Bind(f.copy.Get(), oracleNode.srv.Get(), different, changed.rtv.Get(), kHighW, kHighH); f.context->Draw(3, 0);
            f.Bind(f.copy.Get(), node.srv.Get(), different, actual.rtv.Get(), kHighW, kHighH);
            Check(chain.Before({false, 3}, f.copyProgram, f.vertexProgram, kLowW, kLowH) && chain.NeedsResolve(), chain.Error());
            Check(chain.ReplayCount() == 1, "one real postprocess replay");
            Check(chain.Bind(high.image.Get()), chain.Error()); f.context->Draw(3, 0);
            Check(chain.Returned(), "physical current output delivery");
            const auto a = f.Read(actual), b = f.Read(expected), c = f.Read(changed);
            unsigned maxError = 0; size_t varied = 0, affected = 0;
            for (size_t i = 0; i < a.size(); ++i) {
                maxError = std::max(maxError, static_cast<unsigned>(std::abs(int(a[i]) - int(b[i]))));
                if (i % 4 != 3) { varied += b[i] != b[i % 4]; affected += b[i] != c[i]; }
            }
            char label[128]; std::snprintf(label, sizeof(label), "PS %016llX fmt=%u frame=%u mode=%u maxError=%u",
                static_cast<unsigned long long>(hash), unsigned(format), frame, mode, maxError);
            Check(maxError <= 1, label); Check(varied > 1000, "oracle has nonconstant RGB");
            if (hash != kCopy) Check(affected > 1000, "changed CB/auxiliary changes independent oracle");
            std::printf("PASS %s\n", label);
        }
    }
    for (UINT64 i = 0; i < f.debug->GetNumStoredMessages(); ++i) {
        SIZE_T size = 0; f.debug->GetMessage(i, nullptr, &size); std::vector<unsigned char> storage(size);
        auto* message = reinterpret_cast<D3D11_MESSAGE*>(storage.data()); Hr(f.debug->GetMessage(i, message, &size));
        Check(message->Severity > D3D11_MESSAGE_SEVERITY_WARNING, message->pDescription);
    }
    std::printf("PASS captured postprocess GPU replay: %u checks\n", checks); return 0;
} catch (const std::exception& e) { std::fprintf(stderr, "FAIL: %s\n", e.what()); return 1; } }

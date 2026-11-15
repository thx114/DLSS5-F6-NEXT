// Runs only in an isolated fixture directory against the actual built DLL.
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_params.h>
#include "../F6EvalReceiptV1.h"
#include "../NeoBeforeSrV1.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>

using Microsoft::WRL::ComPtr;
using namespace neo::ngx;
namespace
{
unsigned checks = 0;
thread_local const neo::interop::BeforeSrFrameV1* beforeSrFrame = nullptr;
unsigned beforeSrQueries = 0, beforeSrMismatches = 0;
void Check(bool ok, const char* label)
{
    ++checks;
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", label); std::exit(1); }
}
void Hr(HRESULT result, const char* label) { Check(SUCCEEDED(result), label); }
void Ngx(NVSDK_NGX_Result result, const char* label)
{
    if (result != NVSDK_NGX_Result_Success) std::fprintf(stderr, "NGX result=%08x\n", static_cast<unsigned>(result));
    Check(result == NVSDK_NGX_Result_Success, label);
}
template<class Fn> Fn Entry(HMODULE module, const char* name)
{
    const auto entry = GetProcAddress(module, name);
    Check(entry != nullptr, name);
    return reinterpret_cast<Fn>(entry);
}
struct Frame
{
    ComPtr<ID3D11Texture2D> color, motion, depth, output, staging;
    ComPtr<ID3D11RenderTargetView> colorView, motionView, depthView, outputView;
    UINT render, display;
    Frame(ID3D11Device* device, UINT size) : render(size), display(size * 2)
    {
        const auto texture = [&](UINT width, DXGI_FORMAT format, ComPtr<ID3D11Texture2D>& tex,
                                 ComPtr<ID3D11RenderTargetView>& view, UINT extra = 0) {
            D3D11_TEXTURE2D_DESC desc {};
            desc.Width = desc.Height = width;
            desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
            desc.Format = format;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET | extra;
            Hr(device->CreateTexture2D(&desc, nullptr, &tex), "create frame texture");
            Hr(device->CreateRenderTargetView(tex.Get(), nullptr, &view), "create frame RTV");
        };
        texture(render, DXGI_FORMAT_R10G10B10A2_UNORM, color, colorView);
        texture(render, DXGI_FORMAT_R16G16_FLOAT, motion, motionView);
        texture(render, DXGI_FORMAT_R32_FLOAT, depth, depthView);
        texture(display, DXGI_FORMAT_R8G8B8A8_UNORM, output, outputView, D3D11_BIND_UNORDERED_ACCESS);
        D3D11_TEXTURE2D_DESC desc {};
        output->GetDesc(&desc);
        desc.BindFlags = 0; desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        Hr(device->CreateTexture2D(&desc, nullptr, &staging), "create readback");
    }
    void Fill(ID3D11DeviceContext* context, const std::array<UINT,3>& rgb)
    {
        const float colorValue[] {rgb[0]/1023.0f, rgb[1]/1023.0f, rgb[2]/1023.0f, 1};
        const float zero[] {0,0,0,0}, depthValue[] {0.5f,0,0,0}, sentinel[] {1,0,1,1};
        context->ClearRenderTargetView(colorView.Get(), colorValue);
        context->ClearRenderTargetView(motionView.Get(), zero);
        context->ClearRenderTargetView(depthView.Get(), depthValue);
        context->ClearRenderTargetView(outputView.Get(), sentinel);
    }
    void CheckPixels(ID3D11DeviceContext* context, const std::array<UINT,3>& rgb, bool sentinel)
    {
        context->CopyResource(staging.Get(), output.Get());
        D3D11_MAPPED_SUBRESOURCE mapped {};
        Hr(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "map actual target");
        const auto* center = static_cast<const unsigned char*>(mapped.pData) + (display/2)*mapped.RowPitch + (display/2)*4;
        std::printf("target center=%u,%u,%u,%u expected=%s\n", center[0], center[1], center[2], center[3],
                    sentinel ? "untouched" : "current");
        bool good = true;
        // Read the interior, not only one center pixel. DLSS edge padding is
        // outside this uniform-color oracle; alpha is not an SR color proof.
        for (UINT y = 32; y < display - 32; ++y)
            for (UINT x = 32; x < display - 32; ++x)
            {
                const auto* p = static_cast<const unsigned char*>(mapped.pData) + y*mapped.RowPitch + x*4;
                for (UINT c = 0; c < 3; ++c)
                {
                    const int expected = sentinel ? (c == 1 ? 0 : 255) : static_cast<int>((rgb[c]*255+511)/1023);
                    good &= std::abs(static_cast<int>(p[c]) - expected) <= (sentinel ? 0 : 5);
                }
            }
        context->Unmap(staging.Get(), 0);
        Check(good, sentinel ? "no-output preserves target pixels" : "ordered target contains this source color");
    }
    void Bind(NVSDK_NGX_Parameter* p) const
    {
        p->Set(NVSDK_NGX_Parameter_Color, static_cast<ID3D11Resource*>(color.Get()));
        p->Set(NVSDK_NGX_Parameter_MotionVectors, static_cast<ID3D11Resource*>(motion.Get()));
        p->Set(NVSDK_NGX_Parameter_Depth, static_cast<ID3D11Resource*>(depth.Get()));
        p->Set(NVSDK_NGX_Parameter_Output, static_cast<ID3D11Resource*>(output.Get()));
        p->Set(NVSDK_NGX_Parameter_ExposureTexture, static_cast<ID3D11Resource*>(nullptr));
        p->Set(NVSDK_NGX_Parameter_DLSS_Pre_Exposure, 1.0f);
        p->Set(NVSDK_NGX_Parameter_DLSS_Exposure_Scale, 1.0f);
        p->Set(NVSDK_NGX_Parameter_FrameTimeDeltaInMsec, 16.67f);
        p->Set(NVSDK_NGX_Parameter_Jitter_Offset_X, 0.0f);
        p->Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, 0.0f);
        p->Set(NVSDK_NGX_Parameter_MV_Scale_X, 1.0f);
        p->Set(NVSDK_NGX_Parameter_MV_Scale_Y, 1.0f);
        p->Set(NVSDK_NGX_Parameter_Sharpness, 0.0f);
        p->Set(NVSDK_NGX_Parameter_Reset, 1);
        p->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, render);
        p->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, render);
    }
};
}

// This executable stands in for a renamed bridge and calls the real F6 DLL.
extern "C" __declspec(dllexport) HRESULT WINAPI Dx11FsrBridge_QueryBeforeSrFrameV1(
    ID3D11DeviceContext* context, ID3D11Resource* color, ID3D11Resource* depth,
    neo::interop::BeforeSrFrameV1* out) noexcept
{
    ++beforeSrQueries;
    if (!beforeSrFrame || beforeSrFrame->context != context || beforeSrFrame->color != color ||
        beforeSrFrame->depth != depth || !out || out->size != sizeof(*out) || out->version != 1) {
        ++beforeSrMismatches; return S_FALSE;
    }
    *out = *beforeSrFrame;
    return S_OK;
}

int wmain(int argc, wchar_t** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc != 3 && argc != 4) return 2;
    const bool skip = std::wstring_view(argv[2]) == L"skip";
    const bool native = std::wstring_view(argv[2]) == L"native";
    const bool beforeSr = std::wstring_view(argv[2]) == L"before-sr";
    const bool legacyOnly = std::wstring_view(argv[2]) == L"legacy-shutdown";
    const bool includeShutdown = legacyOnly || (argc == 4 && std::wstring_view(argv[3]) == L"shutdown");
    const auto module = LoadLibraryW(argv[1]);
    Check(module != nullptr, "load isolated F6 DLL");
    const auto evaluate = legacyOnly ? nullptr : Entry<F6EvaluateV1>(module, k_f6_evaluate_v1);
    const auto legacy = Entry<decltype(&NVSDK_NGX_D3D11_EvaluateFeature)>(module, "NVSDK_NGX_D3D11_EvaluateFeature");
    using Init = NVSDK_NGX_Result (NVSDK_CONV *)(unsigned long long, const wchar_t*, ID3D11Device*,
                                               NVSDK_NGX_Version, const NVSDK_NGX_FeatureCommonInfo*);
    const auto init = Entry<Init>(module, "NVSDK_NGX_D3D11_Init_Ext");
    const auto allocate = Entry<decltype(&NVSDK_NGX_D3D11_AllocateParameters)>(module, "NVSDK_NGX_D3D11_AllocateParameters");
    const auto destroy = Entry<decltype(&NVSDK_NGX_D3D11_DestroyParameters)>(module, "NVSDK_NGX_D3D11_DestroyParameters");
    const auto create = Entry<decltype(&NVSDK_NGX_D3D11_CreateFeature)>(module, "NVSDK_NGX_D3D11_CreateFeature");
    const auto release = Entry<decltype(&NVSDK_NGX_D3D11_ReleaseFeature)>(module, "NVSDK_NGX_D3D11_ReleaseFeature");
    const auto shutdown = Entry<decltype(&NVSDK_NGX_D3D11_Shutdown1)>(module, "NVSDK_NGX_D3D11_Shutdown1");

    // Invalid ABI must be rejected before touching arbitrary NGX arguments.
    if (!legacyOnly)
    {
    auto* invalidContext = reinterpret_cast<ID3D11DeviceContext*>(uintptr_t {1});
    auto* invalidHandle = reinterpret_cast<NVSDK_NGX_Handle*>(uintptr_t {2});
    auto* invalidParameters = reinterpret_cast<NVSDK_NGX_Parameter*>(uintptr_t {3});
    for (unsigned size : {0u, 8u, 55u, 57u})
    {
        F6EvalReceiptV1 receipt; receipt.size = size; receipt.reserved = 0x77777777;
        const auto before = receipt;
        Check(evaluate(invalidContext, invalidHandle, invalidParameters, nullptr, 7, &receipt) ==
              NVSDK_NGX_Result_FAIL_InvalidParameter, "reject wrong size before Evaluate");
        Check(std::memcmp(&before, &receipt, sizeof(receipt)) == 0, "wrong size leaves receipt unchanged");
    }
    F6EvalReceiptV1 wrongVersion; wrongVersion.version = 2;
    const auto beforeVersion = wrongVersion;
    Check(evaluate(invalidContext, invalidHandle, invalidParameters, nullptr, 7, &wrongVersion) ==
          NVSDK_NGX_Result_FAIL_InvalidParameter, "reject unsupported version");
    Check(std::memcmp(&wrongVersion, &beforeVersion, sizeof(wrongVersion)) == 0, "wrong version unchanged");
    Check(evaluate(invalidContext, invalidHandle, invalidParameters, nullptr, 7, nullptr) ==
          NVSDK_NGX_Result_FAIL_InvalidParameter, "reject null receipt");
    F6EvalReceiptV1 early; early.status = F6OutputStatus::current_output_ordered; early.reserved = 9;
    Check(evaluate(nullptr, nullptr, nullptr, nullptr, 19, &early) == NVSDK_NGX_Result_FAIL_FeatureNotFound,
          "null handle returns original error");
    Check(early.request_id == 19 && !early.context && !early.handle && !early.output && !early.reserved &&
          early.status == F6OutputStatus::no_output, "early return initializes all receipt identities and status");
    }

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    Hr(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                         &device, nullptr, &context), "create real hardware device");
    Ngx(init(0x1337, L".", device.Get(), NVSDK_NGX_Version_API, nullptr), "NGX Init");
    uint64_t request = 20;
    for (UINT size : {512u, 384u, 512u})
    {
        Frame frame(device.Get(), size);
        NVSDK_NGX_Parameter* parameters = nullptr;
        Ngx(allocate(&parameters), "allocate actual NGX table");
        parameters->Set(NVSDK_NGX_Parameter_Width, size);
        parameters->Set(NVSDK_NGX_Parameter_Height, size);
        parameters->Set(NVSDK_NGX_Parameter_OutWidth, size*2);
        parameters->Set(NVSDK_NGX_Parameter_OutHeight, size*2);
        parameters->Set(NVSDK_NGX_Parameter_PerfQualityValue, NVSDK_NGX_PerfQuality_Value_MaxPerf);
        parameters->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, static_cast<int>(
            NVSDK_NGX_DLSS_Feature_Flags_AutoExposure | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
            NVSDK_NGX_DLSS_Feature_Flags_DepthInverted));
        NVSDK_NGX_Handle* handle = nullptr;
        Ngx(create(context.Get(), NVSDK_NGX_Feature_SuperSampling, parameters, &handle), "create real SR feature");
        Check(handle != nullptr, "real feature handle");
        unsigned n = 0;
        for (const std::array<UINT,3> rgb : {std::array<UINT,3>{32,96,900}, {704,352,128}})
        {
            frame.Fill(context.Get(), rgb); frame.Bind(parameters);
            F6EvalReceiptV1 receipt; receipt.reserved = 0x99999999;
            ++request;
            neo::interop::BeforeSrFrameV1 input;
            input.context = context.Get(); input.color = frame.color.Get(); input.depth = frame.depth.Get();
            input.motion = frame.motion.Get(); input.render_width = input.render_height = size;
            input.display_width = input.display_height = size * 2;
            input.color_storage_format = input.color_view_format = DXGI_FORMAT_R10G10B10A2_UNORM;
            input.frame = request; input.instance = reinterpret_cast<uintptr_t>(handle);
            input.flags = neo::interop::k_before_sr_depth_inverted;
            beforeSrFrame = beforeSr ? &input : nullptr;
            const auto evaluated = legacyOnly ? legacy(context.Get(), handle, parameters, nullptr) :
                evaluate(context.Get(), handle, parameters, nullptr, request, &receipt);
            beforeSrFrame = nullptr;
            Ngx(evaluated, "V1 Evaluate");
            const auto status = f6_output_status(receipt, request, handle, context.Get(), frame.output.Get());
            const bool skipped = skip && n == 0;
            Check(legacyOnly || status == (skipped ? F6OutputStatus::no_output : F6OutputStatus::current_output_ordered),
                  "exact per-call delivery status");
            ID3D11Resource* restored = nullptr;
            Ngx(parameters->Get(NVSDK_NGX_Parameter_Output, &restored), "read restored caller output");
            Check(restored == frame.output.Get(), "caller Output retains original DX11 identity");
            std::printf("receipt request=%llu handle=%p context=%p output=%p status=%u frame=%llu size=%u color=%u\n",
                static_cast<unsigned long long>(request), static_cast<void*>(handle), static_cast<void*>(context.Get()),
                static_cast<void*>(frame.output.Get()), static_cast<unsigned>(status),
                static_cast<unsigned long long>(receipt.provider_frame), size, ++n);
            frame.CheckPixels(context.Get(), rgb, skipped);
        }
        // Old ABI must still evaluate once and deliver the selected provider.
        const std::array<UINT,3> third {192,704,352};
        frame.Fill(context.Get(), third); frame.Bind(parameters);
        Ngx(legacy(context.Get(), handle, parameters, nullptr), "legacy Evaluate remains callable");
        frame.CheckPixels(context.Get(), third, false);
        Ngx(release(handle), "release after real readback completion");
        Ngx(destroy(parameters), "destroy table after release");
    }
    Check(beforeSrQueries == (beforeSr ? 6u : 0u) && beforeSrMismatches == 0,
        "BeforeSR query receives this call's original DX11 resources only when requested");
    std::printf("before_sr queries=%u mismatches=%u enabled=%u transfer=unknown\n",
        beforeSrQueries, beforeSrMismatches, beforeSr ? 1u : 0u);
    if (includeShutdown) Ngx(shutdown(device.Get()), "shutdown after all handles and readbacks");
    else std::puts("LIMIT: vendor Shutdown1 not exercised; this suite proves Evaluate, pixels and per-handle Release only");
    std::printf("PASS actual F6 V1 ABI and %s provider pixels: %u checks\n", native ? "native" : "interop", checks);
    // This suite does not test CRT/DLL teardown. Use the normal Windows process
    // exit entry so F6 can revoke callbacks before loader detach, as in the
    // existing on12 factory host. Explicit Shutdown remains a separate mode.
    ExitProcess(0);
    return 0;
}

#include <integration/HoYoShadeBeforeSrInput.h>
#include <integration/HoYoShade.h>
#include <wrl/client.h>
#include <cstdio>
#include <stdexcept>
#include <thread>
using Microsoft::WRL::ComPtr;
using namespace neo::interop;
namespace {
thread_local const BeforeSrFrameV1* current = nullptr;
thread_local BeforeSrFrameV1 reply;
unsigned checks = 0;
void Check(bool value, const char* message) {
    ++checks;
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", message); throw std::runtime_error(message); }
}
void Hr(HRESULT hr) { Check(SUCCEEDED(hr), "WARP fixture API"); }
}
extern "C" __declspec(dllexport) HRESULT WINAPI Dx11FsrBridge_QueryBeforeSrFrameV1(
    ID3D11DeviceContext* context, ID3D11Resource* color, ID3D11Resource* depth, BeforeSrFrameV1* out) noexcept {
    if (!current || context != current->context || color != current->color || depth != current->depth) return S_FALSE;
    if (!out || out->size != sizeof(*out) || out->version != 1) return E_INVALIDARG;
    *out = reply;
    return S_OK;
}
int main() {
    try {
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        Hr(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
            &device, nullptr, &context));
        const auto texture = [&](DXGI_FORMAT format) {
            D3D11_TEXTURE2D_DESC d{};
            d.Width = d.Height = 64; d.MipLevels = d.ArraySize = d.SampleDesc.Count = 1;
            d.Format = format; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            ComPtr<ID3D11Texture2D> result;
            Hr(device->CreateTexture2D(&d, nullptr, &result)); return result;
        };
        auto color = texture(DXGI_FORMAT_R8G8B8A8_UNORM);
        auto other = texture(DXGI_FORMAT_R8G8B8A8_UNORM);
        auto depth = texture(DXGI_FORMAT_R32_FLOAT);
        auto motion = texture(DXGI_FORMAT_R16G16_FLOAT);
        BeforeSrFrameV1 frame;
        frame.context = context.Get(); frame.color = color.Get(); frame.depth = depth.Get(); frame.motion = motion.Get();
        frame.render_width = frame.render_height = 64; frame.display_width = frame.display_height = 128;
        frame.frame = 17; frame.instance = 91;
        frame.color_storage_format = DXGI_FORMAT_R8G8B8A8_UNORM;
        frame.color_view_format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        frame.flags = k_before_sr_depth_inverted | k_before_sr_motion_jittered | k_before_sr_reset;
        current = &frame; reply = frame;
        const auto call = [&] {
            return interop::QueryBeforeSrInput(reinterpret_cast<const void*>(&Dx11FsrBridge_QueryBeforeSrFrameV1),
                context.Get(), color.Get(), depth.Get(), motion.Get());
        };
        const HoYoShade::BeforeSrBinding binding {context.Get(), color.Get(), depth.Get(), motion.Get(),
            DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, 64, 64};
        auto source = call();
        Check(source.query == interop::BeforeSrQuery::Matched, "caller-module export resolves without a DLL filename");
        Check(source.frame.frame == 17 && source.frame.instance == 91 && source.frame.flags == frame.flags &&
            source.frame.display_width == 128, "same-call dimensions flags and route metadata preserved");
        Check(HoYoShade::MatchesBeforeSrInput(&source, binding), "exact retained source identities and rectangle");
        Check(HoYoShade::AdmitBeforeSrInput(&source, binding) == HoYoShade::BeforeSrColor::TransferUnknown,
            "sRGB view plus UNORM storage cannot authorize unknown transfer");
        reply.color_transfer = ColorTransfer::srgb; source = call();
        Check(HoYoShade::AdmitBeforeSrInput(&source, binding) == HoYoShade::BeforeSrColor::DeclaredSdr,
            "supported declared SDR independent of final swapchain color space");
        for (auto transfer : {ColorTransfer::linear, ColorTransfer::pq, static_cast<ColorTransfer>(255)}) {
            reply.color_transfer = transfer; source = call();
            Check(HoYoShade::AdmitBeforeSrInput(&source, binding) == HoYoShade::BeforeSrColor::TransferUnsupported,
                "unsupported producer transfer never falls back to global SDR");
        }
        reply.color_transfer = ColorTransfer::srgb; source = call();
        auto wrong = binding; wrong.color = other.Get();
        Check(HoYoShade::AdmitBeforeSrInput(&source, wrong) == HoYoShade::BeforeSrColor::ResourceMismatch,
            "another shared batch cannot borrow this declaration");
        wrong = binding; wrong.depth = other.Get();
        Check(!HoYoShade::MatchesBeforeSrInput(&source, wrong), "mismatched depth cannot borrow direction");
        wrong = binding; wrong.motion = other.Get();
        Check(!HoYoShade::MatchesBeforeSrInput(&source, wrong), "mismatched motion cannot borrow metadata");
        wrong = binding; wrong.width = 32;
        Check(HoYoShade::AdmitBeforeSrInput(&source, wrong) == HoYoShade::BeforeSrColor::RegionMismatch,
            "render extent mismatch rejects a former frame");
        for (unsigned axis = 0; axis != 4; ++axis) {
            wrong = binding;
            if (axis == 0) wrong.colorX = 1; else if (axis == 1) wrong.colorY = 1;
            else if (axis == 2) wrong.depthX = 1; else wrong.depthY = 1;
            Check(HoYoShade::AdmitBeforeSrInput(&source, wrong) == HoYoShade::BeforeSrColor::RegionMismatch,
                "V1 full-input contract does not authorize a cropped region");
        }
        wrong = binding; wrong.transport = DXGI_FORMAT_R10G10B10A2_UNORM;
        Check(HoYoShade::AdmitBeforeSrInput(&source, wrong) == HoYoShade::BeforeSrColor::FormatMismatch,
            "transport conversion is not inferred from source declaration");
        reply.color_view_format = DXGI_FORMAT_B8G8R8A8_UNORM; source = call();
        Check(HoYoShade::AdmitBeforeSrInput(&source, binding) == HoYoShade::BeforeSrColor::FormatMismatch,
            "foreign typed view cannot grant a working format");
        reply = frame;
        for (unsigned invalid = 0; invalid != 8; ++invalid) {
            reply = frame;
            if (invalid == 0) reply.size = 0; else if (invalid == 1) reply.version = 2;
            else if (invalid == 2) reply.color = other.Get(); else if (invalid == 3) reply.depth = other.Get();
            else if (invalid == 4) reply.motion = other.Get(); else if (invalid == 5) reply.render_width = 32;
            else if (invalid == 6) reply.color_storage_format = DXGI_FORMAT_R10G10B10A2_UNORM;
            else reply.context = reinterpret_cast<ID3D11DeviceContext*>(uintptr_t{1});
            source = call();
            Check(source.query == interop::BeforeSrQuery::Invalid &&
                HoYoShade::AdmitBeforeSrInput(&source, binding) == HoYoShade::BeforeSrColor::InvalidSource,
                "invalid producer answer is never a legacy SDR authorization");
        }
        reply = frame;
        ComPtr<ID3D11DeviceContext> deferred; Hr(device->CreateDeferredContext(0, &deferred));
        auto noMatch = interop::QueryBeforeSrInput(reinterpret_cast<const void*>(&Dx11FsrBridge_QueryBeforeSrFrameV1),
            deferred.Get(), color.Get(), depth.Get(), motion.Get());
        Check(noMatch.query == interop::BeforeSrQuery::NoMatch, "query requires the original context");
        noMatch = interop::QueryBeforeSrInput(reinterpret_cast<const void*>(&Dx11FsrBridge_QueryBeforeSrFrameV1),
            context.Get(), other.Get(), depth.Get(), motion.Get());
        Check(noMatch.query == interop::BeforeSrQuery::NoMatch, "query requires the original Color");
        noMatch = interop::QueryBeforeSrInput(reinterpret_cast<const void*>(&Dx11FsrBridge_QueryBeforeSrFrameV1),
            context.Get(), color.Get(), other.Get(), motion.Get());
        Check(noMatch.query == interop::BeforeSrQuery::NoMatch, "query requires the original Depth");
        std::thread worker([&] { noMatch = call(); }); worker.join();
        Check(noMatch.query == interop::BeforeSrQuery::NoMatch, "another thread cannot borrow the active scope");
        current = nullptr; noMatch = call();
        Check(noMatch.query == interop::BeforeSrQuery::NoMatch &&
            HoYoShade::AdmitBeforeSrInput(&noMatch, binding) == HoYoShade::BeforeSrColor::Legacy,
            "closed scope retains the ordinary NGX path");
        noMatch = interop::QueryBeforeSrInput(reinterpret_cast<const void*>(&GetTickCount64),
            context.Get(), color.Get(), depth.Get(), motion.Get());
        Check(noMatch.query == interop::BeforeSrQuery::NoExport &&
            HoYoShade::AdmitBeforeSrInput(&noMatch, binding) == HoYoShade::BeforeSrColor::Legacy,
            "module without the new export retains the ordinary path");
        Check(HoYoShade::AdmitBeforeSrInput(nullptr, binding) == HoYoShade::BeforeSrColor::Legacy,
            "legacy Evaluate is independent of the optional metadata");
        auto typeless = texture(DXGI_FORMAT_R8G8B8A8_TYPELESS);
        frame.color = typeless.Get(); frame.color_storage_format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
        frame.color_transfer = ColorTransfer::srgb;
        auto typelessBinding = binding;
        typelessBinding.color = typeless.Get();
        typelessBinding.storage = typelessBinding.transport = DXGI_FORMAT_R8G8B8A8_TYPELESS;
        current = &frame;
        for (auto view : {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB}) {
            D3D11_SHADER_RESOURCE_VIEW_DESC desc {};
            desc.Format = view; desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            desc.Texture2D.MipLevels = 1;
            ComPtr<ID3D11ShaderResourceView> srv;
            Hr(device->CreateShaderResourceView(typeless.Get(), &desc, &srv));
            srv->GetDesc(&desc);
            frame.color_view_format = desc.Format; reply = frame;
            source = interop::QueryBeforeSrInput(reinterpret_cast<const void*>(&Dx11FsrBridge_QueryBeforeSrFrameV1),
                context.Get(), typeless.Get(), depth.Get(), motion.Get());
            Check(source.query == interop::BeforeSrQuery::Matched, "real typeless source and typed SRV query");
            Check(HoYoShade::AdmitBeforeSrInput(&source, typelessBinding) == HoYoShade::BeforeSrColor::DeclaredSdr,
                "declared SDR typeless input preserves actual transport and compatible typed view");
            auto admission = HoYoShade::ResolveBeforeSrInput(&source, typelessBinding);
            Check(admission.formats.storage == DXGI_FORMAT_R8G8B8A8_TYPELESS && admission.formats.view == view &&
                admission.formats.transport.endpoint == DXGI_FORMAT_R8G8B8A8_TYPELESS &&
                admission.formats.transport.runtime == DXGI_FORMAT_R8G8B8A8_UNORM,
                "source storage view actual transport and private typed runtime remain distinct");
            HoYoShade::ColorEndpoint endpoint;
            endpoint.stage = HoYoShade::Stage::BeforeSr;
            endpoint.naturalContract = HoYoShade::NaturalSourceContract::BeforeSrBridgeInput;
            endpoint.color = reinterpret_cast<ID3D12Resource*>(uintptr_t{11});
            endpoint.queue = reinterpret_cast<ID3D12CommandQueue*>(uintptr_t{12});
            endpoint.stream = 13; endpoint.generation = 14; endpoint.frame = 15;
            endpoint.width = endpoint.height = 64;
            Check(HoYoShade::BindBeforeSrInput(endpoint, &source, typelessBinding) == HoYoShade::BeforeSrColor::DeclaredSdr &&
                endpoint.sdr && endpoint.declaredInput, "production admission publishes this endpoint's qualified view");
            Check(bool(HoYoShade::ResolveColorFormats(endpoint, DXGI_FORMAT_R8G8B8A8_TYPELESS)),
                "qualified endpoint retains its exact typeless format through stage admission");
            for (unsigned mismatch = 0; mismatch != 13; ++mismatch) {
                auto changed = endpoint;
                if (mismatch == 0) changed.color = nullptr;
                else if (mismatch == 1) changed.queue = nullptr;
                else if (mismatch == 2) ++changed.stream;
                else if (mismatch == 3) ++changed.generation;
                else if (mismatch == 4) ++changed.frame;
                else if (mismatch == 5) ++changed.colorX;
                else if (mismatch == 6) ++changed.colorY;
                else if (mismatch == 7) --changed.width;
                else if (mismatch == 8) --changed.height;
                else if (mismatch == 9) changed.sdr = false;
                else if (mismatch == 10) changed.stage = HoYoShade::Stage::BeforeNr;
                else if (mismatch == 11) changed.output = HoYoShade::OutputKind::GeneratedUnavailable;
                else changed.naturalContract = HoYoShade::NaturalSourceContract::TypedBackbuffer;
                Check(!HoYoShade::ResolveColorFormats(changed, DXGI_FORMAT_R8G8B8A8_TYPELESS),
                    "a different resource queue frame region or stage cannot borrow typed-view admission");
            }
            Check(!HoYoShade::ResolveColorFormats(endpoint, DXGI_FORMAT_R8G8B8A8_UNORM),
                "a transport allocation cannot be relabeled with its typed view");
            endpoint.declaredInput = {};
            Check(!HoYoShade::ResolveColorFormats(endpoint, DXGI_FORMAT_R8G8B8A8_TYPELESS) &&
                !HoYoShade::ColorContract::IsRuntimeFormat(DXGI_FORMAT_R8G8B8A8_TYPELESS),
                "SDR bool alone cannot allow typeless input or a typeless private swapchain");
            for (auto transport : {DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM,
                                   DXGI_FORMAT_R8G8B8A8_UNORM_SRGB}) {
                auto actual = typelessBinding; actual.transport = transport;
                const auto bound = HoYoShade::ResolveBeforeSrInput(&source, actual);
                Check(bound.color == HoYoShade::BeforeSrColor::DeclaredSdr &&
                    bound.formats.transport.endpoint == transport, "compatible transport preserves its actual descriptor format");
            }
            auto unknown = source; unknown.frame.color_transfer = ColorTransfer::unknown;
            Check(HoYoShade::AdmitBeforeSrInput(&unknown, typelessBinding) == HoYoShade::BeforeSrColor::TransferUnknown,
                "real typed SRV never turns an unknown typeless transfer into SDR");
            endpoint.sdr = true;
            HoYoShade::BindBeforeSrInput(endpoint, &unknown, typelessBinding);
            Check(!endpoint.sdr && !endpoint.declaredInput,
                "unknown current producer revokes a global SDR hint and former typed view together");
            for (auto unsupported : {ColorTransfer::linear, ColorTransfer::pq}) {
                auto hdr = source; hdr.frame.color_transfer = unsupported;
                endpoint.sdr = true;
                Check(HoYoShade::BindBeforeSrInput(endpoint, &hdr, typelessBinding) == HoYoShade::BeforeSrColor::TransferUnsupported &&
                    !endpoint.sdr && !endpoint.declaredInput, "unsupported HDR input grants no typeless stage endpoint");
            }
            for (auto badView : {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_R8G8B8A8_TYPELESS,
                DXGI_FORMAT_R8G8B8A8_UINT, DXGI_FORMAT_B8G8R8A8_UNORM}) {
                auto invalid = source; invalid.frame.color_view_format = badView;
                Check(HoYoShade::AdmitBeforeSrInput(&invalid, typelessBinding) == HoYoShade::BeforeSrColor::FormatMismatch,
                    "unknown typeless integer and foreign typed views cannot qualify this source");
            }
            auto foreign = typelessBinding; foreign.transport = DXGI_FORMAT_B8G8R8A8_UNORM;
            Check(HoYoShade::AdmitBeforeSrInput(&source, foreign) == HoYoShade::BeforeSrColor::FormatMismatch,
                "same byte count is not a compatible transport format");
        }
        std::printf("PASS BeforeSR current-input query and transport admission: %u checks\n", checks);
        return 0;
    } catch (const std::exception&) { return 1; }
}

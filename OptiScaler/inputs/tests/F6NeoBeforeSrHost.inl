// The real Neo DLL recognizes controlled Render/FSR calls and invokes the real
// F6 DLL. The host never exports a declaration on the bridge's behalf.
#include <tests/create-routing/DrawCapture.h>
#include <tests/producer-transfer/ProducerFixture.h>
#include <game/AnchorPatterns.h>
#include <dxgi1_2.h>

namespace NeoBeforeSrHost
{
// Test-only readback observes the submitted producer output; no declaration is
// supplied by this host. Product certification never reads GPU pixels.
std::uint32_t ReadCenterPixel(ID3D11Device& device, ID3D11DeviceContext& context, ID3D11Texture2D& source)
{
    D3D11_TEXTURE2D_DESC desc {};
    source.GetDesc(&desc);
    const UINT x=desc.Width/2, y=desc.Height/2;
    desc.Width=desc.Height=1; desc.Usage=D3D11_USAGE_STAGING;
    desc.BindFlags=0; desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ; desc.MiscFlags=0;
    ComPtr<ID3D11Texture2D> readback;
    Hr(device.CreateTexture2D(&desc,nullptr,&readback), "create producer output readback");
    const D3D11_BOX center {x,y,0,x+1,y+1,1};
    context.CopySubresourceRegion(readback.Get(),0,0,0,0,&source,0,&center);
    D3D11_MAPPED_SUBRESOURCE mapped {};
    Hr(context.Map(readback.Get(),0,D3D11_MAP_READ,0,&mapped), "map actual producer output");
    std::uint32_t pixel {};
    std::memcpy(&pixel,mapped.pData,sizeof(pixel));
    context.Unmap(readback.Get(),0);
    return pixel;
}

void CheckCurrentPixels(ID3D11Device* device, ID3D11DeviceContext* context,
    ID3D11Texture2D* output, std::uint32_t rgb)
{
    D3D11_TEXTURE2D_DESC desc {};
    output->GetDesc(&desc);
    const UINT width=desc.Width, height=desc.Height;
    desc.Usage=D3D11_USAGE_STAGING; desc.BindFlags=0; desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> readback;
    Hr(device->CreateTexture2D(&desc,nullptr,&readback), "create joint output readback");
    context->CopyResource(readback.Get(),output);
    D3D11_MAPPED_SUBRESOURCE mapped {};
    Hr(context->Map(readback.Get(),0,D3D11_MAP_READ,0,&mapped), "map completed joint SR output");
    bool current=true;
    for (UINT y=32;y<height-32;++y)
        for (UINT x=32;x<width-32;++x) {
            const auto* p=static_cast<const unsigned char*>(mapped.pData)+y*mapped.RowPitch+x*4;
            for (unsigned c=0;c<3;++c)
                current &= std::abs(int(p[c])-int((rgb>>(c*8))&255u))<=5;
        }
    context->Unmap(readback.Get(),0);
    Check(current, "joint output interior contains the expected current SR input color");
    std::printf("neo pixels: extent=%ux%u color=%08x interior_tolerance=5 current=1\n",width,height,rgb);
}
constexpr std::size_t RenderOffset = 0x70;
constexpr auto Methods()
{
    constexpr std::uint8_t tail[] {0x5B,0x5D,0x5F,0x5E,0x41,0x5C,0x41,0x5D,0x41,0x5E,0x41,0x5F,0xC3};
    std::array<std::uint8_t, RenderOffset + sizeof(neo::game::k_render_head) + sizeof(tail)> code {};
    std::copy_n(neo::game::k_ucb_head, sizeof(neo::game::k_ucb_head), code.begin());
    auto end = std::copy_n(neo::game::k_render_head, sizeof(neo::game::k_render_head), code.begin() + RenderOffset);
    std::copy_n(tail, sizeof(tail), end);
    return code;
}
#pragma section(".neort", read, execute)
__declspec(allocate(".neort")) constinit const auto methods = Methods();

void Run(std::wstring_view mode)
{
    const bool rejectGuides = mode == L"before-sr-neo-guide-reject";
    const bool producerInput = mode == L"before-sr-neo-srgb" || mode == L"before-sr-neo-gamma-reject";
    const bool filteredInput = mode == L"before-sr-neo-srgb";
    const auto rva = reinterpret_cast<std::uintptr_t>(methods.data()) -
        reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    // All settings are in this disposable test directory.
    std::ofstream ini("Dx11FsrBridge.ini");
    ini << "[Dx11FsrBridge]\nEnabled=1\nRenderScaleMenu=0\nTransparentJitter=0\n"
        "Fsr2TranslationMode=2\nFsr2Mode2OnDemandState=1\nFsr2Il2CppHook=1\n"
        "Ffx12=1\nFfx12Hdr=0\nFfx12NonLinear=0\nFfx12GpuInterop=1\nFfx12JitterMode=0\n"
        "Ffx12AsyncUpscale=0\nFfx12Backend=ngx\nNgxHdr=0\nNgxLogLevel=3\n"
        "NgxProvider=external\nNgxBeforeSrAo=0\nFfx12DepthInverted=1\n"
        "Fsr2Il2CppRenderRva=0x" << std::hex << rva + RenderOffset <<
        "\nFsr2Il2CppUcbRva=0x" << rva << "\n[Log]\nto_file=1\nlevel=debug\n";
    ini.close();
    Check(bool(ini), "write isolated Neo configuration");
    Check(LoadLibraryW(L"Dx11FsrBridge.dll") != nullptr, "load actual Neo product DLL");

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    Hr(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
        &device, nullptr, &context), "create actual Neo/F6 hardware device");
    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    Hr(device.As(&dxgiDevice), "device DXGI identity");
    Hr(dxgiDevice->GetAdapter(&adapter), "device adapter");
    Hr(adapter->GetParent(IID_PPV_ARGS(&factory)), "device factory");
    const HWND window = CreateWindowExW(0, L"STATIC", L"F6 BeforeSR joint fixture", WS_POPUP,
        0, 0, 1024, 1024, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(window != nullptr, "create hidden joint fixture window");
    DXGI_SWAP_CHAIN_DESC1 desc {};
    desc.Width = desc.Height = 1024; desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    desc.SampleDesc.Count = desc.BufferCount = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT | DXGI_USAGE_SHADER_INPUT;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    desc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    ComPtr<IDXGISwapChain1> presentation;
    Hr(factory->CreateSwapChainForHwnd(device.Get(), window, &desc, nullptr, nullptr, &presentation),
        "create actual Neo/F6 ForHwnd swapchain");
    const auto deadline = GetTickCount64() + 10000;
    while (!coreConnected.load(std::memory_order_acquire) && GetTickCount64() < deadline) {
        ReadEarlyConnection(); Sleep(100);
    }
    Check(coreConnected.load() && coreManaged.load(), "managed pinned core connected before Neo input");
    std::array<std::uint8_t, 0x80> fields {};
    std::array<std::uint8_t, 0x80> rejectedFields {};
    std::uint32_t frameNumber = 0;
    unsigned rejectedFrames = 0;
    CaptureTextures textures;
    std::optional<producer_fixture::ProducerFixture> producer;
    if (producerInput) {
        producer.emplace(*device.Get());
        Check(producer->ready(), "create actual game producer bytecode fixture");
    }
    std::optional<std::uint32_t> previousSource;
    for (UINT size : {512u,384u,512u}) {
        for (std::uint32_t rgb : {0xFF286AA4u, 0xFF934236u}) {
            CaptureFrame frame;
            frame.render = {size,size}; frame.display = {size*2,size*2}; frame.expect_provider = true;
            frame.color_source_view = DXGI_FORMAT_R8G8B8A8_UNORM;
            frame.retained_textures = &textures;
            frame.alternate_scene_pixel = rgb;
            frame.color_r10 = ((rgb & 255u) * 1023u + 127u) / 255u;
            frame.color_g10 = (((rgb >> 8) & 255u) * 1023u + 127u) / 255u;
            frame.color_b10 = (((rgb >> 16) & 255u) * 1023u + 127u) / 255u;
            std::uint32_t expectedRgb = rgb;
            if (producerInput) {
                frame.color_producer = [&](ID3D11DeviceContext& writer, ID3D11Texture2D& source) {
                    producer_fixture::Controls controls;
                    controls.gamma = filteredInput ? 1.f : 1.25f;
                    controls.linear_rgb = rgb == 0xFF286AA4u ?
                        std::array<float,3>{0.18f,0.06f,0.02f} : std::array<float,3>{0.04f,0.22f,0.40f};
                    controls.update_method = frameNumber % 2 ? producer_fixture::UpdateMethod::map_discard :
                        producer_fixture::UpdateMethod::update;
                    if (!producer->record(writer,source,controls)) return false;
                    writer.OMSetRenderTargets(0,nullptr,nullptr);
                    const auto actual = ReadCenterPixel(*device.Get(),writer,source);
                    Check(!previousSource || *previousSource != actual, "producer changes the actual current source color");
                    previousSource = actual;
                    expectedRgb = (actual & 0xFFFFFF00u) |
                        (std::min)(255u,(actual & 255u)+(filteredInput ? 32u : 0u));
                    frame.color_r10 = ((expectedRgb & 255u)*1023u+127u)/255u;
                    frame.color_g10 = (((expectedRgb>>8)&255u)*1023u+127u)/255u;
                    frame.color_b10 = (((expectedRgb>>16)&255u)*1023u+127u)/255u;
                    std::printf("neo producer: source=%08x expected=%08x filter=%u gamma=%.2f\n",
                        actual,expectedRgb,filteredInput,controls.gamma);
                    return true;
                };
            }
            frame.publish_render = [&] {
                const auto put = [&](std::size_t offset, const auto& value) {
                    std::memcpy(fields.data()+offset, &value, sizeof(value));
                };
                put(0x24,0.f); put(0x28,0.f); put(0x50,++frameNumber);
                put(0x58,size); put(0x5C,size); put(0x68,size*2); put(0x6C,size*2);
                reinterpret_cast<void (*)(void*,void*)>(methods.data()+RenderOffset)(fields.data(),nullptr);
            };
            Hr(presentation->Present(0,0), "actual Present before Neo source draw");
            Check(draw_capture(device.Get(),context.Get(),frameNumber % 2 ? CaptureEntry::indexed :
                CaptureEntry::draw,frame,stdout), "actual Neo/F6 current SR output survives producer transfer admission");
            CheckCurrentPixels(device.Get(),context.Get(),textures.textures[8].Get(),expectedRgb);
            if (rejectGuides) {
                CaptureFrame rejected = frame;
                rejected.expect_provider = false;
                // These are controlled legal views, not an assertion about the
                // game's unknown motion encoding. They must retain native draw.
                rejected.motion_source_format = DXGI_FORMAT_R8G8_TYPELESS;
                rejected.motion_source_view = DXGI_FORMAT_R8G8_UNORM;
                rejected.depth_source_format = DXGI_FORMAT_R32G8X24_TYPELESS;
                rejected.publish_render = [&] {
                    std::memcpy(rejectedFields.data(), fields.data(), fields.size());
                    const auto number = ++frameNumber;
                    std::memcpy(rejectedFields.data()+0x50, &number, sizeof(number));
                    reinterpret_cast<void (*)(void*,void*)>(methods.data()+RenderOffset)(rejectedFields.data(),nullptr);
                };
                Check(draw_capture(device.Get(),context.Get(),CaptureEntry::draw,rejected,stdout),
                    "unsupported second instance preserves this call's native pixels");
                ++rejectedFrames;
            }
        }
    }
    if (rejectGuides) {
        std::ifstream log("OptiScaler.log");
        Check(log.is_open(), "read isolated F6 lifecycle evidence");
        unsigned creates = 0;
        for (std::string line; std::getline(log,line);)
            if (line.find("NVSDK_NGX_D3D11_CreateFeature HandleId:") != std::string::npos) ++creates;
        std::printf("neo rejected guides: calls=%u F6_creates=%u expected_dimension_generations=3\n",
            rejectedFrames,creates);
        Check(rejectedFrames == 6 && creates == 3,
            "rejected instances never recreate the valid F6 feature");
        std::ifstream bridgeLog("Dx11FsrBridge.log");
        Check(bridgeLog.is_open(), "read isolated Neo provider lifecycle evidence");
        unsigned providerSwitches = 0;
        for (std::string line; std::getline(bridgeLog,line);)
            if (line.find("ngx_inst switch from=") != std::string::npos) ++providerSwitches;
        std::printf("neo provider lifecycle: switches=%u expected_valid_instance=1\n",providerSwitches);
        Check(providerSwitches == 1,
            "rejected instances never select or retire a provider instance");
    }
    Check(beforeSrQueries == 0, "real Neo never used the EXE declaration substitute");
    std::printf("neo product: frames=%u substitute_queries=%u filtered=%u producer_fixture=%u\n",
        frameNumber,beforeSrQueries,filteredInput,producerInput);
    std::printf("PASS actual F6 V1 ABI and real Neo current-transfer SR pixels: %u checks\n",checks);
    // Vendor Shutdown1 and DLL teardown are separate from this input contract.
    ExitProcess(0);
}
}

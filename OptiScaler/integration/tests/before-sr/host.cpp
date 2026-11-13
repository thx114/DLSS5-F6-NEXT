// Real pinned ReShade core, WARP and production owned-stage implementation.
// Hidden test windows only; no game, injector, user preset or launcher access.
#include "pch.h"
#include <integration/HoYoShade.h>
#include <integration/HoYoShadeNaturalAssociation.h>
#include <integration/HoYoShadeApi.h>
#include <integration/HoYoShadeDepth.h>
#include <d3d11_4.h>
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <filesystem>
#include <cstdio>
#include <stdexcept>
#include <thread>
#include <cstring>
#include <source_location>
using Microsoft::WRL::ComPtr;
namespace api = reshade::api;
unsigned checks = 0;
constexpr unsigned Width = 192, Height = 128;
void Check(bool ok, const char* message) { ++checks; if (!ok) throw std::runtime_error(message); }
void Hr(HRESULT hr, const std::source_location& at = std::source_location::current()) { if (FAILED(hr)) { std::printf("HRESULT %08lx at %s:%u %s\n", hr, at.file_name(), at.line(), at.function_name()); throw std::runtime_error("WARP operation failed"); } }
template<class Function> Function Export(HMODULE module, const char* name) {
    auto value = reinterpret_cast<Function>(GetProcAddress(module, name)); Check(value != nullptr, name); return value;
}
void Wait(ID3D12Fence* fence, uint64_t value) {
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr); Check(event != nullptr, "completion event");
    Hr(fence->SetEventOnCompletion(value, event));
    const auto result = WaitForSingleObject(event, 10000); CloseHandle(event);
    Check(result == WAIT_OBJECT_0 && fence->GetCompletedValue() != UINT64_MAX && fence->GetCompletedValue() >= value, "actual GPU completion");
}
struct TestDepthReader final : HoYoShade::Depth::Reader {
    HoYoShade::Depth::Access expected;
    ComPtr<ID3D12Resource> resource;
    ComPtr<ID3D12Fence> completion;
    uint64_t completedValue = 0;
    bool quarantined = false;
    bool Validate(const HoYoShade::Depth::Access& a) const noexcept override {
        return !quarantined && a.resource == resource.Get() && a.queue == expected.queue &&
            a.readyFence == expected.readyFence && a.readyValue == expected.readyValue &&
            a.owner == expected.owner && a.generation == expected.generation && a.frame == expected.frame && a.epoch == expected.epoch;
    }
    bool BindCompletion(ID3D12Fence* fence, uint64_t value) noexcept override {
        if (!fence || !value || value == UINT64_MAX || fence->GetCompletedValue() == UINT64_MAX || completion) return false;
        completion = fence; completedValue = value; return true;
    }
    void Quarantine() noexcept override { quarantined = true; }
};
struct Host {
    ComPtr<IDXGIFactory4> factory; ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D11Device> device11; ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain1> chain; HWND window = nullptr;
    ComPtr<ID3D12InfoQueue> info;
    ComPtr<ID3D12Device> device; ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator; ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Resource> color, upload, readback, depth[2], depthUpload, observedNormals;
    bool useDepth = false;
    unsigned pattern = 0; ComPtr<ID3D12Fence> done;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{}; uint64_t serial = 0;
    api::effect_runtime* natural = nullptr;
    using Update = void (*)(api::effect_runtime*); Update update = nullptr;
    using Destroy = void (*)(api::effect_runtime*); Destroy destroy = nullptr;
    explicit Host(HMODULE core, const std::filesystem::path& config, bool depthEnabled = false) : useDepth(depthEnabled) {
        Hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory))); Hr(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)));
        constexpr D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_0};
        Hr(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels, 1, D3D11_SDK_VERSION, &device11, nullptr, &context));
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
        Hr(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
        device.As(&info);
        D3D12_COMMAND_QUEUE_DESC q{}; Hr(device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)));
        Hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
        Hr(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list))); Hr(list->Close());
        Hr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&done)));
        WNDCLASSW wc{}; wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"F6BeforeSrWarp";
        RegisterClassW(&wc); window = CreateWindowExW(0, wc.lpszClassName, L"F6 GPU fixture", WS_POPUP, 0, 0, Width, Height, nullptr, nullptr, wc.hInstance, nullptr);
        Check(window != nullptr, "hidden source window");
        DXGI_SWAP_CHAIN_DESC1 sc{}; sc.Width = Width; sc.Height = Height; sc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sc.SampleDesc.Count = 1; sc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sc.BufferCount = 2; sc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        Hr(factory->CreateSwapChainForHwnd(device11.Get(), window, &sc, nullptr, nullptr, &chain));
        ComPtr<ID3D11Device> chainDevice; Hr(chain->GetDevice(IID_PPV_ARGS(&chainDevice)));
        std::printf("natural creation api=%x device=%p chain_device=%p context=%p chain=%p config=%s\n",
            static_cast<unsigned>(api::device_api::d3d11), device11.Get(), chainDevice.Get(), context.Get(), chain.Get(), config.string().c_str()); std::fflush(stdout);
        auto create = Export<HoYoShade::CoreApi::CreateEffectRuntime>(core, "ReShadeCreateEffectRuntime");
        update = Export<Update>(core, "ReShadeUpdateAndPresentEffectRuntime"); destroy = Export<Destroy>(core, "ReShadeDestroyEffectRuntime");
        const auto created = create(api::device_api::d3d11, chainDevice.Get(), context.Get(), chain.Get(), config.string().c_str(), &natural);
        std::printf("natural creation result=%d runtime=%p error=%lu\n", created, natural, GetLastError()); std::fflush(stdout);
        Check(created && natural, "real DX11 natural runtime");
        HoYoShade::RegisterBridgeSource(this, chain.Get(), device11.Get());
        D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{}; desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; desc.Width = Width; desc.Height = Height;
        desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1; desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
        Hr(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&color)));
        if (useDepth) {
            auto depthDesc = desc; depthDesc.Format = DXGI_FORMAT_R32_FLOAT;
            for (auto& input : depth) Hr(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
                &depthDesc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&input)));
        }
        uint64_t bytes = 0; device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
        desc = {}; desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; desc.Width = bytes; desc.Height = desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        Hr(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)));
        if (useDepth) Hr(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&depthUpload)));
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        Hr(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)));
    }
    ~Host() { HoYoShade::ForgetBridgeSource(this); if (natural) destroy(natural); if (window) DestroyWindow(window); }
    void Begin() { Hr(allocator->Reset()); Hr(list->Reset(allocator.Get(), nullptr)); }
    void Submit() {
        const auto closed=list->Close();
        if(FAILED(closed) && info) {
            for(UINT64 i=0;i<info->GetNumStoredMessages();++i){SIZE_T size=0;info->GetMessage(i,nullptr,&size);std::vector<unsigned char> bytes(size);auto* message=reinterpret_cast<D3D12_MESSAGE*>(bytes.data());
                if(SUCCEEDED(info->GetMessage(i,message,&size)) && message->Severity<=D3D12_MESSAGE_SEVERITY_WARNING)std::fprintf(stderr,"D3D12 %s\n",message->pDescription);}
        }
        Hr(closed);
        ID3D12CommandList* commands[]{list.Get()}; queue->ExecuteCommandLists(1, commands);
        Hr(queue->Signal(done.Get(), ++serial));
        Wait(done.Get(), serial);
    }
    void Upload() {
        unsigned char* mapped = nullptr; Hr(upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped)));
        for (unsigned y = 0; y < Height; ++y) for (unsigned x = 0; x < Width; ++x) {
            auto* p = mapped + y * footprint.Footprint.RowPitch + x * 4; p[0] = p[1] = p[2] = 32; p[3] = 255;
        }
        upload->Unmap(0, nullptr); Begin();
        D3D12_TEXTURE_COPY_LOCATION src{}, dst{}; src.pResource = upload.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint = footprint;
        dst.pResource = color.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        if (useDepth) {
            unsigned char* depthBytes = nullptr; Hr(depthUpload->Map(0, nullptr, reinterpret_cast<void**>(&depthBytes)));
            for (unsigned y = 0; y < Height; ++y) for (unsigned x = 0; x < Width; ++x) {
                const float value = ((x < Width/3) != (y < Height/2)) != bool(pattern & 1) ? 0.25f : 0.75f;
                std::memcpy(depthBytes + y*footprint.Footprint.RowPitch + x*4, &value, 4);
            }
            depthUpload->Unmap(0, nullptr);
            src.pResource = depthUpload.Get(); src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R32_FLOAT;
            dst.pResource = depth[pattern & 1].Get(); list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }
        Submit();
    }
    unsigned Read() {
        Begin(); D3D12_TEXTURE_COPY_LOCATION src{}, dst{}; src.pResource = color.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.pResource = readback.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = footprint;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        Submit();
        unsigned char* mapped = nullptr; Hr(readback->Map(0, nullptr, reinterpret_cast<void**>(&mapped))); const auto red = mapped[0];
        for (unsigned y = 0; y < Height; ++y) for (unsigned x = 0; x < Width; ++x) Check(mapped[y * footprint.Footprint.RowPitch + x * 4] == red, "effect covers every SR input pixel");
        readback->Unmap(0, nullptr); return red;
    }
    void ObserveTextures(api::effect_runtime* runtime, api::resource_view rtv) {
        if (!rtv.handle || runtime->get_device()->get_api() != api::device_api::d3d12 ||
            runtime->get_device()->get_native() != reinterpret_cast<uint64_t>(device.Get())) return;
        runtime->enumerate_texture_variables(nullptr, [&](auto* r, auto variable) {
            char name[256]{}; r->get_texture_variable_name(variable,name);
            const std::string_view key(name);
            if (!key.ends_with("NormalsTexV4") || key.ends_with("GeoNormalsTexV4")) return;
            api::resource_view srv{}, srgb{}; r->get_texture_binding(variable,&srv,&srgb);
            const auto resource = r->get_device()->get_resource_from_view(srv);
            static unsigned reports=0;
            if(reports++<8){char effect[256]{};r->get_texture_variable_effect_name(variable,effect);std::printf("LP normal observation effect=%s name=%s resource=%llx\n",effect,name,resource.handle);}
            if (resource.handle) observedNormals = reinterpret_cast<ID3D12Resource*>(resource.handle);
        });
    }
    uint64_t ReadNormals() {
        Check(observedNormals != nullptr,"actual LP normals resource reflected");
        const auto desc=observedNormals->GetDesc();
        Check(desc.Format==DXGI_FORMAT_R16G16_UNORM && desc.Width==Width && desc.Height==Height,"LP normals texture contract");
        Begin(); D3D12_RESOURCE_BARRIER barrier{}; barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource=observedNormals.Get(); barrier.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore=HoYoShade::Depth::ShaderRead; barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;
        list->ResourceBarrier(1,&barrier);
        D3D12_TEXTURE_COPY_LOCATION src{},dst{}; src.pResource=observedNormals.Get(); src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.pResource=readback.Get(); dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint=footprint;
        dst.PlacedFootprint.Footprint.Format=desc.Format; list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
        std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter); list->ResourceBarrier(1,&barrier); Submit();
        unsigned char* mapped=nullptr; Hr(readback->Map(0,nullptr,reinterpret_cast<void**>(&mapped)));
        uint64_t hash=14695981039346656037ull; unsigned nonzero=0;
        for(unsigned y=0;y<Height;++y)for(unsigned x=0;x<Width*4;++x){auto v=mapped[y*footprint.Footprint.RowPitch+x];nonzero+=v!=0;hash=(hash^v)*1099511628211ull;}
        std::printf("LP normals nonzero=%u hash=%llx first=%u,%u,%u,%u\n",nonzero,hash,mapped[0],mapped[1],mapped[2],mapped[3]);
        readback->Unmap(0,nullptr); Check(nonzero>Width*Height,"LP normals contain real GPU data");
        return hash;
    }
    void CheckMxaoPixels() {
        Begin(); D3D12_TEXTURE_COPY_LOCATION src{},dst{}; src.pResource=color.Get(); src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.pResource=readback.Get(); dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint=footprint;
        list->CopyTextureRegion(&dst,0,0,0,&src,nullptr); Submit();
        unsigned char* mapped=nullptr; Hr(readback->Map(0,nullptr,reinterpret_cast<void**>(&mapped))); unsigned changed=0;
        for(unsigned y=0;y<Height;++y)for(unsigned x=0;x<Width;++x){const auto r=mapped[y*footprint.Footprint.RowPitch+x*4];changed+=r!=32;}
        readback->Unmap(0,nullptr); Check(changed>0,"actual MXAO changes SR input pixels with supplied depth");
    }
    void CheckDepthPixels() {
        Begin(); D3D12_TEXTURE_COPY_LOCATION src{}, dst{}; src.pResource = color.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.pResource = readback.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = footprint;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr); Submit();
        unsigned char* mapped = nullptr; Hr(readback->Map(0, nullptr, reinterpret_cast<void**>(&mapped)));
        for (unsigned y = 0; y < Height; ++y) for (unsigned x = 0; x < Width; ++x) {
            const auto expected = (((x < Width/3) != (y < Height/2)) != bool(pattern & 1)) ? 64u : 191u;
            Check(mapped[y*footprint.Footprint.RowPitch+x*4] == expected, "same-frame asymmetric depth pixels and Y orientation");
        }
        readback->Unmap(0, nullptr);
    }
    void NaturalPixels(bool suppress, bool expectedSuppression, bool worker = false) {
        // The exported update invokes the real runtime draw. Event facts are
        // supplied explicitly because this host disables backend interception.
        ComPtr<ID3D11Texture2D> back = reinterpret_cast<ID3D11Texture2D*>(natural->get_current_back_buffer().handle);
        ComPtr<ID3D11RenderTargetView> rtv; Hr(device11->CreateRenderTargetView(back.Get(), nullptr, &rtv));
        constexpr float clear[]{32.0f / 255, 32.0f / 255, 32.0f / 255, 1};
        context->ClearRenderTargetView(rtv.Get(), clear);
        bool suppressed = false;
        if (suppress) {
            const auto invoke = [&] { suppressed = HoYoShade::Testing::BeforePresent(natural, chain.Get()); };
            if (worker) { std::thread thread(invoke); thread.join(); } else invoke();
        }
        Check(suppressed == expectedSuppression, "exact registered source placement, including worker delivery");
        update(natural);
        D3D11_TEXTURE2D_DESC desc{}; back->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = desc.MiscFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging; Hr(device11->CreateTexture2D(&desc, nullptr, &staging));
        context->CopyResource(staging.Get(), back.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{}; Hr(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
        const auto expected = expectedSuppression ? 32u : 64u;
        std::printf("natural probe suppression=%d worker=%d actual_red=%u expected=%u effects_enabled=%d\n",
            suppressed, worker, static_cast<unsigned char*>(mapped.pData)[0], expected, natural->get_effects_state()); std::fflush(stdout);
        for (unsigned y = 0; y < Height; ++y) for (unsigned x = 0; x < Width; ++x)
            Check(static_cast<unsigned char*>(mapped.pData)[y * mapped.RowPitch + x * 4] == expected,
                "natural Present pixels obey BeforeSR placement");
        context->Unmap(staging.Get(), 0);
        std::printf("natural suppression=%d worker=%d red=%u\n", suppressed, worker, expected); std::fflush(stdout);
    }
    HoYoShade::StageResult Filter(uint64_t frame, bool provideDepth = true, bool mismatch = false) {
        HoYoShade::BeginBeforeSrSource(context.Get(), 1);
        HoYoShade::NaturalAssociation::Source::Scope source(HoYoShade::CaptureBridgeSource(device11.Get(), 1));
        Check(source.Get() != nullptr, "current source proof"); HoYoShade::BindNaturalSource(source.Get());
        const auto& identity = source.Get()->Observation(); HoYoShade::ColorEndpoint endpoint;
        endpoint.stage = HoYoShade::Stage::BeforeSr; endpoint.color = color.Get(); endpoint.queue = queue.Get(); endpoint.ready = {queue.Get(), done.Get(), serial};
        endpoint.width = Width; endpoint.height = Height; endpoint.stateKnown = endpoint.sdr = true;
        endpoint.state = D3D12_RESOURCE_STATE_COMMON; endpoint.stream = 1; endpoint.generation = identity.generation; endpoint.frame = identity.frame;
        endpoint.naturalSource = source.Get(); endpoint.naturalContract = HoYoShade::NaturalSourceContract::BeforeSrBridgeInput;
        endpoint.naturalDevice = identity.sourceDevice; endpoint.naturalBackbuffer = identity.buffer;
        std::shared_ptr<TestDepthReader> reader;
        if (useDepth && provideDepth) {
            auto& input = endpoint.depth; input.resource = depth[pattern & 1].Get(); input.typedFormat = DXGI_FORMAT_R32_FLOAT;
            input.width = Width; input.height = Height; input.stateKnown = input.directionKnown = true;
            input.queue = queue.Get(); input.readyFence = done.Get(); input.readyValue = serial;
            input.owner = endpoint.stream; input.generation = endpoint.generation; input.frame = endpoint.frame; input.epoch = 1;
            reader = std::make_shared<TestDepthReader>(); reader->resource = input.resource; reader->expected = input;
            input.reader = reader;
            if (mismatch) ++input.frame;
        }
        auto result = HoYoShade::RenderStage(endpoint);
        if (result.Submitted()) Wait(result.completion.fence.Get(), result.completion.value);
        if (result.depthAvailable) Check(reader && reader->completion && !reader->quarantined &&
            reader->completion->GetCompletedValue() >= reader->completedValue, "real depth reader completion");
        std::printf("attempt=%llu reason=%s submitted=%d effects=%d\n", frame, HoYoShade::Text(result.reason), result.Submitted(), result.EffectsExecuted()); std::fflush(stdout);
        return result;
    }
};
Host* observedHost = nullptr;
void Finished(api::effect_runtime* runtime,api::command_list*,api::resource_view rtv,api::resource_view) { if(observedHost) observedHost->ObserveTextures(runtime,rtv); }
int main(int argc, char** argv) {
    try {
        Check(argc == 2 || argc == 3, "sandbox path argument");
        const bool depthMode = argc == 3 && std::string_view(argv[2]) == "depth";
        const bool launchpadMode = argc == 3 && std::string_view(argv[2]) == "launchpad"; const auto root = std::filesystem::absolute(argv[1]);
        SetEnvironmentVariableW(L"RESHADE_DISABLE_GRAPHICS_HOOK", L"1");
        SetEnvironmentVariableW(L"RESHADE_DISABLE_LOADING_CHECK", L"1");
        SetEnvironmentVariableW(L"RESHADE_DISABLE_INPUT_HOOK", L"1");
        SetEnvironmentVariableW(L"RESHADE_DISABLE_NETWORK_HOOK", L"1");
        auto core = LoadLibraryW((root / "HoYoShade/ReShade64.dll").c_str());
        if (!core) std::printf("LoadLibrary error %lu\n", GetLastError());
        Check(core != nullptr, "load sandbox pinned core");
        Check(HoYoShade::Testing::ConnectHost(GetModuleHandleW(nullptr), (root / "natural.ini").string().c_str(), HoYoShade::Stage::BeforeSr), "real API20 identity handshake");
        Host host(core, root / "natural.ini", depthMode || launchpadMode);
        observedHost=&host;
        auto registerEvent=Export<HoYoShade::CoreApi::RegisterEvent>(core,"ReShadeRegisterEvent");
        registerEvent(reshade::addon_event::reshade_finish_effects,reinterpret_cast<void*>(Finished));
        bool executed = false;
        bool warmupObserved = false;
        for (unsigned frame = 1; frame <= 600; ++frame) {
            host.update(host.natural); host.Upload();
            const auto result = host.Filter(frame);
            if (result.Uncertain()) throw std::runtime_error("owned stage became uncertain");
            if (launchpadMode && result.inputWarmup && result.recordedTechniques) {
                Check(result.Submitted() && result.depthAvailable && !result.EffectsExecuted(),"first IPC batch is depth-bound private warmup");
                Check(host.Read()==32,"IPC warmup leaves SR input unchanged"); warmupObserved=true;
            }
            if (result.EffectsExecuted()) {
                if (launchpadMode) { Check(warmupObserved,"LP/MXAO applied only after real warmup fence"); host.CheckMxaoPixels(); executed=true; break; }
                if (depthMode) { host.CheckDepthPixels(); executed = true; break; }
                const auto red = host.Read(); std::printf("filtered input red=%u expected=64\n", red);
                Check(red == 64, "one real ReShade effect copied back before SR"); executed = true; break;
            }
            Sleep(10);
        }
        Check(executed, "owned BeforeSR effect executed");
        if (launchpadMode) {
            const auto first=host.ReadNormals(); host.pattern=1; host.update(host.natural); host.Upload();
            const auto result=host.Filter(701); Check(result.EffectsExecuted() && result.depthAvailable,"LP/MXAO next frame current input");
            const auto next=host.ReadNormals(); Check(first!=next,"LP normals follow changed asymmetric depth");
            host.update(host.natural); host.Upload();
            const auto missing=host.Filter(702,false); Check(!missing.EffectsExecuted() && host.Read()==32,"LP/MXAO missing depth leaves SR input unchanged");
            host.update(host.natural); host.Upload();
            const auto mismatch=host.Filter(703,true,true); Check(!mismatch.EffectsExecuted() && host.Read()==32,"LP/MXAO mismatched frame rejected");
            std::printf("PASS real LP/MXAO WARP: %u checks, normals=%llx/%llx; no game/performance acceptance\n",checks,first,next);return 0;
        }
        if (depthMode) {
            for (unsigned frame = 241; frame < 249; ++frame) {
                host.pattern = frame & 1; host.update(host.natural); host.Upload();
                const auto result = host.Filter(frame); Check(result.EffectsExecuted() && result.depthAvailable, "alternating source depth remains available");
                host.CheckDepthPixels();
            }
            std::printf("PASS real core same-frame depth: %u checks; WARP, no game/performance acceptance\n", checks); return 0;
        }
        // The natural runtime's startup splash is independent GUI work. Let
        // its five-second display expire before comparing every effect pixel.
        Sleep(5200);
        host.update(host.natural); // finish any owned-stage suppression epoch
        host.NaturalPixels(false, false);
        host.NaturalPixels(true, true);
        host.NaturalPixels(true, true, true);
        {
            HoYoShade::BridgeSourceResize resize(&host);
            host.NaturalPixels(true, false);
        }
        HoYoShade::ForgetBridgeSource(&host);
        host.NaturalPixels(true, false);
        HoYoShade::RegisterDirectChain(host.chain.Get(), &host);
        host.NaturalPixels(true, true, true);
        {
            HoYoShade::DirectResizeScope resize(host.chain.Get(), &host);
            Check(SUCCEEDED(resize.Result()), "idle direct resize reservation");
            host.NaturalPixels(true, false);
            resize.Complete(S_OK);
        }
        host.NaturalPixels(true, true);
        HoYoShade::ForgetDirectChain(host.chain.Get(), &host);
        host.NaturalPixels(true, false);
        HoYoShade::RegisterBridgeSource(&host, host.chain.Get(), host.device11.Get());
        HoYoShade::Testing::EnableHost(false); host.Upload();
        Check(!host.Filter(241).Submitted() && host.Read() == 32, "off leaves unfiltered SR input");
        std::printf("PASS real ReShade BeforeSR WARP: %u checks; no NGX/game/display acceptance\n", checks);
        return 0;
    } catch (const std::exception& error) { std::fprintf(stderr, "FAIL: %s\n", error.what()); return 1; }
}

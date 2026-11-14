// Share the pinned-core host and pixel oracles. This entry exercises the real
// BeforeNR transaction and depth snapshot, without an NVIDIA SDK or a game.
#define main BeforeSrHostMain
#include "host.cpp"
#undef main
#include <native/NrConsumerFilter.h>

// Standalone transport seam: the product wrapper alone constructs BridgeScope.
// The fixture supplies actual chain/buffer, upload Execute/Signal and queue Wait.
class Dx11wDx12SC {
public:
    static bool Filter(native::nrfilter::PresentTransaction& transaction, Host& host,
        IDXGISwapChain* presenter, ID3D12Resource* color, uint64_t epoch,
        const native::nrfilter::depth::Capture& depth) {
        ComPtr<IDXGISwapChain3> source; Hr(host.chain.As(&source));
        native::nrfilter::BridgeScope scope(host.chain.Get(), host.device11.Get(),
            source->GetCurrentBackBufferIndex(), presenter, host.queue.Get(), host.queue.Get(), epoch,
            host.queue.Get(), host.done.Get(), host.serial);
        Hr(host.queue->Wait(host.done.Get(), host.serial));
        return transaction.BeforeNr(presenter, host.queue.Get(), color, true, epoch,
                                    true, host.queue.Get(), depth);
    }
};

struct NrHost : Host {
    ComPtr<IDXGISwapChain3> presenter;
    native::nrfilter::PresentTransaction transaction;
    HWND outputWindow = nullptr;
    api::effect_runtime* physical = nullptr;
    HMODULE core;
    std::filesystem::path config;
    explicit NrHost(HMODULE loaded, const std::filesystem::path& path, bool depthEnabled)
        : Host(loaded, path, depthEnabled), core(loaded), config(path) {
        outputWindow = CreateWindowExW(0, L"F6BeforeSrWarp", L"F6 NR output", WS_POPUP,
            0, 0, Width, Height, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        Check(outputWindow != nullptr, "hidden NR presenter");
        DXGI_SWAP_CHAIN_DESC1 desc{}; desc.Width=Width; desc.Height=Height;
        desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count=1;
        desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT; desc.BufferCount=2;
        desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
        ComPtr<IDXGISwapChain1> chain12;
        Hr(factory->CreateSwapChainForHwnd(queue.Get(), outputWindow, &desc, nullptr, nullptr, &chain12));
        Hr(chain12.As(&presenter));
        HoYoShade::RegisterDirectChain(presenter.Get(), this);
    }
    ~NrHost() {
        if (physical) destroy(physical);
        HoYoShade::ForgetDirectChain(presenter.Get(), this);
        if (outputWindow) DestroyWindow(outputWindow);
    }
    static void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
        D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER b{}; b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after};
        list->ResourceBarrier(1,&b);
    }
    ComPtr<ID3D12Resource> Buffer() {
        ComPtr<ID3D12Resource> result;
        Hr(presenter->GetBuffer(presenter->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&result)));
        return result;
    }
    void CopyToPresenter() {
        auto target=Buffer(); Begin();
        Transition(list.Get(),target.Get(),D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyResource(target.Get(),color.Get());
        Transition(list.Get(),target.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PRESENT);
        Submit();
    }
    void Consume() {
        // A real downstream queue copy reads the same final texture NR consumes.
        // CPU return/status alone cannot make the original upload pass an oracle.
        auto input=Buffer(); Begin();
        Transition(list.Get(),input.Get(),D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyResource(color.Get(),input.Get());
        Transition(list.Get(),input.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_PRESENT);
        Submit();
    }
    bool FilterNr(uint64_t frame, bool provideDepth=true, bool mismatch=false) {
        CopyToPresenter();
        native::nrfilter::depth::Capture capture{};
        if (useDepth && provideDepth) {
            Begin(); Transition(list.Get(),depth[pattern&1].Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST); Submit();
            capture={depth[pattern&1].Get(),queue.Get(),done.Get(),serial,serial,
                mismatch ? frame+1 : frame,{0,0,Width,Height,true},true,false};
        }
        auto buffer=Buffer();
        const auto safe=Dx11wDx12SC::Filter(transaction,*this,presenter.Get(),buffer.Get(),frame,capture);
        if (safe) Consume(); // failed/uncertain output is never consumed
        if (useDepth && provideDepth && transaction.DepthSourceFinished()) {
            Begin(); Transition(list.Get(),depth[pattern&1].Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON); Submit();
        }
        transaction.CollectDepth();
        return safe;
    }
    void PhysicalPixels(bool suppress, bool expected, bool worker) {
        Upload(); CopyToPresenter();
        const auto invoke=[&] { return HoYoShade::Testing::BeforePresent(physical,presenter.Get()); };
        bool suppressed=false;
        if(suppress) { if(worker) { std::thread t([&]{suppressed=invoke();}); t.join(); } else suppressed=invoke(); }
        Check(suppressed==expected,"physical runtime stage placement");
        update(physical); Consume();
        Check(Read()==(expected?32u:64u),"physical runtime does not redraw BeforeNR at Present");
    }
};

HRESULT FailSignal(ID3D12CommandQueue*,ID3D12Fence*,uint64_t) { return E_FAIL; }
int main(int argc,char** argv) {
    try {
        Check(argc==3,"BeforeNR fixture arguments");
        const std::string_view mode(argv[2]); const auto root=std::filesystem::absolute(argv[1]);
        const bool depthMode=mode=="nr-depth", lp=mode=="nr-launchpad", fault=mode=="nr-uncertain";
        for(auto name:{L"RESHADE_DISABLE_GRAPHICS_HOOK",L"RESHADE_DISABLE_LOADING_CHECK",L"RESHADE_DISABLE_INPUT_HOOK",L"RESHADE_DISABLE_NETWORK_HOOK"}) SetEnvironmentVariableW(name,L"1");
        auto core=LoadLibraryW((root/"HoYoShade/ReShade64.dll").c_str()); Check(core!=nullptr,"pinned core load");
        Check(HoYoShade::Testing::ConnectHost(GetModuleHandleW(nullptr),(root/"natural.ini").string().c_str(),HoYoShade::Stage::BeforeNr),"real BeforeNR handshake");
        NrHost host(core,root/"natural.ini",depthMode||lp); observedHost=&host;
        Export<HoYoShade::CoreApi::RegisterEvent>(core,"ReShadeRegisterEvent")(
            reshade::addon_event::reshade_finish_effects,reinterpret_cast<void*>(Finished));
        bool executed=false,warmed=false;
        for(uint64_t frame=1;frame<=600;++frame) {
            host.update(host.natural); host.Upload(); Check(host.FilterNr(frame),"BeforeNR consumer remains safe");
            const auto status=HoYoShade::ReadStatus();
            Check(status.read==HoYoShade::StatusRead::Ready,"current transaction status");
            if(status.latest.known && status.latest.frame==frame) {
                if(status.latest.reason==HoYoShade::Reason::EffectInputsPending) {
                    Check(host.Read()==32,"LP warmup leaves NR input unchanged"); warmed=true;
                }
                if(status.latest.submission==HoYoShade::SubmissionState::Submitted && status.latest.techniques) {
                    if(lp) { Check(warmed,"LP/MXAO real warmup before NR write"); host.CheckMxaoPixels(); }
                    else if(depthMode) host.CheckDepthPixels();
                    else Check(host.Read()==64,"real ReShade before NR consumer copy");
                    executed=true; break;
                }
            }
            Sleep(10);
        }
        Check(executed,"real BeforeNR transaction executed");
        if(fault) {
            host.update(host.natural); host.Upload();
            HoYoShade::OwnedRuntime::Testing::Hooks hooks{}; hooks.signal=FailSignal;
            HoYoShade::OwnedRuntime::Testing::SetHooks(hooks);
            Check(!host.FilterNr(700),"failed real filter Signal stops consumer");
            Check(HoYoShade::ColorConsumers(host.presenter.Get())!=HoYoShade::ConsumerAccess::Safe,"uncertain color retained");
            HoYoShade::Testing::EnableHost(false);
            Check(!host.FilterNr(701),"disable cannot permit uncertain consumer");
            std::printf("PASS real BeforeNR uncertain: %u checks; no game/SDK proof\n",checks); return 0;
        }
        if(depthMode||lp) {
            const auto first=lp?host.ReadNormals():0;
            for(uint64_t frame=701;frame<704;++frame) {
                host.pattern=unsigned(frame&1); host.update(host.natural); host.Upload();
                Check(host.FilterNr(frame),"alternating depth source healthy");
                if(depthMode) host.CheckDepthPixels();
            }
            if(lp) Check(first!=host.ReadNormals(),"BeforeNR normals follow real changed depth");
            // LP/MXAO explicitly requires depth; the generic probe deliberately
            // keeps the optional-depth contract of arbitrary ReShade effects.
            if(lp) {
                host.update(host.natural); host.Upload(); Check(host.FilterNr(710,false),"missing depth untouched refusal");
                Check(host.Read()==32,"missing depth never writes NR input");
                host.update(host.natural); host.Upload(); Check(host.FilterNr(711,true,true),"wrong-frame depth untouched refusal");
                Check(host.Read()==32,"wrong-frame depth never writes NR input");
            }
            std::printf("PASS real BeforeNR depth/LP: %u checks; production snapshot + real filter/consumer fences\n",checks); return 0;
        }
        auto create=Export<HoYoShade::CoreApi::CreateEffectRuntime>(core,"ReShadeCreateEffectRuntime");
        Check(create(api::device_api::d3d12,host.device.Get(),host.queue.Get(),host.presenter.Get(),
            (root/"natural.ini").string().c_str(),&host.physical)&&host.physical,"real physical runtime");
        for(unsigned i=0;i<100;++i) { host.update(host.physical); Sleep(10); }
        Sleep(5200); host.update(host.natural);
        host.NaturalPixels(true,true,true);
        host.PhysicalPixels(true,true,true);
        { HoYoShade::DirectResizeScope resize(host.presenter.Get(),&host);
          Check(SUCCEEDED(resize.Result()),"BeforeNR direct resize reservation");
          host.PhysicalPixels(true,false,false); resize.Complete(S_OK); }
        host.PhysicalPixels(true,true,false);
        HoYoShade::Testing::EnableHost(false); host.PhysicalPixels(true,false,false);
        std::printf("PASS real BeforeNR color/placement: %u checks; real core, GPU pixels, source/physical/worker/resize\n",checks); return 0;
    } catch(const std::exception& error) { std::fprintf(stderr,"FAIL BeforeNR: %s\n",error.what()); return 1; }
}

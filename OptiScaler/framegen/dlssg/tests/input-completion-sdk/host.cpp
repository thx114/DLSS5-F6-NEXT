// Real Streamline DLL probe in a standalone process. No injection, game,
// user configuration, ASI or ReShade runtime is involved.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <sl.h>
#include <sl_dlss_g.h>
#include <sl_reflex.h>
#include <sl_pcl.h>
#include <DirectXMath.h>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>
using Microsoft::WRL::ComPtr;

void Require(bool value,const char* operation) {
    if(!value) throw std::runtime_error(operation);
}
void Hr(HRESULT result,const char* operation) {
    std::printf("DXGI op=%s result=%08lx\n",operation,static_cast<unsigned long>(result));
    Require(SUCCEEDED(result),operation);
}
void Sl(sl::Result result,const char* operation) {
    std::printf("SDK op=%s result=%u\n",operation,static_cast<unsigned>(result));
    Require(result==sl::Result::eOk,operation);
}
template<class T> T* Export(HMODULE module,const char* name) {
    auto* function=reinterpret_cast<T*>(GetProcAddress(module,name));
    Require(function!=nullptr,name); return function;
}

struct Library final {
    HMODULE module=nullptr;
    explicit Library(const std::filesystem::path& runtime) {
        module=LoadLibraryExW((runtime/L"sl.interposer.dll").c_str(),nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        Require(module!=nullptr,"load actual interposer");
    }
    ~Library() {if(module) FreeLibrary(module);}
};
struct Sdk final {
    HMODULE module=nullptr;
    bool initialized=false;
    PFun_slShutdown* shutdown=nullptr;
    PFun_slGetFeatureFunction* featureFunction=nullptr;
    PFun_slSetD3DDevice* setDevice=nullptr;
    PFun_slIsFeatureSupported* supported=nullptr;
    PFun_slDLSSGGetState* getState=nullptr;
    PFun_slDLSSGSetOptions* setOptions=nullptr;
    PFun_slReflexSetOptions* setReflex=nullptr;
    PFun_slReflexSleep* sleep=nullptr;
    PFun_slPCLSetMarker* marker=nullptr;
    PFun_slGetNewFrameToken* token=nullptr;
    PFun_slSetConstants* constants=nullptr;
    PFun_slSetTagForFrame* tag=nullptr;
    PFun_slFreeResources* freeResources=nullptr;
    Sdk(HMODULE library,const std::filesystem::path& runtime,const std::filesystem::path& output):module(library) {
        auto* init=Export<PFun_slInit>(module,"slInit");
        shutdown=Export<PFun_slShutdown>(module,"slShutdown");
        featureFunction=Export<PFun_slGetFeatureFunction>(module,"slGetFeatureFunction");
        setDevice=Export<PFun_slSetD3DDevice>(module,"slSetD3DDevice");
        supported=Export<PFun_slIsFeatureSupported>(module,"slIsFeatureSupported");
        token=Export<PFun_slGetNewFrameToken>(module,"slGetNewFrameToken");
        constants=Export<PFun_slSetConstants>(module,"slSetConstants");
        tag=Export<PFun_slSetTagForFrame>(module,"slSetTagForFrame");
        freeResources=Export<PFun_slFreeResources>(module,"slFreeResources");
        const auto directory=runtime.wstring(),logs=output.wstring();
        const wchar_t* paths[]{directory.c_str()};
        const sl::Feature features[]{sl::kFeatureDLSS_G,sl::kFeatureReflex,sl::kFeaturePCL};
        sl::Preferences preferences;
        preferences.applicationId=0x0F71CA1E; // Existing product's application identity.
        preferences.renderAPI=sl::RenderAPI::eD3D12;
        preferences.featuresToLoad=features; preferences.numFeaturesToLoad=3;
        preferences.pathsToPlugins=paths; preferences.numPathsToPlugins=1;
        preferences.pathToLogsAndData=logs.c_str(); preferences.showConsole=false;
        preferences.logLevel=sl::LogLevel::eVerbose;
        preferences.logMessageCallback=[](sl::LogType type,const char* message) {
            std::printf("SL_LOG type=%u %s",static_cast<unsigned>(type),message); std::fflush(stdout);
        };
        preferences.flags|=sl::PreferenceFlags::eUseFrameBasedResourceTagging|sl::PreferenceFlags::eUseDXGIFactoryProxy;
        preferences.flags&=~sl::PreferenceFlags::eUseManualHooking;
        preferences.flags&=~sl::PreferenceFlags::eAllowOTA;
        preferences.flags&=~sl::PreferenceFlags::eLoadDownloadedPlugins;
        const auto result=init(preferences,sl::kSDKVersion);
        initialized=result==sl::Result::eOk;
        Sl(result,"slInit");
    }
    void Bind(ID3D12Device* device) {
        Sl(setDevice(device),"slSetD3DDevice");
        void* function=nullptr;
        Sl(featureFunction(sl::kFeatureDLSS_G,"slDLSSGGetState",function),"resolve active slDLSSGGetState");
        getState=reinterpret_cast<PFun_slDLSSGGetState*>(function);
        Require(getState!=nullptr,"active SDK state function");
        HMODULE active=nullptr;
        Require(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(function),&active)!=FALSE,"actual state function module");
        wchar_t path[32768]{};
        Require(GetModuleFileNameW(active,path,32768)!=0,"actual state function path");
        std::printf("ACTIVE_STATE_MODULE path=%ls\n",path);
        setOptions=Feature<PFun_slDLSSGSetOptions>(sl::kFeatureDLSS_G,"slDLSSGSetOptions");
        setReflex=Feature<PFun_slReflexSetOptions>(sl::kFeatureReflex,"slReflexSetOptions");
        sleep=Feature<PFun_slReflexSleep>(sl::kFeatureReflex,"slReflexSleep");
        marker=Feature<PFun_slPCLSetMarker>(sl::kFeaturePCL,"slPCLSetMarker");
    }
    template<class T> T* Feature(sl::Feature feature,const char* name) {
        void* function=nullptr; Sl(featureFunction(feature,name,function),name);
        Require(function!=nullptr,name); return reinterpret_cast<T*>(function);
    }
    ~Sdk() {
        if(initialized && !SlNoThrow(shutdown(),"slShutdown")) {
            std::fputs("CLEANUP_FAILED actual SDK shutdown failed; ending standalone process\n",stderr);
            ExitProcess(5);
        }
    }
    static bool SlNoThrow(sl::Result result,const char* operation) noexcept {
        std::printf("SDK cleanup=%s result=%u\n",operation,static_cast<unsigned>(result));
        return result==sl::Result::eOk;
    }
};

struct Window final {
    HWND handle=nullptr;
    explicit Window(bool visible) {
        WNDCLASSW wc{}; wc.lpfnWndProc=DefWindowProcW; wc.hInstance=GetModuleHandleW(nullptr);
        wc.lpszClassName=L"F6SdkInputProbe"; RegisterClassW(&wc);
        handle=CreateWindowExW(0,wc.lpszClassName,L"F6 standalone SDK input probe",WS_POPUP,
            0,0,1920,1080,nullptr,nullptr,wc.hInstance,nullptr);
        Require(handle!=nullptr,"create isolated hidden window");
        if(visible) {
            // Manual opt-in only. Do not take focus or synthesize any input.
            ShowWindow(handle,SW_SHOWNOACTIVATE);
            std::puts("MANUAL_FOCUS_REQUIRED click the standalone probe window; no game is launched");
        }
    }
    ~Window() {if(handle) DestroyWindow(handle);}
};

struct Probe final {
    static constexpr UINT Width=1920,Height=1080;
    static constexpr D3D12_RESOURCE_STATES Readable=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE|
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    Sdk& sdk;
    Window window;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<IDXGISwapChain3> chain;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12DescriptorHeap> views;
    ComPtr<ID3D12Resource> depth,motion,hudless;
    ComPtr<ID3D12Fence> fence;
    UINT64 serial=0;
    HANDLE event=nullptr;
    sl::DLSSGOptions options;
    bool visible=false;
    Probe(Sdk& value,IDXGIFactory6* factory,ID3D12Device* hardware,bool show)
        :sdk(value),window(show),device(hardware),visible(show) {
        D3D12_COMMAND_QUEUE_DESC q{};
        Hr(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)),"create presenting queue");
        Hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"create host completion fence");
        event=CreateEventW(nullptr,FALSE,FALSE,nullptr); Require(event!=nullptr,"host completion event");
        DXGI_SWAP_CHAIN_DESC1 desc{}; desc.Width=Width; desc.Height=Height;
        desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count=1;
        desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT; desc.BufferCount=2;
        desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
        ComPtr<IDXGISwapChain1> created;
        Hr(factory->CreateSwapChainForHwnd(queue.Get(),window.handle,&desc,nullptr,nullptr,&created),"create SDK swapchain");
        Hr(created.As(&chain),"SDK swapchain3");
        Hr(chain->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709),"declare SDR");
        Hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"create allocator");
        Hr(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)),"create command list");
        Hr(list->Close(),"initial list close");
        D3D12_DESCRIPTOR_HEAP_DESC heap{}; heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;heap.NumDescriptors=4;
        Hr(device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&views)),"create RTV heap");
        CreateInput(depth,DXGI_FORMAT_R32_FLOAT,0);
        CreateInput(motion,DXGI_FORMAT_R16G16_FLOAT,1);
        CreateInput(hudless,DXGI_FORMAT_R8G8B8A8_UNORM,2);
        sl::ReflexOptions reflex; reflex.mode=sl::ReflexMode::eLowLatency;
        Sl(sdk.setReflex(reflex),"enable actual SDK Reflex");
        options.mode=sl::DLSSGMode::eOn;options.numFramesToGenerate=1;
        options.numBackBuffers=2;options.colorWidth=Width;options.colorHeight=Height;
        options.mvecDepthWidth=Width;options.mvecDepthHeight=Height;
        options.colorBufferFormat=DXGI_FORMAT_R8G8B8A8_UNORM;
        options.hudLessBufferFormat=DXGI_FORMAT_R8G8B8A8_UNORM;
        options.mvecBufferFormat=DXGI_FORMAT_R16G16_FLOAT;options.depthBufferFormat=DXGI_FORMAT_R32_FLOAT;
        options.queueParallelismMode=sl::DLSSGQueueParallelismMode::eBlockPresentingClientQueue;
        Sl(sdk.setOptions(sl::ViewportHandle(0),options),"enable real SDK FG");
    }
    D3D12_CPU_DESCRIPTOR_HANDLE View(UINT slot) const {
        auto handle=views->GetCPUDescriptorHandleForHeapStart();
        handle.ptr+=slot*device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);return handle;
    }
    void CreateInput(ComPtr<ID3D12Resource>& resource,DXGI_FORMAT format,UINT slot) {
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width=Width;desc.Height=Height;desc.DepthOrArraySize=desc.MipLevels=desc.SampleDesc.Count=1;
        desc.Format=format;desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        Hr(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,Readable,nullptr,IID_PPV_ARGS(&resource)),"create owned input texture");
        device->CreateRenderTargetView(resource.Get(),nullptr,View(slot));
    }
    void Transition(ID3D12Resource* resource,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after};list->ResourceBarrier(1,&b);
    }
    void Drain() {
        Hr(queue->Signal(fence.Get(),++serial),"host tail Signal");
        Hr(fence->SetEventOnCompletion(serial,event),"host completion notification");
        const auto wait=WaitForSingleObject(event,10000);
        Require(wait==WAIT_OBJECT_0 && fence->GetCompletedValue()!=UINT64_MAX && fence->GetCompletedValue()>=serial,
            "actual host/present queue completion");
    }
    unsigned Run() {
        unsigned consumed=0;
        const uint32_t frames=visible?120:24;
        if(visible) {
            const auto deadline=GetTickCount64()+45000;
            while(GetForegroundWindow()!=window.handle && GetTickCount64()<deadline) {
                MSG message{};while(PeekMessageW(&message,window.handle,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}
                Sleep(20);
            }
            if(GetForegroundWindow()!=window.handle) {
                std::puts("PROBE_LEVEL=NO_FOCUSED_PRESENT manual_focus_timeout=true game_test=false"); return 0;
            }
        }
        ComPtr<IUnknown> previousFence;
        UINT64 previousTarget=0;
        for(uint32_t frame=1;frame<=frames;++frame) {
            MSG message{};while(PeekMessageW(&message,window.handle,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}
            sl::FrameToken* token=nullptr;Sl(sdk.token(token,&frame),"frame token");Require(token!=nullptr,"nonempty frame token");
            Sl(sdk.sleep(*token),"Reflex sleep");
            Sl(sdk.marker(sl::PCLMarker::eSimulationStart,*token),"simulation start");
            Sl(sdk.marker(sl::PCLMarker::eSimulationEnd,*token),"simulation end");
            Drain();
            Sl(sdk.marker(sl::PCLMarker::eRenderSubmitStart,*token),"render submit start");
            Hr(allocator->Reset(),"allocator Reset after actual completion");Hr(list->Reset(allocator.Get(),nullptr),"list Reset");
            auto index=chain->GetCurrentBackBufferIndex();
            ComPtr<ID3D12Resource> back;
            Hr(chain->GetBuffer(index,IID_PPV_ARGS(&back)),"actual SDK input buffer");
            device->CreateRenderTargetView(back.Get(),nullptr,View(3));
            const float picture[]{float(frame%8)/8.0f,0.25f,0.5f,1},zero[]{0,0,0,0},z[]{0.5f,0,0,0};
            const auto clear=[&](ID3D12Resource* resource,UINT slot,const float* color,D3D12_RESOURCE_STATES initial) {
                Transition(resource,initial,D3D12_RESOURCE_STATE_RENDER_TARGET);
                list->ClearRenderTargetView(View(slot),color,0,nullptr);
                Transition(resource,D3D12_RESOURCE_STATE_RENDER_TARGET,initial);
            };
            clear(depth.Get(),0,z,Readable);clear(motion.Get(),1,zero,Readable);clear(hudless.Get(),2,picture,Readable);
            clear(back.Get(),3,picture,D3D12_RESOURCE_STATE_PRESENT);
            sl::Constants c;
            using namespace DirectX;
            const auto projection=XMMatrixPerspectiveFovRH(1.0f,float(Width)/Height,0.1f,1000.0f);
            const auto store=[](sl::float4x4& out,FXMMATRIX value){XMFLOAT4X4 m;XMStoreFloat4x4(&m,value);std::memcpy(&out,&m,sizeof(m));};
            store(c.cameraViewToClip,projection);store(c.clipToCameraView,XMMatrixInverse(nullptr,projection));
            store(c.clipToLensClip,XMMatrixIdentity());store(c.clipToPrevClip,XMMatrixIdentity());store(c.prevClipToClip,XMMatrixIdentity());
            c.jitterOffset={0,0};c.mvecScale={1.0f/Width,1.0f/Height};c.cameraPinholeOffset={0,0};
            c.cameraPos={0,0,0};c.cameraUp={0,1,0};c.cameraRight={1,0,0};c.cameraFwd={0,0,-1};
            c.cameraNear=0.1f;c.cameraFar=1000;c.cameraFOV=1;c.cameraAspectRatio=float(Width)/Height;
            c.depthInverted=sl::Boolean::eFalse;c.cameraMotionIncluded=sl::Boolean::eTrue;
            c.motionVectors3D=sl::Boolean::eFalse;c.motionVectorsDilated=sl::Boolean::eTrue;
            c.reset=frame==1?sl::Boolean::eTrue:sl::Boolean::eFalse;
            Sl(sdk.constants(c,*token,sl::ViewportHandle(0)),"same-frame constants");
            sl::Resource resources[]{sl::Resource(sl::ResourceType::eTex2d,depth.Get(),Readable),
                sl::Resource(sl::ResourceType::eTex2d,motion.Get(),Readable),sl::Resource(sl::ResourceType::eTex2d,hudless.Get(),Readable)};
            sl::ResourceTag tags[3];
            const sl::BufferType types[]{sl::kBufferTypeDepth,sl::kBufferTypeMotionVectors,sl::kBufferTypeHUDLessColor};
            for(UINT i=0;i<3;++i) {
                auto desc=static_cast<ID3D12Resource*>(resources[i].native)->GetDesc();
                resources[i].width=Width;resources[i].height=Height;resources[i].nativeFormat=desc.Format;
                tags[i].resource=&resources[i];tags[i].type=types[i];tags[i].lifecycle=sl::ResourceLifecycle::eValidUntilPresent;
                tags[i].extent={0,0,Width,Height};
            }
            Sl(sdk.tag(*token,sl::ViewportHandle(0),tags,3,list.Get()),"same-frame owned input tags");
            Hr(list->Close(),"producer list close");ID3D12CommandList* commands[]{list.Get()};queue->ExecuteCommandLists(1,commands);
            Sl(sdk.marker(sl::PCLMarker::eRenderSubmitEnd,*token),"render submit end");
            Sl(sdk.marker(sl::PCLMarker::ePresentStart,*token),"Present start");
            const auto result=chain->Present(0,0);
            // Exact thread, immediately after the actual SDK Present returns.
            sl::DLSSGState state;const auto query=sdk.getState(sl::ViewportHandle(0),state,nullptr);
            ComPtr<ID3D12Fence> completion;
            HRESULT qi=E_NOINTERFACE;
            if(query==sl::Result::eOk && state.structVersion>=3 && state.structVersion<=4 && state.inputsProcessingCompletionFence)
                qi=static_cast<IUnknown*>(state.inputsProcessingCompletionFence)->QueryInterface(IID_PPV_ARGS(&completion));
            const auto target=state.lastPresentInputsProcessingCompletionFenceValue;
            const auto completed=completion?completion->GetCompletedValue():0;
            ComPtr<ID3D12Device> inputDevice,fenceDevice;
            ComPtr<IUnknown> inputIdentity,fenceIdentity,currentFence;
            const bool sameDevice=completion && SUCCEEDED(back->GetDevice(IID_PPV_ARGS(&inputDevice))) &&
                SUCCEEDED(completion->GetDevice(IID_PPV_ARGS(&fenceDevice))) &&
                SUCCEEDED(inputDevice.As(&inputIdentity)) && SUCCEEDED(fenceDevice.As(&fenceIdentity)) && inputIdentity==fenceIdentity;
            if(completion) completion.As(&currentFence);
            const bool progressed=target && target!=UINT64_MAX &&
                (currentFence!=previousFence || target>previousTarget);
            std::printf("PRESENT_SAMPLE frame=%u thread=%lu index=%u color=%p present=%08lx query=%u abi=%zu status=%u actual_presented=%u fence=%p qi=%08lx target=%llu completed=%llu same_device=%u target_advanced=%u focused=%u\n",
                frame,GetCurrentThreadId(),index,back.Get(),static_cast<unsigned long>(result),static_cast<unsigned>(query),state.structVersion,
                static_cast<unsigned>(state.status),state.numFramesActuallyPresented,completion.Get(),static_cast<unsigned long>(qi),
                static_cast<unsigned long long>(target),static_cast<unsigned long long>(completed),sameDevice,progressed,
                GetForegroundWindow()==window.handle);
            if(result==S_OK && query==sl::Result::eOk && state.status==sl::DLSSGStatus::eOk && SUCCEEDED(qi) &&
               sameDevice && progressed && completed!=UINT64_MAX) ++consumed;
            if(progressed) {previousFence=currentFence;previousTarget=target;}
            Sl(sdk.marker(sl::PCLMarker::ePresentEnd,*token),"Present end");
            if(visible) Sleep(16); // synthetic source cadence only; never a game latency measurement
        }
        Drain();
        std::printf("PROBE_LEVEL=REAL_PRESENT frames=%u nonzero_owning_observations=%u hidden_window=%s queue_separated=false game_test=false\n",frames,consumed,visible?"false":"true");
        return consumed;
    }
    ~Probe() {
        try {
            options.mode=sl::DLSSGMode::eOff;
            const bool disabled=Sdk::SlNoThrow(sdk.setOptions(sl::ViewportHandle(0),options),"disable standalone FG");
            if(queue && fence && event) Drain();
            const bool freed=Sdk::SlNoThrow(sdk.freeResources(sl::kFeatureDLSS_G,sl::ViewportHandle(0)),"free standalone FG resources");
            Require(disabled && freed,"actual SDK teardown must succeed");
        } catch(...) {
            std::fputs("CLEANUP_FAILED actual SDK/host retirement unproven; ending standalone process\n",stderr);
            // Do not unwind and release GPU owners after a failed drain. This
            // terminates only this standalone probe process; the OS retires it.
            ExitProcess(5);
        }
        if(event) CloseHandle(event);
    }
};

int wmain(int argc,wchar_t** argv) {
    try {
        std::setvbuf(stdout,nullptr,_IONBF,0);
        Require(argc==3 || (argc==4 && std::wstring(argv[3])==L"--visible"),"expected runtime directory, output and optional --visible");
        const auto runtime=std::filesystem::absolute(argv[1]);
        const auto output=std::filesystem::absolute(argv[2]);
        // Proxies are released before SDK shutdown; its DLL remains loaded
        // through both. A proxy must never call a vtable in an unloaded module.
        Library library(runtime);
        Sdk sdk(library.module,runtime,output);
        ComPtr<IDXGIFactory6> factory;
        ComPtr<IDXGIAdapter1> adapter;
        ComPtr<ID3D12Device> device;
        // Initialize SL before creating D3D objects. Use actual automatic proxies.
        auto* createFactory=Export<decltype(CreateDXGIFactory2)>(sdk.module,"CreateDXGIFactory2");
        auto* createDevice=Export<decltype(D3D12CreateDevice)>(sdk.module,"D3D12CreateDevice");
        Hr(createFactory(0,IID_PPV_ARGS(&factory)),"SDK factory");
        DXGI_ADAPTER_DESC1 description{};
        for(UINT i=0;;++i) {
            ComPtr<IDXGIAdapter1> candidate;
            if(factory->EnumAdapterByGpuPreference(i,DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,IID_PPV_ARGS(&candidate))==DXGI_ERROR_NOT_FOUND) break;
            Hr(candidate->GetDesc1(&description),"adapter description");
            if(description.VendorId==0x10DE && !(description.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)) {adapter=candidate;break;}
        }
        Require(adapter!=nullptr,"actual NVIDIA hardware adapter");
        std::printf("ADAPTER vendor=%04x device=%04x luid=%08lx:%08lx\n",description.VendorId,description.DeviceId,
            static_cast<unsigned long>(description.AdapterLuid.HighPart),description.AdapterLuid.LowPart);
        Hr(createDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)),"actual hardware device");
        sl::AdapterInfo info;
        info.deviceLUID=reinterpret_cast<uint8_t*>(&description.AdapterLuid);
        info.deviceLUIDSizeInBytes=sizeof(description.AdapterLuid);
        const auto capability=sdk.supported(sl::kFeatureDLSS_G,info);
        std::printf("CAPABILITY dlssg=%u\n",static_cast<unsigned>(capability));
        if(capability!=sl::Result::eOk) return 3;
        sdk.Bind(device.Get());
        sl::DLSSGState state;
        const auto result=sdk.getState(sl::ViewportHandle(0),state,nullptr);
        std::printf("STATE scope=capabilities result=%u request_abi=4 returned_abi=%u status=%u max_generated=%u min_size=%u fence=%p target=%llu\n",
            static_cast<unsigned>(result),static_cast<unsigned>(state.structVersion),static_cast<unsigned>(state.status),
            state.numFramesToGenerateMax,state.minWidthOrHeight,state.inputsProcessingCompletionFence,
            static_cast<unsigned long long>(state.lastPresentInputsProcessingCompletionFenceValue));
        Sl(result,"real SDK capability state");
        Probe probe(sdk,factory.Get(),device.Get(),argc==4);
        const auto observations=probe.Run();
        return observations?0:4;
    } catch(const std::exception& error) {
        std::fprintf(stderr,"FAIL real SDK probe: %s (last_error=%lu)\n",error.what(),GetLastError());
        return 1;
    }
}

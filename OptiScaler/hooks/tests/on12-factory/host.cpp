// Actual pinned ReShade, production factory routes and real WARP D3D11On12.
#include <hooks/D3D11On12FactoryRoutes.h>
#include <integration/HoYoShade.h>
#include <d3d11on12.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <source_location>
#include <stdexcept>
#include <thread>
using Microsoft::WRL::ComPtr;
unsigned checks=0;
void Check(bool ok,const std::source_location& at=std::source_location::current()) {
    ++checks;if(!ok){std::printf("FAILED %s:%u\n",at.file_name(),at.line());std::fflush(stdout);throw std::runtime_error("check failed");}
}
void Hr(HRESULT value,const std::source_location& at=std::source_location::current()) {
    if(FAILED(value))std::printf("HRESULT %08lX\n",value);Check(SUCCEEDED(value),at);
}
LONG WINAPI Crash(EXCEPTION_POINTERS* e) {
    std::printf("EXCEPTION %08lX at %p\n",e->ExceptionRecord->ExceptionCode,e->ExceptionRecord->ExceptionAddress);
    std::fflush(stdout);ExitProcess(e->ExceptionRecord->ExceptionCode);
}
void CheckCopy(ID3D11Device* device,ID3D11DeviceContext* context,unsigned marker) {
    const unsigned pixels[]={marker,marker+1,marker+2,marker+3};
    D3D11_BUFFER_DESC desc{};desc.ByteWidth=sizeof(pixels);desc.Usage=D3D11_USAGE_DEFAULT;
    D3D11_SUBRESOURCE_DATA data{};data.pSysMem=pixels;ComPtr<ID3D11Buffer> source,readback;
    Hr(device->CreateBuffer(&desc,&data,&source));desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    Hr(device->CreateBuffer(&desc,nullptr,&readback));context->CopyResource(readback.Get(),source.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};Hr(context->Map(readback.Get(),0,D3D11_MAP_READ,0,&mapped));
    Check(std::memcmp(mapped.pData,pixels,sizeof(pixels))==0);context->Unmap(readback.Get(),0);context->Flush();
}
bool IsProxy(IUnknown* factory) {
    for(const auto& iid:{NativeObjectContracts::ReShade,NativeObjectContracts::Streamline}) {
        ComPtr<IUnknown> native;
        if(factory->QueryInterface(iid,reinterpret_cast<void**>(native.GetAddressOf()))==S_OK && native) return true;
    }
    return false;
}
int wmain(int argc,wchar_t** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);SetUnhandledExceptionFilter(Crash);
    try {
        Check(argc==3);const std::wstring mode=argv[2];
        const bool negative=mode==L"negative",before=mode==L"before",systemOnly=mode==L"system-only-negative";
        ComPtr<IDXGIFactory4> factory;Hr(CreateDXGIFactory2(0,IID_PPV_ARGS(&factory)));
        ComPtr<IDXGIAdapter> adapter;Hr(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)));
        ComPtr<ID3D12Device> device;Hr(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)));
        D3D12_COMMAND_QUEUE_DESC queueDesc{};ComPtr<ID3D12CommandQueue> queue;
        Hr(device->CreateCommandQueue(&queueDesc,IID_PPV_ARGS(&queue)));IUnknown* queues[]={queue.Get()};
        {
            ComPtr<ID3D11Device> d11;ComPtr<ID3D11DeviceContext> context;
            Hr(D3D11On12CreateDevice(device.Get(),0,nullptr,0,queues,1,0,&d11,&context,nullptr));
            CheckCopy(d11.Get(),context.Get(),11);std::puts("PASS native D3D11On12 and GPU readback");std::fflush(stdout);
        }
        const auto system=GetModuleHandleW(L"dxgi.dll");
        auto systemEntry=reinterpret_cast<PVOID>(GetProcAddress(system,"CreateDXGIFactory2"));Check(systemEntry!=nullptr);
        if(before||systemOnly)Check(D3D11On12FactoryRoutes::Install(systemEntry)==NO_ERROR);
        SetEnvironmentVariableW(L"RESHADE_DISABLE_LOADING_CHECK",L"1");
        SetEnvironmentVariableW(L"RESHADE_DISABLE_INPUT_HOOK",L"1");
        SetEnvironmentVariableW(L"RESHADE_DISABLE_NETWORK_HOOK",L"1");
        const auto core=LoadLibraryW(argv[1]);Check(core!=nullptr);
        const auto coreEntry=reinterpret_cast<PVOID>(GetProcAddress(core,"CreateDXGIFactory2"));Check(coreEntry!=nullptr);
        const auto create=reinterpret_cast<PFN_D3D11ON12_CREATE_DEVICE>(GetProcAddress(core,"D3D11On12CreateDevice"));Check(create!=nullptr);
        if(!negative&&!systemOnly) {
            if(!before)Check(D3D11On12FactoryRoutes::Install(systemEntry)==NO_ERROR);
            auto owner=HoYoShade::AcquireExistingFactoryOwner(coreEntry,"CreateDXGIFactory2");Check(owner!=nullptr);
            Check(HoYoShade::ExistingFactoryOwnerIntact(owner,coreEntry,"CreateDXGIFactory2"));
            auto* dos=reinterpret_cast<IMAGE_DOS_HEADER*>(core);
            auto* nt=reinterpret_cast<IMAGE_NT_HEADERS64*>(reinterpret_cast<unsigned char*>(core)+dos->e_lfanew);
            const auto originalBase=nt->OptionalHeader.ImageBase;DWORD protection=0,ignored=0;
            Check(VirtualProtect(&nt->OptionalHeader.ImageBase,sizeof(originalBase),PAGE_READWRITE,&protection)!=FALSE);
            nt->OptionalHeader.ImageBase=reinterpret_cast<uintptr_t>(core)+0x10000;
            Check(VirtualProtect(&nt->OptionalHeader.ImageBase,sizeof(originalBase),protection,&ignored)!=FALSE);
            const auto wrongBase=HoYoShade::AcquireExistingFactoryOwner(coreEntry,"CreateDXGIFactory2");
            Check(VirtualProtect(&nt->OptionalHeader.ImageBase,sizeof(originalBase),PAGE_READWRITE,&protection)!=FALSE);
            nt->OptionalHeader.ImageBase=originalBase;
            Check(VirtualProtect(&nt->OptionalHeader.ImageBase,sizeof(originalBase),protection,&ignored)!=FALSE);
            Check(!wrongBase && HoYoShade::ExistingFactoryOwnerIntact(owner,coreEntry,"CreateDXGIFactory2"));
            Check(D3D11On12FactoryRoutes::Install(coreEntry)==NO_ERROR);
            Check(D3D11On12FactoryRoutes::Install(coreEntry)==NO_ERROR); // duplicate keeps actual original
        }
        const auto createFactory=reinterpret_cast<D3D11On12FactoryRoutes::Function>(coreEntry);
        if(!negative&&!systemOnly) {
            ComPtr<IDXGIFactory4> ordinary;Hr(createFactory(0,IID_PPV_ARGS(&ordinary)));Check(IsProxy(ordinary.Get()));
            {D3D11On12FactoryBoundary::Scope scope;ComPtr<IDXGIFactory4> stillPublic;
                Hr(createFactory(0,IID_PPV_ARGS(&stillPublic)));Check(IsProxy(stillPublic.Get()));
                ComPtr<IDXGIFactory4> internal;Hr(createFactory(0x80000000u,IID_PPV_ARGS(&internal)));Check(!IsProxy(internal.Get()));}
        }
        for(unsigned repeat=0;repeat<12;++repeat) {
            D3D11On12FactoryBoundary::Scope scope;
            ComPtr<ID3D11Device> d11;ComPtr<ID3D11DeviceContext> context;
            Hr(create(device.Get(),0,nullptr,0,queues,1,0,&d11,&context,nullptr));Check(d11&&context);
            CheckCopy(d11.Get(),context.Get(),100+repeat);
        }
        std::atomic<bool> publicResult=false;
        {D3D11On12FactoryBoundary::Scope scope;
            std::thread publicCreation([&] {ComPtr<IDXGIFactory4> ordinary;
                publicResult=SUCCEEDED(createFactory(0,IID_PPV_ARGS(&ordinary)))&&IsProxy(ordinary.Get())&&!D3D11On12FactoryBoundary::Internal(0x80000000u);});
            publicCreation.join();Check(publicResult);}
        std::vector<HookOwnership::Own::Slot*> slots;D3D11On12FactoryRoutes::routes.AppendSlots(slots);
        Check(!slots.empty());std::printf("PASS %u checks: real pinned core, order=%ls, exact factory routes=%zu, repeated On12/GPU readback, public proxies and concurrent scope isolation\n",checks,mode.c_str(),slots.size());std::fflush(stdout);
        // The core owns process-wide graphics hooks; this isolated host ends here.
        ExitProcess(0);
    }catch(const std::exception& e){std::printf("FAIL %s\n",e.what());std::fflush(stdout);ExitProcess(1);}
}

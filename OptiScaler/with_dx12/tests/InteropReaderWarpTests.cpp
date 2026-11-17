// SPDX-License-Identifier: GPL-3.0-or-later
// Actual production writer admission, actual DX11/DX12 shared fence and pixels.
#define NOMINMAX
#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <array>
#include <atomic>
#include <climits>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>
using Microsoft::WRL::ComPtr;
unsigned checks=0;
void Check(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
void Hr(HRESULT result){if(FAILED(result))throw std::runtime_error("D3D HRESULT "+std::to_string(unsigned(result)));}
void WaitCpu(ID3D12Fence* fence,UINT64 value){HANDLE e=CreateEventW(nullptr,FALSE,FALSE,nullptr);Hr(fence->SetEventOnCompletion(value,e));Check(WaitForSingleObject(e,10000)==WAIT_OBJECT_0,"real GPU drain");CloseHandle(e);}
bool ReportInteropCopyFailure(const char*){return false;}
#include "InteropSlotAdmissionProduction.h"
class Dx11wDx12SC {
 public:
    std::atomic<bool> _sourceCopyUncertain{false};
    ID3D12Fence* _copyFence=nullptr;
    HANDLE _copyFenceEvent=nullptr;
    ID3D11Fence* _dx11CopyReaderFence=nullptr;
    ID3D11DeviceContext4* _dx11Context4=nullptr;
    std::vector<ID3D12CommandAllocator*> _copyAllocators;
    std::vector<UINT64> _copyAllocatorFenceValues, _sharedTextureReaderValues;
    UINT _nextCopyAllocator=0,_currentFakeIndex=0;
    UINT64 _copyFenceValue=1;
    unsigned fallbackWaits=0;
    bool _PrepareSharedWriter(UINT,CopySlotAdmission&);
    bool _PrepareSharedWriterLegacy(UINT,CopySlotAdmission&);
    bool _WaitForCopyAllocator(UINT,std::vector<UINT64>&){++fallbackWaits;return false;}
    bool _WaitForCopyAllocator(UINT,std::vector<UINT64>&,CopySlotAdmission&);
    void _ReportCopyFenceFault(const char*,UINT,UINT64,UINT64,DWORD){}
};
#include "InteropReaderWriterProduction.inl"
struct World {
    ComPtr<ID3D12Device> d12;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> gate,readerFence,writerFence;
    ComPtr<ID3D11Device> d11;
    ComPtr<ID3D11Device5> d11v5;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11DeviceContext4> context4;
    ComPtr<ID3D11Fence> reader11,writer11;
    ComPtr<ID3D11Texture2D> texture,blueSource;
    ComPtr<ID3D12Resource> shared;
    std::array<ComPtr<ID3D12CommandAllocator>,3> allocators;
    std::array<ComPtr<ID3D12GraphicsCommandList>,3> lists;
    std::array<ComPtr<ID3D12Resource>,2> readbacks;
    Dx11wDx12SC owner;
    World(){
        ComPtr<IDXGIFactory4> factory;ComPtr<IDXGIAdapter> warp;
        Hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));Hr(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)));
        Hr(D3D12CreateDevice(warp.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&d12)));
        Hr(D3D11CreateDevice(warp.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&d11,nullptr,&context));
        Hr(d11.As(&d11v5));Hr(context.As(&context4));
        D3D12_COMMAND_QUEUE_DESC q{};Hr(d12->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)));
        Hr(d12->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&gate)));
        Hr(d12->CreateFence(0,D3D12_FENCE_FLAG_SHARED,IID_PPV_ARGS(&readerFence)));
        HANDLE h=nullptr;Hr(d12->CreateSharedHandle(readerFence.Get(),nullptr,GENERIC_ALL,nullptr,&h));Hr(d11v5->OpenSharedFence(h,IID_PPV_ARGS(&reader11)));CloseHandle(h);
        Hr(d11v5->CreateFence(0,D3D11_FENCE_FLAG_SHARED,IID_PPV_ARGS(&writer11)));
        Hr(writer11->CreateSharedHandle(nullptr,GENERIC_ALL,nullptr,&h));Hr(d12->OpenSharedHandle(h,IID_PPV_ARGS(&writerFence)));CloseHandle(h);
        std::array<unsigned,16> red;red.fill(0xff0000ff);
        D3D11_TEXTURE2D_DESC t{};t.Width=t.Height=4;t.MipLevels=t.ArraySize=t.SampleDesc.Count=1;t.Format=DXGI_FORMAT_R8G8B8A8_UNORM;t.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;t.MiscFlags=D3D11_RESOURCE_MISC_SHARED|D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
        D3D11_SUBRESOURCE_DATA data{red.data(),16,64};Hr(d11->CreateTexture2D(&t,&data,&texture));
        red.fill(0xffff0000);t.MiscFlags=0;Hr(d11->CreateTexture2D(&t,&data,&blueSource));
        ComPtr<IDXGIResource1> resource;Hr(texture.As(&resource));Hr(resource->CreateSharedHandle(nullptr,DXGI_SHARED_RESOURCE_READ|DXGI_SHARED_RESOURCE_WRITE,nullptr,&h));Hr(d12->OpenSharedHandle(h,IID_PPV_ARGS(&shared)));CloseHandle(h);
        Hr(context4->Signal(writer11.Get(),1));context4->Flush();Hr(queue->Wait(writerFence.Get(),1));
        for(unsigned i=0;i<3;++i){Hr(d12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocators[i])));Hr(d12->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocators[i].Get(),nullptr,IID_PPV_ARGS(&lists[i])));Hr(lists[i]->Close());owner._copyAllocators.push_back(allocators[i].Get());}
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC b{};b.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;b.Width=1024;b.Height=1;b.DepthOrArraySize=1;b.MipLevels=1;b.SampleDesc.Count=1;b.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        for(auto& rb:readbacks)Hr(d12->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&b,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&rb)));
        owner._copyFence=readerFence.Get();owner._copyFenceEvent=CreateEventW(nullptr,FALSE,FALSE,nullptr);owner._dx11CopyReaderFence=reader11.Get();owner._dx11Context4=context4.Get();owner._copyAllocatorFenceValues={1,0,0};owner._sharedTextureReaderValues={1};
    }
    void Record(unsigned slot,unsigned output){
        Hr(lists[slot]->Reset(allocators[slot].Get(),nullptr));
        D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;barrier.Transition.pResource=shared.Get();barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_COMMON;barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;barrier.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;lists[slot]->ResourceBarrier(1,&barrier);
        D3D12_TEXTURE_COPY_LOCATION src{};src.pResource=shared.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION dst{};dst.pResource=readbacks[output].Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint.Footprint={DXGI_FORMAT_R8G8B8A8_UNORM,4,4,1,256};lists[slot]->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
        std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);lists[slot]->ResourceBarrier(1,&barrier);Hr(lists[slot]->Close());
    }
    void Execute(unsigned slot){ID3D12CommandList* l[]{lists[slot].Get()};queue->ExecuteCommandLists(1,l);}
    unsigned Pixel(unsigned i){void* p=nullptr;D3D12_RANGE range{0,1024};Hr(readbacks[i]->Map(0,&range,&p));const auto pixel=*static_cast<unsigned*>(p);D3D12_RANGE none{0,0};readbacks[i]->Unmap(0,&none);return pixel;}
    ~World(){if(gate)gate->Signal(1);if(queue&&readerFence){queue->Signal(readerFence.Get(),100);WaitCpu(readerFence.Get(),100);}if(owner._copyFenceEvent)CloseHandle(owner._copyFenceEvent);}
};
int main(int argc,char** argv){
 try{
    const bool negative=argc==2&&std::string(argv[1])=="--negative-skip-reader-wait";
    World w;w.Record(0,0);Hr(w.queue->Wait(w.gate.Get(),1));w.Execute(0);Hr(w.queue->Signal(w.readerFence.Get(),1));
    CopySlotAdmission proof;
    const auto prepare=negative?&Dx11wDx12SC::_PrepareSharedWriterLegacy:&Dx11wDx12SC::_PrepareSharedWriter;
    Check((w.owner.*prepare)(0,proof)&&proof.RecordingSlot()==1&&w.owner.fallbackWaits==0,"pending texture uses existing free recording without CPU wait");
    Check(w.owner._WaitForCopyAllocator(1,w.owner._copyAllocatorFenceValues,proof),"exact pre-write proof admits spare allocator");
    Hr(w.allocators[1]->Reset());
    w.context->CopyResource(w.texture.Get(),w.blueSource.Get());Hr(w.context4->Signal(w.writer11.Get(),2));w.context4->Flush();
    if(negative){HANDLE e=CreateEventW(nullptr,FALSE,FALSE,nullptr);Hr(w.writer11->SetEventOnCompletion(2,e));Check(WaitForSingleObject(e,10000)==WAIT_OBJECT_0,"negative waits its deliberately unordered DX11 write");CloseHandle(e);}
    Check(w.writer11->GetCompletedValue()<2,"DX11 write remains behind the actual pending reader");
    w.Record(1,1);Hr(w.queue->Wait(w.writerFence.Get(),2));w.Execute(1);Hr(w.queue->Signal(w.readerFence.Get(),2));Hr(w.gate->Signal(1));WaitCpu(w.readerFence.Get(),2);
    std::printf("pixel generations old=%08X next=%08X\n",w.Pixel(0),w.Pixel(1));
    Check(w.Pixel(0)==0xff0000ff,"old reader sees exact red pixels before DX11 overwrite");Check(w.Pixel(1)==0xffff0000,"next reader sees exact blue pixels after ordered overwrite");
    std::printf("PASS interop shared reader WARP: %u checks; actual DX11/DX12 fences and both pixel generations\n",checks);return 0;
 }catch(const std::exception& e){std::fprintf(stderr,"FAIL interop reader WARP: %s\n",e.what());return 1;}
}

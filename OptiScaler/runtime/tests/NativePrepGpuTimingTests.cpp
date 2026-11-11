// SPDX-License-Identifier: GPL-3.0-or-later
#define NOMINMAX
#include "../NativePrepGpuTiming.h"
#include "../../native/NativeWorkDevice.h"
#include <dxgi1_4.h>
#include <cstdio>
#include <cstdlib>
using Microsoft::WRL::ComPtr;
namespace {
unsigned checks=0;
void Check(bool ok,const char* why){++checks;if(!ok){std::fprintf(stderr,"FAIL %s\n",why);std::exit(1);}}
void Hr(HRESULT hr,const char* why){Check(SUCCEEDED(hr),why);}
struct FailingReceipt {
    unsigned calls=0,failAt=1;
    bool throws=true;
    ComPtr<ID3D12DeviceChild> observed[2];
    template<class T> bool Created(const char*,HRESULT hr,T* object,bool) {
        Check(SUCCEEDED(hr) && object,"failure injection uses successfully created WARP objects");
        observed[calls]=object;
        if(++calls!=failAt)return true;
        if(throws)throw 1;
        return false;
    }
};
}
int main(){
    ComPtr<IDXGIFactory4> factory;ComPtr<IDXGIAdapter> adapter;ComPtr<ID3D12Device> device;
    Hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"DXGI factory");
    Hr(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)),"WARP adapter");
    Hr(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)),"D3D12 WARP");
    D3D12_COMMAND_QUEUE_DESC q{};q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue,otherQueue;
    Hr(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)),"actual queue");
    Hr(device->CreateCommandQueue(&q,IID_PPV_ARGS(&otherQueue)),"different queue");
    for(unsigned failAt=1;failAt<=2;++failAt)for(const bool throws:{false,true}){
        FailingReceipt failing;failing.failAt=failAt;failing.throws=throws;
        NativePrepGpuTiming::Recorder<3> optional;
        Check(!optional.Init(device.Get(),queue.Get(),failing),"optional receipt failure disables timing without exception");
        Check(failing.calls==failAt,"failure reached intended creation receipt");
        for(unsigned i=0;i<failing.calls;++i){
            failing.observed[i]->AddRef();
            Check(failing.observed[i]->Release()==1,"failed recorder released its reference to actual WARP object");
        }
        Check(!optional.Init(device.Get(),queue.Get(),failing) && failing.calls==failAt,"failed initialization never retries");
        Check(!optional.Begin(0,queue.Get(),{}),"failed initialization cannot record commands");
    }
    D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{};rd.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;rd.Width=rd.Height=rd.DepthOrArraySize=rd.MipLevels=1;
    rd.Format=DXGI_FORMAT_R8G8B8A8_UNORM;rd.SampleDesc.Count=1;
    ComPtr<ID3D12Resource> picture;Hr(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&picture)),"work picture");
    native::NativeWorkDevice work;Check(work.Resolve(picture.Get(),queue.Get()),"resolve actual work domain");
    native::NativeCreationDomain domain;Check(domain.Bind(work),"bind actual native creation domain");
    ComPtr<ID3D12CommandAllocator> allocator;Hr(device->CreateCommandAllocator(q.Type,IID_PPV_ARGS(&allocator)),"allocator");
    Check(domain.Created("test-anchor",S_OK,allocator.Get(),true,true),"actual public owner anchor");
    NativePrepGpuTiming::Recorder<3> recorder;Check(recorder.Init(device.Get(),queue.Get(),domain),"real query heap/readback with exact domain receipts");
    ComPtr<ID3D12Fence> fence,wrong;Hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"fence");
    Hr(device->CreateFence(100,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&wrong)),"unrelated completed fence");
    ComPtr<ID3D12GraphicsCommandList> list;Hr(device->CreateCommandList(0,q.Type,allocator.Get(),nullptr,IID_PPV_ARGS(&list)),"list");
    NativePrepGpuTiming::Reading reading;
    Check(!recorder.Begin(3,queue.Get(),{}),"slot bound");
    Check(!recorder.Begin(0,otherQueue.Get(),{}),"foreign queue refuses recording");
    const HANDLE completed=CreateEventW(nullptr,FALSE,FALSE,nullptr);Check(completed!=nullptr,"test completion event");
    for(unsigned slot=0;slot<3;++slot){
        NativePrepGpuTiming::Identity input{100+slot,200+slot,300+slot,400+slot,500+slot,reinterpret_cast<uintptr_t>(picture.Get())};
        Check(recorder.Begin(slot,queue.Get(),input),"start exact input sample");
        for(unsigned stamp=0;stamp<4;++stamp)recorder.Stamp(slot,stamp,list.Get());
        recorder.Resolve(slot,list.Get());Hr(list->Close(),"close timestamp list");
        ID3D12CommandList* batch[]{list.Get()};queue->ExecuteCommandLists(1,batch);
        const UINT64 target=slot+1;recorder.Submitted(slot,queue.Get(),fence.Get(),target);
        Check(!recorder.Poll(slot,queue.Get(),wrong.Get(),reading),"unrelated completed fence is no proof");
        Check(!recorder.Poll(slot,otherQueue.Get(),fence.Get(),reading),"foreign queue is no proof");
        Check(!recorder.Poll(slot,queue.Get(),fence.Get(),reading),"unsignalled real fence is no proof");
        Check(!recorder.Begin(slot,queue.Get(),input),"in-flight sample not overwritten");
        Hr(queue->Signal(fence.Get(),target),"actual final queue signal");
        Hr(fence->SetEventOnCompletion(target,completed),"test fence wait setup");
        Check(WaitForSingleObject(completed,10000)==WAIT_OBJECT_0,"test GPU completion");
        Check(recorder.Poll(slot,queue.Get(),fence.Get(),reading) && reading.known,"actual completed GPU interval");
        Check(reading.input.frame==input.frame && reading.input.sourceGeneration==input.sourceGeneration &&
            reading.input.pictureSerial==input.pictureSerial && reading.input.picture==input.picture,"same submitted input identity");
        Check(reading.target==target && reading.completed>=target && reading.fence==reinterpret_cast<uintptr_t>(fence.Get()) &&
            reading.queue==reinterpret_cast<uintptr_t>(queue.Get()),"exact queue/fence witness");
        for(const auto ms:reading.milliseconds)Check(ms>=0,"monotonic same-list boundaries");
        Check(!recorder.Poll(slot,queue.Get(),fence.Get(),reading),"sample published once");
        Hr(allocator->Reset(),"completed allocator reset");Hr(list->Reset(allocator.Get(),nullptr),"completed list reset");
    }
    Hr(list->Close(),"close final empty list");CloseHandle(completed);
    std::printf("NativePrepGpuTiming: %u WARP checks; actual queue/fence, no SDK/game performance proof\n",checks);
}

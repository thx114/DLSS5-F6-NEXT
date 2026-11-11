// SPDX-License-Identifier: GPL-3.0-or-later
// Real WARP queues/fences exercise the production completion decision.
// No game, SDK, HWND mutation or displayed-pixel claim.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <cstdio>
#include <cstdlib>
#include <dxgi1_6.h>
template<class... A> void IgnoreLog(const char*,A&&...) noexcept {}
#define LOG_INFO(...) IgnoreLog(__VA_ARGS__)
#include "../GenshinVisibleChild.h"
using Microsoft::WRL::ComPtr;
namespace {
unsigned checks=0;
void Check(bool ok,const char* why){++checks;if(!ok){std::fprintf(stderr,"FAIL: %s\n",why);std::abort();}}
void Hr(HRESULT hr,const char* why){Check(SUCCEEDED(hr),why);}
void Complete(ID3D12Fence* fence,uint64_t value){
    HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);Check(event!=nullptr,"completion event");
    Hr(fence->SetEventOnCompletion(value,event),"arm actual fence event");
    Check(WaitForSingleObject(event,10000)==WAIT_OBJECT_0,"bounded fixture wait for actual owning fence");
    CloseHandle(event);
}
}
struct GenshinVisibleChildTestAccess {
static void Run(){
    ComPtr<IDXGIFactory4> factory;Hr(CreateDXGIFactory2(0,IID_PPV_ARGS(&factory)),"factory");
    ComPtr<IDXGIAdapter> adapter;Hr(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)),"WARP adapter");
    ComPtr<ID3D12Device> device;Hr(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)),"WARP device");
    D3D12_COMMAND_QUEUE_DESC qd{};qd.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue,foreign;
    Hr(device->CreateCommandQueue(&qd,IID_PPV_ARGS(&queue)),"actual output queue");
    Hr(device->CreateCommandQueue(&qd,IID_PPV_ARGS(&foreign)),"foreign queue");
    ComPtr<ID3D12Fence> fence,blocker,wrong;
    Hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"actual output fence");
    Hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&blocker)),"test queue blocker");
    Hr(device->CreateFence(100,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&wrong)),"unrelated completed fence");
    DXGI_SWAP_CHAIN_DESC1 desc{};desc.Width=desc.Height=16;desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count=1;desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.BufferCount=2;
    desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;desc.AlphaMode=DXGI_ALPHA_MODE_IGNORE;
    ComPtr<IDXGISwapChain1> initial;ComPtr<IDXGISwapChain3> chain;ComPtr<ID3D12Resource> color;
    Hr(factory->CreateSwapChainForComposition(queue.Get(),&desc,nullptr,&initial),"unshown composition chain");
    Hr(initial.As(&chain),"physical chain identity");Hr(chain->GetBuffer(0,IID_PPV_ARGS(&color)),"actual color resource");
    GenshinVisibleChild owner(nullptr);auto& gate=*owner.display_;
    GenshinVisibleChild::PhysicalContent content;
    content.streamOwner={1,2,reinterpret_cast<uintptr_t>(device.Get()),16,16};
    content.streamLive=std::make_shared<std::atomic<bool>>(true);
    content.outputLive=std::make_shared<std::atomic<bool>>(true);
    content.chain=chain;content.color=reinterpret_cast<uintptr_t>(color.Get());
    content.queue=queue;content.fence=fence;content.device=device;
    content.stream=content.outputGeneration=content.sequence=content.value=1;content.primed=true;
    gate.streamTail.queue=queue;gate.streamTail.fence=fence;gate.streamTail.device=device;
    gate.streamTail.value=2;gate.hiddenAcknowledged=true;
    Hr(queue->Signal(fence.Get(),1),"submit saved output tail");Complete(fence.Get(),1);
    Hr(queue->Wait(blocker.Get(),1),"hold later output on its actual queue");
    Hr(queue->Signal(fence.Get(),2),"submit later output tail");
    uint64_t completed=0;
    Check(owner.OutputTailReady(content,true,completed)&&completed==1,"visible lease renews from its completed new output despite a later pending raw");
    Check(!owner.OutputTailReady(content,false,completed),"first HWND reveal still refuses a pending last raw");
    Check(!gate.streamTail.Complete()&&gate.streamTail.value==2,"renewal does not retire or change latest queue custody");
    content.value=2;Check(!owner.OutputTailReady(content,true,completed),"pending saved output cannot renew");content.value=1;
    gate.streamTail.inFlight=true;
    Check(owner.OutputTailReady(content,true,completed),"no HWND transition when a later CPU raw is active");
    Check(!owner.OutputTailReady(content,false,completed),"active raw still blocks HWND reveal");gate.streamTail.inFlight=false;
    gate.streamTail.uncertain=true;Check(!owner.OutputTailReady(content,true,completed),"uncertain stream cannot renew");gate.streamTail.uncertain=false;
    gate.streamTail.queue=foreign;Check(!owner.OutputTailReady(content,true,completed),"foreign queue cannot certify saved output");gate.streamTail.queue=queue;
    gate.streamTail.fence=wrong;Check(!owner.OutputTailReady(content,true,completed),"foreign completed fence cannot certify saved output");gate.streamTail.fence=fence;
    content.streamLive->store(false);Check(!owner.OutputTailReady(content,true,completed),"revoked SDK stream cannot renew");content.streamLive->store(true);
    content.outputLive->store(false);Check(!owner.OutputTailReady(content,true,completed),"retired output generation cannot renew");content.outputLive->store(true);
    Hr(blocker->Signal(1),"release test blocker");Complete(fence.Get(),2);
    Check(owner.OutputTailReady(content,false,completed)&&completed>=2,"first reveal becomes eligible only after actual latest raw completes");
}
};
int main(){GenshinVisibleChildTestAccess::Run();std::printf("PASS %u WARP completion checks; HWND/SDK/game/display untested\n",checks);}

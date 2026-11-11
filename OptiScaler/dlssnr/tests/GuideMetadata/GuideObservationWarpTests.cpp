// SPDX-License-Identifier: GPL-3.0-or-later
// Standalone WARP regression source. Lead owns compilation and execution.
#include <shaders/dlssnr/DlssNr_GuideObservation.h>
#include <dxgi1_6.h>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#pragma comment(lib,"d3d12.lib")
#pragma comment(lib,"dxgi.lib")

int main() {
    using Microsoft::WRL::ComPtr;
    DlssNrFrameInfo raw;
    raw.NativeMotionJitterKnown=true;raw.NativeMotionVectorsJittered=false;
    raw.NativeCurrentJitterKnown=true;raw.NativeJitterX=.25f;
    raw.NativeRouteConventionVerified=true;
    const auto rawStamp=DlssNrGuideObservation::Source(raw);
    assert(rawStamp.motionJitterKnown && !rawStamp.motionJittered);
    assert(!rawStamp.addressPhaseKnown); // final SR color/raw guide grids are not yet aligned
    ComPtr<IDXGIFactory4> factory;ComPtr<IDXGIAdapter> adapter;
    ComPtr<ID3D12Device> device;
    assert(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))));
    assert(SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter))));
    assert(SUCCEEDED(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device))));
    ComPtr<ID3D12CommandQueue> queue;
    D3D12_COMMAND_QUEUE_DESC q{};q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
    assert(SUCCEEDED(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue))));
    ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;
    assert(SUCCEEDED(device->CreateCommandAllocator(q.Type,IID_PPV_ARGS(&allocator))));
    assert(SUCCEEDED(device->CreateCommandList(0,q.Type,allocator.Get(),nullptr,IID_PPV_ARGS(&list))));
    assert(SUCCEEDED(list->Close()));
    ComPtr<ID3D12Fence> own,unrelated;
    assert(SUCCEEDED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&own))));
    assert(SUCCEEDED(device->CreateFence(100,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&unrelated))));
    auto token=std::make_shared<DlssNrGpuLifetime::Token>();
    token->generation=17;token->listOwner=list;
    token->executions[0]={queue,own,1};
    GuideDiagnostics::Snapshot s;s.source=30;s.epoch=30;s.tick=7;s.modelSuccess=true;
    s.stage=GuideDiagnostics::Stage::Recorded;
    DlssNrGuideObservation::Recorded(GuideDiagnostics::Consumer::NrModel,s,token);
    token->submitted=true;
    DlssNrGuideObservation::Poll();
    GuideDiagnostics::View view;assert(GuideDiagnostics::Read(view));
    assert(unrelated->GetCompletedValue()==100 && view.lanes[0].latest.stage==GuideDiagnostics::Stage::Submitted);
    assert(!view.lanes[0].completed.source && DlssNrGuideObservation::PendingRecords().size()==1);
    ID3D12CommandList* lists[]={list.Get()};queue->ExecuteCommandLists(1,lists);
    assert(SUCCEEDED(queue->Signal(own.Get(),1)));
    HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);assert(event);
    assert(SUCCEEDED(own->SetEventOnCompletion(1,event)));
    assert(WaitForSingleObject(event,5000)==WAIT_OBJECT_0);CloseHandle(event);
    assert(own->GetCompletedValue()!=UINT64_MAX && own->GetCompletedValue()>=1);
    DlssNrGuideObservation::Poll();assert(GuideDiagnostics::Read(view));
    assert(view.lanes[0].completed.source==30 && view.lanes[0].completed.modelSuccess);
    assert(view.lanes[0].completed.tick==7 && GuideDiagnostics::Stale(view.lanes[0].completed,2000));
    assert(DlssNrGuideObservation::PendingRecords().size()==1); // fence is not executable-list closure
    assert(SUCCEEDED(allocator->Reset()));assert(SUCCEEDED(list->Reset(allocator.Get(),nullptr)));
    token->closed=true; // only after this actual successful Reset
    DlssNrGuideObservation::Poll();assert(DlssNrGuideObservation::PendingRecords().empty());
    assert(SUCCEEDED(list->Close()));
}

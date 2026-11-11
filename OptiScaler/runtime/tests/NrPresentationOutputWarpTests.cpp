// SPDX-License-Identifier: GPL-3.0-or-later
// Real DX12 pixel and delayed-queue tests of the production presentation copies.
#define NOMINMAX
#include <runtime/NrPresentationOutput.h>
#include <runtime/NrPresentationSource.h>
#include <shaders/dlssnr/DlssNr_RecordingLifetime.h>
#include <dxgi1_4.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace {
unsigned checks = 0;
void Check(bool ok, const char* why) { ++checks; if (!ok) throw std::runtime_error(why); }
void Hr(HRESULT hr) { Check(SUCCEEDED(hr), "DX12 operation failed"); }
constexpr UINT Width = 17, Height = 7;
using Pixel = std::array<float, 4>;
Pixel Pattern(unsigned frame, unsigned x, unsigned y)
{ return {float(frame), float(x) / Width, float(y) / Height, 1.0f}; }
void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
                D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    if (before == after) return;
    D3D12_RESOURCE_BARRIER b {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
    list->ResourceBarrier(1, &b);
}
struct Capture {
    ComPtr<ID3D12Resource> readback;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
    void Expect(unsigned frame) const {
        void* data = nullptr;
        Hr(readback->Map(0, nullptr, &data));
        const auto* mapped = static_cast<const unsigned char*>(data);
        for (unsigned y=0; y<Height; ++y) for (unsigned x=0; x<Width; ++x) {
            Pixel p {};
            std::memcpy(p.data(), mapped+footprint.Offset+size_t(y)*footprint.Footprint.RowPitch+x*sizeof(Pixel), sizeof(p));
            Check(p == Pattern(frame,x,y), "actual pixel belongs to the expected source/model result");
        }
        D3D12_RANGE unchanged {0,0}; readback->Unmap(0, &unchanged);
    }
};
struct Fixture {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    std::vector<ComPtr<ID3D12Resource>> uploads;
    Fixture() {
        ComPtr<IDXGIFactory4> factory; ComPtr<IDXGIAdapter> warp;
        Hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory))); Hr(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)));
        Hr(D3D12CreateDevice(warp.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)));
        D3D12_COMMAND_QUEUE_DESC q {}; q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        Hr(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)));
        Hr(device->CreateCommandAllocator(q.Type,IID_PPV_ARGS(&allocator)));
        Hr(device->CreateCommandList(0,q.Type,allocator.Get(),nullptr,IID_PPV_ARGS(&list)));
    }
    ComPtr<ID3D12Resource> Buffer(UINT64 bytes, D3D12_HEAP_TYPE heapType, D3D12_RESOURCE_STATES state) {
        D3D12_HEAP_PROPERTIES hp {}; hp.Type = heapType;
        D3D12_RESOURCE_DESC d {}; d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width=bytes; d.Height=1; d.DepthOrArraySize=d.MipLevels=1; d.SampleDesc.Count=1;
        d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> result;
        Hr(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,state,nullptr,IID_PPV_ARGS(&result)));
        return result;
    }
    ComPtr<ID3D12Resource> Texture(unsigned frame, D3D12_RESOURCE_STATES state) {
        D3D12_HEAP_PROPERTIES hp {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC d {}; d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width=Width; d.Height=Height; d.DepthOrArraySize=d.MipLevels=1; d.SampleDesc.Count=1;
        d.Format=DXGI_FORMAT_R32G32B32A32_FLOAT; d.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        ComPtr<ID3D12Resource> texture;
        Hr(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&texture)));
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp {}; UINT64 bytes=0;
        device->GetCopyableFootprints(&d,0,1,0,&fp,nullptr,nullptr,&bytes);
        auto upload=Buffer(bytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
        unsigned char* mapped=nullptr; Hr(upload->Map(0,nullptr,reinterpret_cast<void**>(&mapped)));
        for (unsigned y=0;y<Height;++y) for (unsigned x=0;x<Width;++x) {
            const auto p=Pattern(frame,x,y);
            std::memcpy(mapped+fp.Offset+size_t(y)*fp.Footprint.RowPitch+x*sizeof(Pixel),p.data(),sizeof(p));
        }
        upload->Unmap(0,nullptr); uploads.push_back(upload);
        D3D12_TEXTURE_COPY_LOCATION from {},to {};
        from.pResource=upload.Get(); from.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; from.PlacedFootprint=fp;
        to.pResource=texture.Get(); to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
        Transition(list.Get(),texture.Get(),D3D12_RESOURCE_STATE_COPY_DEST,state);
        return texture;
    }
    Capture Snapshot(ID3D12Resource* texture,D3D12_RESOURCE_STATES state) {
        Capture result; UINT64 bytes=0; const auto d=texture->GetDesc();
        device->GetCopyableFootprints(&d,0,1,0,&result.footprint,nullptr,nullptr,&bytes);
        result.readback=Buffer(bytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
        Transition(list.Get(),texture,state,D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION from {},to {};
        from.pResource=texture; from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        to.pResource=result.readback.Get(); to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; to.PlacedFootprint=result.footprint;
        list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
        Transition(list.Get(),texture,D3D12_RESOURCE_STATE_COPY_SOURCE,state);
        return result;
    }
    void Submit() {
        Hr(list->Close()); ID3D12CommandList* batch [] {list.Get()}; queue->ExecuteCommandLists(1,batch);
    }
    void Drain() {
        ComPtr<ID3D12Fence> fence; Hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));
        Hr(queue->Signal(fence.Get(),1));
        HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr); Check(event!=nullptr,"completion event");
        const auto hr=fence->SetEventOnCompletion(1,event);
        const auto waited=SUCCEEDED(hr)?WaitForSingleObject(event,10000):WAIT_FAILED;
        CloseHandle(event); Hr(hr); Check(waited==WAIT_OBJECT_0,"actual submission queue completed");
    }
    void Reset() { Hr(allocator->Reset()); Hr(list->Reset(allocator.Get(),nullptr)); }
};
void DirectInput();
void Pixels() {
    DirectInput();
    Fixture f;
    auto raw=f.Texture(1,D3D12_RESOURCE_STATE_PRESENT);
    auto work=f.Texture(9,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    auto model=f.Texture(2,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Check(!NrPresentation::RecordInput(nullptr,raw.Get(),work.Get()),"missing recording rejected");
    Check(!NrPresentation::RecordInput(f.list.Get(),raw.Get(),raw.Get()),"raw cannot be its own work target");
    Check(!NrPresentation::RecordPublish(f.list.Get(),nullptr,raw.Get()),"missing result rejected");
    Check(NrPresentation::RecordInput(f.list.Get(),raw.Get(),work.Get()),"copy current raw input");
    const auto input=f.Snapshot(work.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    // Stand in for a completed model/Look/detail stage with an asymmetric answer.
    Check(NrPresentation::RecordCopy(f.list.Get(),model.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        work.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS),"private stages receive a fresh result");
    const auto untouched=f.Snapshot(raw.Get(),D3D12_RESOURCE_STATE_PRESENT);
    NrPresentation::SourceClock sources; const NrPresentation::Source source {1,2,3,4,5};
    const auto attempt=sources.Begin(); Check(sources.Complete(attempt,source,true),"real source admission");
    Check(sources.Claim(attempt,source),"single source endpoint claimed");
    Check(NrPresentation::RecordPublish(f.list.Get(),work.Get(),raw.Get()),"publish complete stages once");
    const auto final=f.Snapshot(raw.Get(),D3D12_RESOURCE_STATE_PRESENT);
    Check(!sources.Claim(attempt,source),"generated callback cannot publish another result");
    f.Submit(); f.Drain(); input.Expect(1); untouched.Expect(1); final.Expect(2); f.Reset();

    // Model failure/warmup may change private work, but must leave this new raw
    // source visible, without borrowing the preceding model's completed colour.
    auto next=f.Texture(3,D3D12_RESOURCE_STATE_PRESENT);
    Check(NrPresentation::RecordInput(f.list.Get(),next.Get(),work.Get()),"next source refreshes private input");
    const auto fresh=f.Snapshot(work.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const auto skipped=f.Snapshot(next.Get(),D3D12_RESOURCE_STATE_PRESENT);
    f.Submit(); f.Drain(); fresh.Expect(3); skipped.Expect(3); f.Reset();
    Check(NrPresentation::RecordPublish(f.list.Get(),model.Get(),next.Get()),"record unexecuted answer");
    Hr(f.list->Close()); f.Reset(); // actual discard; no Execute
    const auto discarded=f.Snapshot(next.Get(),D3D12_RESOURCE_STATE_PRESENT);
    f.Submit(); f.Drain(); discarded.Expect(3);
}
void DirectInput() {
    Fixture f;
    auto raw=f.Texture(3,D3D12_RESOURCE_STATE_PRESENT);
    auto work=f.Texture(9,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    auto model=f.Texture(4,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    using namespace NrPresentation;
    Check(!PrepareColourInput(nullptr,raw.Get(),D3D12_RESOURCE_STATE_PRESENT,work.Get(),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,false).resource,"direct input still requires a real recording");
    Check(!CompatibleColourInput(raw.Get(),raw.Get()),"direct input may not alias private output");
    const auto input=PrepareColourInput(f.list.Get(),raw.Get(),D3D12_RESOURCE_STATE_PRESENT,work.Get(),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,false);
    Check(input.resource==raw.Get() && input.state==D3D12_RESOURCE_STATE_PRESENT,
        "ordinary encode reads the actual current source");
    const auto current=f.Snapshot(input.resource,input.state);
    const auto uncopied=f.Snapshot(work.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Check(RecordCopy(f.list.Get(),model.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,work.Get(),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS),"private result is produced independently of input");
    const auto untouched=f.Snapshot(raw.Get(),D3D12_RESOURCE_STATE_PRESENT);
    Check(RecordPublish(f.list.Get(),work.Get(),raw.Get()),"only final success publishes to source");
    const auto final=f.Snapshot(raw.Get(),D3D12_RESOURCE_STATE_PRESENT);
    f.Submit();f.Drain();current.Expect(3);uncopied.Expect(9);untouched.Expect(3);final.Expect(4);f.Reset();
    auto next=f.Texture(5,D3D12_RESOURCE_STATE_PRESENT);
    const auto writable=PrepareColourInput(f.list.Get(),next.Get(),D3D12_RESOURCE_STATE_PRESENT,work.Get(),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,true);
    Check(writable.resource==work.Get() && writable.state==D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        "LUT/hold keep a private writable input");
    const auto copied=f.Snapshot(work.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Check(RecordCopy(f.list.Get(),model.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,work.Get(),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS),"LUT/hold writes only to private input");
    const auto failed=f.Snapshot(next.Get(),D3D12_RESOURCE_STATE_PRESENT);
    f.Submit();f.Drain();copied.Expect(5);failed.Expect(5);
}
void DelayedQueue() {
    Fixture f; DlssNrGpuLifetime::Registry registry;
    auto raw=f.Texture(1,D3D12_RESOURCE_STATE_PRESENT);
    auto work=f.Texture(2,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    f.Submit(); f.Drain(); f.Reset();
    auto use=registry.Recording(f.device.Get(),f.list.Get());
    Check(DlssNrGpuLifetime::Retain(use,{raw.Get(),work.Get()}),"actual recording retains its resources");
    DlssNrGpuLifetime::Use production[2],inspector[3];unsigned pn=0,dn=0;
    for(unsigned n=0;n<3;++n)
        Check(DlssNrGpuLifetime::AcquireSlot(inspector,dn,use)>=0,"diagnostic slot belongs to the real recording");
    Check(DlssNrGpuLifetime::AcquireSlot(inspector,dn,use)<0,"full diagnostic ring skips instead of growing");
    Check(DlssNrGpuLifetime::AvailableSlots(production)==2,"diagnostic saturation leaves production capacity untouched");
    for(unsigned n=0;n<2;++n)
        Check(DlssNrGpuLifetime::AcquireSlot(production,pn,use)>=0,"production passes still acquire their independent slots");
    Check(NrPresentation::RecordInput(f.list.Get(),raw.Get(),work.Get()),"delayed source copy");
    ComPtr<ID3D12Fence> gate; Hr(f.device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&gate)));
    Hr(f.queue->Wait(gate.Get(),1)); f.Submit();
    ID3D12CommandList* batch [] {f.list.Get()}; registry.Submitted(f.queue.Get(),1,batch);
    Check(!registry.CanResetRecording(f.list.Get()),"no Reset while real queue is delayed");
    Check(!use->Complete(),"CPU return is not resource completion");
    Check(DlssNrGpuLifetime::AvailableSlots(inspector)==0&&DlssNrGpuLifetime::AvailableSlots(production)==0,
          "delayed actual queue cannot free either descriptor ring");
    ComPtr<ID3D12Fence> unrelated;
    Hr(f.device->CreateFence(100,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&unrelated)));
    Check(unrelated->GetCompletedValue()==100 && !registry.CanResetRecording(f.list.Get()),"other fence grants no permission");
    Hr(gate->Signal(1)); f.Drain();
    Check(registry.CanResetRecording(f.list.Get()) && use->GpuComplete(),"real completion permits owner Reset");
    Check(!use->Complete(),"GPU completion still needs executable recording closure");
    Check(DlssNrGpuLifetime::AvailableSlots(inspector)==0,"diagnostic descriptors cannot reuse on completion alone");
    f.Reset(); registry.ResetRecording(f.list.Get());
    Check(use->Complete() && registry.Tracked()==0,"actual Reset retires completed generation");
    Check(DlssNrGpuLifetime::AvailableSlots(inspector)==3&&DlssNrGpuLifetime::AvailableSlots(production)==2,
          "actual owner completion plus Reset reopens both independent rings");
    Hr(f.list->Close());
}
}
int main() {
    try { Pixels(); DelayedQueue(); std::printf("NR presentation output DX12 WARP: %u checks passed\n",checks); return 0; }
    catch (const std::exception& e) { std::fprintf(stderr,"FAIL %s\n",e.what()); return 1; }
}

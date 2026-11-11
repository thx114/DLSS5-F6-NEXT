// SPDX-License-Identifier: GPL-3.0-or-later
// Production release flow, real WARP queues/fences/Reset, explicit HRESULT faults.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#define DLSSNR_LIFETIME_TEST_SEAM
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <shaders/dlssnr/DlssNr_RecordingLifetime.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>
using Microsoft::WRL::ComPtr;
constexpr size_t DX11WDX12_COMMAND_BUFFER_COUNT = 3;
unsigned checks = 0, scenarios = 0, callbacks = 0, allocatorResets = 0, listResets = 0, ticks = 0;
bool failAllocator = false, failList = false, failClose = false, dropReset = false, hookReset = false, sentinel = false;
DlssNrGpuLifetime::TestFaults faults;
DlssNrGpuLifetime::Registry g_nrGpuWork {&faults};
std::recursive_mutex g_nrMutex;
void Check(bool ok, const char* why) { ++checks; if (!ok) throw std::runtime_error(why); }
void Hr(HRESULT hr) { Check(SUCCEEDED(hr), "real WARP operation failed"); }
#define LOG_ERROR(...) ((void)0)
#define LOG_WARN(...) ((void)0)
#define LOG_INFO(...) ((void)0)
#define SAFE_RELEASE(value) do { if (value) { (value)->Release(); (value) = nullptr; } } while (false)
namespace GenshinHookCpuTiming {
enum class Stage { FenceQueryApi, FenceArmApi, CpuEventWait };
enum class ResultKind { Fence, Hresult, Event };
template<class F> auto Call(Stage stage, F&& fn, ResultKind) {
    auto result = fn();
    if constexpr (std::is_same_v<decltype(result), UINT64>)
        if (stage == Stage::FenceQueryApi && sentinel) return UINT64_MAX;
    return result;
}
}
namespace DetailReuse {
ID3D12CommandList* RealSubmittedCommands(ID3D12CommandList* cmd) { return cmd; }
}
void TickNrRetired() { ++ticks; g_nrGpuWork.Collect(); }
namespace DlssNr {
void FinishedPictureResetCommandList(ID3D12CommandList* list) {
    ++callbacks; if (!dropReset) g_nrGpuWork.ResetRecording(list);
}
HRESULT ResetAllocator(ID3D12CommandAllocator* allocator) {
    ++allocatorResets; return failAllocator ? E_FAIL : allocator->Reset();
}
HRESULT ResetList(ID3D12GraphicsCommandList* cmd, ID3D12CommandAllocator* allocator) {
    ++listResets; const auto hr = failList ? E_FAIL : cmd->Reset(allocator, nullptr);
    if (SUCCEEDED(hr) && hookReset) FinishedPictureResetCommandList(cmd);
    return hr;
}
HRESULT CloseList(ID3D12GraphicsCommandList* cmd) { return failClose ? E_FAIL : cmd->Close(); }
HRESULT RetireFinishedPictureRecording(ID3D12GraphicsCommandList*, ID3D12CommandAllocator*);
}
struct Provider {
    unsigned* released;
    explicit Provider(unsigned& value) : released(&value) {}
    ~Provider() { ++*released; }
};
struct Domain {
    bool retired = false;
    void RetireRecordings() { retired = true; }
};
class IFeature_Dx11wDx12 {
public:
    ID3D12GraphicsCommandList* Dx12CommandList[3] {};
    ID3D12CommandAllocator* Dx12CommandAllocator[3] {};
    ID3D12Fence* Dx12Fence = nullptr;
    UINT64 Dx12CommandAllocatorFenceValue[3] {}, Dx12FenceValue = 0, genshinLastSubmittedFence = 0;
    HANDLE Dx12FenceEvent = nullptr;
    bool genshinSubmissionUnfenced = false, genshinLastSubmissionIsolated = false;
    std::array<unsigned,3> genshinRecordings {};
    Domain genshinWorkDomain;
    std::unique_ptr<Provider> dx12Feature;
    bool ReleaseSharedResources(bool releaseProvider = false);
    bool BaselineRelease(bool releaseProvider = false);
};
#include "BridgeRetirementProduction.inl"

struct Fixture {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue, other;
    ComPtr<ID3D12Fence> gate, tail;
    std::array<ComPtr<ID3D12GraphicsCommandList>,3> lists;
    std::array<ComPtr<ID3D12CommandAllocator>,3> allocators;
    DlssNrGpuLifetime::Uses uses;
    unsigned providerReleased = 0;
    IFeature_Dx11wDx12 bridge;
    Fixture() {
        ++scenarios; callbacks = allocatorResets = listResets = ticks = 0;
        failAllocator = failList = failClose = dropReset = hookReset = sentinel = false;
        faults = {};
        ComPtr<IDXGIFactory4> factory; ComPtr<IDXGIAdapter> warp;
        Hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory))); Hr(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)));
        Hr(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
        D3D12_COMMAND_QUEUE_DESC desc {}; desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        Hr(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue)));
        Hr(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&other)));
        Hr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)));
        Hr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&tail)));
        Hr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&bridge.Dx12Fence)));
        bridge.Dx12FenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        Check(bridge.Dx12FenceEvent != nullptr, "event creation");
        bridge.dx12Feature = std::make_unique<Provider>(providerReleased);
        for (unsigned i = 0; i < 3; ++i) {
            Hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocators[i])));
            Hr(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators[i].Get(), nullptr, IID_PPV_ARGS(&lists[i])));
            uses.push_back(g_nrGpuWork.Recording(device.Get(), lists[i].Get()));
            Check(uses.back() != nullptr, "actual registry recording");
            Hr(lists[i]->Close());
            bridge.Dx12CommandList[i] = lists[i].Get(); lists[i]->AddRef();
            bridge.Dx12CommandAllocator[i] = allocators[i].Get(); allocators[i]->AddRef();
        }
    }
    static void Wait(ID3D12Fence* fence, UINT64 value) {
        if (fence->GetCompletedValue() >= value) return;
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        Hr(fence->SetEventOnCompletion(value, event));
        const auto result = WaitForSingleObject(event, 5000); CloseHandle(event);
        Check(result == WAIT_OBJECT_0 && fence->GetCompletedValue() != UINT64_MAX &&
              fence->GetCompletedValue() >= value, "exact queue completion");
    }
    void Submit(ID3D12CommandQueue* actual = nullptr, bool signalBridge = true) {
        if (!actual) actual = queue.Get();
        ID3D12CommandList* raw[3] {lists[0].Get(),lists[1].Get(),lists[2].Get()};
        actual->ExecuteCommandLists(3, raw); g_nrGpuWork.Submitted(actual, 3, raw);
        Hr(actual->Signal(tail.Get(), 1));
        if (signalBridge) {
            Hr(queue->Signal(bridge.Dx12Fence, 3)); bridge.Dx12FenceValue = 3;
            for (auto& value : bridge.Dx12CommandAllocatorFenceValue) value = 3;
        }
    }
    void Retained() {
        Check(providerReleased == 0 && bridge.dx12Feature != nullptr, "refusal retains provider");
        for (unsigned i=0; i<3; ++i) Check(bridge.Dx12CommandList[i] && bridge.Dx12CommandAllocator[i], "refusal retains command owners");
        Check(!bridge.genshinWorkDomain.retired, "refusal cannot retire domain");
    }
    ~Fixture() {
        // Test cleanup explicitly unblocks and waits every actual queue; this
        // is not production retirement or discarded completion evidence.
        gate->Signal(1);
        ComPtr<ID3D12Fence> ownerTail, otherTail;
        device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&ownerTail));
        queue->Signal(ownerTail.Get(), 1); Wait(ownerTail.Get(), 1);
        device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&otherTail));
        other->Signal(otherTail.Get(), 1); Wait(otherTail.Get(), 1);
        for (auto& list : lists) { list->Close(); g_nrGpuWork.ResetRecording(list.Get()); }
        g_nrGpuWork = DlssNrGpuLifetime::Registry {&faults}; uses.clear();
        for (unsigned i=0; i<3; ++i) { SAFE_RELEASE(bridge.Dx12CommandList[i]); SAFE_RELEASE(bridge.Dx12CommandAllocator[i]); }
        SAFE_RELEASE(bridge.Dx12Fence); if (bridge.Dx12FenceEvent) CloseHandle(bridge.Dx12FenceEvent);
        bridge.dx12Feature.reset();
    }
};
void Baseline() {
    Fixture f; f.Submit(); Fixture::Wait(f.tail.Get(), 1); Fixture::Wait(f.bridge.Dx12Fence, 3);
    for (const auto& use : f.uses) Check(use->GpuComplete() && !use->Complete(), "completion alone leaves old model pinned");
    Check(f.bridge.BaselineRelease(true), "old bridge release reports success");
    for (const auto& use : f.uses) Check(use->Complete(), "released SR feature must close NR recording before model rebuild");
}
void SuccessAndContext(bool releaseProvider, bool hooks) {
    Fixture f; hookReset = hooks; f.Submit(); Fixture::Wait(f.tail.Get(),1);
    Check(f.bridge.ReleaseSharedResources(releaseProvider), "release closes completed recordings");
    Check(f.providerReleased == (releaseProvider ? 1u : 0u), "context recreation preserves provider");
    Check(g_nrGpuWork.Tracked() == 0 && callbacks == (hooks ? 6u : 3u), "all old generation tokens closed exactly once");
    for (const auto& use : f.uses) Check(use->Complete() && !use->listOwner, "old model owners can retire");
    Check(allocatorResets == 3 && listResets == 3 && f.bridge.genshinWorkDomain.retired, "successful Reset before release");
    ComPtr<ID3D12CommandAllocator> nextAllocator; ComPtr<ID3D12GraphicsCommandList> next;
    Hr(f.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&nextAllocator)));
    Hr(f.device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,nextAllocator.Get(),nullptr,IID_PPV_ARGS(&next)));
    auto use = g_nrGpuWork.Recording(f.device.Get(),next.Get());
    Check(use && !use->closed, "new SR/NR generation can record without focus change");
    Hr(next->Close()); Hr(DlssNr::RetireFinishedPictureRecording(next.Get(),nextAllocator.Get()));
    Check(use->Complete() && use->discarded, "unsubmitted creation discarded only after actual Reset");
}
void DelayedOwnerQueue() {
    Fixture f; Hr(f.queue->Wait(f.gate.Get(),1)); f.Submit();
    Check(f.bridge.Dx12Fence->GetCompletedValue() < 3, "own queue really pending");
    std::thread unblock([gate=f.gate]{ Sleep(35); gate->Signal(1); });
    const bool ok = f.bridge.ReleaseSharedResources(true); unblock.join();
    Check(ok && f.providerReleased == 1 && f.uses[0]->Complete(), "drain then Reset then release");
}
void OtherActualQueue() {
    Fixture f; Hr(f.other->Wait(f.gate.Get(),1)); f.Submit(f.other.Get());
    Fixture::Wait(f.bridge.Dx12Fence,3);
    Check(!f.bridge.ReleaseSharedResources(true), "completed unrelated owner fence cannot reset actual in-flight NR");
    f.Retained(); Check(allocatorResets == 0 && callbacks == 0, "pending other queue must not attempt Reset");
    Hr(f.gate->Signal(1)); Fixture::Wait(f.tail.Get(),1);
    Check(f.bridge.ReleaseSharedResources(true) && f.uses[0]->Complete(), "actual queue completion recovers");
}
void Refusals() {
    for (unsigned mode=0; mode<9; ++mode) {
        Fixture f; f.Submit(); Fixture::Wait(f.tail.Get(),1); Fixture::Wait(f.bridge.Dx12Fence,3);
        ComPtr<ID3D12Fence> saved;
        if(mode==0) failAllocator=true;
        if(mode==1) failList=true;
        if(mode==2) failClose=true;
        if(mode==3) dropReset=true;
        if(mode==4) f.bridge.genshinSubmissionUnfenced=true;
        if(mode==5) sentinel=true;
        if(mode==6) { saved.Attach(f.bridge.Dx12Fence); f.bridge.Dx12Fence=nullptr; }
        if(mode==7) f.bridge.Dx12FenceValue=UINT64_MAX;
        if(mode==8) faults.deviceRemoved=true;
        Check(!f.bridge.ReleaseSharedResources(true), "failure never publishes safe release"); f.Retained();
        if(mode==6) f.bridge.Dx12Fence=saved.Detach();
        if(mode==0 || mode==1) Check(callbacks==0 && !f.uses[0]->closed, "failed Reset cannot fabricate closure");
        if(mode>=4) Check(allocatorResets==0 && callbacks==0, "unknown completion cannot reach Reset");
    }
}
void SignalFailure() {
    Fixture f; faults.failSignal=true; f.Submit(); Fixture::Wait(f.tail.Get(),1);
    Check(!f.bridge.ReleaseSharedResources(true), "NR Signal failure retains after owner queue drains");
    f.Retained(); Check(allocatorResets==0 && callbacks==0, "quarantine cannot be cleared by Reset");
}
int main(int argc,char** argv) {
    try {
        if(argc==2 && std::string(argv[1])=="--baseline") { Baseline(); return 2; }
        Check(argc==1, "unknown argument");
        SuccessAndContext(true,false); SuccessAndContext(false,false); SuccessAndContext(true,true);
        DelayedOwnerQueue(); OtherActualQueue(); Refusals(); SignalFailure();
        std::printf("PASS bridge retirement production + WARP: %u scenarios, %u checks; HRESULT faults explicit\n",scenarios,checks);
        return 0;
    } catch(const std::exception& e) {
        std::fprintf(stderr,"FAIL scenario=%u check=%u: %s\n",scenarios,checks,e.what()); return 1;
    }
}

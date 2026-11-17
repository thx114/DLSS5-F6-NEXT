// SPDX-License-Identifier: GPL-3.0-or-later
// Actual production lifetime and scratch bodies; real WARP queue/fence/Reset.
// NGX release/destroy are counters. No NGX inference or game acceptance.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#define DLSSNR_LIFETIME_TEST_SEAM
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <charconv>
#include <climits>
#include <filesystem>
#include <memory>
#include <cstdio>
#include <stdexcept>
namespace Util { inline bool CheckForRealObject(const char*, IUnknown*, IUnknown**) { return false; } }
#include <shaders/dlssnr/DlssNr_RecordingLifetime.h>
#include <ui_mask/DrawUiMask.h>
using Microsoft::WRL::ComPtr;
unsigned checks = 0, releasedFeatures = 0, destroyedParameters = 0;
void Check(bool value, const char* message) { ++checks; if (!value) throw std::runtime_error(message); }
void Hr(HRESULT value, const char* message) { Check(SUCCEEDED(value), message); }
struct NVSDK_NGX_Handle {};
struct NVSDK_NGX_Parameter {};
struct DlssNr_Dx12 {};
namespace NVNGXProxy {
void Release(NVSDK_NGX_Handle* handle) { ++releasedFeatures; delete handle; }
void Destroy(NVSDK_NGX_Parameter* parameters) { ++destroyedParameters; delete parameters; }
auto D3D12_ReleaseFeature() { return &Release; }
auto D3D12_DestroyParameters() { return &Destroy; }
}
DlssNrGpuLifetime::Registry* registry = nullptr;
auto NrRecording(ID3D12Device* device, ID3D12CommandList* list) { return registry->Recording(device, list); }
#include "DeferredLifetimeProduction.inl"
bool maskGame = true, maskSubscribed = false;
namespace GameRouting { bool IsGenshinExecutable(const std::wstring&) { return maskGame; } }
namespace Util { std::filesystem::path ExePath() { return L"GenshinImpact.exe"; } }
namespace MaskObservation {
enum class Slot { UiMask };
void Subscribe(Slot, const int*, bool active) { maskSubscribed = active; }
}
namespace DrawUiMask {
namespace obs = MaskObservation;
struct Rule { uint64_t ps=0,psBytes=0,vs=0,vsBytes=0;UINT count=0; };
struct Configuration { Settings settings; std::array<Rule,16> rules{}; size_t count=0; uint64_t epoch=0; };
std::atomic<std::shared_ptr<const Configuration>> configured;
std::atomic_uint64_t nextConfiguration {0};
std::atomic_bool enabled {false};
constexpr int callbacks = 0;
#include "UiMaskDemandProduction.inl"
void TestDemand() {
    Settings settings {true, false, false, "1:64:2:64:3"};
    Configure(settings); Check(!Enabled() && !maskSubscribed, "no consumer cannot subscribe even with valid mask rules");
    settings.nr=true; Configure(settings); Check(Enabled() && maskSubscribed, "NR has a real mask subscription");
    settings.nr=false; settings.filters=true; Configure(settings); Check(Enabled() && maskSubscribed, "Filter retains its own subscription");
    settings.enabled=false; Configure(settings); Check(!Enabled() && !maskSubscribed, "disabled mask stays unsubscribed");
    settings.enabled=true; maskGame=false; Configure(settings); Check(!Enabled() && !maskSubscribed, "unsupported game cannot capture");
    maskGame=true; settings.rules="invalid"; Configure(settings); Check(!Enabled() && !maskSubscribed, "unknown draw rule cannot capture");
}
}
namespace Scratch {
struct State {
    int *output, *passScratch, *layerLookScratch, *passClampScratch, *passClampScratch2;
    int *colorCopy, *hdrCopy, *colorSmall, *outputNative, *activeColor, *lutScratch;
    bool layerLookScratchFailed = true, passScratchFailed = true, passClampScratchFailed = true;
    bool passClampScratch2Failed = true, lutScratchFailed = true;
} g_nr;
unsigned parked = 0;
void ParkNrResource(int*& resource) { if (resource) { ++parked; resource = nullptr; } }
#include "NrScratchRebuildProduction.inl"
void Test() {
    for (unsigned mode = 0; mode < 4; ++mode) {
        int resource = 0;
        g_nr = { &resource, &resource, &resource, &resource, &resource,
                 &resource, &resource, &resource, &resource, &resource, &resource };
        parked = 0;
        Rebuild((mode & 1) != 0, (mode & 2) != 0);
        Check(parked == (mode ? 11u : 0u), "resolution/seam changes retire every scratch; tuning retains them");
        Check(mode ? !g_nr.output && !g_nr.colorCopy : g_nr.output && g_nr.colorCopy,
              "output and colour input rebuild together");
    }
}
}
struct World {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    DlssNrGpuLifetime::TestFaults faults;
    DlssNrGpuLifetime::Registry work { &faults };
    World() {
        ComPtr<IDXGIFactory4> factory; ComPtr<IDXGIAdapter> adapter;
        Hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "DXGI factory");
        Hr(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)), "WARP adapter");
        Hr(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "WARP device");
        D3D12_COMMAND_QUEUE_DESC desc {};
        Hr(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue)), "direct queue");
        Hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "allocator");
        Hr(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                    IID_PPV_ARGS(&list)), "command list");
        registry = &work;
    }
    std::unique_ptr<Generation> Make() {
        auto g = std::make_unique<Generation>();
        g->device = device.Get(); g->device->AddRef();
        g->queue = queue.Get(); g->queue->AddRef();
        g->feature = new NVSDK_NGX_Handle; g->parameters = new NVSDK_NGX_Parameter;
        return g;
    }
    void Execute() {
        ID3D12CommandList* commands[] { list.Get() };
        queue->ExecuteCommandLists(1, commands);
        work.Submitted(queue.Get(), 1, commands); // production post-Execute observation
    }
    void Drain() {
        ComPtr<ID3D12Fence> fence;
        Hr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "drain fence");
        Hr(queue->Signal(fence.Get(), 1), "actual queue drain signal");
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        Check(event != nullptr, "drain event");
        Hr(fence->SetEventOnCompletion(1, event), "drain completion");
        const auto result = WaitForSingleObject(event, 10000); CloseHandle(event);
        Check(result == WAIT_OBJECT_0, "real GPU drain");
    }
    void Reset() {
        Hr(allocator->Reset(), "completed/discarded allocator Reset");
        Hr(list->Reset(allocator.Get(), nullptr), "successful list Reset");
        work.ResetRecording(list.Get());
    }
};
int main() { try {
    DrawUiMask::TestDemand();
    Scratch::Test();
    { World w; auto g = w.Make();
      Use before(*g, w.list.Get()), after(*g, w.list.Get());
      Check(before.valid && after.valid && g->uses.size() == 1, "Before/After share one actual recording generation");
      Check(!g->Idle(), "unsubmitted executable generation pins private NGX");
      Hr(w.list->Close(), "close discarded recording"); w.Reset();
      Check(g->Idle(), "real unsubmitted Reset discards and retires private generation");
      g.reset(); Check(releasedFeatures == 1 && destroyedParameters == 1, "retired NGX released exactly once"); }
    { World w; auto g = w.Make(); Use use(*g, w.list.Get()); g->creationUse = use.recording;
      Check(!DlssNrGpuLifetime::CreationReady(g->creationUse), "CPU creation return cannot authorize Evaluate");
      ComPtr<ID3D12Fence> gate; Hr(w.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)), "GPU gate");
      Hr(w.queue->Wait(gate.Get(), 1), "hold actual GPU submission");
      Hr(w.list->Close(), "close pending work"); w.Execute();
      Check(!g->Idle() && !DlssNrGpuLifetime::CreationReady(g->creationUse), "pending owning queue fence protects creation and retirement");
      Hr(gate->Signal(1), "release GPU gate"); w.Drain();
      Check(DlssNrGpuLifetime::CreationReady(g->creationUse), "actual creation submission completion authorizes Evaluate");
      Check(!g->Idle(), "GPU completion alone cannot retire an executable recording");
      Check(releasedFeatures == 1, "private NGX remains alive before recording closes");
      w.Reset(); Check(g->Idle(), "owning fence plus successful Reset retires generation");
      g.reset(); Check(releasedFeatures == 2 && destroyedParameters == 2, "submitted generation released exactly once"); }
    for (unsigned failure = 0; failure < 2; ++failure) {
      World w; auto g = w.Make(); Use use(*g, w.list.Get());
      Hr(w.list->Close(), "close quarantine recording"); w.faults.failSignal = failure == 1;
      w.Execute(); w.Drain(); if (failure == 0) { w.Execute(); w.Drain(); }
      Check(use.recording->quarantined && !g->Idle(), "replay or failed Signal quarantines private NGX generation");
      w.Reset(); Check(!g->Idle(), "Reset cannot erase uncertain actual executions");
      (void)g.release(); // Same deliberate retention as production Shutdown; GPU is drained in this fixture.
    }
    std::printf("PASS Deferred ownership / scratch rebuild: %u checks; real WARP, NGX teardown stubbed\n", checks);
    return 0;
} catch (const std::exception& e) { std::fprintf(stderr, "FAIL Deferred ownership after %u checks: %s\n", checks, e.what()); return 1; } }

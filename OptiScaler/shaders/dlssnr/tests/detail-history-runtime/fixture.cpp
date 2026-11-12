// SPDX-License-Identifier: GPL-3.0-or-later
// Lead compiles this standalone TU; see README.md. No generated/copied policy.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#define DLSSNR_LIFETIME_TEST_SEAM
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#pragma warning(push)
#pragma warning(disable: 4324) // Production constants have deliberate CB alignment.
#include <shaders/dlssnr/DlssNr_Common.h>
#include <shaders/dlssnr/DlssNr_DetailReuseConstants.h>
#pragma warning(pop)
#include <shaders/dlssnr/DlssNr_OrderedLifetime.h>
#include <shaders/dlssnr/DlssNr_WatchedLists.h>
#include <shaders/dlssnr/DlssNr_HistoryDebug.h>
#include "../../precompile/dlssnr_detail_reuse_Shader.h"

using Microsoft::WRL::ComPtr;
static unsigned checks = 0;
static void Check(bool condition, const char* message)
{
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
static void Hr(HRESULT result)
{
    if (FAILED(result)) {
        char value[32]; std::snprintf(value, sizeof(value), "HRESULT %08lX", static_cast<unsigned long>(result));
        throw std::runtime_error(value);
    }
}
static bool Near(float a, float b) { return std::abs(a - b) < 0.000001f; }

// Config operand seam, not configuration parsing/defaults coverage.
template<class T> struct Setting {
    T value;
    T value_or_default() const { return value; }
};
struct Config {
    static constexpr uint32_t kPresentedTemporalDurationMaxMs = 2000;
    static constexpr uint32_t kPresentedTemporalMaxAgeMinMs = 16;
    static constexpr uint32_t kPresentedTemporalMaxAgeMaxMs = 5000;
    Setting<uint32_t> DlssNrPresentedTemporalDurationMs {1000}, DlssNrPresentedTemporalMaxAgeMs {1000};
    Setting<float> DlssNrPresentedTemporalStrength {.8f};
    Setting<float> DlssNrPresentedTemporalDepthTolerance {.08f}, DlssNrPresentedTemporalColourTolerance {.08f},
        DlssNrPresentedTemporalMotionTolerance {2}, DlssNrPresentedTemporalNoiseFloor {.01f},
        DlssNrPresentedTemporalStabilityFloor {.65f}, DlssNrPresentedTemporalStabilityClamp {.08f};
    Setting<uint32_t> DlssNrPresentedTemporalStabilityMs {0};
    Setting<float> DlssNrDetailReuseClipGamma {1.25f}, DlssNrDetailReuseClipFalloff {1},
        DlssNrDetailReuseSigmaFloor {.01f};
    Setting<bool> DlssNrDepthTextureFlipY {false}, DlssNrMotionTextureFlipY {false}, DlssNrMotionYInverted {false};
};
namespace DlssNr {
inline float DetailReuseDepthToleranceEffective(const Config&) { return .045f; }
}
namespace Util {
inline bool CheckForRealObject(const char*, IUnknown*, IUnknown**) { return false; }
}

DlssNrGpuLifetime::TestFaults faults;
DlssNrGpuLifetime::Registry g_nrGpuWork {&faults};
struct NrRetired {
    ID3D12Resource* resource = nullptr;
    DlssNrGpuLifetime::Uses uses;
};
std::vector<NrRetired> g_nrRetired;
static unsigned releasedResources = 0;
static DlssNrGpuLifetime::Use NrRecording(ID3D12Device* device, ID3D12CommandList* list)
{
    auto use = g_nrGpuWork.Recording(device, list);
    if (use) DlssNrWatchedLists::Add(list);
    return use;
}
// Host adapter has the same snapshot/ownership transfer contract as ParkNrResource.
// Retirement eligibility itself is the actual production CollectRetired below.
static void ParkNrResource(ID3D12Resource*& resource)
{
    if (!resource) return;
    g_nrRetired.push_back({resource, g_nrGpuWork.Snapshot()});
    resource = nullptr;
}
static void TickRetired()
{
    DlssNrGpuLifetime::CollectRetired(g_nrRetired, [](const NrRetired& retired) {
        ++releasedResources;
        retired.resource->Release();
    });
}
static ID3D12Resource* CreateScratch(ID3D12Device* device, DXGI_FORMAT format, unsigned width, unsigned height)
{
    D3D12_RESOURCE_DESC desc {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width; desc.Height = height; desc.DepthOrArraySize = 1; desc.MipLevels = 1;
    desc.Format = format; desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_HEAP_PROPERTIES heap {}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    ID3D12Resource* resource = nullptr;
    Hr(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&resource)));
    return resource;
}
static void Barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
                    D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
    list->ResourceBarrier(1, &barrier);
}

// Shader dispatch is an explicit seam. Record real GPU UAV barriers and capture
// bindings/constants, but do not emulate HLSL or claim a shader/pixel test.
struct Pass {
    struct Call {
        DlssNrDetailReuseConstants constants;
        std::array<ID3D12Resource*, 11> resources;
    };
    bool ready = true;
    unsigned failCall = 0;
    std::vector<Call> calls;
    bool executeShader = false;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pipeline;
    std::vector<ComPtr<ID3D12DescriptorHeap>> heaps;
    std::vector<ComPtr<ID3D12Resource>> buffers;
    std::vector<unsigned char> shaderOverride; // explicit offline old-binary negative only
    void ExecuteShader(ID3D12GraphicsCommandList* list, const Call& call)
    {
        ComPtr<ID3D12Device> device; Hr(list->GetDevice(IID_PPV_ARGS(&device)));
        if (!pipeline)
        {
            D3D12_DESCRIPTOR_RANGE ranges[] {
                {D3D12_DESCRIPTOR_RANGE_TYPE_SRV,7,0,0,0},
                {D3D12_DESCRIPTOR_RANGE_TYPE_UAV,4,0,0,7},
                {D3D12_DESCRIPTOR_RANGE_TYPE_CBV,1,0,0,11}};
            D3D12_ROOT_PARAMETER parameter {}; parameter.ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            parameter.DescriptorTable={3,ranges};
            D3D12_STATIC_SAMPLER_DESC sampler {};
            sampler.Filter=D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            sampler.MaxLOD=D3D12_FLOAT32_MAX; sampler.ComparisonFunc=D3D12_COMPARISON_FUNC_ALWAYS;
            D3D12_ROOT_SIGNATURE_DESC desc {1,&parameter,1,&sampler,D3D12_ROOT_SIGNATURE_FLAG_NONE};
            ComPtr<ID3DBlob> code,errors;
            Hr(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&code,&errors));
            Hr(device->CreateRootSignature(0,code->GetBufferPointer(),code->GetBufferSize(),IID_PPV_ARGS(&root)));
            D3D12_COMPUTE_PIPELINE_STATE_DESC pso {};pso.pRootSignature=root.Get();
            pso.CS={dlssnr_detail_reuse_cso,sizeof(dlssnr_detail_reuse_cso)};
            if(!shaderOverride.empty())pso.CS={shaderOverride.data(),shaderOverride.size()};
            Hr(device->CreateComputePipelineState(&pso,IID_PPV_ARGS(&pipeline)));
        }
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc {};
        heapDesc.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;heapDesc.NumDescriptors=12;
        heapDesc.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ComPtr<ID3D12DescriptorHeap> heap;Hr(device->CreateDescriptorHeap(&heapDesc,IID_PPV_ARGS(&heap)));
        const auto first=heap->GetCPUDescriptorHandleForHeapStart();
        const auto stride=device->GetDescriptorHandleIncrementSize(heapDesc.Type);
        const auto handle=[&](unsigned slot){return D3D12_CPU_DESCRIPTOR_HANDLE{first.ptr+SIZE_T(slot)*stride};};
        const auto& r=call.resources;
        ID3D12Resource* srv[] {r[0],r[1],r[2],r[3],r[4],r[7],r[9]};
        ID3D12Resource* uav[] {r[5],r[6],r[8],r[10]};
        for(unsigned i=0;i<7;++i)device->CreateShaderResourceView(srv[i]?srv[i]:srv[0],nullptr,handle(i));
        for(unsigned i=0;i<4;++i)device->CreateUnorderedAccessView(uav[i]?uav[i]:uav[0],nullptr,nullptr,handle(7+i));
        D3D12_HEAP_PROPERTIES upload {};upload.Type=D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC buffer {};buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width=(sizeof(call.constants)+255)&~255ull;buffer.Height=buffer.DepthOrArraySize=buffer.MipLevels=1;
        buffer.SampleDesc.Count=1;buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> constants;
        Hr(device->CreateCommittedResource(&upload,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_GENERIC_READ,
                                           nullptr,IID_PPV_ARGS(&constants)));
        void* mapped=nullptr;const D3D12_RANGE none {0,0};Hr(constants->Map(0,&none,&mapped));
        std::memcpy(mapped,&call.constants,sizeof(call.constants));constants->Unmap(0,nullptr);
        const D3D12_CONSTANT_BUFFER_VIEW_DESC cb {constants->GetGPUVirtualAddress(),UINT(buffer.Width)};
        device->CreateConstantBufferView(&cb,handle(11));
        ID3D12DescriptorHeap* tables[] {heap.Get()};list->SetDescriptorHeaps(1,tables);
        list->SetComputeRootSignature(root.Get());list->SetPipelineState(pipeline.Get());
        list->SetComputeRootDescriptorTable(0,heap->GetGPUDescriptorHandleForHeapStart());
        list->Dispatch((call.constants.WorkWidth+7)/8,(call.constants.WorkHeight+7)/8,1);
        // Fixture-owned shader operands outlive every actual queue drain/Reset;
        // they are never used as production retirement or completion evidence.
        heaps.push_back(std::move(heap));buffers.push_back(std::move(constants));
    }
    bool DetailReuseReady() const { return ready; }
    bool DispatchDetailReuse(ID3D12GraphicsCommandList* list, const DlssNrDetailReuseConstants& constants,
        unsigned, unsigned, ID3D12Resource* input, ID3D12Resource* answer,
        ID3D12Resource* detail, ID3D12Resource* colour, ID3D12Resource* motion,
        ID3D12Resource* output, ID3D12Resource* secondary = nullptr, ID3D12Resource* depth = nullptr,
        ID3D12Resource* birthContext = nullptr, ID3D12Resource* oldStability = nullptr, ID3D12Resource* newStability = nullptr)
    {
        calls.push_back({constants, {input, answer, detail, colour, motion, output, secondary, depth, birthContext, oldStability, newStability}});
        if (failCall && calls.size() == failCall) return false;
        for (auto* resource : {output, secondary, birthContext, newStability}) {
            if (!resource) continue;
            D3D12_RESOURCE_BARRIER barrier {};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            barrier.UAV.pResource = resource;
            list->ResourceBarrier(1, &barrier);
        }
        if(executeShader)ExecuteShader(list,calls.back());
        return true;
    }
};
namespace DetailReuse {
constexpr size_t kEncodedContractBytes = offsetof(DlssNrConstants, OriginalIsGameColour) + sizeof(uint32_t);
// Minimal host operands used by the actual module. No admission/publication,
// lifetime, source sequence or policy is implemented here.
struct Frame {
    const Config* cfg = nullptr;
    const DlssNrFrameInfo* info = nullptr;
    Pass* pass = nullptr;
    ID3D12GraphicsCommandList* cmdList = nullptr;
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* temporalQueue = nullptr;
    ID3D12Resource* modelInput = nullptr;
    ID3D12Resource* motion = nullptr;
    ID3D12Resource* depth = nullptr;
    DXGI_FORMAT answerFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    uintptr_t owner = 0;
    uint64_t revision = 1, frameNumber = 0;
    unsigned workWidth = 8, workHeight = 4, motionWidth = 8, motionHeight = 4,
        depthWidth = 8, depthHeight = 4, motionBaseX = 0, motionBaseY = 0,
        depthBaseX = 0, depthBaseY = 0, reversibleMode = 0;
    float mvScaleX = 1, mvScaleY = -1;
    float currentRasterJitterUvX = 0, currentRasterJitterUvY = 0,
        nativeMotionPhaseUvX = 0, nativeMotionPhaseUvY = 0,
        nativeDepthPhaseUvX = 0, nativeDepthPhaseUvY = 0,
        nativeMotionJitterCancelUvX = 0, nativeMotionJitterCancelUvY = 0;
    bool independentFeedbackRequested = true, nativePreSrRoute = true,
        beforeUpscale = true, finishedPicture = false, baseIdentityVerified = true,
        currentDepthVerified = true, nativeGuideContractVerified = true,
        nativeGuidePhaseVerified = true, preSrProducerQueueVerified = true,
        orderedNativeQueue = false, exposureContractKnown = true;
    bool vulkan = false, unsupportedVulkanGameOrigin = false, edgeCompression = false,
        blocked = false, depthInverted = false, passthrough = false, modelReset = false;
    std::array<uint64_t, 3> preSrOwner {11, 22, 33};
    std::array<uint64_t, 8> exposureContract {};
    std::array<uint64_t, 23> sceneGuideContract {};
    std::array<unsigned char, kEncodedContractBytes> encodedTransform {};
};
struct Ledger { uint64_t modelCalls = 0, modelFailures = 0; } ledger;
static unsigned nativeRetireRequests = 0;
// Deliberately no private native pool here; borrowed lists are never reset by
// Before. Real Reset/registry callbacks are supplied explicitly by the fixture.
inline void RetireCompletedNativeRecordings(const Frame&) { ++nativeRetireRequests; }
}

// Full, current product module and its transitive policy/lifetime headers.
#include "../../DlssNr_DetailHistory.inl"

struct Recording {
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    std::vector<ComPtr<ID3D12CommandAllocator>> oldAllocators;
    bool closed = false;
};
struct World {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue, otherQueue;
    ComPtr<ID3D12Resource> input, motion, depth, fresh;
    // Retain allocators through World destruction, including exception unwinds.
    std::vector<ComPtr<ID3D12CommandAllocator>> allAllocators;
    Config config;
    DlssNrFrameInfo info;
    Pass pass;
    explicit World(bool hardware=false)
    {
        ComPtr<IDXGIFactory4> factory;
        ComPtr<IDXGIAdapter> warp;
        Hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
        if(hardware)
        {
            ComPtr<IDXGIAdapter1> adapter;
            for(UINT i=0;factory->EnumAdapters1(i,&adapter)!=DXGI_ERROR_NOT_FOUND;++i)
            {
                DXGI_ADAPTER_DESC1 desc {};Hr(adapter->GetDesc1(&desc));
                if(!(desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)&&
                   SUCCEEDED(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device))))
                {std::printf("Production DXIL GPU vendor=%04X device=%04X\n",desc.VendorId,desc.DeviceId);break;}
                adapter.Reset();
            }
            Check(device!=nullptr,"physical D3D12 adapter available for embedded-DXIL pixel test");
        }
        else
        {
            Hr(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)));
            Hr(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
        }
        D3D12_COMMAND_QUEUE_DESC desc {}; desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        Hr(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue)));
        Hr(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&otherQueue)));
        input.Attach(CreateScratch(device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, 8, 4));
        motion.Attach(CreateScratch(device.Get(), DXGI_FORMAT_R32G32_FLOAT, 8, 4));
        depth.Attach(CreateScratch(device.Get(), DXGI_FORMAT_R32_FLOAT, 8, 4));
        fresh.Attach(CreateScratch(device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, 8, 4));
        info.BeforeUpscale = true;
        info.NativeRouteConventionVerified = info.NativeGuideMetadataKnown = info.NativeFeatureFlagsKnown = true;
        info.NativeCurrentJitterKnown = info.NativeMotionJitterKnown = info.MotionVectorsLowResolution = true;
        info.RenderSubrectWidth = info.OutputWidth = 8;
        info.RenderSubrectHeight = info.OutputHeight = 4;
        info.SourceCaptureEpoch = 73;
    }
    ~World()
    {
        // Queue gates unwind before World. Drain even if an assertion failed;
        // never release a submitted allocator just because the test threw.
        try { Drain(); Drain(otherQueue.Get()); }
        catch (...) { std::fprintf(stderr, "WARP teardown drain failed\n"); std::terminate(); }
    }
    Recording New()
    {
        Recording recording;
        Hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&recording.allocator)));
        Hr(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, recording.allocator.Get(), nullptr,
            IID_PPV_ARGS(&recording.list)));
        allAllocators.push_back(recording.allocator);
        return recording;
    }
    DetailReuse::Frame Frame(Recording& recording, uint64_t source)
    {
        info.SourceCaptureSerial = source;
        info.GuideObservationTick = 1000 + source * 16;
        DetailReuse::Frame frame;
        frame.cfg = &config; frame.info = &info; frame.pass = &pass;
        frame.cmdList = recording.list.Get(); frame.device = device.Get(); frame.temporalQueue = queue.Get();
        frame.modelInput = input.Get(); frame.motion = motion.Get(); frame.depth = depth.Get();
        frame.owner = 99; frame.frameNumber = source;
        return frame;
    }
    static void Close(Recording& recording)
    {
        if (!recording.closed) { Hr(recording.list->Close()); recording.closed = true; }
    }
    void Notify(ID3D12CommandQueue* actual, Recording& recording)
    {
        ID3D12CommandList* lists[] {recording.list.Get()};
        // Same ordering as FinishedPictureSubmittedReal: registry before history.
        g_nrGpuWork.Submitted(actual, 1, lists);
        DetailHistory::Submitted(actual, 1, lists);
    }
    void Execute(Recording& recording, ID3D12CommandQueue* actual = nullptr)
    {
        if (!actual) actual = queue.Get();
        Close(recording);
        ID3D12CommandList* lists[] {recording.list.Get()};
        actual->ExecuteCommandLists(1, lists);
        Notify(actual, recording);
    }
    void Reset(Recording& recording)
    {
        Close(recording);
        // Reset a list while its old GPU work is pending using a NEW allocator;
        // preserve every old allocator until the real queue drain at test end.
        ComPtr<ID3D12CommandAllocator> next;
        Hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&next)));
        allAllocators.push_back(next);
        Hr(recording.list->Reset(next.Get(), nullptr));
        recording.oldAllocators.push_back(std::move(recording.allocator));
        recording.allocator = std::move(next); recording.closed = false;
        g_nrGpuWork.ResetRecording(recording.list.Get());
        DetailHistory::ResetRecording(recording.list.Get());
        const void* identity = recording.list.Get();
        DlssNrWatchedLists::Remove(1, &identity);
    }
    void Drain(ID3D12CommandQueue* actual = nullptr)
    {
        if (!actual) actual = queue.Get();
        ComPtr<ID3D12Fence> fence;
        Hr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
        Hr(actual->Signal(fence.Get(), 1));
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        Check(event != nullptr, "completion event created");
        const auto result = fence->SetEventOnCompletion(1, event);
        const auto wait = SUCCEEDED(result) ? WaitForSingleObject(event, 10000) : WAIT_FAILED;
        CloseHandle(event);
        Hr(result); Check(wait == WAIT_OBJECT_0, "actual WARP queue drained before allocator/resource teardown");
    }
    void Finish(Recording& recording)
    {
        Drain(); Reset(recording); DetailHistory::Collect();
        Check(DetailHistory::SafeToRelease(), "completed and reset generation can release");
        DetailHistory::Release(); TickRetired();
        Check(g_nrRetired.empty(), "production collector releases closed completed allocations");
    }
};
struct Gate {
    ComPtr<ID3D12Fence> fence;
    bool open = false;
    Gate(World& world, ID3D12CommandQueue* queue)
    {
        Hr(world.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
        Hr(queue->Wait(fence.Get(), 1));
    }
    void Open() { Hr(fence->Signal(1)); open = true; }
    ~Gate() { if (!open && fence) fence->Signal(1); }
};
static void Capture(World& world, const DetailReuse::Frame& frame)
{
    auto* answer = world.fresh.Get();
    ++DetailReuse::ledger.modelCalls;
    DetailHistory::After(frame, true, answer);
    Check(answer == DetailHistory::fused && DetailHistory::fusedReadable, "complete fresh model captures and hands fused answer to resolve");
    Check(DetailHistory::recordings.current.publish, "only complete capture may publish");
    DetailHistory::AfterResolve(frame.cmdList);
    Check(!DetailHistory::fusedReadable, "resolve returns fused scratch to UAV");
}
static void ExpectBusy(const DetailReuse::Frame& frame)
{
    const auto before = DetailHistory::snapshot.recordedCount;
    Check(!DetailHistory::Before(frame).active, "outstanding incompatible recording blocks new work");
    Check(std::string(DetailHistory::Status().reason) == "recording-awaits-completion-and-reset", "busy refusal has exact lifecycle reason");
    Check(DetailHistory::snapshot.recordedCount == before, "busy path records no new history");
}
static void Fresh()
{
    World world; auto first = world.New(); Gate gate(world, world.queue.Get());
    auto frame = world.Frame(first, 1);
    const auto plan = DetailHistory::Before(frame);
    Check(plan.active && !plan.historyUsable, "fresh capture is active without a published predecessor");
    Capture(world, frame);
    Check(world.pass.calls.size() == 1 && world.pass.calls[0].constants.Mode == NrDetailHistoryPolicy::ResolveMode,
        "answer, residual and birth context use one dispatch");
    Check(world.pass.calls[0].resources[8] == DetailHistory::colourDepth[DetailHistory::activeWrite],
        "same dispatch writes the exact retained birth context");
    Check(world.pass.calls[0].constants.FeedbackHistoryValid == 0, "first frame never samples previous history");
    Check(world.pass.calls[0].resources[0] == world.input.Get() && world.pass.calls[0].resources[1] == world.fresh.Get() &&
        world.pass.calls[0].resources[4] == world.motion.Get() && world.pass.calls[0].resources[7] == world.depth.Get(), "actual fresh/guide bindings reach the shader seam");
    Check(!DetailHistory::Status().published, "recording alone grants no publication");
    Check(!DetailHistory::recordings.current.fence, "history creates no speculative completion fence before Execute");
    world.Execute(first);
    auto use = DetailHistory::recordings.current.resourceUse;
    const auto* receipt = DlssNrGpuLifetime::ActualSubmission(use, first.list.Get(), world.queue.Get());
    Check(receipt && DetailHistory::recordings.current.fence == receipt->fence &&
        DetailHistory::recordings.current.value == receipt->target &&
        DetailHistory::recordings.current.retirementQueue == receipt->queue,
        "history uses the exact already-signalled recording receipt without an extra Signal");
    Check(!DlssNrGpuLifetime::ActualSubmission(use, first.list.Get(), world.otherQueue.Get()),
        "a different actual queue cannot borrow this submission receipt");
    auto other = world.New();
    Check(!DlssNrGpuLifetime::ActualSubmission(use, other.list.Get(), world.queue.Get()),
        "another executable recording cannot borrow this submission receipt");
    Check(DetailHistory::Status().published && DetailHistory::Status().queueOrdered &&
        DetailHistory::Status().historySource == 1, "post Execute and successful signal publish exact ordered source");
    Check(!use->GpuComplete() && !use->Complete(), "real queue gate keeps execution incomplete after logical publication");
    DetailHistory::Release();
    Check(DetailHistory::detail[0] != nullptr && !use->owners.empty(), "Release never frees open pending owners");
    // Reset closes the executable generation, but cannot complete its GPU work.
    world.Reset(first);
    Check(!DlssNrGpuLifetime::ActualSubmission(use, first.list.Get(), world.queue.Get()),
        "Reset never relabels the old generation receipt as a new execution");
    Check(use->closed && !use->Complete() && !DetailHistory::SafeToRelease(), "Reset alone cannot retire a queue-blocked execution");
    gate.Open(); world.Drain(); DetailHistory::Collect();
    Check(use->Complete() && use->owners.empty(), "real fence completion plus Reset retires token owners");
    DetailHistory::Release(); TickRetired();
    Check(releasedResources == 5 && g_nrRetired.empty(), "all five module allocations retire through production collector");
}
static void Adjacent()
{
    World world; auto first = world.New(); auto second = world.New(); Gate gate(world, world.queue.Get());
    auto a = world.Frame(first, 10);
    Check(DetailHistory::Before(a).active, "first ordered frame admitted"); Capture(world, a); world.Execute(first);
    auto prior = DetailHistory::recordings.current.resourceUse;
    const auto captured = DetailHistory::publishedKey.sample;
    auto b = world.Frame(second, 11);
    b.currentRasterJitterUvX = .25f / 8; world.info.NativeJitterX = .25f;
    const auto plan = DetailHistory::Before(b);
    Check(plan.active && plan.historyUsable && !prior->GpuComplete(), "adjacent authenticated queue advances before CPU fence completion");
    Check(DetailHistory::recordings.retained[0].resourceUse == prior && !prior->owners.empty(), "ordered advance preserves exact old recording and owners");
    Check(DetailHistory::publishedKey.sample.ordinal == captured.ordinal &&
        DetailHistory::publishedKey.sample.sourceSeconds == captured.sourceSeconds, "preparing next source never overwrites published capture identity/time");
    Check(Near(DetailHistory::constants.FeedbackDeltaSeconds, .016f) &&
        Near(DetailHistory::constants.FeedbackRasterOffsetUvX, -.25f / 8), "production policy uses immutable source time and previous-minus-current jitter");
    Capture(world, b); world.Execute(second);
    Check(DetailHistory::Status().historySource == 11 && DetailHistory::Status().publishedCount == 2 &&
        DetailHistory::Status().pending == 2, "two logical publications retain two real incomplete recordings");
    // Transfer the host reference to the same parked-resource contract used by
    // the module. Both actual production recording tokens already retain input.
    auto* parkedInput = world.input.Detach();
    ParkNrResource(parkedInput); TickRetired();
    Check(g_nrRetired.size() == 1 && releasedResources == 0,
        "actual production collector cannot retire a parked shared input before owning GPU completion");
    gate.Open(); world.Drain(); DetailHistory::Collect();
    Check(prior->GpuComplete() && !prior->Complete() && !prior->owners.empty(), "completed fence alone cannot close replayable recording");
    const auto publications = DetailHistory::snapshot.publishedCount;
    DetailHistory::Collect();
    Check(DetailHistory::snapshot.publishedCount == publications, "completed collect does not republish or roll back generation");
    TickRetired();
    Check(g_nrRetired.size() == 1 && releasedResources == 0,
        "completed queue fences cannot retire a parked input while executable generations remain open");
    world.Reset(first); TickRetired();
    Check(g_nrRetired.size() == 1, "Reset of one generation cannot retire another generation's shared input");
    world.Reset(second); DetailHistory::Collect(); TickRetired();
    Check(g_nrRetired.empty() && releasedResources == 1,
        "parked input retires only after both actual GPU completions and both successful Resets");
    Check(DetailHistory::SafeToRelease(), "both exact generations closed after queue completion");
    DetailHistory::Release(); TickRetired();
    Check(releasedResources == 6, "shared parked input and five module allocations retire exactly once");
}
static void FinishedNativeRoute()
{
    World world;
    auto first = world.New(); auto second = world.New();
    const auto makeFrame = [&](Recording& recording, uint64_t source) {
        auto frame = world.Frame(recording, source);
        world.info.BeforeUpscale = false;
        world.info.FinishedPicture = true;
        world.info.NativeSceneRoute = true;
        world.info.SceneGuideOwner[0] = 0x101;
        world.info.SceneGuideOwner[1] = 0x202;
        world.info.SceneGuideOwner[2] = 73;
        world.info.NativeJitterX = .25f;
        world.info.NativeJitterY = -.25f;
        world.info.NativeRouteConventionVerified = true;
        world.info.NativeGuideMetadataKnown = true;
        world.info.NativeFeatureFlagsKnown = true;
        world.info.NativeCurrentJitterKnown = true;
        world.info.NativeMotionJitterKnown = true;
        frame.beforeUpscale = false;
        frame.finishedPicture = true;
        frame.nativePreSrRoute = false;
        frame.independentFeedbackRequested = true;
        frame.baseIdentityVerified = true;
        frame.currentDepthVerified = true;
        frame.nativeGuideContractVerified = true;
        frame.nativeGuidePhaseVerified = true;
        frame.nativeMotionPhaseUvX = frame.nativeDepthPhaseUvX = .25f / 8;
        frame.nativeMotionPhaseUvY = frame.nativeDepthPhaseUvY = -.25f / 4;
        frame.orderedNativeQueue = true;
        return frame;
    };
    auto a = makeFrame(first, 40);
    auto firstPlan = DetailHistory::Before(a);
    Check(firstPlan.active && !firstPlan.historyUsable,
        "finished native scene route records a fresh residual seed");
    Capture(world, a); world.Execute(first);

    auto b = makeFrame(second, 41);
    const auto next = DetailHistory::Before(b);
    Check(next.active && next.historyUsable && DetailHistory::constants.FeedbackHistoryValid == 1,
        "adjacent Genshin presentation source reuses its published NGX scene history");
    Check(DetailHistory::Status().historySource == 40,
        "finished-picture feedback names the exact previous source");
    Capture(world, b); world.Execute(second);
    world.Drain();
    world.Reset(first); world.Reset(second); DetailHistory::Collect();
    Check(DetailHistory::SafeToRelease(), "finished-picture owners retire only after both completion and Reset");
    DetailHistory::Release(); TickRetired();
}
static void Pending()
{
    World world; auto first = world.New(); auto second = world.New();
    auto a = world.Frame(first, 1);
    Check(DetailHistory::Before(a).active, "unsubmitted capture admitted"); Capture(world, a);
    auto use = DetailHistory::recordings.current.resourceUse;
    auto b = world.Frame(second, 2); ExpectBusy(b);
    Check(!DetailHistory::Status().published && !use->closed, "unsubmitted recording owns resources without publication");
    world.Reset(first);
    Check(use->discarded && use->Complete(), "actual Reset discards never executed generation");
    b = world.Frame(second, 3);
    const auto next = DetailHistory::Before(b);
    Check(next.active && !next.historyUsable, "refused source is observed and next accepted source starts fresh");
    Capture(world, b); world.Execute(second); world.Finish(second);
}
static void CompletionOnly()
{
    World world; auto first = world.New(); auto second = world.New(); Gate gate(world, world.queue.Get());
    auto a = world.Frame(first, 1); a.preSrProducerQueueVerified = false;
    Check(DetailHistory::Before(a).active, "unauthenticated ordered producer may capture completion-only history");
    Capture(world, a); world.Execute(first);
    Check(!DetailHistory::Status().published, "queue hint cannot publish before completion");
    auto b = world.Frame(second, 2); b.preSrProducerQueueVerified = false; ExpectBusy(b);
    gate.Open(); world.Drain(); DetailHistory::Collect();
    Check(DetailHistory::Status().published && !DetailHistory::Status().queueOrdered &&
        !DetailHistory::SafeToRelease(), "completion-only publication still retains executable generation until Reset");
    world.Reset(first); DetailHistory::Release(); TickRetired();
}
static void Fault(const std::string& kind)
{
    World world; auto first = world.New(); auto second = world.New();
    auto a = world.Frame(first, 1);
    Check(DetailHistory::Before(a).active, "fault setup capture admitted"); Capture(world, a);
    auto use = DetailHistory::recordings.current.resourceUse;
    if (kind == "history-signal") faults.missingHistoryReceipt = true; // preserve the existing fault-case CLI
    if (kind == "registry-signal") faults.failSignal = true;
    auto* actual = kind == "wrong-queue" ? world.otherQueue.Get() : world.queue.Get();
    world.Execute(first, actual); world.Drain(actual);
    if (kind == "replay") {
        Check(DetailHistory::Status().published, "initial execution publishes before replay");
        // This really executes the same closed generation again, not just a fake callback.
        world.Execute(first, actual); world.Drain(actual);
    }
    Check(use->quarantined && !DetailHistory::Status().published, "wrong queue, failed signal or replay quarantines and invalidates history");
    Check(!use->Complete() && !use->owners.empty(), "completed real queue cannot retire quarantined generation");
    auto b = world.Frame(second, 2); ExpectBusy(b);
    world.Reset(first);
    Check(use->closed && !use->Complete() && !DetailHistory::SafeToRelease(), "successful Reset never cures missing or ambiguous execution proof");
    DetailHistory::Release();
    Check(DetailHistory::detail[0] && !use->owners.empty(), "faulted owners remain pinned until isolated test process exit");
    // No force-completion/flag clearing: quarantined production state is retained.
}
static void Reconfigure(const std::string& change)
{
    World world; auto first = world.New(); auto second = world.New(); Gate gate(world, world.queue.Get());
    auto a = world.Frame(first, 10);
    Check(DetailHistory::Before(a).active, "reconfiguration setup admitted"); Capture(world, a); world.Execute(first);
    const auto oldKey = DetailHistory::publishedKey;
    auto* originalDetail = DetailHistory::detail[0];
    if (change == "duration-zero") world.config.DlssNrPresentedTemporalDurationMs.value = 0;
    else if (change == "strength") world.config.DlssNrPresentedTemporalStrength.value = .3f;
    auto b = world.Frame(second, 11);
    if (change == "revision") b.revision = 2;
    if (change == "encoding") b.encodedTransform[0] = 1;
    if (change == "mapping") b.mvScaleY = -2;
    ExpectBusy(b);
    Check(DetailHistory::detail[0] == originalDetail && DetailHistory::publishedKey == oldKey,
        "reconfiguration never replaces storage or publisher while prior recording is live");
    gate.Open(); world.Drain();
    Check(!DetailHistory::SafeToRelease(), "GPU completion alone does not permit config teardown");
    world.Reset(first);
    // The refused source was observed. Use the next real source, not a replay of 11.
    b = world.Frame(second, 12);
    if (change == "revision") b.revision = 2;
    if (change == "encoding") b.encodedTransform[0] = 1;
    if (change == "mapping") b.mvScaleY = -2;
    const auto plan = DetailHistory::Before(b);
    if (change == "duration-zero")
    {
        Check(!plan.active && !plan.historyUsable && !DetailHistory::Status().published,
            "zero duration records no history and revokes the prior publisher");
        const auto calls = world.pass.calls.size();
        auto* answer = world.fresh.Get(); DetailHistory::After(b, true, answer);
        Check(world.pass.calls.size() == calls && answer == world.fresh.Get(),
            "zero duration has no additional dispatch");
        world.Finish(second);
        return;
    }
    Check(plan.active && !plan.historyUsable, "closed old generation allows fresh recording with changed contract");
    Check(DetailHistory::constants.FeedbackHistoryValid == 0, "changed config has zero old-history sampling permission");
    Capture(world, b); world.Execute(second); world.Finish(second);
}
static void ZeroWeight(bool strengthZero)
{
    // No texture/cache/dispatch is needed when history has zero visible weight.
    World world;
    if (strengthZero) world.config.DlssNrPresentedTemporalStrength.value = 0;
    else world.config.DlssNrPresentedTemporalDurationMs.value = 0;
    for (uint64_t ordinal=1; ordinal<4; ++ordinal) {
        auto recording=world.New(); auto frame=world.Frame(recording,ordinal);
        Check(!DetailHistory::Before(frame).active,"zero duration bypasses history preparation");
        auto* answer=world.fresh.Get(); DetailHistory::After(frame,true,answer);
        Check(answer==world.fresh.Get() && world.pass.calls.empty() &&
            DetailHistory::allocatedBytes==0,"fresh-only allocates and dispatches nothing");
        Check(std::string(DetailHistory::Status().reason)=="fresh-only-settings","zero duration has a stable reason");
        world.Finish(recording);
    }
    DetailHistory::Release(); TickRetired();
}
static void DispatchFailure()
{
    World world; auto first = world.New(); world.pass.failCall = 1;
    auto frame = world.Frame(first, 1);
    Check(DetailHistory::Before(frame).active, "partial capture setup admitted");
    auto* answer = world.fresh.Get(); DetailHistory::After(frame, true, answer);
    Check(answer == world.fresh.Get() && !DetailHistory::recordings.current.publish &&
        !DetailHistory::Status().recorded, "failed capture cannot hand off partial answer or request publication");
    world.Execute(first); world.Drain();
    Check(!DetailHistory::Status().published && !DetailHistory::SafeToRelease(), "partial GPU recording still requires actual completion plus Reset");
    world.Reset(first); DetailHistory::Release(); TickRetired();
}

static void FinishedNativeDxil(const char* oldShader=nullptr, bool stabilizer=false)
{
    World world(true);
    if(stabilizer) world.config.DlssNrPresentedTemporalStabilityMs.value=200;
    world.pass.executeShader=true;
    if(oldShader)
    {
        FILE* file=nullptr;
        Check(fopen_s(&file,oldShader,"rb")==0&&file,"old-binary negative shader opened");
        std::fseek(file,0,SEEK_END);const auto length=std::ftell(file);std::rewind(file);
        if(length<=0||length>4*1024*1024){std::fclose(file);throw std::runtime_error("invalid old shader size");}
        world.pass.shaderOverride.resize(size_t(length));
        const auto read=std::fread(world.pass.shaderOverride.data(),1,size_t(length),file);std::fclose(file);
        Check(read==size_t(length),"old-binary negative shader read completely");
    }
    world.config.DlssNrPresentedTemporalDurationMs.value=stabilizer?0:820;
    world.config.DlssNrPresentedTemporalMaxAgeMs.value=2346;
    world.info.BeforeUpscale=false;world.info.FinishedPicture=world.info.NativeSceneRoute=true;
    world.info.SceneGuideOwner[0]=0x101;world.info.SceneGuideOwner[1]=0x202;world.info.SceneGuideOwner[2]=73;
    auto recording=world.New();
    const auto upload=[&](ID3D12Resource* texture,D3D12_RESOURCE_STATES before,const void* pixel,UINT pixelBytes) {
        const auto desc=texture->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};UINT64 bytes=0;
        world.device->GetCopyableFootprints(&desc,0,1,0,&footprint,nullptr,nullptr,&bytes);
        D3D12_HEAP_PROPERTIES heap {};heap.Type=D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC buffer {};buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;buffer.Width=bytes;
        buffer.Height=buffer.DepthOrArraySize=buffer.MipLevels=1;buffer.SampleDesc.Count=1;
        buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;ComPtr<ID3D12Resource> resource;
        Hr(world.device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_GENERIC_READ,
                                                nullptr,IID_PPV_ARGS(&resource)));
        void* mapped=nullptr;const D3D12_RANGE none {0,0};Hr(resource->Map(0,&none,&mapped));
        std::memset(mapped,0,size_t(bytes));
        for(UINT y=0;y<desc.Height;++y)for(UINT x=0;x<desc.Width;++x)
            std::memcpy(static_cast<char*>(mapped)+footprint.Offset+y*footprint.Footprint.RowPitch+x*pixelBytes,
                        pixel,pixelBytes);
        resource->Unmap(0,nullptr);
        Barrier(recording.list.Get(),texture,before,D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION dst {};dst.pResource=texture;dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION src {};src.pResource=resource.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint=footprint;recording.list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
        Barrier(recording.list.Get(),texture,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        world.pass.buffers.push_back(std::move(resource));
    };
    for(unsigned n=0;n<40;++n)
    {
        const auto prior=n?D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE:D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        const uint16_t raw[] {0x3800,0x3800,0x3800,0x3c00}; // exact FP16 .5 RGB, alpha 1
        const uint16_t answer[] {uint16_t(stabilizer?(n%2?0x3700:0x3880):(n?0x3800:0x3880)),0x3800,0x3800,0x3c00}; // seed adds .0625 red
        const float depth=.5f,motion[] {n%2?-.25f:.25f,0};
        upload(world.input.Get(),prior,raw,sizeof(raw));upload(world.fresh.Get(),prior,answer,sizeof(answer));
        upload(world.depth.Get(),prior,&depth,sizeof(depth));upload(world.motion.Get(),prior,motion,sizeof(motion));
        auto frame=world.Frame(recording,n+1);frame.beforeUpscale=false;frame.finishedPicture=true;
        frame.nativePreSrRoute=false;frame.preSrProducerQueueVerified=false;frame.orderedNativeQueue=true;
        const auto plan=DetailHistory::Before(frame);
        Check(plan.active&&plan.historyUsable==bool(n),"actual finished route has adjacent history for every continuous source");
        Capture(world,frame);
        // Read exactly the answer selected by the real module, after actual
        // production DXIL, FP16 writes and queue execution. No CPU pixel oracle.
        auto* fused=DetailHistory::fused;const auto desc=fused->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};UINT64 bytes=0;
        world.device->GetCopyableFootprints(&desc,0,1,0,&footprint,nullptr,nullptr,&bytes);
        D3D12_HEAP_PROPERTIES heap {};heap.Type=D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC buffer {};buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;buffer.Width=bytes;
        buffer.Height=buffer.DepthOrArraySize=buffer.MipLevels=1;buffer.SampleDesc.Count=1;
        buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;ComPtr<ID3D12Resource> readback;
        Hr(world.device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_COPY_DEST,
                                                nullptr,IID_PPV_ARGS(&readback)));
        world.pass.buffers.push_back(readback);
        Barrier(recording.list.Get(),fused,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION dst {};dst.pResource=readback.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint=footprint;D3D12_TEXTURE_COPY_LOCATION src {};src.pResource=fused;
        src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        recording.list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
        Barrier(recording.list.Get(),fused,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        world.Execute(recording);world.Drain();
        void* data=nullptr;const D3D12_RANGE range {0,SIZE_T(bytes)};Hr(readback->Map(0,&range,&data));
        const auto* probe=reinterpret_cast<const uint16_t*>(static_cast<const char*>(data)+
                          footprint.Offset+2*footprint.Footprint.RowPitch+4*8);
        const float red=std::ldexp(1.0f+float(probe[0]&1023)/1024.0f,int((probe[0]>>10)&31)-15);
        readback->Unmap(0,nullptr);
        Check(stabilizer ? (n<2||(red>.46f&&red<.54f)) : (red>.505f&&red<=.5625f),
              "embedded production DXIL retains detail or suppresses alternating signed NR noise through actual published FP16 sources");
        Check(DetailHistory::Status().historySource==n+1,"physical recording publishes only its exact current source");
        world.Reset(recording);
    }
    world.Finish(recording);
}

static void StabilityOwnership()
{
    World world;world.config.DlssNrPresentedTemporalStabilityMs.value=200;
    world.config.DlssNrPresentedTemporalDurationMs.value=0;
    auto first=world.New();auto second=world.New();Gate gate(world,world.queue.Get());
    auto a=world.Frame(first,1);Check(DetailHistory::Before(a).active,"stabilizer runs independently of long duration");
    Capture(world,a);
    Check(DetailHistory::stability[0]&&DetailHistory::stability[1],"only enabled stabilizer allocates two residual textures");
    Check(world.pass.calls.back().resources[10]==DetailHistory::stability[DetailHistory::activeWrite],
          "one production dispatch writes the exact retained stabilization target");
    world.Execute(first);world.Reset(first);
    auto b=world.Frame(second,2);Check(DetailHistory::Before(b).historyUsable,"published adjacent source feeds both histories");
    Capture(world,b);world.Execute(second);
    auto use=DetailHistory::recordings.current.resourceUse;
    Check(!use->GpuComplete(),"real queue gate blocks stabilization execution too");
    DetailHistory::Release();Check(DetailHistory::stability[0],"release cannot free blocked stabilization operands");
    world.Reset(second);Check(!use->Complete(),"Reset alone never retires stabilization state");
    gate.Open();world.Drain();DetailHistory::Collect();DetailHistory::Release();TickRetired();
    Check(!DetailHistory::stability[0]&&!DetailHistory::stability[1]&&releasedResources==7,
          "all seven resources retire only after exact queue completion and Reset");
}
static const char* cases[] {
    "stability-ownership",
    "fresh", "adjacent", "finished-native-route", "pending", "completion-only", "wrong-queue", "history-signal", "registry-signal",
    "replay", "reconfigure-strength", "reconfigure-revision", "reconfigure-encoding", "reconfigure-mapping",
    "reconfigure-duration-zero", "duration-zero", "strength-zero", "dispatch-failure"
};
int main(int argc, char** argv)
{
    if(argc==3&&std::string(argv[1])=="--old-dxil-negative")
    {
        try {FinishedNativeDxil(argv[2]);std::puts("Unexpected old DXIL pixel test success");return 0;}
        catch(const std::exception& error){std::fprintf(stderr,"FAIL old-dxil-negative: %s\n",error.what());return 1;}
    }
    if (argc == 2 && std::string(argv[1]) == "--list") {
        for (const auto* name : cases) std::puts(name);
        return 0;
    }
    if(argc==2&&std::string(argv[1])=="--list-hardware") {std::puts("finished-native-dxil");std::puts("stability-native-dxil");return 0;}
    if (argc != 3 || std::string(argv[1]) != "--case") {
        std::fprintf(stderr, "Usage: DetailHistoryRuntime.exe --list | --case NAME\n"); return 2;
    }
    const std::string name = argv[2];
    try {
        if (name == "stability-ownership") StabilityOwnership();
        else if (name == "fresh") Fresh();
        else if (name == "adjacent") Adjacent();
        else if (name == "finished-native-route") FinishedNativeRoute();
        else if (name == "finished-native-dxil") FinishedNativeDxil();
        else if (name == "stability-native-dxil") FinishedNativeDxil(nullptr,true);
        else if (name == "pending") Pending();
        else if (name == "completion-only") CompletionOnly();
        else if (name == "wrong-queue" || name == "history-signal" || name == "registry-signal" || name == "replay") Fault(name);
        else if (name.starts_with("reconfigure-")) {
            if (std::none_of(std::begin(cases), std::end(cases), [&](const char* candidate) { return name == candidate; })) return 2;
            Reconfigure(name.substr(12));
        }
        else if (name == "duration-zero" || name == "strength-zero") ZeroWeight(name == "strength-zero");
        else if (name == "dispatch-failure") DispatchFailure();
        else return 2;
        std::printf("PASS %s: %u checks (production module/lifetime; %s)\n", name.c_str(), checks,
                    (name=="finished-native-dxil"||name=="stability-native-dxil")?"physical GPU, embedded production DXIL and actual FP16 pixels":"real WARP, shader seam");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL %s after %u checks: %s\n", name.c_str(), checks, error.what()); return 1;
    }
}

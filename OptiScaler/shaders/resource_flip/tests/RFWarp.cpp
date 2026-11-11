// Standalone real D3D12 WARP pixel regression. No OptiScaler runtime or game.
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <array>
#include <bit>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4324) // Intentional D3D12 CBV alignment in the production constants.
#endif
#include "../RF_Common.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif
#include "../precompiled/RF_Shader.h"

#ifndef RF_SOURCE_FNV1A
#error Run Generate-RFShader.ps1 before compiling this fixture.
#else
static_assert(RFSourceHash() == RF_SOURCE_FNV1A, "RF bytecode/source mismatch");
#endif

using Microsoft::WRL::ComPtr;
namespace
{
void Require(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}
void Hr(HRESULT value, const char* message)
{
    if (FAILED(value))
    {
        std::cerr << message << ": HRESULT=0x" << std::hex << static_cast<unsigned long>(value) << std::dec << '\n';
        throw std::runtime_error(message);
    }
}
struct Format
{
    DXGI_FORMAT format;
    UINT channels;
    UINT bytes;
    bool velocity;
};
constexpr Format Formats[] = {
    {DXGI_FORMAT_R32_FLOAT, 1, 4, false},
    {DXGI_FORMAT_R16G16_FLOAT, 2, 4, true},
    {DXGI_FORMAT_R32G32_FLOAT, 2, 8, true},
};

// Fixture samples are exact binary fractions in half's normal range, plus zero.
// Expected bytes are independent of shader arithmetic and exercise sign-bit changes.
std::uint16_t Half(float value)
{
    const auto bits = std::bit_cast<std::uint32_t>(value);
    const auto sign = (bits >> 16) & 0x8000u;
    if ((bits & 0x7fffffffu) == 0) return static_cast<std::uint16_t>(sign);
    const auto exponent = (bits >> 23) & 255u;
    Require(exponent >= 113 && exponent <= 142 && (bits & 0x1fffu) == 0, "non-exact half sample");
    return static_cast<std::uint16_t>(sign | ((exponent - 112) << 10) | ((bits >> 13) & 1023u));
}
void Store(std::uint8_t* target, Format format, float x, float y)
{
    if (format.format == DXGI_FORMAT_R16G16_FLOAT)
    {
        const std::uint16_t pixel[] = {Half(x), Half(y)};
        std::memcpy(target, pixel, sizeof(pixel));
    }
    else
    {
        const float pixel[] = {x, y};
        std::memcpy(target, pixel, format.bytes);
    }
}
std::array<float, 2> Sample(UINT x, UINT y)
{
    return {static_cast<float>(static_cast<int>((x * 3 + y * 5) % 41) - 20) / 8.0f,
            static_cast<float>(static_cast<int>((x * 7 + y * 11) % 53) - 26) / 8.0f};
}
D3D12_RESOURCE_DESC Texture(UINT width, UINT height, Format format, bool uav)
{
    D3D12_RESOURCE_DESC result {};
    result.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    result.Width = width;
    result.Height = height;
    result.DepthOrArraySize = 1;
    result.MipLevels = 1;
    result.Format = format.format;
    result.SampleDesc.Count = 1;
    result.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    return result;
}
struct Harness
{
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12RootSignature> root;
    HANDLE event = nullptr;
    UINT64 sequence = 0;
    UINT cases = 0;
    UINT64 pixels = 0;

    Harness()
    {
        ComPtr<IDXGIFactory4> factory;
        ComPtr<IDXGIAdapter> warp;
        Hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory");
        Hr(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)), "WARP adapter");
        Hr(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "WARP device");
        D3D12_COMMAND_QUEUE_DESC desc {};
        desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        Hr(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue)), "queue");
        Hr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence");
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        Require(event != nullptr, "fence event");

        D3D12_DESCRIPTOR_RANGE ranges[2] {};
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        for (auto& range : ranges)
        {
            range.NumDescriptors = 1;
            range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        }
        D3D12_ROOT_PARAMETER parameters[2] {};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[0].DescriptorTable.NumDescriptorRanges = 2;
        parameters[0].DescriptorTable.pDescriptorRanges = ranges;
        parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        D3D12_ROOT_SIGNATURE_DESC signature {};
        signature.NumParameters = 2;
        signature.pParameters = parameters;
        ComPtr<ID3DBlob> blob, errors;
        Hr(D3D12SerializeRootSignature(&signature, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors), "root serialization");
        Hr(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root)), "root");
    }
    ~Harness() { if (event) CloseHandle(event); }

    ComPtr<ID3D12Resource> Resource(const D3D12_RESOURCE_DESC& desc, D3D12_HEAP_TYPE type,
                                  D3D12_RESOURCE_STATES state)
    {
        D3D12_HEAP_PROPERTIES heap {};
        heap.Type = type;
        heap.CreationNodeMask = heap.VisibleNodeMask = 1;
        ComPtr<ID3D12Resource> result;
        Hr(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                           IID_PPV_ARGS(&result)), "resource");
        return result;
    }
    ComPtr<ID3D12Resource> Buffer(UINT64 size, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state)
    {
        D3D12_RESOURCE_DESC desc {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = size;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        return Resource(desc, type, state);
    }
    static void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
                           D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
    {
        D3D12_RESOURCE_BARRIER barrier {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = resource;
        barrier.Transition.StateBefore = before;
        barrier.Transition.StateAfter = after;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1, &barrier);
    }
    ComPtr<ID3D12PipelineState> Pipeline(const void* bytes, SIZE_T size)
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc {};
        desc.pRootSignature = root.Get();
        desc.CS = {bytes, size};
        ComPtr<ID3D12PipelineState> result;
        Hr(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&result)), "pixel pipeline");
        return result;
    }

    void Run(ID3D12PipelineState* pipeline, Format format, UINT width, UINT height,
             bool flip, bool invert, bool offsetRequested, UINT invalid = 0)
    {
        const UINT srcW = width + 4, srcH = height + 7, dstW = width + 5, dstH = height + 4;
        const auto srcDesc = Texture(srcW, srcH, format, false);
        const auto dstDesc = Texture(dstW, dstH, format, true);
        auto source = Resource(srcDesc, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
        auto destination = Resource(dstDesc, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT srcFoot {}, dstFoot {};
        UINT64 srcBytes = 0, dstBytes = 0;
        device->GetCopyableFootprints(&srcDesc, 0, 1, 0, &srcFoot, nullptr, nullptr, &srcBytes);
        device->GetCopyableFootprints(&dstDesc, 0, 1, 0, &dstFoot, nullptr, nullptr, &dstBytes);
        auto srcUpload = Buffer(srcBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        auto dstUpload = Buffer(dstBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        auto readback = Buffer(dstBytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
        std::uint8_t* mapped = nullptr;
        const D3D12_RANGE noRead {0, 0};
        Hr(srcUpload->Map(0, &noRead, reinterpret_cast<void**>(&mapped)), "source upload map");
        std::memset(mapped, 0xcd, static_cast<SIZE_T>(srcBytes));
        for (UINT y = 0; y < srcH; ++y)
            for (UINT x = 0; x < srcW; ++x)
            {
                const auto value = Sample(x, y);
                Store(mapped + srcFoot.Offset + static_cast<SIZE_T>(y) * srcFoot.Footprint.RowPitch + x * format.bytes,
                      format, value[0], value[1]);
            }
        srcUpload->Unmap(0, nullptr);
        Hr(dstUpload->Map(0, &noRead, reinterpret_cast<void**>(&mapped)), "sentinel upload map");
        std::memset(mapped, 0xcd, static_cast<SIZE_T>(dstBytes));
        for (UINT y = 0; y < dstH; ++y)
            for (UINT x = 0; x < dstW; ++x)
                Store(mapped + dstFoot.Offset + static_cast<SIZE_T>(y) * dstFoot.Footprint.RowPitch + x * format.bytes,
                      format, 121.0f, -117.0f);
        dstUpload->Unmap(0, nullptr);

        RFRegion region {};
        Require(TryRFRegion(width, height, srcW, srcH, dstW, dstH, flip && offsetRequested, region), "valid host region");
        RFConstants constants {};
        constants.width = width;
        constants.height = height;
        constants.offset = region.offset;
        constants.velocity = format.velocity ? 1u : 0u;
        constants.rowFlip = flip ? 1u : 0u;
        // Pass even depth's invert flag to the shader: velocity must gate it there too.
        constants.negateMotionY = invert ? 1u : 0u;
        switch (invalid)
        {
        case 1: constants.width = 0; break;
        case 2: constants.height = 0; break;
        case 3: constants.width = srcW + 1; break;
        case 4: constants.height = srcH + 1; break;
        case 5: constants.height = dstH + 1; break;
        case 6: constants.offset = srcH - height + 1; break;
        case 7: constants.offset = 0xffffffffu; break;
        case 8: constants.width = 0xffffffffu; break;
        default: break;
        }
        auto cb = Buffer(sizeof(constants), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        Hr(cb->Map(0, &noRead, reinterpret_cast<void**>(&mapped)), "constants map");
        std::memcpy(mapped, &constants, sizeof(constants));
        cb->Unmap(0, nullptr);

        D3D12_DESCRIPTOR_HEAP_DESC heapDesc {};
        heapDesc.NumDescriptors = 2;
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ComPtr<ID3D12DescriptorHeap> heap;
        Hr(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&heap)), "descriptor heap");
        auto cpu = heap->GetCPUDescriptorHandleForHeapStart();
        D3D12_SHADER_RESOURCE_VIEW_DESC srv {};
        srv.Format = format.format;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(source.Get(), &srv, cpu);
        cpu.ptr += device->GetDescriptorHandleIncrementSize(heapDesc.Type);
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav {};
        uav.Format = format.format;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(destination.Get(), nullptr, &uav, cpu);
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
        Hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "allocator");
        Hr(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pipeline,
                                     IID_PPV_ARGS(&list)), "list");
        D3D12_TEXTURE_COPY_LOCATION srcLocation {}, dstLocation {};
        srcLocation.pResource = srcUpload.Get();
        srcLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        srcLocation.PlacedFootprint = srcFoot;
        dstLocation.pResource = source.Get();
        dstLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&dstLocation, 0, 0, 0, &srcLocation, nullptr);
        srcLocation.pResource = dstUpload.Get();
        srcLocation.PlacedFootprint = dstFoot;
        dstLocation.pResource = destination.Get();
        list->CopyTextureRegion(&dstLocation, 0, 0, 0, &srcLocation, nullptr);
        Transition(list.Get(), source.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(list.Get(), destination.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ID3D12DescriptorHeap* heaps[] = {heap.Get()};
        list->SetDescriptorHeaps(1, heaps);
        list->SetComputeRootSignature(root.Get());
        list->SetComputeRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
        list->SetComputeRootConstantBufferView(1, cb->GetGPUVirtualAddress());
        // Extra groups deliberately cover destination padding on both axes.
        list->Dispatch(region.groupsX + 1, region.groupsY + 1, 1);
        Transition(list.Get(), destination.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        srcLocation.pResource = destination.Get();
        srcLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        srcLocation.SubresourceIndex = 0;
        dstLocation.pResource = readback.Get();
        dstLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dstLocation.PlacedFootprint = dstFoot;
        list->CopyTextureRegion(&dstLocation, 0, 0, 0, &srcLocation, nullptr);
        Hr(list->Close(), "close");
        ID3D12CommandList* lists[] = {list.Get()};
        queue->ExecuteCommandLists(1, lists);
        const auto value = ++sequence;
        Hr(queue->Signal(fence.Get(), value), "signal actual WARP queue");
        Hr(fence->SetEventOnCompletion(value, event), "fence event setup");
        Require(WaitForSingleObject(event, 30000) == WAIT_OBJECT_0 && fence->GetCompletedValue() >= value,
                "actual WARP queue completion");
        Hr(device->GetDeviceRemovedReason(), "device after dispatch");
        const D3D12_RANGE readRange {0, static_cast<SIZE_T>(dstBytes)};
        Hr(readback->Map(0, &readRange, reinterpret_cast<void**>(&mapped)), "readback map");
        for (UINT y = 0; y < dstH; ++y)
            for (UINT x = 0; x < dstW; ++x)
            {
                std::array<std::uint8_t, 8> expected {};
                if (invalid == 0 && x < width && y < height)
                {
                    const auto sourceY = (flip ? height - 1 - y : y) + region.offset;
                    auto sample = Sample(x, sourceY);
                    if (format.velocity && invert) sample[1] = -sample[1];
                    Store(expected.data(), format, sample[0], sample[1]);
                }
                else Store(expected.data(), format, 121.0f, -117.0f);
                const auto* actual = mapped + dstFoot.Offset + static_cast<SIZE_T>(y) * dstFoot.Footprint.RowPitch + x * format.bytes;
                if (std::memcmp(actual, expected.data(), format.bytes) != 0)
                {
                    std::cerr << "pixel mismatch case=" << cases << " format=" << format.format
                              << " size=" << width << 'x' << height << " flip=" << flip << " invert=" << invert
                              << " offsetRequested=" << offsetRequested << " invalid=" << invalid
                              << " pixel=" << x << ',' << y << '\n';
                    readback->Unmap(0, &noRead);
                    throw std::runtime_error("shader pixel/sentinel mismatch");
                }
                ++pixels;
            }
        readback->Unmap(0, &noRead);
        ++cases;
    }
};

void HostRegions()
{
    RFRegion region {};
    Require(TryRFRegion(17, 19, 23, 27, 22, 21, true, region), "host odd extent");
    Require(region.width == 17 && region.height == 19 && region.offset == 8 &&
            region.groupsX == 2 && region.groupsY == 2, "host extent/offset/groups");
    Require(TryRFRegion(17, 19, 23, 27, 22, 21, false, region) && region.offset == 0, "host vector-only offset");
    const std::array<std::array<UINT64, 6>, 10> invalid {{
        {{0, 1, 4, 4, 4, 4}}, {{1, 0, 4, 4, 4, 4}}, {{5, 1, 4, 4, 8, 8}},
        {{1, 5, 4, 4, 8, 8}}, {{5, 1, 8, 8, 4, 4}}, {{1, 5, 8, 8, 4, 4}},
        {{1, 1, 0x100000000ull, 4, 4, 4}}, {{0x100000000ull, 1, 4, 4, 4, 4}},
        {{1048561, 1, 1048561, 1, 1048561, 1}}, {{1, 1048561, 1, 1048561, 1, 1048561}}
    }};
    for (const auto& test : invalid)
    {
        region.width = 123;
        Require(!TryRFRegion(test[0], test[1], test[2], test[3], test[4], test[5], true, region), "host invalid region accepted");
        Require(region.width == 0 && region.groupsX == 0, "host rejected region left usable output");
    }
}
} // namespace

int main()
{
    try
    {
        HostRegions();
        Harness harness;
        ComPtr<ID3DBlob> compiled, messages;
        const auto result = D3DCompile(rfSource, sizeof(rfSource) - 1, "RF runtime source", nullptr, nullptr,
                                      "CSMain", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_ENABLE_STRICTNESS,
                                      0, &compiled, &messages);
        if (FAILED(result) && messages) std::cerr << static_cast<const char*>(messages->GetBufferPointer());
        Hr(result, "compile production rfCode");
        const std::array<ComPtr<ID3D12PipelineState>, 2> pipelines = {
            harness.Pipeline(compiled->GetBufferPointer(), compiled->GetBufferSize()),
            harness.Pipeline(RF_cso, sizeof(RF_cso))
        };
        const std::array<std::array<UINT, 2>, 4> sizes {{{1, 1}, {3, 5}, {17, 19}, {31, 33}}};
        for (const auto& pipeline : pipelines)
            for (const auto format : Formats)
            {
                for (const auto& size : sizes)
                    for (const bool flip : {false, true})
                        for (const bool invert : {false, true})
                            for (const bool offset : {false, true})
                                harness.Run(pipeline.Get(), format, size[0], size[1], flip, invert, offset);
                for (UINT invalid = 1; invalid <= 8; ++invalid)
                    harness.Run(pipeline.Get(), format, 17, 19, true, true, true, invalid);
            }
        std::cout << "RF WARP: " << harness.cases << " dispatch cases, " << harness.pixels
                  << " exact pixels; R32 depth, RG16/RG32 motion, source+embedded, all row/sign/offset combinations.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "RF WARP FAIL: " << error.what() << '\n';
        return 1;
    }
}

// SPDX-License-Identifier: GPL-3.0-or-later
// Standalone Windows 10 1703+ WARP test. Link d3d11.lib d3d12.lib dxgi.lib.
// Executes the production import helper, not PictureJob/PreparedDepthLease.
// No game, SDK, display ownership or model admission is exercised here.
#include <d3d11_4.h>
#include <dxgi1_4.h>
#include "../Dx11SharedRead.h"
#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using Microsoft::WRL::ComPtr;
namespace {
unsigned checks = 0;
constexpr UINT Width = 19, Height = 7;
constexpr DXGI_FORMAT Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
using Pixel = std::array<float, 4>;
using Pixels = std::array<Pixel, static_cast<size_t>(Width) * Height>;
static_assert(sizeof(Pixel) == 16);

void Check(bool good, const char* label) {
    ++checks;
    if (!good) { std::fprintf(stderr, "FAILED: %s\n", label); std::abort(); }
}
void Hr(HRESULT result, const char* label) {
    if (FAILED(result)) std::fprintf(stderr, "%s: HRESULT 0x%08lX\n", label, static_cast<unsigned long>(result));
    Check(SUCCEEDED(result), label);
}
struct Handle {
    HANDLE value = nullptr;
    ~Handle() { if (value) CloseHandle(value); }
    Handle() = default;
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    void Close() { if (value) { CloseHandle(value); value = nullptr; } }
};
bool Same(IUnknown* a, IUnknown* b) {
    ComPtr<IUnknown> x, y;
    return a && b && SUCCEEDED(a->QueryInterface(IID_PPV_ARGS(&x))) &&
        SUCCEEDED(b->QueryInterface(IID_PPV_ARGS(&y))) && x == y;
}
Pixels Pattern(unsigned pass) {
    // Exact FLOAT32 bits: signed zero, subnormals, adjacent values and HDR-range
    // values catch normalized/half/one-channel conversions. No shader arithmetic.
    constexpr std::array<std::uint32_t, 16> bits{
        0x00000000u, 0x80000000u, 0x3F800000u, 0xBF800000u,
        0x3F800001u, 0x3F7FFFFFu, 0x3EAAAAABu, 0x3F000000u,
        0x40000000u, 0xC0200000u, 0x00000001u, 0x80000001u,
        0x00800000u, 0x7F7FFFFFu, 0x41200000u, 0xC2480000u};
    Pixels pixels{};
    for (size_t i = 0; i < pixels.size(); ++i)
        for (size_t channel = 0; channel < 4; ++channel)
            pixels[i][channel] = std::bit_cast<float>(bits[(i * 5 + channel * 3 + pass * 7) % bits.size()]);
    return pixels;
}
struct Warp {
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D11Device> device11;
    ComPtr<ID3D11Device5> device5;
    ComPtr<ID3D11DeviceContext> context11;
    ComPtr<ID3D11DeviceContext4> context4;
    ComPtr<ID3D12Device> device12;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D11Fence> ready11;
    ComPtr<ID3D12Fence> ready12, done;
    Handle event;
    UINT64 timeline = 0;

    Warp() {
        Hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "create DXGI factory4");
        Hr(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)), "enumerate one WARP adapter");
        constexpr D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_0};
        Hr(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
            levels, 1, D3D11_SDK_VERSION, &device11, nullptr, &context11), "create D3D11 on selected WARP adapter");
        Hr(device11.As(&device5), "query D3D11Device5");
        Hr(context11.As(&context4), "query D3D11DeviceContext4");
        Hr(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device12)),
            "create D3D12 on the same WARP adapter");
        DXGI_ADAPTER_DESC1 adapterDesc{};
        Hr(adapter->GetDesc1(&adapterDesc), "read WARP adapter LUID");
        const auto luid = device12->GetAdapterLuid();
        Check(luid.LowPart == adapterDesc.AdapterLuid.LowPart && luid.HighPart == adapterDesc.AdapterLuid.HighPart,
            "D3D11 and D3D12 use the identical selected adapter");
        D3D12_COMMAND_QUEUE_DESC q{}; q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        Hr(device12->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)), "create direct consumer queue");
        Hr(device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&ready11)), "create real D3D11 shared ready fence");
        Handle sharedFence;
        Hr(ready11->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &sharedFence.value), "export real ready fence");
        Hr(device12->OpenSharedHandle(sharedFence.value, IID_PPV_ARGS(&ready12)), "open same ready timeline on D3D12");
        ComPtr<ID3D12Device> fenceOwner;
        Hr(ready12->GetDevice(IID_PPV_ARGS(&fenceOwner)), "read imported fence owner");
        Check(Same(fenceOwner.Get(), device12.Get()), "ready fence has exact consumer owner");
        Hr(device12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&done)), "create actual consumer done fence");
        event.value = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        Check(event.value != nullptr, "create bounded completion event");
    }
    ComPtr<ID3D11Texture2D> Texture(bool shared = true, bool shaderReadable = true) const {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = Width; d.Height = Height; d.MipLevels = d.ArraySize = d.SampleDesc.Count = 1;
        d.Format = Format; d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = shaderReadable ? D3D11_BIND_SHADER_RESOURCE : 0;
        d.MiscFlags = shared ? D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED : 0;
        ComPtr<ID3D11Texture2D> texture;
        Hr(device11->CreateTexture2D(&d, nullptr, &texture), "create fixture source texture");
        return texture;
    }
    void ReadExact(ID3D11Texture2D* source, ID3D12Resource* imported, const Pixels& expected) {
        // All allocations below are explicit test readback/command storage.
        // The production helper must alias source; it creates no pixel texture.
        const auto desc = imported->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT rows = 0; UINT64 rowBytes = 0, total = 0;
        device12->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &rows, &rowBytes, &total);
        Check(rows == Height && rowBytes == Width * sizeof(Pixel) && total != 0, "full FLOAT32x4 readback extent");
        Check(footprint.Footprint.RowPitch > rowBytes, "non-aligned width exercises row-pitch handling");
        D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_READBACK;
        heap.CreationNodeMask = heap.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC buffer{};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; buffer.Width = total;
        buffer.Height = 1; buffer.DepthOrArraySize = 1; buffer.MipLevels = 1; buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> readback;
        Hr(device12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)), "create explicit test readback buffer");
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
        Hr(device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "create readback allocator");
        Hr(device12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
            IID_PPV_ARGS(&list)), "create readback command list");
        D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = imported; barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        list->ResourceBarrier(1, &barrier);
        D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource = imported;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource = readback.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = footprint;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
        list->ResourceBarrier(1, &barrier);
        Hr(list->Close(), "close exact readback list");

        const UINT64 value = ++timeline;
        context11->UpdateSubresource(source, 0, nullptr, expected.data(), Width * static_cast<UINT>(sizeof(Pixel)), 0);
        Hr(context4->Signal(ready11.Get(), value), "real D3D11 input-ready Signal");
        context11->Flush();
        Hr(queue->Wait(ready12.Get(), value), "real D3D12 queue Wait on exact D3D11 timeline/value");
        ID3D12CommandList* commands[]{list.Get()};
        queue->ExecuteCommandLists(1, commands);
        Hr(queue->Signal(done.Get(), value), "actual consumer queue signals done after copy");
        Hr(done->SetEventOnCompletion(value, event.value), "register actual consumer completion");
        Check(WaitForSingleObject(event.value, 10000) == WAIT_OBJECT_0, "bounded wait for real consumer completion");
        const auto completed = done->GetCompletedValue();
        Check(completed != UINT64_MAX && completed >= value, "actual done fence proves readback retirement");
        Check(ready12->GetCompletedValue() != UINT64_MAX && ready12->GetCompletedValue() >= value,
            "source-ready timeline also reached the exact read");
        void* data = nullptr; const D3D12_RANGE readRange{0, static_cast<SIZE_T>(total)};
        Hr(readback->Map(0, &readRange, &data), "map only completed test readback");
        const auto* bytes = static_cast<const unsigned char*>(data) + static_cast<size_t>(footprint.Offset);
        for (UINT y = 0; y < Height; ++y)
            Check(std::memcmp(bytes + static_cast<size_t>(y) * footprint.Footprint.RowPitch,
                expected.data() + static_cast<size_t>(y) * Width, Width * sizeof(Pixel)) == 0,
                "every channel of every pixel preserves exact FLOAT32 bits");
        const D3D12_RANGE noWrites{0, 0}; readback->Unmap(0, &noWrites);
        Hr(device11->GetDeviceRemovedReason(), "D3D11 remains healthy after alias read");
        Hr(device12->GetDeviceRemovedReason(), "D3D12 remains healthy after alias read");
    }
};

void Rejections(Warp& w, FramePipeline::Dx11SharedRead& cached, ID3D11Texture2D* source,
    ID3D12Resource* resource, HANDLE validSharedHandle) {
    Check(!cached.Open(nullptr, w.device12.Get(), Width, Height, Format), "null source refuses");
    Check(!cached.Open(source, nullptr, Width, Height, Format), "null/wrong device refuses");
    Check(!cached.Open(source, w.device12.Get(), 0, Height, Format), "zero extent refuses");
    Check(!cached.Open(source, w.device12.Get(), Width + 1, Height, Format), "cached width mismatch refuses");
    Check(!cached.Open(source, w.device12.Get(), Width, Height + 1, Format), "cached height mismatch refuses");
    Check(!cached.Open(source, w.device12.Get(), Width, Height, DXGI_FORMAT_R32_FLOAT), "cached format mismatch refuses");
    const auto other = w.Texture();
    Check(!cached.Open(other.Get(), w.device12.Get(), Width, Height, Format), "same descriptor different allocation refuses cached binding");
    Check(cached.Open(source, w.device12.Get(), Width, Height, Format) == resource, "invalid requests preserve cached exact resource");
    {
        FramePipeline::Dx11SharedRead bad; const auto local = w.Texture(false);
        Check(!bad.Open(local.Get(), w.device12.Get(), Width, Height, Format, validSharedHandle), "nonshared source refuses despite unrelated valid handle");
        Check(!bad.Open(source, w.device12.Get(), Width, Height, Format, validSharedHandle), "failed immutable binding does not retry on another source");
    }
    {
        FramePipeline::Dx11SharedRead bad;
        ComPtr<IDXGIResource1> exported; Hr(other.As(&exported), "query cold mismatch shared resource");
        Handle handle; Hr(exported->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
            nullptr, &handle.value), "export cold mismatch source exactly once");
        Check(!bad.Open(other.Get(), w.device12.Get(), Width + 1, Height, Format, handle.value), "cold descriptor mismatch refuses with its real export handle");
        Check(!bad.Open(other.Get(), w.device12.Get(), Width, Height, Format, handle.value), "cold descriptor failure is remembered without retry");
    }
    // D3D12 devices are singleton per adapter. Do not call a second WARP device
    // a foreign owner. Exercise an actual distinct device if one is available.
    bool testedForeignDevice = false;
    for (UINT index = 0; ; ++index) {
        ComPtr<IDXGIAdapter1> adapter;
        const HRESULT result = w.factory->EnumAdapters1(index, &adapter);
        if (result == DXGI_ERROR_NOT_FOUND) break;
        Hr(result, "enumerate optional foreign adapter");
        ComPtr<ID3D12Device> device;
        if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))) ||
            Same(device.Get(), w.device12.Get())) continue;
        Check(!cached.Open(source, device.Get(), Width, Height, Format), "actual foreign device refuses cached binding");
        Check(cached.Open(source, w.device12.Get(), Width, Height, Format) == resource, "foreign request cannot replace original import");
        testedForeignDevice = true; break;
    }
    if (!testedForeignDevice) std::puts("NOTE: distinct-device case unavailable; null-device refusal covered (no fake second WARP owner).");
}
} // namespace

int main() {
    Warp w;
    auto source = w.Texture();
    ComPtr<IDXGIResource1> exportedSource;
    Hr(source.As(&exportedSource), "query source shared-resource interface");
    Handle sourceHandle;
    Hr(exportedSource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
        nullptr, &sourceHandle.value), "export source exactly once for both game and producer views");
    ComPtr<ID3D11Device> gameDeviceBase;
    ComPtr<ID3D11Device1> gameDevice;
    ComPtr<ID3D11DeviceContext> gameContext;
    constexpr D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_0};
    Hr(D3D11CreateDevice(w.adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
        levels, 1, D3D11_SDK_VERSION, &gameDeviceBase, nullptr, &gameContext),
        "create second game D3D11 device on the same WARP adapter");
    Hr(gameDeviceBase.As(&gameDevice), "query second game D3D11 device1");
    ComPtr<ID3D11Texture2D> gameView;
    Hr(gameDevice->OpenSharedResource1(sourceHandle.value, IID_PPV_ARGS(&gameView)),
        "open the already-exported source as the game D3D11 view");
    D3D11_TEXTURE2D_DESC gameDesc{}; gameView->GetDesc(&gameDesc);
    ComPtr<ID3D11Device> gameOwner; gameView->GetDevice(&gameOwner);
    Check(gameDesc.Width == Width && gameDesc.Height == Height && gameDesc.Format == Format &&
        Same(gameOwner.Get(), gameDeviceBase.Get()), "second D3D11 device opened exact shared texture description");
    FramePipeline::Dx11SharedRead reader;
    Check(!reader.Open(nullptr, w.device12.Get(), Width, Height, Format), "empty invalid request does not bind helper");
    auto* resource = reader.Open(source.Get(), w.device12.Get(), Width, Height, Format, sourceHandle.value);
    Check(resource != nullptr, "production helper imports real typed shared WARP texture");
    ComPtr<ID3D12Device> owner;
    Hr(resource->GetDevice(IID_PPV_ARGS(&owner)), "query actual imported resource owner");
    Check(Same(owner.Get(), w.device12.Get()), "import has exact selected consumer identity");
    const auto desc = resource->GetDesc();
    Check(desc.Format == Format && desc.Width == Width && desc.Height == Height &&
        (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS) != 0,
        "typed dimensions and simultaneous-access contract preserved");
    sourceHandle.Close();
    Check(reader.Open(source.Get(), w.device12.Get(), Width, Height, Format) == resource,
        "cached import remains available after the one exported handle is closed");
    w.ReadExact(source.Get(), resource, Pattern(0));
    auto rejectionSource = w.Texture();
    ComPtr<IDXGIResource1> rejectionExport; Hr(rejectionSource.As(&rejectionExport), "query rejection source shared resource");
    Handle rejectionHandle; Hr(rejectionExport->CreateSharedHandle(nullptr,
        DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &rejectionHandle.value),
        "export rejection source exactly once");
    Rejections(w, reader, source.Get(), resource, rejectionHandle.value);
    Check(reader.Open(source.Get(), w.device12.Get(), Width, Height, Format) == resource,
        "same cached object survives consumer completion without the exported handle");
    // No helper reopen/copy/refresh between these writes: fresh bits must be
    // visible through the old view, rather than a separately allocated snapshot.
    w.ReadExact(source.Get(), resource, Pattern(1));
    w.ReadExact(source.Get(), resource, Pattern(2));
    std::printf("Dx11SharedRead: %u checks passed; real WARP 11Signal/Flush -> 12Wait/copy/done, three exact RGBA32F writes\n", checks);
}

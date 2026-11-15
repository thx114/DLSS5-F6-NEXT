// SPDX-License-Identifier: GPL-3.0-or-later
// Included only by the isolated actual-DLL host. No production GPU readbacks.
#include "../../runtime/F6PresentationProbe.h"
#include <d3d11_1.h>
#include <vector>
#include <fstream>

namespace PresentationHost
{
using namespace F6PresentationProbe;
struct Picture { uint64_t hash; std::array<unsigned, 4> hud, center; };
unsigned serial = 0, eval = 0, writes = 0, sentinelWrites = 0;
bool sentinel = false;
bool requireStable = false, haveSourceNr = false;
const char* change = "unchanged";
Picture raw {}, filtered {};
Picture sourceNr {};

void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
                D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    D3D12_RESOURCE_BARRIER barrier {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to};
    list->ResourceBarrier(1, &barrier);
}

void WINAPI Observe(const Sample* sample)
{
    Check(sample && sample->color && sample->queue, "probe borrows actual FG color and queue");
    ComPtr<ID3D12Resource> actual;
    Hr(sample->chain->GetBuffer(sample->fgIndex, IID_PPV_ARGS(&actual)), "query actual FG buffer");
    Check(actual.Get() == sample->color, "probe observes FG buffer, not wrapper DX11 GetBuffer");
    ComPtr<ID3D12Device> device;
    Hr(sample->queue->GetDevice(IID_PPV_ARGS(&device)), "probe queue device");
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    Hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "probe allocator");
    Hr(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
        IID_PPV_ARGS(&list)), "probe list");
    const auto desc = actual->GetDesc();
    Check(desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM, "probe exact RGBA8 target");

    // A labeled test writer makes a lost publication unambiguous even if the
    // installed NR model cannot run in this synthetic scene. It is not NR proof.
    ComPtr<ID3D12DescriptorHeap> heap;
    if (sentinel && sample->stage == Stage::AfterNr && serial % 4 == 0) {
        D3D12_DESCRIPTOR_HEAP_DESC hd {}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = 1;
        Hr(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)), "sentinel RTV heap");
        const auto rtv = heap->GetCPUDescriptorHandleForHeapStart();
        device->CreateRenderTargetView(actual.Get(), nullptr, rtv);
        Transition(list.Get(), actual.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const float mark[] {0.75f, 0.125f, 0.5f, 1};
        list->ClearRenderTargetView(rtv, mark, 0, nullptr);
        Transition(list.Get(), actual.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        ++sentinelWrites;
    }
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp {}; UINT64 bytes = 0;
    device->GetCopyableFootprints(&desc, 0, 1, 0, &fp, nullptr, nullptr, &bytes);
    D3D12_HEAP_PROPERTIES hp {}; hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd {}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = bytes;
    bd.Height = bd.DepthOrArraySize = bd.MipLevels = bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> readback;
    Hr(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr, IID_PPV_ARGS(&readback)), "probe readback");
    Transition(list.Get(), actual.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION from {}, to {};
    from.pResource = actual.Get(); from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    to.pResource = readback.Get(); to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; to.PlacedFootprint = fp;
    list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    Transition(list.Get(), actual.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
    Hr(list->Close(), "close probe");
    if (sample->fence) Hr(sample->queue->Wait(sample->fence, sample->value), "order exact raw-copy producer");
    ID3D12CommandList* batch[] {list.Get()}; sample->queue->ExecuteCommandLists(1, batch);
    ComPtr<ID3D12Fence> fence; Hr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "probe fence");
    Hr(sample->queue->Signal(fence.Get(), 1), "signal probe's own real queue");
    const auto event = CreateEventW(nullptr, FALSE, FALSE, nullptr); Check(event != nullptr, "probe event");
    Hr(fence->SetEventOnCompletion(1, event), "probe event registration");
    const auto waited = WaitForSingleObject(event, 30000); CloseHandle(event);
    Check(waited == WAIT_OBJECT_0, "probe own actual queue completed");
    void* data = nullptr; const D3D12_RANGE range {0, size_t(bytes)};
    Hr(readback->Map(0, &range, &data), "map FG input readback");
    const auto* pixels = static_cast<const unsigned char*>(data) + fp.Offset;
    Picture picture {14695981039346656037ull, {}, {}};
    for (UINT y = 0; y < desc.Height; ++y)
        for (UINT x = 0; x < UINT(desc.Width) * 4; ++x) {
            picture.hash ^= pixels[size_t(y) * fp.Footprint.RowPitch + x]; picture.hash *= 1099511628211ull;
        }
    for (unsigned c = 0; c < 4; ++c) {
        picture.hud[c] = pixels[32ull * fp.Footprint.RowPitch + 32 * 4 + c];
        picture.center[c] = pixels[size_t(desc.Height / 2) * fp.Footprint.RowPitch + (desc.Width / 2) * 4 + c];
    }
    const D3D12_RANGE none {0, 0}; readback->Unmap(0, &none);
    if (sample->stage == Stage::RawCopy) raw = picture;
    if (sample->stage == Stage::AfterNr) { filtered = picture; writes += sample->nrResult == 1; }
    if (sample->stage == Stage::FgInput) {
        // Opti's own composition is between these observations. Preserve both
        // hashes; an overlay delta is not an NR publication failure.
        if (serial % 4 == 0) {
            haveSourceNr = sentinel || sample->nrResult == 1;
            sourceNr = picture;
        }
        if (requireStable && serial % 4 == 1 && haveSourceNr)
            Check(picture.center == sourceNr.center, "unchanged second Present must retain this source's completed NR pixels");
        if (eval == 13) Check(sample->nrResult != 1 && std::abs(int(picture.center[0]) - 64) <= 1 &&
            picture.center[1] == 0 && std::abs(int(picture.center[2]) - 128) <= 1,
            "invalid B keeps its current raw scene, never A's NR");
        if (serial % 4 && sentinel) Check(picture.hash == raw.hash, "repeated Present currently publishes fresh raw color");
    }
    std::printf("sample eval=%u present=%u change=%s stage=%u dx11=%u fg=%u color=%p queue=%p fence=%p:%llu nr=%u hash=%016llx hud=%u,%u,%u center=%u,%u,%u\n",
        eval, serial, change, unsigned(sample->stage), sample->dx11Index, sample->fgIndex, sample->color,
        sample->queue, sample->fence, static_cast<unsigned long long>(sample->value), sample->nrResult,
        static_cast<unsigned long long>(picture.hash), picture.hud[0], picture.hud[1], picture.hud[2],
        picture.center[0], picture.center[1], picture.center[2]);
}

void Run(HMODULE module, bool useSentinel, bool stable)
{
    sentinel = useSentinel;
    requireStable = stable;
    std::printf("probe require_stable=%u sentinel=%u\n", requireStable ? 1u : 0u, sentinel ? 1u : 0u);
    Entry<Install>(module, "F6_Test_SetPresentationProbe")(&Observe);
    const auto createFactory = Entry<decltype(&CreateDXGIFactory1)>(module, "CreateDXGIFactory1");
    ComPtr<IDXGIFactory2> factory;
    const auto deadline = GetTickCount64() + 10000;
    for (;;) {
        factory.Reset(); Hr(createFactory(IID_PPV_ARGS(&factory)), "real F6 factory");
        std::ifstream log("OptiScaler.log"); std::string line; bool ready = false;
        while (std::getline(log, line)) if (line.find("hkCreateDXGIFactory1 Caller:") != std::string::npos) ready = true;
        if (ready) break;
        Check(GetTickCount64() < deadline, "F6 factory installation completed"); Sleep(20);
    }
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
    Hr(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
        &device, nullptr, &context), "real DX11 device");
    const auto hwnd = CreateWindowExW(0, L"STATIC", L"F6 isolated presentation fixture", WS_POPUP,
        0, 0, 1024, 1024, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(hwnd != nullptr, "isolated hidden window");
    DXGI_SWAP_CHAIN_DESC1 desc {}; desc.Width = desc.Height = 1024; desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = desc.BufferCount = 1; desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT | DXGI_USAGE_SHADER_INPUT;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    ComPtr<IDXGISwapChain1> chain;
    Hr(factory->CreateSwapChainForHwnd(device.Get(), hwnd, &desc, nullptr, nullptr, &chain), "actual ForHwnd");
    MEMORY_BASIC_INFORMATION info {};
    auto** table = *reinterpret_cast<void***>(chain.Get());
    Check(VirtualQuery(table[7], &info, sizeof(info)) && info.AllocationBase == module, "actual F6 wrapper identity");
    ComPtr<ID3D11Texture2D> back; Hr(chain->GetBuffer(0, IID_PPV_ARGS(&back)), "game's actual DX11 buffer");
    ComPtr<ID3D11RenderTargetView> rtv; Hr(device->CreateRenderTargetView(back.Get(), nullptr, &rtv), "game RTV");
    ComPtr<ID3D11DeviceContext1> context1; Hr(context.As(&context1), "HUD-only rectangle writer");
    using Init = NVSDK_NGX_Result (NVSDK_CONV*)(unsigned long long, const wchar_t*, ID3D11Device*, NVSDK_NGX_Version, const NVSDK_NGX_FeatureCommonInfo*);
    Ngx(Entry<Init>(module, "NVSDK_NGX_D3D11_Init_Ext")(0x1337, L".", device.Get(), NVSDK_NGX_Version_API, nullptr), "real NGX init");
    NVSDK_NGX_Parameter* p = nullptr;
    Ngx(Entry<decltype(&NVSDK_NGX_D3D11_AllocateParameters)>(module, "NVSDK_NGX_D3D11_AllocateParameters")(&p), "allocate NGX");
    Frame frame(device.Get(), 512, true);
    p->Set(NVSDK_NGX_Parameter_Width, 512u); p->Set(NVSDK_NGX_Parameter_Height, 512u);
    p->Set(NVSDK_NGX_Parameter_OutWidth, 1024u); p->Set(NVSDK_NGX_Parameter_OutHeight, 1024u);
    p->Set(NVSDK_NGX_Parameter_PerfQualityValue, NVSDK_NGX_PerfQuality_Value_MaxPerf);
    p->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, int(NVSDK_NGX_DLSS_Feature_Flags_AutoExposure |
        NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_DepthInverted));
    NVSDK_NGX_Handle* handle = nullptr;
    Ngx(Entry<decltype(&NVSDK_NGX_D3D11_CreateFeature)>(module, "NVSDK_NGX_D3D11_CreateFeature")(
        context.Get(), NVSDK_NGX_Feature_SuperSampling, p, &handle), "real SR feature");
    const auto evaluate = Entry<F6EvaluateV1>(module, k_f6_evaluate_v1);
    // Several true sources allow an asynchronous NR model to become ready.
    // Each source then has four Presents: identical, identical, HUD-only, scene.
    for (eval = 1; eval <= 12; ++eval) {
        frame.Fill(context.Get(), {128u + eval * 32u, 512u, 800u}); frame.Bind(p);
        F6EvalReceiptV1 receipt;
        Ngx(evaluate(context.Get(), handle, p, nullptr, eval, &receipt), "real current SR Evaluate");
        Check(f6_output_status(receipt, eval, handle, context.Get(), frame.output.Get()) == F6OutputStatus::current_output_ordered,
            "current source receipt before Present");
        context->CopyResource(back.Get(), frame.output.Get());
        for (unsigned n = 0; n < 4; ++n) {
            serial = (eval - 1) * 4 + n;
            change = n < 2 ? "unchanged" : n == 2 ? "hud-only" : "whole-scene";
            if (n == 2) { const D3D11_RECT rect {0,0,64,64}; const float hud[] {0,1,0,1}; context1->ClearView(rtv.Get(), hud, &rect, 1); }
            if (n == 3) { const float scene[] {0,0,1,1}; context->ClearRenderTargetView(rtv.Get(), scene); }
            const DXGI_PRESENT_PARAMETERS parameters {};
            Hr(n % 2 ? chain->Present1(0, 0, &parameters) : chain->Present(0, 0), "actual wrapper Present/Present1");
        }
    }
    // A failed attempt must not authorize reuse of A; a later successful C
    // must be observed independently. The backbuffer is deliberately changed.
    eval = 13;
    frame.Fill(context.Get(), {128, 256, 512}); frame.Bind(p);
    p->Set(NVSDK_NGX_Parameter_Color, static_cast<ID3D11Resource*>(nullptr));
    F6EvalReceiptV1 failed;
    const auto failure = evaluate(context.Get(), handle, p, nullptr, eval, &failed);
    std::printf("invalid-B result=%08x status=%u\n", unsigned(failure), unsigned(failed.status));
    Check(failed.status != F6OutputStatus::current_output_ordered, "invalid B has no authoritative current output receipt");
    const float mark[] {0.25f,0,0.5f,1}; context->ClearRenderTargetView(rtv.Get(), mark);
    change = "failed-B";
    for (unsigned n = 0; n < 2; ++n) {
        serial = 48 + n; Hr(chain->Present(0, 0), "raw B Present after failed Evaluate");
    }
    eval = 14;
    frame.Fill(context.Get(), {704, 128, 256}); frame.Bind(p);
    F6EvalReceiptV1 recovered;
    Ngx(evaluate(context.Get(), handle, p, nullptr, eval, &recovered), "successful C after failed B");
    Check(recovered.status == F6OutputStatus::current_output_ordered, "successful C publishes its own current output");
    context->CopyResource(back.Get(), frame.output.Get()); change = "recovered-C";
    for (unsigned n = 0; n < 2; ++n) {
        serial = 52 + n; Hr(chain->Present(0, 0), "C Present after recovery");
    }
    Check(sentinel ? sentinelWrites != 0 : writes != 0, "test actually observed its declared NR or sentinel writer");
    std::printf("PASS actual presentation observation checks=%u modelWrites=%u testWriter=%u generatedPixels=unknown\n", checks, writes, sentinelWrites);
    std::puts("LIMIT: SR token cannot distinguish unchanged, HUD-only and whole-scene writes; this host observes the gap and does not authorize replay.");
}
}

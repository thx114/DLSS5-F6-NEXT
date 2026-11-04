#include "pch.h"
#include <integration/HoYoShade.h>
#if defined(_MSC_VER)
#pragma execution_character_set("utf-8")
#endif

#include "NativeDriverDx12.h"

#include <native/Dx12FrameSource.h>
#include <native/NativeDriver.h>

#include <Config.h>

#include <menu/menu_overlay_dx.h>
#include <resource_tracking/GenericDepth_Dx12.h>

#include <imgui/imgui.h>
#include <menu/UiLocalization.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>

namespace
{

native::NativeDriver g_driver("Native motion", "");
native::Dx12FrameSource g_source;

// Under frame generation the menu's present hook sees the real swap chain after frame generation ran, on its presenter's
// thread and queue; FGPresent drives the step instead. The menu's hook stands aside while FGPresent ran within this many of
// its presents (frame generation presents several frames for each of the game's).
constexpr uint64_t kFgPresentGrace = 8;
std::atomic<uint64_t> g_menuPresents { 0 };
std::atomic<uint64_t> g_menuPresentsAtFg { 0 };
std::atomic<bool> g_fgDriven { false };

// The two hooks run on different threads while frame generation starts up. The menu's only tries it: its thread is frame
// generation's presenter, which must never wait on the game thread.
std::mutex g_runMutex;

bool g_previewWanted = false; // the menu node is open

// The menu's descriptors for the two preview pictures.
struct PreviewView
{
    bool allocated = false;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu {};
    D3D12_GPU_DESCRIPTOR_HANDLE gpu {};
    ID3D12Resource* resource = nullptr;
};

ID3D12DescriptorHeap* g_srvHeap = nullptr;
PreviewView g_flowView;
PreviewView g_maskView;

// Shows a texture in the menu: a descriptor in the menu's heap (made once, and again if the heap or the texture changes).
// `grayFromRed` shows a one-channel texture as gray.
bool ShowTexture(PreviewView& view, ID3D12Resource* texture, DXGI_FORMAT format, bool grayFromRed, float width,
                 float height)
{
    ID3D12DescriptorHeap* heap = MenuOverlayDx::SrvHeap();

    ID3D12Device* device = g_driver.Producer() != nullptr ? g_driver.Producer()->Device() : nullptr;

    if (heap == nullptr || texture == nullptr || device == nullptr)
        return false;

    if (heap != g_srvHeap)
    {
        g_srvHeap = heap;
        g_flowView = PreviewView {};
        g_maskView = PreviewView {};
    }

    if (!view.allocated && MenuOverlayDx::AllocSrv(&view.cpu, &view.gpu))
        view.allocated = true;

    if (!view.allocated)
        return false;

    if (texture != view.resource)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv {};
        srv.Format = format;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping =
            grayFromRed ? D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(
                              D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0,
                              D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0,
                              D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0,
                              D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1)
                        : D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(texture, &srv, view.cpu);
        view.resource = texture;
    }

    ImGui::Image((ImTextureID) view.gpu.ptr, ImVec2(width, height));
    return true;
}

HoYoShade::ConsumerAccess RunFrame(IDXGISwapChain* swapChain, ID3D12CommandQueue* queue, ID3D12Device* device)
{
    // Enabled always executes the historical off-transition cleanup, including
    // the default HoYo=false + NativeMotion=false path (HYS-03).
    const bool requested = Config::Instance()->DlssNrNativeMotion.value_or_default() ||
        HoYoShade::Selected(HoYoShade::Stage::BeforeNr);
    if (!HoYoShade::AdmitNativeFrame(requested, [] { return g_driver.Enabled(); }))
        return HoYoShade::ColorConsumers(swapChain);
    if (HoYoShade::ColorConsumers(swapChain) != HoYoShade::ConsumerAccess::Safe)
        return HoYoShade::ColorConsumers(swapChain);
    if (swapChain == nullptr || queue == nullptr || device == nullptr || g_driver.Failed())
        return HoYoShade::ColorConsumers(swapChain);

    g_source.SetPresent(swapChain, queue, device);
    const auto result = g_driver.RunFrame(g_source, g_previewWanted);
    if (result.ran) g_previewWanted = false; // UI bookkeeping, NEVER a safety proof
    if (result.colorUncertain) return HoYoShade::ConsumerAccess::Quarantined;
    return HoYoShade::ColorConsumers(swapChain);
}

} // namespace

namespace NativeMotionDx12
{

HoYoShade::ConsumerAccess OnPresent(IDXGISwapChain* swapChain, ID3D12CommandQueue* queue, ID3D12Device* device)
{
    const auto access = HoYoShade::ColorConsumers(swapChain);
    if (access != HoYoShade::ConsumerAccess::Safe) return access;
    if (State::Instance().swapchainInteropApi != SwapchainInteropApi::None) return access;
    const uint64_t presents = ++g_menuPresents;
    std::unique_lock lock(g_runMutex, std::try_to_lock);
    // This mutex also covers menu/cleanup/other chains. Contention alone must
    // not fail Present (or disable a user's FG/SR provider). Only the exact
    // active color writer or persistent unknown endpoint denies consumers.
    if (!lock.owns_lock()) return HoYoShade::ColorConsumers(swapChain);
    if (g_fgDriven.load() && presents - g_menuPresentsAtFg.load() <= kFgPresentGrace)
        return HoYoShade::ColorConsumers(swapChain);
    g_fgDriven = false;
    GenericDepthDx12::OnPresent(swapChain);
    return RunFrame(swapChain, queue, device);
}

HoYoShade::ConsumerAccess OnFGPresent(IDXGISwapChain* swapChain, ID3D12CommandQueue* queue, ID3D12Device* device)
{
    const auto access = HoYoShade::ColorConsumers(swapChain);
    if (access != HoYoShade::ConsumerAccess::Safe) return access;
    g_menuPresentsAtFg = g_menuPresents.load(); g_fgDriven = true;
    std::lock_guard lock(g_runMutex);
    GenericDepthDx12::OnPresent(swapChain);
    return RunFrame(swapChain, queue, device);
}

void DrawStatus()
{
    g_driver.DrawStatus();
}

bool NrOnlyRunning() { return g_driver.NrOnlyRunning(); }

void DrawAdvancedUi()
{
    const bool debugView = Config::Instance()->DlssNrNativeDebugView.value_or_default();

    g_driver.DrawFlowTuning();

    // The pictures and their controls are debugging aids: off by default, nothing is recorded for them while they are hidden.
    if (!debugView)
        return;

    const float boxWidth = 360.0f;
    const float boxHeight = boxWidth * 9.0f / 16.0f;
    bool drawn = false;

    MenuText::TextDisabled("运动：色相表示方向，亮度表示速度。");

    if (g_driver.GetStatus() == native::NativeDriver::Status::Running)
    {
        g_previewWanted = true;

        if (g_driver.Producer() && g_driver.Producer()->PreviewReady())
            drawn = ShowTexture(g_flowView, g_driver.Producer()->Flow()->Preview(), DXGI_FORMAT_R8G8B8A8_UNORM, false, boxWidth, boxHeight);
    }

    if (!drawn)
        ImGui::Dummy(ImVec2(boxWidth, boxHeight));

    // A frame can have no mask (no motion estimate yet); the picture then keeps the last one rather than going black.
    const bool trustRecent = g_driver.TrustRecent();

    if (g_driver.GetStatus() == native::NativeDriver::Status::Running)
    {
        if (trustRecent)
            MenuText::TextDisabled("可信度：白色区域表示上一帧不可靠（发现 %llu 次镜头切换）。",
                                (unsigned long long) g_driver.Cuts());
        else
            MenuText::TextDisabled("可信度：等待运动估计。");
    }
    else
        MenuText::TextDisabled("可信度：暂无");

    g_driver.DrawTrustViewCombo();

    bool maskDrawn = false;

    if (g_driver.GetStatus() == native::NativeDriver::Status::Running && trustRecent && g_driver.Producer())
        maskDrawn = ShowTexture(g_maskView, g_driver.Producer()->Trust()->Mask(), DXGI_FORMAT_R8_UNORM, true, boxWidth, boxHeight);

    if (!maskDrawn)
        ImGui::Dummy(ImVec2(boxWidth, boxHeight));
}

} // namespace NativeMotionDx12

#include "pch.h"
#include <native/OpticalFgNrPolicy.h>
#include <integration/HoYoShade.h>
#if defined(_MSC_VER)
#pragma execution_character_set("utf-8")
#endif

#include "NativeDriverDx11.h"

#include <native/Dx11FrameSource.h>
#include <native/NativeDriver.h>
#include <native/VkPresentBridge.h>

#include <Config.h>
#include <misc/IdentifyGpu.h>
#include <resource_tracking/GenericDepth_Dx11.h>

#include <imgui/imgui.h>
#include <menu/UiLocalization.h>

// The D3D11 counterpart of native/NativeDriverDx12.cpp: the same shared frame step (native::NativeDriver), a different
// adapter (native::Dx11FrameSource, the shared-texture transport). No live flow/trust picture here yet: they are D3D12
// textures and the D3D11 game's menu renders with ImGui_ImplDX11, which needs a D3D11 shader-resource view; showing
// them would need another shared-texture round trip, not done for Story B's first game. The status line and the flow
// tuning work the same as the D3D12 driver's.

namespace
{

native::NativeDriver g_driver("Native motion (D3D11)",
                              ", on a private D3D12 device paired with the game's D3D11 device");
native::Dx11FrameSource g_source;

// The native guides are persistent and DLSS-G normally borrows their pointers.
// Complete the preceding FG queue before this producer overwrites/replaces them.
// This conservative experimental owner trades overlap for an actual completion proof.
bool CompletePreviousOpticalFg()
{
    if (!native::OpticalFgNrEnabled()) return true;
    auto* fg = State::Instance().currentFG;
    auto* queue = fg ? fg->GetCommandQueue() : nullptr;
    if (!queue) return true; // no FG consumer has been made yet
    struct Completion
    {
        Microsoft::WRL::ComPtr<ID3D12Device> device;
        Microsoft::WRL::ComPtr<ID3D12Fence> fence;
        Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
        HANDLE event = nullptr;
        UINT64 value = 0;
    };
    static auto* c = new Completion; // no unfenced process-exit COM destruction
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    if (FAILED(queue->GetDevice(IID_PPV_ARGS(&device)))) return false;
    if (c->device && c->device.Get() != device.Get()) return false;
    if (!c->fence)
    {
        c->device = device;
        if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&c->fence)))) return false;
        c->event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!c->event) return false;
    }
    c->queue = queue;
    const auto value = ++c->value;
    if (FAILED(queue->Signal(c->fence.Get(), value))) return false;
    const auto done = c->fence->GetCompletedValue();
    if (done == UINT64_MAX) return false;
    if (done >= value) return true;
    if (FAILED(c->fence->SetEventOnCompletion(value, c->event))) return false;
    const double begin = Util::MillisecondsNow();
    const bool finished = WaitForSingleObject(c->event, 5000) == WAIT_OBJECT_0 &&
                          c->fence->GetCompletedValue() != UINT64_MAX && c->fence->GetCompletedValue() >= value;
    const double waited = Util::MillisecondsNow() - begin;
    if (waited > 2.0 || !finished)
        LOG_INFO("Optical FG/NR previous-consumer wait: {:.3f} ms, completed {}", waited, finished);
    return finished;
}


// Shared by OnPresent and OnFGPresent; the depth finder's frame close is the caller's job (see the two entry points
// below), since only one of them needs to do it. Returns the D3D12 picture the producer ended up with this frame, or
// null when nothing ran.
ID3D12Resource* RunFrame(IDXGISwapChain* swapChain, ID3D11Device* device, bool copyBack)
{
    if (taa::OwnsInput()) return nullptr;
    // The FG bridge's private input is not the native final-original output.
    // Color-only requests belong to the real native-B copyback route.
    if (!copyBack &&
        !Config::Instance()->DlssNrNativeMotion.value_or_default()) return nullptr;
    if (!g_driver.Enabled())
        return nullptr;

    if (swapChain == nullptr || device == nullptr || g_driver.Failed())
        return nullptr;

    if (!CompletePreviousOpticalFg())
    {
        Config::Instance()->FGEnabled.set_volatile_value(false);
        LOG_ERROR("Optical FG/NR: previous consumer did not complete; FG disabled, restart required");
        return nullptr;
    }
    g_source.SetPresent(swapChain, device, copyBack);

    // The consumer is inert (same as native input and Finished Picture) while a D3D11 game's swap chain has been
    // replaced by Dx11wDx12SC for frame generation -- see OnFGPresent for that path.
    return g_driver.RunFrame(g_source, false).submitted ? g_source.ProcessedPicture() : nullptr;
}

} // namespace

namespace NativeMotionDx11
{

void OnPresent(IDXGISwapChain* swapChain, ID3D11Device* device)
{
    if (VkPresentBridge::DxvkThroughVulkan()) return;
    RunFrame(swapChain, device, true);
}

ID3D12Resource* OnFGPresent(IDXGISwapChain* real, ID3D11Device* device)
{
    GenericDepthDx11::OnPresent(real);
    return RunFrame(real, device, false);
}

void DrawStatus() { g_driver.DrawStatus(); }

bool NrOnlyRunning() { return g_driver.NrOnlyRunning(); }

void DrawAdvancedUi()
{
    g_driver.DrawFlowTuning();

    if (!Config::Instance()->DlssNrNativeDebugView.value_or_default() ||
        g_driver.GetStatus() != native::NativeDriver::Status::Running)
        return;

    MenuText::TextDisabled("运行中（%llu 帧）。可信度：%s（发现 %llu 次镜头切换）。", (unsigned long long) g_driver.Frame(),
                        g_driver.TrustRecent() ? "running" : "waiting for the motion estimate",
                        (unsigned long long) g_driver.Cuts());
}

} // namespace NativeMotionDx11

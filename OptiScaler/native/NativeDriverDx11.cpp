#include "pch.h"
#include <native/OpticalFgNrPolicy.h>
#include <integration/HoYoShade.h>
#if defined(_MSC_VER)
#pragma execution_character_set("utf-8")
#endif

#include "NativeDriverDx11.h"
#include <genshin/GenshinLegacyCapture.h>
#include <inputs/FG/GenshinBridge_Present_Dx12.h>

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
GenshinPresentNr::History g_presentedHistory;
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
ID3D12Resource* RunFrame(IDXGISwapChain* swapChain, ID3D11Device* device, bool copyBack,
    const GenshinBridgeFg::SenderInput* fgSender=nullptr)
{
    // The FG bridge's private input is not the native final-original output.
    // Color-only requests belong to the real native-B copyback route.
    if (!copyBack && !GenshinPresentNr::Scope::Current() &&
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
    return g_driver.RunFrame(g_source, false, fgSender).submitted ? g_source.ProcessedPicture() : nullptr;
}

} // namespace

namespace NativeMotionDx11
{

void OnPresent(IDXGISwapChain* swapChain, ID3D11Device* device)
{
    // The real native parent menu still runs, but its producer must not run a
    // second time after OnFGPresent consumed this exact picture.
    if(auto* p=GenshinBridgeFg::PresentScope::Current();p && p->NativeWasProcessed(swapChain,device))return;
    // DlssNrNativeDxvkVulkan: on a dxvk game this driver's shared-texture bridge stands aside for the Vulkan one
    // (NativeDriverVk.cpp), which reads dxvk's own Vulkan calls instead. OnFGPresent needs no such check: with the key on
    // a dxvk game never gets Dx11wDx12SC (hooks/DxgiFactory_Hooks.cpp); its frame generation is the Vulkan present bridge.
    if (VkPresentBridge::DxvkThroughVulkan())
        return;

    auto ticket=GenshinLegacy::TakePresentNr(swapChain,device);
    if(ticket.Known()) {
        auto& history=g_presentedHistory;
        const bool hadHistory=history.known;
        const auto sourceStep=hadHistory && ticket.source.sourceFrame>=history.previous.sourceFrame
            ? ticket.source.sourceFrame-history.previous.sourceFrame:UINT64_MAX;
        const auto presentStep=hadHistory && ticket.presentEpoch>=history.presentEpoch
            ? ticket.presentEpoch-history.presentEpoch:UINT64_MAX;
        const bool reset=history.Begin(ticket);
        GenshinSubstage::Scope progress(ticket.source.progress);
        GenshinPresentNr::Scope presented(std::move(ticket),reset,history.flowReset);
        RunFrame(swapChain,device,true);
        static uint64_t attempts=0,resets=0,resetStreak=0,noModelStreak=0,consumerEntries=0,modelRuns=0;
        const auto count=++attempts;resets+=reset?1:0;resetStreak=reset?resetStreak+1:0;
        noModelStreak=presented.consumerRan?0:noModelStreak+1;consumerEntries+=presented.consumerReached?1:0;modelRuns+=presented.consumerRan?1:0;
        if(count<=3 || count%300==0 || resetStreak==30 || noModelStreak==120 ||
            (presented.consumerReached && consumerEntries<=3) || (presented.consumerRan && modelRuns<=3))
            LOG_INFO("Genshin presented NR: sourceFrame={} generation={} pictureSerial={} presentEpoch={} swapchain={} srDestination={} backbuffer={} modelRan={} screenCopyOrdered={} completionFence={} completionValue={} reset={} flowReset={} attempts={} sourceStep={} presentStep={} historyKnown={} resetCount={} resetStreak={} consumerReached={} consumerEntries={} noModelStreak={} sr={}x{} picture={}x{}",
                presented.ticket.source.sourceFrame,presented.ticket.source.generation,presented.ticket.pictureSerial,presented.ticket.presentEpoch,
                presented.ticket.swapchain,presented.ticket.source.selectedOutput,GenshinPresentNr::Identity(presented.ticket.backbuffer.Get()),
                presented.consumerRan,presented.screenCopyOrdered,reinterpret_cast<uintptr_t>(presented.completionFence),presented.completionValue,reset,presented.flowReset,
                count,sourceStep,presentStep,hadHistory,resets,resetStreak,presented.consumerReached,consumerEntries,noModelStreak,
                presented.ticket.source.width,presented.ticket.source.height,presented.ticket.pictureWidth,presented.ticket.pictureHeight);
        return;
    }
    RunFrame(swapChain, device, true);
}

ID3D12Resource* OnFGPresent(IDXGISwapChain* real, ID3D11Device* device)
{
    // The one call site that closes the D3D11 depth finder's frame on this path; MenuOverlayDx::Present, which frame
    // generation's present still reaches, skips its own close under Dx11wDx12SC so this frame is not closed twice.
    GenericDepthDx11::OnPresent(real);
    if(auto* wrapper=GenshinBridgeFg::PresentScope::Current();wrapper && wrapper->MatchesNative(real,device)) {
        // V3-R041: TakePresentFg seals/prepares the next motion pair before
        // RunFrame can overwrite a native retention slot. Retire only this
        // producer's positively completed input roots on its actual owner.
        if (auto* producer=g_driver.Producer()) producer->CollectCompletedMotionReferences();
        auto sender=GenshinLegacy::TakePresentFg(real,device);
        if(wrapper->RepeatsPresentedPicture()) {
            // Acquisition already established that no native draw returned.
            // Do not retake the consumed ticket or run a generic native producer
            // over the same picture. The wrapper checks its existing HWND lease.
            wrapper->NativeProcessed();return nullptr;
        }
        const auto nativeNrFallback=[&]() {
            // Use the acquired ordinary ticket exactly once. Neither FG depth,
            // producer/camera readiness nor BindFgSender grants this authority.
            if(sender.ticket.Known()) {
                if(!GenshinPresentNr::MatchesNativeParentTicket(sender.ticket))return;
                const bool reset=g_presentedHistory.Begin(sender.ticket);
                GenshinSubstage::Scope progress(sender.ticket.source.progress);
                GenshinPresentNr::Scope presented(sender.ticket,reset,g_presentedHistory.flowReset);
                RunFrame(real,device,true);
            } else if(HoYoShade::Selected(HoYoShade::Stage::BeforeNr) ||
                      HoYoShade::Selected(HoYoShade::Stage::FinalOutput)) {
                // These independent natural-color consumers retain their exact
                // existing fallback. An empty FG offer alone is not a request
                // to re-enter the generic native-motion driver without a ticket:
                // Enabled/WaitingForUpscaler would reset this same producer's
                // authenticated flow/trust history during an ordinary input gap.
                OnPresent(real,device);
            }
        };
        if(!sender.Known() || !sender.camera || !sender.camera->Matches(sender.ticket)) {
            nativeNrFallback();
            wrapper->NativeProcessed();
            return nullptr;
        }
        static GenshinPresentNr::History fgOriginalHistory;
        // The NR history belongs to the native picture, regardless of whether
        // this frame also obtains an optional FG offer. Original/NoWrite has its
        // own history and never reports a model run to the accepted NR history.
        auto& history=sender.pictureKind==GenshinPresentNr::FgPictureKind::NrWritten
            ?g_presentedHistory:fgOriginalHistory;
        const bool reset=history.Begin(sender.ticket);
        GenshinSubstage::Scope progress(sender.ticket.source.progress);
        GenshinPresentNr::Scope presented(sender.ticket,reset,history.flowReset);
        if(!presented.BindFgSender(sender.producer,sender.provider,sender.providerSwapchain,sender.pictureKind)) {
            if(GenshinPresentNr::MatchesNativeParentTicket(sender.ticket))RunFrame(real,device,true);
            wrapper->NativeProcessed();return nullptr;
        }
        // This exact route returns to native B BEFORE the wrapper records its
        // native-B -> shared copy. Other games retain the original late path.
        RunFrame(real,device,true,&sender);
        wrapper->NativeProcessed();
        return nullptr;
    }
    // The bridge copies the D3D12 result into frame generation's back buffer itself: no copy back to D3D11
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

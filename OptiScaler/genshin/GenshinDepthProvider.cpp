// SPDX-License-Identifier: GPL-3.0-or-later
#include "GenshinDepthProvider.h"
#include "GenshinNativeMrtContract.h"
#include <Util.h>
#include <Logger.h>
#include <imgui/imgui.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>

namespace GenshinDepthProvider {
namespace {
struct Storage {
    std::mutex mutex;
    detail::RawPreviewRing ring;
    std::atomic<uint64_t> wantedUntil{0};
    const char* waiting="Waiting for a claimed producer depth.";
};
// Publish the REAL process-lifetime owner before any request or ring operation.
// A null registry is the never-instantiated state, not an absent cache slot.
std::atomic<Storage*> observationOwner{nullptr};
// Bounded process lifetime: uncertain GPU work is retained through DLL teardown.
Storage& State(){static auto* s=[]{
    auto* value=new Storage;observationOwner.store(value,std::memory_order_release);return value;
}();return *s;}
void ObservationUncertain()noexcept {
    if(auto* s=observationOwner.load(std::memory_order_acquire))s->ring.MarkObservationUnknown();
}
thread_local ID3D11DeviceContext* menuContext=nullptr;
thread_local bool menuRequested=false;
bool Genshin() {return GenshinFsr::IsGenshinExecutable(Util::ExePath().wstring());}
void NoteCopy(const GenshinLegacy::NativeMrt::Frame& frame,bool accepted,const char* reason) noexcept {
    static unsigned successes=0,failures=0;
    auto& count=accepted?successes:failures;
    if(count>=4)return;
    ++count;
    try {LOG_INFO("R686 raw depth copy: pid={} generation={} sourceFrame={} ownerEpoch={} result={} reason={} convention=UNKNOWN preview_only=true",
        GetCurrentProcessId(),frame.generation,frame.frame,frame.ownerEpoch,accepted?"RECORDED":"REFUSED",reason);}catch(...){}
}
}

ObservationDemand ReadObservationDemand() noexcept {
    auto* s=observationOwner.load(std::memory_order_acquire);
    if(!s)return {true,false,false,false}; // No actual owner/request has ever been published.
    const auto request=s->wantedUntil.load(std::memory_order_acquire);
    auto result=s->ring.ReadOwnershipDemand();
    if(request!=s->wantedUntil.load(std::memory_order_acquire)){result.known=false;return result;}
    // The existing UI publishes this BEFORE trying the owner lock/Acquire.
    // Expiry never retires a copy, an ImGui lease or a recorded GPU reader.
    result.requested=request && GetTickCount64()<=request;
    result.draining=!result.requested && result.pending;
    return result;
}

void CaptureClaimed(ID3D11DeviceContext* context,const GenshinLegacy::NativeMrt::Frame& frame,
                    ID3D11ShaderResourceView* view) noexcept {
    try {
        auto& state=State();std::unique_lock lock(state.mutex,std::try_to_lock);if(!lock.owns_lock())return;
        state.ring.Poll();
        const auto wantedUntil=state.wantedUntil.load();
        if(!Genshin() || !wantedUntil || GetTickCount64()>wantedUntil)return;
        state.waiting="Producer identity or layout does not match.";
        struct Outcome {
            Storage& state;const GenshinLegacy::NativeMrt::Frame& frame;bool accepted=false;
            ~Outcome(){NoteCopy(frame,accepted,state.waiting);}
        } outcome{state,frame};
        if(!context || !view ||
           frame.thread!=GetCurrentThreadId() || reinterpret_cast<uintptr_t>(context)!=frame.context ||
           GenshinLegacy::NativeMrt::Validate(frame)!=GenshinLegacy::NativeMrt::Reason::Ready)return;
        const auto& depth=frame.inputs[1];
        if(reinterpret_cast<uintptr_t>(view)!=depth.view)return;
        D3D11_SHADER_RESOURCE_VIEW_DESC desc{};view->GetDesc(&desc);
        if(desc.ViewDimension!=D3D11_SRV_DIMENSION_TEXTURE2D || desc.Format!=depth.viewFormat ||
           desc.Texture2D.MostDetailedMip!=depth.firstMip || desc.Texture2D.MipLevels!=depth.mipCount)return;
        detail::ComPtr<ID3D11Resource> resource;view->GetResource(&resource);
        detail::ComPtr<ID3D11Texture2D> source;
        if(!resource || FAILED(resource.As(&source)) || reinterpret_cast<uintptr_t>(source.Get())!=depth.resource)return;
        detail::ComPtr<ID3D11Device> device;source->GetDevice(&device);
        if(reinterpret_cast<uintptr_t>(device.Get())!=frame.device)return;
        D3D11_TEXTURE2D_DESC actual{};source->GetDesc(&actual);
        if(actual.Width!=depth.desc.Width || actual.Height!=depth.desc.Height || actual.Format!=depth.desc.Format ||
           actual.MipLevels!=depth.desc.MipLevels || actual.ArraySize!=depth.desc.ArraySize ||
           actual.SampleDesc.Count!=depth.desc.SampleDesc.Count || actual.SampleDesc.Quality!=depth.desc.SampleDesc.Quality)return;
        // Copy + our own EVENT does not execute raster/compute work or touch a
        // game query. The shader-replay PrivateWork gate does not apply here.
        // RawPreviewRing separately refuses any bound game predicate.
        const bool captured=state.ring.Capture(context,source.Get(),desc.Format,desc.Texture2D.MostDetailedMip,
            {frame.generation,frame.frame,frame.deviceEpoch,frame.ownerEpoch,frame.configEpoch,frame.instance,GetCurrentThreadId()});
        state.waiting=state.ring.LastFailure();outcome.accepted=captured;
    }catch(...){ObservationUncertain();}
}

void RevokePreview() noexcept {
    try {auto& s=State();s.wantedUntil=0;std::lock_guard lock(s.mutex);s.ring.Revoke();s.waiting="Waiting for a new claimed producer depth.";}catch(...){ObservationUncertain();}
}
void ClearDepth() noexcept {RevokePreview();}

void BeginPreviewDraw(ID3D11DeviceContext* context) noexcept {
    menuContext=nullptr;menuRequested=false;
    try {
        if(!context || !Genshin() || context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return;
        auto& s=State();std::unique_lock lock(s.mutex,std::try_to_lock);if(!lock.owns_lock())return;
        s.ring.Poll();menuContext=context;
    }catch(...){ObservationUncertain();}
}
void EndPreviewDraw(ID3D11DeviceContext* context,bool recorded) noexcept {
    if(menuContext!=context)return;
    menuContext=nullptr;
    try {
        auto& s=State();std::lock_guard lock(s.mutex);
        static unsigned reads=0;
        for(const auto& slot:s.ring.Slots())if(reads<4 && slot.leased && slot.stamp.thread==GetCurrentThreadId() &&
            detail::Same(slot.context.Get(),context)) {
            ++reads;
            try {LOG_INFO("R686 raw depth menu read: pid={} generation={} sourceFrame={} snapshot={} backendRecorded={} gpuCompletion=UNKNOWN",
                GetCurrentProcessId(),slot.stamp.generation,slot.stamp.sourceFrame,slot.serial,recorded);}catch(...){}
        }
        s.ring.Finish(context,recorded);
        // Only an actually drawn, enabled ordinary debug-page control renews demand.
        // Page changes, clipping and menu closure revoke visibility, not GPU owners.
        if(!menuRequested){s.wantedUntil=0;s.ring.Revoke();}
        menuRequested=false;
    }catch(...){ObservationUncertain();}
}

void DrawPreview() {
    if(!ImGui::GetCurrentContext() || !Genshin())return;
    const char* backend=ImGui::GetIO().BackendRendererName;
    if(!backend || std::strcmp(backend,"imgui_impl_dx11")!=0)return;
    // Process-local, off by default. No Optical F5Low/configuration dependency.
    static bool enabled=false;
    ImGui::Checkbox("\xe5\x8e\x9f\xe7\xa5\x9e\xe9\x9b\x86\xe6\x88\x90\x20\x46\x53\x52\x20\xe6\xa1\xa5\xe6\xb7\xb1\xe5\xba\xa6\xe9\xa2\x84\xe8\xa7\x88\xef\xbc\x88\xe5\x8e\x9f\xe5\xa7\x8b\xef\xbc\x89",&enabled);
    const bool visible=ImGui::IsItemVisible();
    auto& s=State();
    if(!enabled || !visible){s.wantedUntil=0;return;}
    ImGui::TextWrapped("Source: integrated FSR bridge claimed depth SRV -> private raw snapshot. Separate from current NR depth input.");
    ImGui::TextWrapped("Display: raw R, no normalization or linearization; 0 = black, 1 = full red, values outside 0..1 clip. Small values can look black even with a source. Near/far, reversed Z and NR/FG validity: UNKNOWN.");
    if(!menuContext) {
        s.wantedUntil=0;
        ImGui::TextDisabled("Waiting for the Genshin DX11 immediate menu context.");return;
    }
    const float width=(std::min)(360.0f,ImGui::GetContentRegionAvail().x);
    if(width<=0){s.wantedUntil=0;return;}
    // Independent demand publisher: native optional-collection subscriptions
    // cannot prevent a newly requested preview from waking the Full path.
    menuRequested=true;s.wantedUntil=GetTickCount64()+500;
    std::unique_lock lock(s.mutex,std::try_to_lock);
    if(!lock.owns_lock()){ImGui::TextDisabled("Preview busy; waiting for the next debug-page draw.");return;}
    auto* image=s.ring.Acquire(menuContext);
    if(!image){ImGui::TextDisabled("No completed fresh snapshot: %s",s.waiting);return;}
    ImGui::TextWrapped("Snapshot ready: generation=%llu sourceFrame=%llu ownerEpoch=%llu snapshot=%llu size=%ux%u format=%u; semantics UNKNOWN.",
        (unsigned long long)image->stamp.generation,(unsigned long long)image->stamp.sourceFrame,
        (unsigned long long)image->stamp.ownerEpoch,(unsigned long long)image->serial,
        image->width,image->height,static_cast<unsigned>(image->viewFormat));
    // Original typed depth plane, origin unchanged. Default ImGui sampling shows
    // R directly: no inverse-Z guess, linearization, contrast or jitter mapping.
    ImGui::Image((ImTextureID)(intptr_t)image->view.Get(),ImVec2(width,width*image->height/image->width));
    static unsigned draws=0;
    if(draws<4){++draws;LOG_INFO("R686 raw depth draw: pid={} generation={} sourceFrame={} ownerEpoch={} snapshot={} size={}x{} format={} channel=RawR convention=UNKNOWN backend=DX11 stage=IMGUI_ENQUEUED visual=UNKNOWN",
        GetCurrentProcessId(),image->stamp.generation,image->stamp.sourceFrame,image->stamp.ownerEpoch,image->serial,
        image->width,image->height,static_cast<unsigned>(image->viewFormat));}
}

extern "C" BOOL WINAPI OptiScalerGetDepthDx11Frame(OptiScalerDepthDx11Frame* frame) {
    // A bool-only legacy ABI must not manufacture a convention for raw data.
    if(frame && !frame->texture && !frame->device)*frame={};
    return FALSE;
}
extern "C" void WINAPI OptiScalerReleaseDepthDx11Frame(OptiScalerDepthDx11Frame* frame) {
    if(!frame)return;
    if(frame->texture){frame->texture->Release();frame->texture=nullptr;}
    if(frame->device){frame->device->Release();frame->device=nullptr;}
}
}

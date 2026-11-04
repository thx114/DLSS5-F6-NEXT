// SPDX-License-Identifier: GPL-3.0-or-later
#include "GenshinFsrContract.h"
#include <cmath>
#include <cwctype>

namespace GenshinFsr
{
bool IsGenshinExecutable(std::wstring_view path) noexcept
{
    path = path.substr(path.find_last_of(L"/\\") == path.npos ? 0 : path.find_last_of(L"/\\") + 1);
    auto equal = [path](std::wstring_view name) {
        if (path.size() != name.size()) return false;
        for (size_t i = 0; i < path.size(); ++i)
            if (std::towlower(path[i]) != std::towlower(name[i])) return false;
        return true;
    };
    return equal(L"YuanShen.exe") || equal(L"GenshinImpact.exe");
}
Reason Discover(std::wstring_view exe, const Discovery& d) noexcept
{
    if (!IsGenshinExecutable(exe)) return Reason::WrongGame;
    // The pinned native capture may attempt SR with explicit Bridge control provenance.
    // This authorizes no engine semantic claim and never admits NR or FG.
    const bool requestedSrOnly=d.srParameterProvenance==SrParameterProvenance::LegacyRequestedControls && !d.nr && !d.fg;
    return d.captureEntryProven && (d.semanticsProven || requestedSrOnly) &&
        d.orderedOutputConsumerProven && d.inputLeaseProven && d.sr ? Reason::None : Reason::CaptureUnproven;
}
const char* ReasonText(Reason r) noexcept
{
    static const char* reasons[] = { "ready", "wrong game", "capture/semantics/output consumption unproven",
        "invalid instance/epoch/generation", "invalid temporal parameters", "unsupported resource/view/subrect",
        "device mismatch", "GPU lease pending", "stale generation", "same generation output changed",
        "backend failed", "full D3D11 context state isolation unavailable", "original draw replay unsafe",
        "device removed", "owner draining", "GPU completion failed" };
    return reasons[static_cast<unsigned>(r)];
}
Reason ValidateParameters(const Parameters& p) noexcept
{
    if (!p.instance || !p.epoch || !p.generation) return Reason::InvalidIdentity;
    if (!p.renderWidth || !p.renderHeight || p.renderWidth > p.outputWidth || p.renderHeight > p.outputHeight ||
        p.outputWidth > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION || p.outputHeight > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
        p.originX || p.originY) return Reason::InvalidParameters;
    for (float value : {p.jitterX, p.jitterY, p.deltaMs, p.preExposure, p.cameraNear, p.cameraFar,
                        p.fovRadians, p.sharpness,p.motionScale})
        if (!std::isfinite(value)) return Reason::InvalidParameters;
    if (std::abs(p.jitterX) > 1 || std::abs(p.jitterY) > 1 || p.deltaMs < 1 || p.deltaMs > 100 ||
        p.preExposure <= 0 || p.cameraNear <= 0 || p.cameraFar <= p.cameraNear ||
        p.fovRadians <= 0 || p.fovRadians >= 3.141592654f || p.sharpness < 0 || p.sharpness > 1 ||
        p.motionScale<0 || p.motionScale>4)
        return Reason::InvalidParameters;
    return Reason::None;
}
static bool ColorFormat(DXGI_FORMAT f)
{
    return f == DXGI_FORMAT_R16G16B16A16_FLOAT || f == DXGI_FORMAT_R8G8B8A8_UNORM ||
           f == DXGI_FORMAT_R10G10B10A2_UNORM || f == DXGI_FORMAT_R11G11B10_FLOAT;
}
Reason ValidateResources(const Parameters& p, const ResourceSet& r, uintptr_t device, uintptr_t context,
                         const PreparedResourceAdmission* prepared, ResourceRefusal* refusal) noexcept
{
    if(refusal)*refusal={};
    const auto reject=[&](Reason reason,const char* role,const char* field) {
        if(refusal)*refusal={role,field};
        return reason;
    };
    // This capability was minted from the current synchronous producer and
    // exact slot before admission. A descriptor, flag or matching size cannot
    // create it. It is valid only for this owner/context and full Parameters.
    const bool preparedCurrent=prepared && (prepared->slot_ || prepared->outputSlot_) && device && context &&
        prepared->device_==device && prepared->context_==context &&
        prepared->deviceEpoch_ && prepared->producerEpoch_ && prepared->configEpoch_ &&
        prepared->thread_==GetCurrentThreadId() && SameParameters(prepared->parameters_,p);
    const auto sharedPrepared=[&](const ResourceInfo* value) {
        if(!preparedCurrent)return false;
        const ResourceInfo* proof=nullptr;
        if(value==&r.depth && prepared->slot_)proof=&prepared->depth_;
        else if(value==&r.motion && prepared->slot_)proof=&prepared->motion_;
        else if(value==&r.output && prepared->outputSlot_)proof=&prepared->output_;
        if(!proof)return false;
        const auto& exact=*proof;
        const auto& a=value->desc;const auto& b=exact.desc;
        return value->identity==exact.identity && value->resourcePointer==exact.resourcePointer &&
            value->deviceIdentity==exact.deviceIdentity && value->deviceIdentity==device &&
            a.Width==b.Width && a.Height==b.Height && a.MipLevels==b.MipLevels &&
            a.ArraySize==b.ArraySize && a.Format==b.Format && a.SampleDesc.Count==b.SampleDesc.Count &&
            a.SampleDesc.Quality==b.SampleDesc.Quality && a.Usage==b.Usage && a.BindFlags==b.BindFlags &&
            a.CPUAccessFlags==b.CPUAccessFlags && a.MiscFlags==b.MiscFlags &&
            a.MiscFlags==(D3D11_RESOURCE_MISC_SHARED|D3D11_RESOURCE_MISC_SHARED_NTHANDLE);
    };
    const ResourceInfo* values[]={&r.color,&r.depth,&r.motion,&r.output,&r.exposure,&r.reactive,&r.transparency};
    const char* roles[]={"color","depth","motion","output","exposure","reactive","transparency"};
    for(size_t i=0;i<7;++i)
    {
        const auto* value=values[i];const auto* role=roles[i];
        if (!value->identity) { if (value->resourcePointer) return reject(Reason::InvalidResource,role,"identity"); continue; }
        if (!value->resourcePointer) return reject(Reason::InvalidResource,role,"resource-pointer");
        if (value->deviceIdentity != device) return reject(Reason::DeviceMismatch,role,"device");
        if (value->desc.SampleDesc.Count != 1) return reject(Reason::InvalidResource,role,"sample-count");
        if (value->desc.ArraySize != 1) return reject(Reason::InvalidResource,role,"array-size");
        if (value->desc.MipLevels != 1) return reject(Reason::InvalidResource,role,"mip-levels");
        if (value->desc.Usage != D3D11_USAGE_DEFAULT) return reject(Reason::InvalidResource,role,"usage");
        if (value->desc.CPUAccessFlags) return reject(Reason::InvalidResource,role,"cpu-access");
        if (value->desc.MiscFlags && !sharedPrepared(value))
            return reject(Reason::InvalidResource,role,"misc-flags-or-prepared-provenance");
    }
    auto fits = [](const ResourceInfo& v, uint32_t w, uint32_t h, UINT bind) {
        return v.identity && v.desc.Width == w && v.desc.Height == h && (v.desc.BindFlags & bind) == bind;
    };
    if (!fits(r.color,p.renderWidth,p.renderHeight,D3D11_BIND_SHADER_RESOURCE) || !ColorFormat(r.color.desc.Format))
        return reject(Reason::InvalidResource,"color","shape-bind-format");
    if (!fits(r.depth,p.renderWidth,p.renderHeight,D3D11_BIND_SHADER_RESOURCE) || r.depth.desc.Format != DXGI_FORMAT_R32_FLOAT)
        return reject(Reason::InvalidResource,"depth","shape-bind-format");
    if (!fits(r.motion,p.renderWidth,p.renderHeight,D3D11_BIND_SHADER_RESOURCE) || r.motion.desc.Format != DXGI_FORMAT_R16G16_FLOAT)
        return reject(Reason::InvalidResource,"motion","shape-bind-format");
    if (!fits(r.output,p.outputWidth,p.outputHeight,D3D11_BIND_UNORDERED_ACCESS) || !ColorFormat(r.output.desc.Format))
        return reject(Reason::InvalidResource,"output","shape-bind-format");
    // Encoded game motion/typeless depth must still be decoded by capture.
    if (r.output.identity == r.color.identity || r.output.identity == r.depth.identity || r.output.identity == r.motion.identity)
        return reject(Reason::InvalidResource,"output","input-alias");
    if ((!p.autoExposure || r.exposure.identity) &&
        (!fits(r.exposure,1,1,D3D11_BIND_SHADER_RESOURCE) || r.exposure.desc.Format != DXGI_FORMAT_R32_FLOAT))
        return reject(Reason::InvalidResource,"exposure","shape-bind-format");
    for(size_t i=5;i<7;++i)
        if(values[i]->identity && (!fits(*values[i],p.renderWidth,p.renderHeight,D3D11_BIND_SHADER_RESOURCE) ||
                                  values[i]->desc.Format!=DXGI_FORMAT_R8_UNORM))
            return reject(Reason::InvalidResource,roles[i],"shape-bind-format");
    return Reason::None;
}
float DecodeSignedSquareMotion(float encoded) noexcept
{
    const float d = encoded - 127.0f / 255.0f;
    return d == 0 ? 0 : -std::copysign(4.0f * d * d, d);
}
bool SameParameters(const Parameters& a, const Parameters& b) noexcept
{
    return a.instance==b.instance && a.epoch==b.epoch && a.generation==b.generation &&
        a.renderWidth==b.renderWidth && a.renderHeight==b.renderHeight && a.outputWidth==b.outputWidth &&
        a.outputHeight==b.outputHeight && a.originX==b.originX && a.originY==b.originY &&
        a.sourceFrame==b.sourceFrame && a.motionScale==b.motionScale && a.jitterX==b.jitterX && a.jitterY==b.jitterY && a.deltaMs==b.deltaMs && a.preExposure==b.preExposure &&
        a.cameraNear==b.cameraNear && a.cameraFar==b.cameraFar && a.fovRadians==b.fovRadians && a.sharpness==b.sharpness &&
        a.depthInverted==b.depthInverted && a.motionJittered==b.motionJittered && a.positiveMotionScale==b.positiveMotionScale &&
        a.hdr==b.hdr && a.autoExposure==b.autoExposure && a.enableSharpening==b.enableSharpening && a.reset==b.reset;
}
FfxFsr2ContextDescription MakeContext(const Parameters& p, ID3D11Device* device) noexcept
{
    FfxFsr2ContextDescription result {};
    result.device = device; result.maxRenderSize = {p.renderWidth,p.renderHeight};
    result.displaySize = {p.outputWidth,p.outputHeight};
    if (p.hdr) result.flags |= FFX_FSR2_ENABLE_HIGH_DYNAMIC_RANGE;
    if (p.depthInverted) result.flags |= FFX_FSR2_ENABLE_DEPTH_INVERTED;
    if (p.autoExposure) result.flags |= FFX_FSR2_ENABLE_AUTO_EXPOSURE;
    if (p.motionJittered) result.flags |= FFX_FSR2_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION;
    return result;
}
FfxFsr2DispatchDescription MakeDispatch(const Parameters& p, ID3D11DeviceContext* context,
                                      const ResourceSet& r, bool forceReset) noexcept
{
    FfxFsr2DispatchDescription d {};
    d.commandList = context;
    // The internal mapper consumes resource pointers; no FFX backend/context creation or export shim is involved.
    d.color.resource = reinterpret_cast<void*>(r.color.resourcePointer);
    d.depth.resource = reinterpret_cast<void*>(r.depth.resourcePointer);
    d.motionVectors.resource = reinterpret_cast<void*>(r.motion.resourcePointer);
    d.output.resource = reinterpret_cast<void*>(r.output.resourcePointer);
    d.exposure.resource = reinterpret_cast<void*>(r.exposure.resourcePointer);
    d.reactive.resource = reinterpret_cast<void*>(r.reactive.resourcePointer);
    d.transparencyAndComposition.resource = reinterpret_cast<void*>(r.transparency.resourcePointer);
    d.jitterOffset = {p.jitterX,p.jitterY};
    const float sign = p.positiveMotionScale ? 1.0f : -1.0f;
    d.motionVectorScale = {sign*p.renderWidth*p.motionScale,sign*p.renderHeight*p.motionScale};
    d.renderSize = {p.renderWidth,p.renderHeight};
    d.frameTimeDelta = p.deltaMs; d.preExposure = p.preExposure;
    d.cameraNear = p.cameraNear; d.cameraFar = p.cameraFar; d.cameraFovAngleVertical = p.fovRadians;
    d.enableSharpening = p.enableSharpening; d.sharpness = p.enableSharpening ? p.sharpness : 0;
    d.reset = p.reset || forceReset;
    return d;
}
bool Result::MaySkipOriginalDraw() const noexcept
{
    return reason == Reason::None && stateRestored && outputIdentity && instance && epoch && generation &&
        (disposition == Disposition::WrittenOutput || disposition == Disposition::ReusedOutput ||
         (disposition == Disposition::QueuedOutput && pendingLease));
}
NativeDrawAction Result::DrawAction() const noexcept
{
    if (MaySkipOriginalDraw()) return NativeDrawAction::OmitWithReplacement;
    return disposition==Disposition::FailClosed || disposition==Disposition::SuppressUntilNextToken ||
           disposition==Disposition::WrittenOutput || disposition==Disposition::QueuedOutput || disposition==Disposition::ReusedOutput
        ? NativeDrawAction::RequireRecovery : NativeDrawAction::ContinueOriginal;
}
Reason Lifecycle::Open(uint64_t instance, uint64_t epoch) noexcept
{
    if (pending_) return Reason::Pending;
    if (!instance || !epoch || (instance_ == instance && epoch_ && epoch <= epoch_)) return Reason::InvalidIdentity;
    instance_ = instance; epoch_ = epoch; last_ = 0; output_ = 0;
    validOutput_ = false; reset_ = true; draining_ = false; uncertain_ = false;
    return Reason::None;
}
Result Lifecycle::Check(const Parameters& p, uintptr_t output, bool orderedRetained) const noexcept
{
    Result r; r.instance=p.instance; r.epoch=p.epoch; r.generation=p.generation; r.outputIdentity=output;
    if (draining_) r.reason=Reason::Draining;
    else if (p.instance!=instance_ || p.epoch!=epoch_ || !p.generation) r.reason=Reason::InvalidIdentity;
    else if (uncertain_) r.reason=Reason::CompletionFailure;
    else if (p.generation < last_) r.reason=Reason::StaleGeneration;
    else if (p.generation == last_ && output!=output_) r.reason=Reason::IdentityChanged;
    else if (p.generation == last_ && validOutput_)
    { r.disposition=pending_ ? Disposition::QueuedOutput : Disposition::ReusedOutput; r.pendingLease=pending_; }
    else if (pending_ && !(orderedRetained && validOutput_)) r.reason=Reason::Pending;
    else if (p.generation==last_) r.reason=Reason::StaleGeneration;
    return r;
}
Result Lifecycle::Submitted(const Parameters& p, uintptr_t output, bool ok, bool restored,
                            bool replaySafe, bool lease) noexcept
{
    last_=p.generation; output_=output; ++pending_; validOutput_=ok && restored && lease;
    reset_=!validOutput_; uncertain_=!lease;
    Result r; r.instance=p.instance; r.epoch=p.epoch; r.generation=p.generation; r.outputIdentity=output;
    r.stateRestored=restored; r.pendingLease=lease;
    if (validOutput_) { r.disposition=Disposition::QueuedOutput; return r; }
    r.reason=!lease ? Reason::CompletionFailure : !restored ? Reason::StateUnavailable : Reason::BackendFailure;
    r.disposition=restored && replaySafe ? Disposition::RunNative : Disposition::FailClosed;
    return r;
}
void Lifecycle::Complete(bool success) noexcept
{
    // Retiring an older invocation must not clear a newer failure or reset history.
    if (!success) { validOutput_=false; reset_=true; uncertain_=true; return; }
    if (pending_) --pending_;
}
void Lifecycle::DeviceRemoved() noexcept
{
    // Explicit device cancellation is the only way to drop uncertain leases.
    pending_=0; Invalidate();
}
void Lifecycle::Invalidate() noexcept { draining_=true; validOutput_=false; reset_=true; }
Reason OwnerRegistry::CanAcquire(uint64_t instance, uint64_t epoch, uintptr_t device, uintptr_t owner) const noexcept
{
    if (!instance || !epoch || !device || !owner) return Reason::InvalidIdentity;
    if (auto found=instances_.find(instance);found!=instances_.end())
    {
        if (found->second.active) return Reason::Pending;
        if (epoch<=found->second.epoch) return Reason::StaleGeneration;
    }
    for (const auto& [id,entry] : instances_)
        if (entry.active && (entry.owner==owner || entry.device!=device)) return Reason::DeviceMismatch;
    return Reason::None;
}
Reason OwnerRegistry::Acquire(uint64_t instance, uint64_t epoch, uintptr_t device, uintptr_t owner)
{
    auto reason=CanAcquire(instance,epoch,device,owner);
    if (reason==Reason::None) instances_.insert_or_assign(instance,Entry {epoch,device,owner,true});
    return reason;
}
bool OwnerRegistry::Owns(uintptr_t owner, uintptr_t device) const noexcept
{
    for (const auto& [id,entry] : instances_)
        if (entry.active && entry.owner==owner && entry.device==device) return true;
    return false;
}
bool OwnerRegistry::Release(uintptr_t owner) noexcept
{
    for (auto& [id,entry] : instances_)
        if (entry.active && entry.owner==owner) {entry.active=false;return true;}
    return false;
}
}

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <memory>
#include <string_view>
#include <unordered_map>
#include <d3d11.h>
#include <fsr2/ffx_fsr2.h>

namespace GenshinTransport { class ProducerFrame; }
namespace GenshinFsr
{
enum class Reason { None, WrongGame, CaptureUnproven, InvalidIdentity, InvalidParameters, InvalidResource,
                    DeviceMismatch, Pending, StaleGeneration, IdentityChanged, BackendFailure, StateUnavailable,
                    ReplayUnsafe, DeviceLost, Draining, CompletionFailure };
enum class Disposition { NotApplicable, RunNative, WrittenOutput, QueuedOutput, ReusedOutput,
                         SuppressUntilNextToken, FailClosed };
enum class NativeDrawAction { ContinueOriginal, OmitWithReplacement, RequireRecovery };
// Internal Genshin only: an incomplete successful backend phase, never SR output.
enum class Transition { None, UnsubmittedBackendRebuild };
enum class SrParameterProvenance : uint8_t { Unknown = 0, LegacyRequestedControls = 1 };
struct Discovery
{
    // Internal owner evidence, never INI proof overrides.
    bool captureEntryProven = false;
    // Reserved for independently verified current engine/profile semantics.
    bool semanticsProven = false;
    bool orderedOutputConsumerProven = false;
    bool inputLeaseProven = false;
    bool sr = false, nr = false, fg = false;
    // Legacy SR parameters remain requested assumptions, not engine or NR/FG proof.
    SrParameterProvenance srParameterProvenance = SrParameterProvenance::Unknown;
};
bool IsGenshinExecutable(std::wstring_view path) noexcept;
Reason Discover(std::wstring_view executable, const Discovery& evidence) noexcept;
const char* ReasonText(Reason reason) noexcept;
struct Parameters
{
    uint64_t instance = 0, epoch = 0, generation = 0;
    uint32_t sourceFrame = 0;
    uint32_t renderWidth = 0, renderHeight = 0, outputWidth = 0, outputHeight = 0;
    // Full texture, origin zero is the only supported subrect until capture proves otherwise.
    uint32_t originX = 0, originY = 0;
    float jitterX = 0, jitterY = 0, deltaMs = 0, preExposure = 0;
    float motionScale = 1;
    float cameraNear = 0, cameraFar = 0, fovRadians = 0, sharpness = 0;
    bool depthInverted = false, motionJittered = false, positiveMotionScale = false;
    bool hdr = false, autoExposure = true, enableSharpening = false, reset = false;
};
Reason ValidateParameters(const Parameters& p) noexcept;
struct ResourceInfo
{
    uintptr_t identity = 0, deviceIdentity = 0;
    uintptr_t resourcePointer = 0;
    D3D11_TEXTURE2D_DESC desc {};
};
struct ResourceSet
{
    ResourceInfo color, depth, motion, output, exposure, reactive, transparency;
};
class Dx11Owner;
struct ResourceRefusal
{
    const char* role = "none";
    const char* field = "none";
};
// A per-invocation capability, never an allow-shared boolean in ResourceInfo.
// Only the authenticated ProducerFrame can populate it; ordinary callers get
// the original private-resource contract. Input and output roles own separate
// exact prepared slots; neither role can authorize another resource.
class PreparedResourceAdmission final
{
    friend class GenshinTransport::ProducerFrame;
    friend Reason ValidateResources(const Parameters&,const ResourceSet&,uintptr_t,uintptr_t,
                                    const PreparedResourceAdmission*,ResourceRefusal*) noexcept;
    Parameters parameters_{};
    ResourceInfo depth_{},motion_{},output_{};
    uintptr_t device_=0,context_=0;
    uint64_t deviceEpoch_=0,producerEpoch_=0,configEpoch_=0;
    uint32_t thread_=0;
    std::shared_ptr<const void> slot_,outputSlot_;
public:
    PreparedResourceAdmission()=default;
};
Reason ValidateResources(const Parameters& p, const ResourceSet& r, uintptr_t device,
                         uintptr_t context=0,const PreparedResourceAdmission* prepared=nullptr,
                         ResourceRefusal* refusal=nullptr) noexcept;
float DecodeSignedSquareMotion(float encoded) noexcept;
bool SameParameters(const Parameters& a, const Parameters& b) noexcept;
FfxFsr2ContextDescription MakeContext(const Parameters& p, ID3D11Device* device) noexcept;
FfxFsr2DispatchDescription MakeDispatch(const Parameters& p, ID3D11DeviceContext* context,
                                      const ResourceSet& resources, bool forceReset) noexcept;
struct Result
{
    Disposition disposition = Disposition::RunNative;
    Reason reason = Reason::None;
    uint64_t instance = 0, epoch = 0, generation = 0;
    uintptr_t outputIdentity = 0;
    bool stateRestored = true, pendingLease = false;
    // The recording slot is pending before Lease::Begin. No provider submission
    // or lifecycle pending count exists for this native-replayed invocation.
    bool unsubmittedRecordingPending = false;
    // Classification only, after the caller authenticated this invocation.
    // Never reinterpret a leased submission, queued output or provider failure.
    void NoteUnsubmittedRecordingPending(bool invocationLive, bool leaseCreated) noexcept {
        if(invocationLive && !leaseCreated && reason==Reason::Pending &&
           disposition==Disposition::RunNative && stateRestored && !pendingLease)
            unsubmittedRecordingPending=true;
    }
    // Unsupported initial exposure was refused before provider registration,
    // CreateFeature or submission. It is not a failed backend epoch.
    bool unsubmittedManualExposure = false;
    Transition transition = Transition::None;
    bool MaySkipOriginalDraw() const noexcept;
    // Caller must use this action, not turn all false replacement guards into native replay.
    NativeDrawAction DrawAction() const noexcept;
};
// The GPU owner drives this state machine only on the owning immediate-context thread.
// A lease is retained even after backend failure because commands may already be queued.
class Lifecycle
{
    uint64_t instance_ = 0, epoch_ = 0, last_ = 0;
    uintptr_t output_ = 0;
    uint64_t pending_ = 0;
    bool validOutput_ = false, reset_ = true, draining_ = false, uncertain_ = false;
public:
    Reason Open(uint64_t instance, uint64_t epoch) noexcept;
    // orderedRetained is supplied only by the immediate-context owner that retains
    // a separate lease for every invocation and serializes mutable provider work.
    Result Check(const Parameters& p, uintptr_t output, bool orderedRetained = false) const noexcept;
    Result Submitted(const Parameters& p, uintptr_t output, bool backendOk,
                     bool stateRestored, bool replaySafe, bool leaseEstablished) noexcept;
    void Complete(bool success) noexcept;
    void DeviceRemoved() noexcept;
    void Invalidate() noexcept;
    // Drop reusable output/history authority, retaining every prior pending owner.
    void BackendRebuildPending() noexcept { validOutput_=false; reset_=true; }
    bool Pending() const noexcept { return pending_; }
    bool ForceReset() const noexcept { return reset_; }
    bool CanDestroy() const noexcept { return !pending_; }
    bool NeedsDrain() const noexcept { return draining_; }
};
// Caller serializes access. Keep the last epoch after release to reject stale/ABA owner recreation.
class OwnerRegistry
{
    struct Entry { uint64_t epoch; uintptr_t device, owner; bool active; };
    std::unordered_map<uint64_t,Entry> instances_;
public:
    Reason CanAcquire(uint64_t instance, uint64_t epoch, uintptr_t device, uintptr_t owner) const noexcept;
    Reason Acquire(uint64_t instance, uint64_t epoch, uintptr_t device, uintptr_t owner);
    bool Owns(uintptr_t owner, uintptr_t device) const noexcept;
    bool Release(uintptr_t owner) noexcept;
};
}

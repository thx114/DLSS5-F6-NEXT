#pragma once
#include <Windows.h>
#include "HoYoShadeDeviceDomain.h"
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <atomic>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <utility>

namespace HoYoShade { namespace NaturalAssociation { class Source; }
void BindNaturalSource(const NaturalAssociation::Source*) noexcept;
void CaptureNaturalDomain(const NaturalAssociation::Source*) noexcept; }
namespace HoYoShade::NaturalAssociation
{
// Association authority only. A source device is a scheduling entry, a chain
// device is the result of IDXGISwapChain::GetDevice, and owner is the result of
// the exact typed backbuffer's GetDevice. They need not be pointer aliases.
// No object here authorizes GPU work, a queue, a fence, or a physical child.
enum class Predicate : unsigned { Missing, UnsupportedApi, SourceIncomplete, ChainDevice,
    ChainBuffer, ResourceChanged, ResourceOwner, Captured, Expired, EndpointChanged,
    ThreadChanged, ApiMismatch, OwnerMismatch, BufferMismatch, ObservationIncomplete, Ambiguous, Unbound, NoRuntime, FrameUnavailable, LifetimeChanged, EpochChanged, LegacyBeforeSr, DomainChanged, SelectionDonor, SelectionUnavailable, SettingsChanged, SelectionChanged };
inline const char* Text(Predicate value) noexcept
{
    constexpr const char* names[]{"source-proof-missing", "source-api-unsupported", "source-incomplete",
        "chain-get-device", "chain-get-buffer", "source-resource-changed", "resource-get-device",
        "typed-source-captured", "source-scope-expired", "source-endpoint-changed", "source-thread-changed",
        "runtime-api-mismatch", "runtime-resource-owner-mismatch", "runtime-resource-mismatch",
        "natural-observation-incomplete", "ambiguous-typed-source", "runtime-not-bound",
        "typed-runtime-unmapped", "natural-frame-unavailable", "runtime-lifetime-changed", "runtime-epoch-changed", "legacy-before-sr-exact-device", "source-domain-marker-changed",
        "before-sr-selection-only-donor", "donor-selection-unavailable", "donor-settings-changed", "donor-selection-changed"};
    return static_cast<unsigned>(value) < sizeof(names)/sizeof(*names) ? names[static_cast<unsigned>(value)] : "unknown";
}
struct Evidence
{
    Predicate predicate = Predicate::Missing;
    unsigned api = 0;
    int32_t hr = E_PENDING;
    uint64_t chain = 0, sourceDevice = 0, chainDevice = 0, buffer = 0, owner = 0;
    uint64_t stream = 0, generation = 0, frame = 0;
    DWORD thread = 0;
};
// Observation only. This scalar is never consulted by CheckRuntime or a gate.
struct CanonicalDeviceObservation
{
    bool attempted = false;
    HRESULT hr = E_PENDING;
    uint64_t identity = 0;
};
// A single process-wide diagnostic window: at most eight extra source QIs.
// Only the target BeforeNR stage arms its actual source stream/thread.
inline std::atomic<uint64_t> canonicalSourceStream{0};
inline std::atomic<DWORD> canonicalSourceThread{0};
inline std::atomic<unsigned> canonicalSourceQueries{0};
inline void ArmCanonicalSourceObservation(uint64_t stream, DWORD thread) noexcept
{ canonicalSourceThread.store(thread, std::memory_order_relaxed);
  canonicalSourceStream.store(stream, std::memory_order_release); }
inline void DisarmCanonicalSourceObservation() noexcept
{ canonicalSourceStream.store(0, std::memory_order_release); }
inline bool TakeCanonicalSourceObservation(uint64_t stream, DWORD thread) noexcept
{
    if (!stream || !thread || canonicalSourceStream.load(std::memory_order_acquire) != stream ||
        canonicalSourceThread.load(std::memory_order_relaxed) != thread) return false;
    auto used = canonicalSourceQueries.load(std::memory_order_relaxed);
    while (used < 8)
        if (canonicalSourceQueries.compare_exchange_weak(used, used + 1, std::memory_order_acq_rel)) return true;
    return false;
}
// The protocol is shared with the small CPU fixture. Only Source::Capture can
// turn a successful protocol into a retained product proof; scalar Evidence
// supplied by a caller cannot construct a Source.
template<class Access> bool Resolve(Access& access, Evidence& value)
{
    if (value.api != 0xb000 && value.api != 0xc000) { value.predicate = Predicate::UnsupportedApi; return false; }
    if (!value.chain || !value.sourceDevice || !value.buffer || !value.stream ||
        !value.generation || !value.frame || !value.thread) { value.predicate = Predicate::SourceIncomplete; return false; }
    value.predicate = Predicate::ChainDevice;
    value.hr = access.ChainDevice(value.chainDevice);
    if (value.hr < 0 || !value.chainDevice) return false;
    value.predicate = Predicate::ChainBuffer;
    uint64_t current = 0;
    value.hr = access.ChainBuffer(current);
    if (value.hr < 0 || !current) return false;
    value.predicate = Predicate::ResourceChanged;
    if (current != value.buffer) return false;
    value.predicate = Predicate::ResourceOwner;
    value.hr = access.ResourceOwner(value.owner);
    if (value.hr < 0 || !value.owner) return false;
    value.predicate = Predicate::Captured;
    return true;
}
inline Predicate CheckEndpoint(const Evidence& value, bool active, DWORD thread,
    uint64_t stream, uint64_t generation, uint64_t frame, uint64_t sourceDevice, uint64_t buffer) noexcept
{
    if (value.predicate != Predicate::Captured) return value.predicate;
    if (!active) return Predicate::Expired;
    if (thread != value.thread) return Predicate::ThreadChanged;
    if (stream != value.stream || generation != value.generation || frame != value.frame ||
        sourceDevice != value.sourceDevice || buffer != value.buffer) return Predicate::EndpointChanged;
    return Predicate::Captured;
}
template<class Buffers> Predicate CheckRuntime(const Evidence& value, unsigned api, uint64_t owner, const Buffers& buffers) noexcept
{
    if (value.predicate != Predicate::Captured) return value.predicate;
    if (api != value.api) return Predicate::ApiMismatch;
    if (!owner || owner != value.owner) return Predicate::OwnerMismatch;
    for (auto buffer : buffers) if (buffer && buffer == value.buffer) return Predicate::Captured;
    return Predicate::BufferMismatch;
}
inline bool LegacyBeforeSr(bool requested, bool beforeSr, bool original, bool hasProof,
    uint64_t device, uint64_t backbuffer) noexcept
{ return requested && beforeSr && original && !hasProof && device && !backbuffer; }
struct Binding
{
    Predicate predicate = Predicate::Unbound;
    uint64_t runtime = 0, lifetime = 0, epoch = 0;
};
inline Predicate CheckBinding(const Binding& binding, uint64_t runtime, uint64_t lifetime, uint64_t epoch) noexcept
{
    if (binding.predicate != Predicate::Captured) return binding.predicate;
    if (!runtime || runtime != binding.runtime || !lifetime || lifetime != binding.lifetime) return Predicate::LifetimeChanged;
    if (!epoch || epoch == UINT64_MAX || epoch != binding.epoch) return Predicate::EpochChanged;
    return Predicate::Captured;
}
// Selection association is NOT a typed-backbuffer proof. This same-call
// scalar token carries no SDK handles, GPU objects, queues or device aliases.
// Integration observes it under executionMutex and checks it again across
// every reentrant boundary. Frame/settings fields are not binding cache keys.
struct SelectionDonor
{
    uint64_t runtime = 0, lifetime = 0, epoch = 0, settings = 0;
    uint64_t revision = 0, topology = 0, snapshots = 0, graphBuilds = 0;
    uint64_t files = 0;
    DWORD thread = 0;
    bool operator==(const SelectionDonor&) const = default;
};
inline bool SameSelectionConfiguration(const SelectionDonor& a, const SelectionDonor& b) noexcept
{
    return a.runtime == b.runtime && a.lifetime == b.lifetime &&
        a.snapshots == b.snapshots && a.files == b.files;
}
class Source final
{
    friend void HoYoShade::BindNaturalSource(const Source*) noexcept;
    friend void HoYoShade::CaptureNaturalDomain(const Source*) noexcept;
    // Binding is read/written ONLY by integration under executionMutex, while
    // the same-thread Scope holds the proof. Scope destruction changes only
    // scope_ atomically, never this binding. A failed/nested scope cannot revoke
    // its owner. Cross-thread endpoints fail before any binding read.
    mutable Binding binding_;
    Evidence evidence_;
    CanonicalDeviceObservation canonicalDevice_;
    Microsoft::WRL::ComPtr<ID3D11Device> owner11_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> buffer11_;
    mutable DeviceDomain::Witness domain_;
    mutable DeviceDomain::Marker marker_;
    Microsoft::WRL::ComPtr<IUnknown> chain_, sourceDevice_, chainDevice_, buffer_, owner_;
    // Captured -> active synchronous RunFrame/Present -> expired, never rearmed.
    mutable std::atomic<unsigned> scope_{0};
    Source() = default;
    template<class Device, class Resource>
    static std::shared_ptr<const Source> CaptureTyped(IDXGISwapChain* chain, Device* sourceDevice,
        Resource* buffer, UINT index, unsigned api, uint64_t stream, uint64_t generation, uint64_t frame)
    {
        auto proof = std::shared_ptr<Source>(new Source);
        auto key = [](const void* p) { return reinterpret_cast<uintptr_t>(p); };
        proof->evidence_ = {Predicate::Missing, api, E_PENDING, key(chain), key(sourceDevice), 0,
            key(buffer), 0, stream, generation, frame, GetCurrentThreadId()};
        proof->chain_ = chain; proof->sourceDevice_ = sourceDevice; proof->buffer_ = buffer;
        struct Access
        {
            IDXGISwapChain* chain; Resource* buffer; UINT index;
            Microsoft::WRL::ComPtr<Device> chainDevice, owner;
            Microsoft::WRL::ComPtr<Resource> current;
            HRESULT ChainDevice(uint64_t& out) {
                const auto hr = chain->GetDevice(IID_PPV_ARGS(&chainDevice));
                out = reinterpret_cast<uintptr_t>(chainDevice.Get()); return hr;
            }
            HRESULT ChainBuffer(uint64_t& out) {
                const auto hr = chain->GetBuffer(index, IID_PPV_ARGS(&current));
                out = reinterpret_cast<uintptr_t>(current.Get()); return hr;
            }
            HRESULT ResourceOwner(uint64_t& out) {
                if constexpr (std::is_same_v<Device, ID3D11Device>) {
                    buffer->GetDevice(&owner);
                    out = reinterpret_cast<uintptr_t>(owner.Get()); return owner ? S_OK : E_NOINTERFACE;
                } else {
                    const auto hr = buffer->GetDevice(IID_PPV_ARGS(&owner));
                    out = reinterpret_cast<uintptr_t>(owner.Get()); return hr;
                }
            }
        } access{chain, buffer, index};
        Resolve(access, proof->evidence_);
        proof->chainDevice_ = access.chainDevice; proof->owner_ = access.owner;
        if constexpr (std::is_same_v<Device, ID3D11Device>) {
            proof->owner11_=access.owner;proof->buffer11_=buffer;
            // The actual B.GetDevice owner remains strongly held. Query its
            // canonical IUnknown in this Capture call, never on another thread.
            if (proof->evidence_.predicate == Predicate::Captured &&
                TakeCanonicalSourceObservation(stream, proof->evidence_.thread)) {
                proof->canonicalDevice_.attempted = true;
                Microsoft::WRL::ComPtr<IUnknown> canonical;
                proof->canonicalDevice_.hr = access.owner->QueryInterface(IID_PPV_ARGS(&canonical));
                if (SUCCEEDED(proof->canonicalDevice_.hr) && canonical)
                    proof->canonicalDevice_.identity = reinterpret_cast<uintptr_t>(canonical.Get());
            }
        }
        return proof;
    }
public:
    Source(const Source&) = delete;
    Source& operator=(const Source&) = delete;
    static std::shared_ptr<const Source> Capture(IDXGISwapChain* chain, ID3D11Device* device,
        ID3D11Texture2D* buffer, UINT index, uint64_t stream, uint64_t generation, uint64_t frame)
    noexcept { try { return CaptureTyped(chain,device,buffer,index,0xb000,stream,generation,frame); } catch (...) { return {}; } }
    static std::shared_ptr<const Source> Capture(IDXGISwapChain* chain, ID3D12Device* device,
        ID3D12Resource* buffer, UINT index, uint64_t stream, uint64_t generation, uint64_t frame)
    noexcept { try { return CaptureTyped(chain,device,buffer,index,0xc000,stream,generation,frame); } catch (...) { return {}; } }
    const Evidence& Observation() const noexcept { return evidence_; }
    const CanonicalDeviceObservation& CanonicalDevice() const noexcept { return canonicalDevice_; }
    const Binding& BoundRuntime() const noexcept { return binding_; }
    Predicate Bound(uint64_t runtime, uint64_t lifetime, uint64_t epoch) const noexcept
    { return CheckBinding(binding_,runtime,lifetime,epoch); }
    const DeviceDomain::Witness& DomainObservation() const noexcept {return domain_;}
    bool DomainCurrent() const noexcept {
        DeviceDomain::Marker current;
        return buffer11_ && marker_ && DeviceDomain::ReadMarker(buffer11_.Get(),current)==DeviceDomain::MarkerRead::KnownShape &&
            current==marker_;
    }
    Predicate Endpoint(uint64_t stream, uint64_t generation, uint64_t frame, uint64_t device, uint64_t buffer) const noexcept
    {
        const auto status=CheckEndpoint(evidence_,scope_.load(std::memory_order_acquire)==1,GetCurrentThreadId(),
            stream,generation,frame,device,buffer);
        if(status!=Predicate::Captured)return status;
        return domain_.result==DeviceDomain::Result::Accepted && !DomainCurrent()?Predicate::DomainChanged:status;
    }
    template<class Buffers> Predicate Runtime(unsigned api, uint64_t owner, const Buffers& buffers, uint64_t sdkDevice=0) const noexcept
    {
        const auto exact=CheckRuntime(evidence_,api,owner,buffers);
        if(exact!=Predicate::OwnerMismatch)return exact;
        if(!DeviceDomain::Matches(domain_,api,sdkDevice,owner,evidence_.buffer,evidence_.owner) || !DomainCurrent())return exact;
        for(auto buffer:buffers)if(buffer && buffer==evidence_.buffer)return Predicate::Captured;
        return Predicate::BufferMismatch;
    }
    // The only owner of the active interval. Move the acquired proof OUT of
    // FrameInput before NativeProducer retains its inputs through a GPU fence:
    // keeping a swapchain buffer in that ring would obstruct ResizeBuffers.
    class Scope final
    {
        std::shared_ptr<const Source> proof_;
        bool acquired_ = false;
    public:
        explicit Scope(std::shared_ptr<const Source> proof) noexcept : proof_(std::move(proof)) {
            unsigned expected = 0;
            if (proof_) acquired_ = proof_->scope_.compare_exchange_strong(expected,1,std::memory_order_acq_rel);
        }
        ~Scope() { if (acquired_) proof_->scope_.store(2,std::memory_order_release); }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
        const Source* Get() const noexcept { return acquired_ ? proof_.get() : nullptr; }
    };
};
}

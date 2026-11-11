#pragma once
#include "FrameContract.h"

namespace native
{
// Acquire-only transaction. No consumer may read the input until HandOff.
// BeginReader retains its content/fence roots in the producer's bounded ring.
// An abort without an exact, successfully signalled input-ready witness leaves
// that begun reader quarantined there; dropping this guard never releases it.
class BridgeDepthReader final
{
public:
    enum class State { Empty, Begun, InputReady, HandedOff, Retired, Quarantined };
private:
    BridgeDepthLeasePtr lease_;
    ID3D12Resource* resource_ = nullptr; // owned by the begun lease
    ID3D12Fence* ready_ = nullptr; // owned by the begun lease
    uint64_t value_ = 0;
    State state_ = State::Empty;
public:
    ~BridgeDepthReader() { Abort(); }
    BridgeDepthReader() = default;
    BridgeDepthReader(const BridgeDepthReader&) = delete;
    BridgeDepthReader& operator=(const BridgeDepthReader&) = delete;
    State Status() const noexcept { return state_; }
    bool Begin(const BridgeDepthLeasePtr& lease, IUnknown* source, ID3D12Resource* resource,
               ID3D12Fence* ready, uint64_t value) noexcept
    {
        if (state_ != State::Empty || !lease || !source || !resource || !ready ||
            !value || value == UINT64_MAX || !lease->BeginReader(source, resource, ready, value)) return false;
        lease_ = lease; resource_ = resource; ready_ = ready; value_ = value;
        state_ = State::Begun;
        return true;
    }
    // Only after Acquire's actual D3D11 Signal succeeds and Flush is issued.
    void InputReady() noexcept { if (state_ == State::Begun) state_ = State::InputReady; }
    bool HandOff(const FrameInput& input) noexcept
    {
        if (state_ != State::InputReady || input.depthCount != 1 || input.depthLease != lease_ ||
            input.depth[0] != resource_ || input.depthResource != resource_ ||
            input.ready.fence != ready_ || input.ready.value != value_ ||
            !lease_->MatchesReader(resource_, ready_, value_)) return false;
        state_ = State::HandedOff;
        return true;
    }
    void Abort() noexcept
    {
        if (state_ != State::Begun && state_ != State::InputReady) return;
        const bool ready = state_ == State::InputReady;
        state_ = State::Quarantined;
        // No consumer has been called. The input-ready signal covers the real
        // D3D11 source copy (mapped) or the unconsumed queued offer (native).
        // Begin owns this exact one-shot reader and no consumer was handed off.
        // Revocation closes admission (MatchesReader/Ready), not the obligation
        // to register completion. BindReaderCompletion still checks the begun
        // lease, device, one-shot done fence and owner where required.
        // This is a retirement REQUEST; the lease still polls its real fences.
        if (ready && lease_->BindReaderCompletion(ready_, value_) &&
            lease_->RetireReader()) state_ = State::Retired;
    }
};
}

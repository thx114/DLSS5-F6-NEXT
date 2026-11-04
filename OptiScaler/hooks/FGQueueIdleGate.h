#pragma once
#include <Windows.h>
#include <d3d12.h>
#include <dxgi.h>
#include <limits>

// Private policy; the adapter owns one queue/device/fence/event epoch and serializes access.
namespace fg_queue_idle
{
inline HRESULT Refusal(HRESULT hr) { return FAILED(hr) ? hr : E_UNEXPECTED; }

enum class Reason { Complete, Unbound, Exhausted, Device, Signal, Arm, Wait, Incomplete };
struct Result
{
    HRESULT hr = E_UNEXPECTED;
    Reason reason = Reason::Unbound;
    UINT64 target = 0, completed = 0;
    DWORD wait = WAIT_FAILED;
    bool submitted = false;
    explicit operator bool() const { return hr == S_OK && reason == Reason::Complete && submitted; }
};

template<class Io> Result Complete(Io& io, UINT64& counter)
{
    Result r;
    if (!io.Bound()) return r;
    r.hr = io.DeviceStatus();
    if (r.hr != S_OK) { r.hr = Refusal(r.hr); r.reason = Reason::Device; return r; }
    constexpr auto removed = (std::numeric_limits<UINT64>::max)();
    if (counter >= removed - 1)
    { r.hr = E_UNEXPECTED; r.reason = Reason::Exhausted; return r; }
    r.target = ++counter;
    r.hr = io.Signal(r.target);
    if (r.hr != S_OK) { r.hr = Refusal(r.hr); r.reason = Reason::Signal; return r; }
    r.submitted = true; // Receipt for THIS queue/fence/target, never inferred from an old log.
    r.completed = io.Completed();
    if (r.completed == removed)
    { r.hr = DXGI_ERROR_DEVICE_REMOVED; r.reason = Reason::Device; return r; }
    if (r.completed < r.target)
    {
        r.hr = io.Arm(r.target);
        if (r.hr != S_OK) { r.hr = Refusal(r.hr); r.reason = Reason::Arm; return r; }
        r.wait = io.Wait(); // The production adapter preserves the existing 5000ms budget.
        if (r.wait != WAIT_OBJECT_0)
        { r.hr = DXGI_ERROR_WAS_STILL_DRAWING; r.reason = Reason::Wait; return r; }
        r.completed = io.Completed();
    }
    // Wakeup is only a hint: old registrations, shared wakes and removal grant no authority.
    if (r.completed == removed)
    { r.hr = DXGI_ERROR_DEVICE_REMOVED; r.reason = Reason::Device; return r; }
    if (r.completed < r.target)
    { r.hr = DXGI_ERROR_WAS_STILL_DRAWING; r.reason = Reason::Incomplete; return r; }
    r.hr = io.DeviceStatus();
    if (r.hr != S_OK) { r.hr = Refusal(r.hr); r.reason = Reason::Device; return r; }
    r.reason = Reason::Complete;
    return r;
}

// One incoming caller reference is consumed on EVERY branch. Only the explicit F5 pin may follow it.
// Counts returned by COM are advisory: conservative refusal retains our pin, never consumes other refs.
template<class Ops> ULONG ReleaseCaller(Ops& ops)
{
    const ULONG remaining = ops.ReleaseOriginal();
    if (remaining == 0) { ops.Quarantine(); return remaining; }
    if (remaining != 1 || !ops.MayRetire()) return remaining;
    if (!ops.DrainAndRetire()) { ops.Quarantine(); return remaining; }
    return ops.ReleaseOwnedPin();
}

// A single-queue owner cannot authorize future retirement against a different ResizeBuffers1 queue.
// Leave an unsupported queue transition to the caller as an actual HRESULT failure, before mutation.
template<class Same> HRESULT PresentQueues(UINT count, IUnknown* const* queues, Same same)
{
    if (!queues) return S_OK; // No queue array supplied: let the real API validate its unchanged/default route.
    if (count == 0 || count > DXGI_MAX_SWAP_CHAIN_BUFFERS) return DXGI_ERROR_INVALID_CALL;
    for (UINT i = 0; i < count; ++i)
        if (!queues[i] || !same(queues[i])) return DXGI_ERROR_UNSUPPORTED;
    return S_OK;
}
}

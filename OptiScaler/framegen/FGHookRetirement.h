#pragma once
#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <mutex>
#include <utility>

class DLSSG_Dx12;
// Pending is an accepted, resumable retirement, not a completion or a terminal failure.
enum class FGHookRetirementState { Pending, Refused, Quarantined, Completed };
struct FGHookRetirementResult
{
    FGHookRetirementState state = FGHookRetirementState::Refused;
    HRESULT hr = DXGI_ERROR_UNSUPPORTED;
};
struct FGHookRetirementRequest
{
    const void* owner = nullptr;
    const void* provider = nullptr;
    IDXGISwapChain* chain = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12Device* device = nullptr;
    ID3D12Fence* fence = nullptr;
    HWND window = nullptr;
    UINT64 serial = 0;
    bool terminal = false; // Only final Release/shutdown may use a bounded CPU wait.
    bool Known() const noexcept
    {
        return owner && provider && chain && queue && device && fence && window && serial && serial != UINT64_MAX;
    }
    bool SameOwner(const FGHookRetirementRequest& b) const noexcept
    {
        return Known() && b.Known() && owner == b.owner && provider == b.provider && chain == b.chain &&
            queue == b.queue && device == b.device && fence == b.fence && window == b.window && serial == b.serial;
    }
    bool Same(const FGHookRetirementRequest& b) const noexcept
    {
        return SameOwner(b) && terminal == b.terminal;
    }
};

// Only an implementing provider can mint success. The execution lease remains held through
// the hook's final queue check, observer retirement and exactly one F5-owned pin Release.
// This is the public host teardown contract, not a private-worker/global-quiescence claim.
class FGHookRetirementReceipt final
{
    friend class DLSSG_Dx12;
    HRESULT _hr = DXGI_ERROR_UNSUPPORTED;
    FGHookRetirementState _state = FGHookRetirementState::Refused;
    FGHookRetirementRequest _binding;
    UINT64 _providerOwner = 0, _epoch = 0;
    DWORD _thread = 0;
    const UINT64* _epochSource = nullptr; // Borrowed only while the execution lease is owned.
    std::unique_lock<std::recursive_mutex> _execution;
    FGHookRetirementReceipt(const FGHookRetirementRequest& binding, UINT64 providerOwner, UINT64 epoch,
                            const UINT64* epochSource, std::unique_lock<std::recursive_mutex>&& execution)
        : _hr(S_OK), _state(FGHookRetirementState::Completed), _binding(binding), _providerOwner(providerOwner), _epoch(epoch),
          _thread(GetCurrentThreadId()), _epochSource(epochSource), _execution(std::move(execution)) {}
  public:
    FGHookRetirementReceipt() = default;
    FGHookRetirementReceipt(FGHookRetirementReceipt&&) = default;
    FGHookRetirementReceipt& operator=(FGHookRetirementReceipt&&) = default;
    FGHookRetirementReceipt(const FGHookRetirementReceipt&) = delete;
    FGHookRetirementReceipt& operator=(const FGHookRetirementReceipt&) = delete;
    static FGHookRetirementReceipt Refused(HRESULT hr)
    {
        FGHookRetirementReceipt r; r._hr = FAILED(hr) ? hr : E_UNEXPECTED; return r;
    }
    static FGHookRetirementReceipt Pending()
    {
        FGHookRetirementReceipt r; r._state = FGHookRetirementState::Pending;
        r._hr = DXGI_ERROR_WAS_STILL_DRAWING; return r;
    }
    static FGHookRetirementReceipt Quarantined(HRESULT hr)
    {
        auto r = Refused(hr); r._state = FGHookRetirementState::Quarantined; return r;
    }
    HRESULT Status() const noexcept { return _hr; }
    FGHookRetirementResult Result() const noexcept { return { _state, _hr }; }
    bool Authorizes(const FGHookRetirementRequest& request) const noexcept
    {
        return _state == FGHookRetirementState::Completed && _hr == S_OK && _providerOwner && _providerOwner != UINT64_MAX && _epoch && _epoch != UINT64_MAX &&
            _thread == GetCurrentThreadId() && _execution.owns_lock() && _epochSource && *_epochSource == _epoch &&
            _binding.Same(request);
    }
};

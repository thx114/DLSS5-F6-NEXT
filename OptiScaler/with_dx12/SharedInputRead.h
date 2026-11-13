#pragma once
#include <d3d11_4.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <memory>
#include <mutex>
#include <runtime/D3D12DeviceIdentity.h>

namespace interop {
// A single prepared cache generation. Retains both API objects while the owned
// reader runs; permission to overwrite comes from the actual return GPU Wait.
class SharedInputRead final {
    mutable std::mutex mutex;
    Microsoft::WRL::ComPtr<ID3D12Fence> completion;
    uint64_t completionValue = 0;
    bool active = true, returned = false, quarantined = false;
    void (*blockWrites)() noexcept;
public:
    Microsoft::WRL::ComPtr<ID3D12Resource> color, depth;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> color11, depth11, sourceColor11, sourceDepth11;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
    Microsoft::WRL::ComPtr<ID3D12Fence> ready;
    uint64_t readyValue = 0, cacheFrame = 0, stream = 0, generation = 0, frame = 0;
    explicit SharedInputRead(void (*block)() noexcept) : blockWrites(block) {}
    bool Current() const noexcept {
        std::lock_guard lock(mutex);
        return active && !returned && !quarantined;
    }
    bool CanOverwrite() const noexcept {
        std::lock_guard lock(mutex);
        if (quarantined) return false;
        if (!active || returned) return true;
        if (!completion || !completionValue) return false;
        const auto value = completion->GetCompletedValue();
        return value != UINT64_MAX && value >= completionValue;
    }
    bool BindCompletion(ID3D12Fence* fence, uint64_t value) noexcept {
        std::lock_guard lock(mutex);
        Microsoft::WRL::ComPtr<ID3D12Device> expected, actual;
        if (!active || quarantined || !fence || !value || value == UINT64_MAX ||
            !queue || FAILED(queue->GetDevice(IID_PPV_ARGS(&expected))) ||
            FAILED(fence->GetDevice(IID_PPV_ARGS(&actual))) || !D3D12DeviceIdentity::Same(expected.Get(), actual.Get()) ||
            FAILED(actual->GetDeviceRemovedReason()) || fence->GetCompletedValue() == UINT64_MAX ||
            (completion && (completion.Get() != fence || completionValue != value))) return false;
        completion = fence; completionValue = value; return true;
    }
    // Caller queues the existing shared-fence Signal/Wait after the reader.
    bool ReturnedAfterGpuWait() noexcept {
        std::lock_guard lock(mutex);
        if (!completion || !completionValue || quarantined) return false;
        returned = true; return true;
    }
    void CancelUntouched() noexcept { std::lock_guard lock(mutex); if (!completion) active = false; }
    void Quarantine() noexcept {
        { std::lock_guard lock(mutex); quarantined = true; }
        blockWrites();
    }
};
using SharedInputReadRef = std::shared_ptr<SharedInputRead>;
}

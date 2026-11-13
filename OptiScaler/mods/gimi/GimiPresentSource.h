#pragma once
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>

namespace gimi::runtime {
// CPU admission only. Capture generations and submitted GPU packets keep their
// own ownership. Selecting a source never grants GPU completion.
struct NativePresentSource final {
    Microsoft::WRL::ComPtr<IDXGISwapChain> chain;
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    uint64_t generation = 0;
    std::atomic<bool> live {true};
};
using NativePresentSourcePtr = std::shared_ptr<const NativePresentSource>;

class PresentSources final {
    std::mutex mutex_;
    std::shared_ptr<NativePresentSource> selected_;
    uint64_t generation_ = 0;
public:
    void Select(IDXGISwapChain* chain, ID3D11Device* device, ID3D11DeviceContext* context) {
        if (!chain || !device || !context) return;
        std::lock_guard lock(mutex_);
        if (selected_ && selected_->chain.Get() == chain && selected_->device.Get() == device &&
            selected_->context.Get() == context) return;
        auto next = std::make_shared<NativePresentSource>();
        next->chain = chain; next->device = device; next->context = context;
        next->generation = ++generation_;
        if (selected_) selected_->live.store(false, std::memory_order_release);
        selected_ = std::move(next);
    }
    NativePresentSourcePtr Acquire(IDXGISwapChain* chain) {
        std::lock_guard lock(mutex_);
        return selected_ && selected_->chain.Get() == chain ? selected_ : nullptr;
    }
    void Forget(IDXGISwapChain* chain) {
        std::lock_guard lock(mutex_);
        if (selected_ && selected_->chain.Get() == chain) {
            selected_->live.store(false, std::memory_order_release);
            selected_.reset();
        }
    }
};
// Registry replacement and semantic-owner confirmation share runtime's owner
// mutex. Acquire only retains a token; it never confirms a capture by itself.
bool SelectPresentSource(IDXGISwapChain*,ID3D11Device*,ID3D11DeviceContext*) noexcept;
NativePresentSourcePtr AcquirePresentSource(IDXGISwapChain*) noexcept;
void ForgetPresentSource(IDXGISwapChain*) noexcept;
}

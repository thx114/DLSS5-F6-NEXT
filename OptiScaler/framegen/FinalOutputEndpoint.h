#pragma once
#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <memory>
#include <functional>

// One owner of physical-output admission, tokens, completion and resize.
// Menu/State registries are deliberately not producer authority.
namespace framegen::final_output
{
class Provider;
std::shared_ptr<Provider> CreateProvider() noexcept;
void BindInput(const std::shared_ptr<Provider>&, IDXGISwapChain*) noexcept;
void RetireProvider(std::shared_ptr<Provider>&) noexcept;
// Closing admission persists on pending/failure. No wait and no implicit rollback to live.
// Commit rechecks all endpoint tails after the provider's public host teardown.
class ProviderRetirement final
{
    std::shared_ptr<Provider> provider;
    uint64_t owner = 0, epoch = 0;
    HRESULT status = DXGI_ERROR_UNSUPPORTED;
    friend ProviderRetirement TryRetireProvider(const std::shared_ptr<Provider>&, IDXGISwapChain*) noexcept;
public:
    ProviderRetirement() = default;
    ProviderRetirement(ProviderRetirement&&) = default;
    ProviderRetirement& operator=(ProviderRetirement&&) = default;
    ProviderRetirement(const ProviderRetirement&) = delete;
    HRESULT Status() const noexcept { return status; }
    HRESULT Commit() noexcept;
};
ProviderRetirement TryRetireProvider(const std::shared_ptr<Provider>&, IDXGISwapChain*) noexcept;
class CreationScope final
{
    std::shared_ptr<Provider> previous, activity;
public:
    explicit CreationScope(const std::shared_ptr<Provider>&) noexcept;
    ~CreationScope();
    CreationScope(const CreationScope&) = delete;
};
// Observe the real source before its natural Present, without suppressing it.
// Cross-API source/output runtimes remain distinct and cannot borrow a ticket
// to suppress each other. No source color/guide/FG-input mutation.
void PrepareSource(IDXGISwapChain* input, IDXGISwapChain* nativeSource,
                   UINT flags, const DXGI_PRESENT_PARAMETERS*) noexcept;
// Genshin-only callsite authority: constructed AFTER successful private copy,
// real queue Wait and FinishedPicture, BEFORE native parent Present. Neither a
// bound child nor the native Present forwarder grants this authority.
class CopiedSourceScope final
{
    std::shared_ptr<Provider> provider;
    uint64_t serial = 0;
public:
    CopiedSourceScope(IDXGISwapChain* input, IDXGISwapChain* nativeSource, bool copied,
        ID3D12CommandQueue* copyQueue, ID3D12Fence* copyFence, uint64_t copyValue,
        uint64_t copyOwner, uint64_t copyGeneration, uint64_t copySerial) noexcept;
    ~CopiedSourceScope();
    void ParentPresented(HRESULT) noexcept;
    // True while disabled; otherwise every observed physical output must have
    // actual techniques, owned submission and raw S_OK. Zero outputs is failure.
    bool Filtered() const noexcept;
    CopiedSourceScope(const CopiedSourceScope&) = delete;
};
class InputScope final
{
    std::shared_ptr<Provider> provider;
public:
    InputScope(IDXGISwapChain*, UINT flags, const DXGI_PRESENT_PARAMETERS*) noexcept;
    ~InputScope();
    InputScope(const InputScope&) = delete;
};
// CPU output identity from an F5-owned creation scope and matching DIRECT queue.
// Independent of filter format/pool admission; no GPU access, provider reference
// or consumer authority escapes this call.
bool CaptureProviderOutputBinding(IDXGISwapChain*, IUnknown* creationQueue) noexcept;
class Endpoint final
{
    struct State;
    std::shared_ptr<State> state;
public:
    static void CollectRetired() noexcept;
    // Internal retirement seam: caller has closed provider admission and checked CPU scopes.
    static HRESULT TryRetire(const std::shared_ptr<Provider>&, bool commit) noexcept;
    // Capture only in an F5-owned DLSSG factory creation scope, with the raw
    // DXGI chain and its actual creation DIRECT queue. No opaque/proxy fallback.
    static std::unique_ptr<Endpoint> Capture(IDXGISwapChain*, IUnknown* creationQueue) noexcept;
    ~Endpoint();
    Endpoint(const Endpoint&) = delete;
    // The callback performs existing overlay followed by the actual raw API.
    // Untouched refusal preserves its HRESULT/flags exactly. After uncertain
    // writes, quarantine refuses consumers; it never fabricates device removal.
    HRESULT Present(UINT flags, const DXGI_PRESENT_PARAMETERS*,
                    const std::function<HRESULT()>& rawAndOverlay) noexcept;
    // Only the real retained Genshin child calls this seam. Source provenance
    // and actual physical completion apply even when FinalOutput is disabled.
    // The override flags are used only on the raw physical flip chain to prime
    // hidden content; SDK input and native parent flags remain unchanged.
    HRESULT PresentGenshin(UINT flags,const DXGI_PRESENT_PARAMETERS*,
                           const std::function<HRESULT(UINT)>& rawAndOverlay) noexcept;
    void SetColorSpace(DXGI_COLOR_SPACE_TYPE) noexcept;
#ifdef HOYOSHADE_STAGE_TEST
    friend struct RetirementFixture;
    struct Observation { uint64_t sequence, signals, techniques; bool submitted, suppressed, retryAmbiguous, quarantined, retired; uint64_t restores; uint64_t originalCreates = 0, allocatorCreates = 0, listCreates = 0; bool contentBlocked = false; uint64_t generation = 0; };
    Observation Observe() const noexcept;
    void FailNextTailSignalForTest() noexcept;
#endif
    // Reserve the entire resize (including early returns) without waiting on
    // a parent/provider lock. A failed resize retains the old queue binding.
    class Resize final
    {
        Endpoint* endpoint = nullptr;
        std::shared_ptr<Provider> activity;
        bool admitted = true;
    public:
        explicit Resize(Endpoint*) noexcept;
        ~Resize();
        explicit operator bool() const noexcept { return admitted; }
        // Only a tail-proven reservation may bypass its OWN content denial;
        // unrelated quarantine/writers/content holds still reject resize.
        bool ConsumersSafe(IDXGISwapChain*) const noexcept;
        void Complete(HRESULT, UINT count = 0, const UINT* nodes = nullptr,
                      IUnknown* const* queues = nullptr, bool resize1 = false) noexcept;
        Resize(const Resize&) = delete;
    };
private:
    HRESULT PresentImpl(UINT,const DXGI_PRESENT_PARAMETERS*,const std::function<HRESULT()>&,
                        UINT* genshinRawFlags) noexcept;
    Endpoint() = default;
};
}

// Real product adapter bodies are inserted by test_fg_hook_install.py.
// Windows/COM/Detours below are controlled CPU doubles, never SDK/GPU calls.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <thread>
#include <utility>
#include <vector>
// Extracted production bodies use RAII/parameters which the CPU doubles make
// empty. Keep all other /W4 diagnostics fatal without rewriting those bodies.
#pragma warning(disable: 4100 4101 4189)

#undef IID_PPV_ARGS
#define IID_PPV_ARGS(value) value
constexpr UINT DXGI_SWAP_EFFECT_SEQUENTIAL = 1, DXGI_SWAP_EFFECT_DISCARD = 0;
constexpr UINT DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL = 3, DXGI_SWAP_EFFECT_FLIP_DISCARD = 4;
constexpr UINT D3D12_FENCE_FLAG_NONE = 0;

unsigned checks = 0;
namespace ModuleLifetime
{
inline bool allowed = true;
inline bool CleanupAllowed() { return allowed; }
}
void Check(bool ok, const char* message) {
    ++checks;
    if (!ok) throw std::runtime_error(message);
}
struct FailureRow { std::string stage; LONG error; };
std::vector<FailureRow> errors;
template<class... Rest> void LogError(const char*, const char* stage, LONG error, Rest...) {
    errors.push_back({stage, error});
}
template<class... Rest> void LogError(const char*, LONG error, Rest...) {
    errors.push_back({"Abort", error});
}
inline void LogError(const char*, UINT) {} // Create quarantine diagnostic.
inline void LogError(const char*) {} // Other admission diagnostics.
#define LOG_ERROR(...) LogError(__VA_ARGS__)
#define LOG_INFO(...) ((void)0)
#define LOG_WARN(...) ((void)0)
#define LOG_DEBUG(...) ((void)0)
#define LOG_TRACE(...) ((void)0)

struct ID3D12Device;
struct ID3D12Fence;
struct ID3D12CommandQueue;
struct IUnknown;
struct IDXGIFactory {};
struct IDXGIOutput {};
struct DXGI_MODE { UINT Width = 1280, Height = 800, Format = 28; };
struct DXGI_SWAP_CHAIN_DESC {
    DXGI_MODE BufferDesc;
    UINT BufferCount = 2, Flags = 0, SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    HWND OutputWindow = reinterpret_cast<HWND>(1);
    BOOL Windowed = TRUE;
};
struct DXGI_SWAP_CHAIN_DESC1 {
    UINT Width = 1280, Height = 800, Format = 28, BufferCount = 2, Flags = 0;
    UINT SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
};
struct DXGI_SWAP_CHAIN_FULLSCREEN_DESC { BOOL Windowed = TRUE; };
unsigned addRefs = 0, releases = 0, binds = 0, providerCreates = 0, teardowns = 0, retirements = 0;
unsigned destroyedOwners = 0;
struct IUnknown {
    // Deliberately first: the production hook reads a COM vtable pointer here.
    void** table = nullptr;
    ULONG refs = 1;
    template<class T> HRESULT QueryInterface(T** out) {
        *out = reinterpret_cast<T*>(this);
        AddRef();
        return S_OK;
    }
    ULONG AddRef() { ++addRefs; return ++refs; }
    ULONG Release() { ++releases; return --refs; }
};
struct IDXGISwapChain : IUnknown {
    HRESULT ResizeBuffers(UINT, UINT, UINT, UINT, UINT) { return S_OK; }
};
using IDXGISwapChain1 = IDXGISwapChain;
struct ID3D12Fence : IUnknown {};
struct ID3D12Device : IUnknown {
    HRESULT GetDeviceRemovedReason() { return S_OK; }
    HRESULT CreateFence(UINT64, UINT, ID3D12Fence** out);
};
struct ID3D12CommandQueue : IUnknown {
    HRESULT GetDevice(ID3D12Device** out);
};
ID3D12Device device;
ID3D12Fence fence;
ID3D12CommandQueue queue;
IDXGISwapChain chain;
HRESULT ID3D12Device::CreateFence(UINT64, UINT, ID3D12Fence** out) { *out = &fence; return S_OK; }
HRESULT ID3D12CommandQueue::GetDevice(ID3D12Device** out) { ++binds; *out = &device; return S_OK; }
namespace Microsoft::WRL {
template<class T> struct ComPtr {
    T* value = nullptr;
    ComPtr& operator=(T* p) { value = p; return *this; }
    T** operator&() { return &value; }
    T* operator->() const { return value; }
    T* Get() const { return value; }
    explicit operator bool() const { return value != nullptr; }
};
}
enum class FGHookRetirementState { Completed, Pending, Refused, Quarantined };
struct FGHookRetirementResult { FGHookRetirementState state; HRESULT hr; };
struct FGHookRetirementReceipt { std::unique_lock<std::recursive_mutex> execution; };
std::recursive_mutex retirementExecution;
bool completeRetirement = false;
unsigned refuseAfterRelease = 0;
namespace fg_queue_idle { inline HRESULT Refusal(HRESULT hr) { return FAILED(hr) ? hr : E_FAIL; } }
enum class FGOutput { DLSSG, XeFG, FSRFG, NoFG };
enum class FGInput { Upscaler };
enum class GameEngineType { Unity, Other };
template<class T> struct Option {
    T value{};
    T value_or_default() const { return value; }
};
struct Config {
    Option<bool> FGPreserveSwapChain, FGDontUseSwapchainBuffers, SimulateWaitableObject;
    static Config* Instance() { static Config config; return &config; }
};
struct IFGFeature_Dx12 {
    virtual ~IFGFeature_Dx12() = default;
    bool CreateSwapchain(IDXGIFactory*, ID3D12CommandQueue*, DXGI_SWAP_CHAIN_DESC* desc,
                         IDXGISwapChain** out, bool) {
        ++providerCreates; *out = &chain; desc->BufferCount = 6; return true;
    }
    bool CreateSwapchain1(IDXGIFactory*, ID3D12CommandQueue*, HWND, DXGI_SWAP_CHAIN_DESC1* desc,
                          DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGISwapChain1** out, bool) {
        ++providerCreates; *out = &chain; desc->BufferCount = 6; return true;
    }
};
struct DLSSG_Dx12 : IFGFeature_Dx12 {};
struct FSRFG_Dx12 : IFGFeature_Dx12 {};
struct XeFG_Dx12 : IFGFeature_Dx12 {};
DLSSG_Dx12 provider;
struct State {
    bool isShuttingDown = false, externalFrameGeneration = false, skipHeapCapture = false;
    FGInput activeFgInput = FGInput::Upscaler;
    FGOutput activeFgOutput = FGOutput::DLSSG;
    GameEngineType gameEngine = GameEngineType::Other;
    IFGFeature_Dx12* currentFG = &provider;
    IDXGISwapChain* currentFGSwapchain = nullptr;
    IUnknown* currentSwapchain = nullptr;
    static State& Instance() { static State state; return state; }
};
struct Util { static auto ExePath() { return std::filesystem::path(L"StarRail.exe"); } };
struct ScopedSkipDxgiLoadChecks {};
std::recursive_mutex fgLifetimeMutex;
thread_local bool fgLifecycleCallback = false;
// INSERT_PRODUCT_CALLBACK
// INSERT_PRODUCT_OWNER
// INSERT_PRODUCT_GLOBALS

bool SameComObject(IUnknown* a, IUnknown* b) { return a == b; }
bool CheckForFGStatus(HWND) { return true; }
FGHookRetirementResult CompleteExistingFGTeardown(FGSwapchainOwner*, bool) {
    ++teardowns; return {FGHookRetirementState::Pending, DXGI_ERROR_WAS_STILL_DRAWING};
}
FGHookRetirementResult RetireFGResources(FGSwapchainOwner* owner, bool) {
    ++retirements;
    if (!completeRetirement) return {FGHookRetirementState::Pending, DXGI_ERROR_WAS_STILL_DRAWING};
    owner->retirement.execution = std::unique_lock<std::recursive_mutex>(retirementExecution);
    return {FGHookRetirementState::Completed, S_OK};
}
// INSERT_PRODUCT_PIN_RESULT
// INSERT_PRODUCT_RELEASE
// INSERT_PRODUCT_RETIRE
// INSERT_PRODUCT_CAPTURE

struct DetoursDouble {
    int failCall = 0, calls = 0, attachCalls = 0, begins = 0, commits = 0, aborts = 0;
    LONG error = ERROR_NOT_ENOUGH_MEMORY, abortError = NO_ERROR;
    bool active = false;
    LONG Next() {
        // Neither original slots nor completed Hook alone may publish Ready.
        Check(fgHookStatus.load() != S_OK, "Detour call observes unpublished readiness");
        Check(State::Instance().currentFGSwapchain != &chain, "Detour call precedes source publication");
        return ++calls == failCall ? error : NO_ERROR;
    }
} detours;
LONG DetourTransactionBegin() {
    ++detours.begins;
    const auto result = detours.Next();
    if (result != ERROR_INVALID_OPERATION) detours.active = true;
    return result;
}
LONG DetourUpdateThread(HANDLE) { return detours.Next(); }
LONG DetourAttach(PVOID*, PVOID) { ++detours.attachCalls; return detours.Next(); }
LONG DetourTransactionCommit() {
    ++detours.commits; detours.active = false; return detours.Next();
}
LONG DetourTransactionAbort() {
    ++detours.aborts;
    if (detours.abortError == NO_ERROR) detours.active = false;
    return detours.abortError;
}

struct FGHooks {
    using Generic = void(*)();
    using PFN_ResizeBuffers = Generic;
    using PFN_ResizeTarget = Generic;
    using PFN_ResizeBuffers1 = Generic;
    using PFN_SetFullscreenState = Generic;
    using PFN_GetFullscreenState = Generic;
    using PFN_GetFullscreenDesc = Generic;
    using PFN_Present = Generic;
    using PFN_Present1 = Generic;
    using PFN_GetFrameLatencyWaitableObject = Generic;
    using PFN_Release = ULONG(*)(IUnknown*);
    // INSERT_PRODUCT_SLOTS
    inline static HWND _hwnd = nullptr;
    inline static IDXGISwapChain* _dx12InteropPresentSC = nullptr;
    inline static HWND _dx12InteropPresentHwnd = nullptr;
    static void hkFGRelease() {}
    static void hkFGPresent() {}
    static void hkResizeTarget() {}
    static void hkResizeBuffers() {}
    static void hkSetFullscreenState() {}
    static void hkFGPresent1() {}
    static void hkResizeBuffers1() {}
    static void hkGetFullscreenState() {}
    static void hkGetFullscreenDesc() {}
    static void hkGetFrameLatencyWaitableObject() {}
    static HRESULT HookFGSwapchain(IDXGISwapChain*);
    static HRESULT SetFGSwapchain(IDXGISwapChain*, HWND);
    static HRESULT SwapchainHookStatus() noexcept;
    static HRESULT CreateSwapChain(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**,
                                   bool* = nullptr);
    static HRESULT CreateSwapChainForHwnd(IDXGIFactory*, IUnknown*, HWND, DXGI_SWAP_CHAIN_DESC1*,
                                          DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**,
                                          bool* = nullptr);
};
// INSERT_PRODUCT_QUERY
// INSERT_PRODUCT_HOOK
// INSERT_PRODUCT_SET
// INSERT_PRODUCT_CREATE
// INSERT_PRODUCT_CREATE_HWND

void Dummy() {}
ULONG OriginalRelease(IUnknown* object) {
    const auto remaining = object->Release();
    if (refuseAfterRelease && releases == refuseAfterRelease) ModuleLifetime::allowed = false;
    return remaining;
}
void* table[40];
void Reset(FGOutput output = FGOutput::DLSSG) {
    if (fgOwner) {
        // Fixture reset only, after assertions; production deliberately retains
        // quarantined owners through process teardown. No GPU exists here.
        delete fgOwner; fgOwner = nullptr; ++destroyedOwners;
    }
    State::Instance() = State{};
    State::Instance().activeFgOutput = output;
    *Config::Instance() = Config{};
    chain.refs = 1;
    for (auto& entry : table) entry = reinterpret_cast<void*>(Dummy);
    table[2] = reinterpret_cast<void*>(OriginalRelease);
    chain.table = table;
    FGHooks::o_FGRelease = nullptr;
    FGHooks::o_FGSCPresent = nullptr;
    FGHooks::o_FGSCSetFullscreenState = nullptr;
    FGHooks::o_FGSCGetFullscreenState = nullptr;
    FGHooks::o_FGSCResizeBuffers = nullptr;
    FGHooks::o_FGSCResizeTarget = nullptr;
    FGHooks::o_FGSCGetFullscreenDesc = nullptr;
    FGHooks::o_FGSCPresent1 = nullptr;
    FGHooks::o_FGSCResizeBuffers1 = nullptr;
    FGHooks::o_FGSCGetFrameLatencyWaitableObject = nullptr;
    FGHooks::_fgHooksInstalled = false;
    FGHooks::_fgHookInstallFailure = S_OK;
    FGHooks::_hwnd = reinterpret_cast<HWND>(17);
    FGHooks::_dx12InteropPresentSC = &chain;
    FGHooks::_dx12InteropPresentHwnd = reinterpret_cast<HWND>(19);
    fgHookStatus.store(HRESULT_FROM_WIN32(ERROR_NOT_READY));
    fgLifecycleCallback = false;
    ModuleLifetime::allowed = true;
    completeRetirement = false;
    refuseAfterRelease = 0;
    addRefs = releases = binds = providerCreates = teardowns = retirements = 0;
    detours = {};
    errors.clear();
}
HRESULT Create(bool hwnd, IDXGISwapChain** out, bool* sourceOwner, UINT* count) {
    if (hwnd) {
        DXGI_SWAP_CHAIN_DESC1 desc;
        const auto hr = FGHooks::CreateSwapChainForHwnd(nullptr, &queue, reinterpret_cast<HWND>(1),
                                                       &desc, nullptr, nullptr, out, sourceOwner);
        *count = desc.BufferCount;
        return hr;
    }
    DXGI_SWAP_CHAIN_DESC desc;
    const auto hr = FGHooks::CreateSwapChain(nullptr, &queue, &desc, out, sourceOwner);
    *count = desc.BufferCount;
    return hr;
}
void CheckUnpublished() {
    Check(State::Instance().currentFGSwapchain == nullptr, "no current FG chain published");
    Check(State::Instance().currentSwapchain == nullptr, "no current chain published");
    Check(FGHooks::_hwnd == reinterpret_cast<HWND>(17), "window publication unchanged");
    Check(FGHooks::_dx12InteropPresentSC == &chain &&
          FGHooks::_dx12InteropPresentHwnd == reinterpret_cast<HWND>(19), "interop fallback unchanged");
    Check(!FGHooks::_fgHooksInstalled, "failed hook is not installed");
}
int main() try {
    unsigned scenarios = 0;
    for (const auto output : {FGOutput::DLSSG, FGOutput::XeFG}) {
        const int attaches = output == FGOutput::XeFG ? 10 : 7;
        const int totalCalls = attaches + 3; // Begin, Update, Attach..., Commit.
        for (bool hwnd : {false, true}) {
            for (int failCall = 1; failCall <= totalCalls; ++failCall) {
                ++scenarios;
                Reset(output);
                Config::Instance()->SimulateWaitableObject.value = true;
                detours.failCall = failCall;
                IDXGISwapChain* out = reinterpret_cast<IDXGISwapChain*>(1);
                bool sourceOwner = true;
                UINT count = 0;
                Check(Create(hwnd, &out, &sourceOwner, &count) == HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_MEMORY),
                      "Create propagates exact failing Detour HRESULT");
                Check(out == nullptr && !sourceOwner && count == 2, "failure clears output and preserves descriptor/receipt");
                CheckUnpublished();
                Check(fgOwner && fgOwner->quarantined && fgOwner->pin == &chain &&
                      fgOwner->unpublishedCreateReference == &chain, "both SDK references retained in quarantine");
                Check(chain.refs == 2 && releases == 0, "no chain release or guessed retirement");
                Check(fgOwner->queue.Get() == &queue && fgOwner->device.Get() == &device &&
                      fgOwner->fence.Get() == &fence && fgOwner->event != nullptr,
                      "actual bound queue/device/fence/event retained");
                Check(fgOwner->queueTarget == 0 && fgOwner->value == 0 &&
                      fgOwner->retireStage == FGSwapchainOwner::RetireStage::Idle,
                      "no fabricated GPU completion or changed retirement gate");
                Check(FGHooks::SwapchainHookStatus() == HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_MEMORY),
                      "atomic query preserves original install error");
                Check(errors.size() == 1 && errors.front().error == ERROR_NOT_ENOUGH_MEMORY,
                      "failed stage and original Win32 code recorded once");
                const std::vector<std::string> stages = output == FGOutput::XeFG ?
                    std::vector<std::string>{"Begin", "UpdateThread", "Attach.Release", "Attach.Present",
                        "Attach.ResizeTarget", "Attach.ResizeBuffers", "Attach.SetFullscreenState",
                        "Attach.Present1", "Attach.ResizeBuffers1", "Attach.GetFullscreenState",
                        "Attach.GetFullscreenDesc", "Attach.GetFrameLatencyWaitableObject", "Commit"} :
                    std::vector<std::string>{"Begin", "UpdateThread", "Attach.Release", "Attach.Present",
                        "Attach.ResizeTarget", "Attach.ResizeBuffers", "Attach.SetFullscreenState",
                        "Attach.Present1", "Attach.ResizeBuffers1", "Commit"};
                Check(errors.front().stage == stages[static_cast<size_t>(failCall - 1)], "correct failing stage identified");
                Check(detours.calls == failCall, "transaction stops at first failed call");
                Check(detours.aborts == ((failCall < totalCalls) ? 1 : 0),
                      "abort only owned unfinished transaction");
                Check(!detours.active, "owned Begin failure and unfinished transaction are rolled back");
                Check(FGHooks::o_FGRelease == nullptr && FGHooks::o_FGSCPresent == nullptr,
                      "known rollback clears original pointers");
                const auto beforeCalls = detours.calls;
                Check(FGHooks::HookFGSwapchain(&chain) == HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_MEMORY) &&
                      detours.calls == beforeCalls, "terminal install error never retries hooks");
                for (bool nextHwnd : {false, true}) {
                    Config::Instance()->FGPreserveSwapChain.value = true;
                    Check(Create(nextHwnd, &out, &sourceOwner, &count) == DXGI_ERROR_WAS_STILL_DRAWING,
                          "repeat Create refuses before preserve/retire/replacement");
                }
                Check(providerCreates == 1 && releases == 0 && teardowns == 0 && retirements == 0,
                      "repeat Create keeps exact provider/owner and completion gates");
                Check(RetireFGForCreate(fgOwner, static_cast<FGHooks::PFN_Release>(nullptr)) ==
                      DXGI_ERROR_WAS_STILL_DRAWING, "Retire refuses null original Release");
                Check(releases == 0 && teardowns == 0, "null release rejection precedes provider and refcount probes");
            }
            ++scenarios;
            Reset(output);
            Config::Instance()->SimulateWaitableObject.value = true;
            IDXGISwapChain* out = nullptr;
            bool sourceOwner = false;
            UINT count = 0;
            Check(Create(hwnd, &out, &sourceOwner, &count) == S_OK, "normal Create succeeds");
            Check(out == &chain && count == 6 && !fgOwner->quarantined && chain.refs == 2,
                  "successful caller reference and one F5 pin unchanged");
            Check(fgOwner->unpublishedCreateReference == nullptr, "success delivers create reference");
            Check(State::Instance().currentFGSwapchain == &chain && State::Instance().currentSwapchain == &chain &&
                  FGHooks::_fgHooksInstalled && FGHooks::SwapchainHookStatus() == S_OK,
                  "success publishes only committed source and readiness");
            Check(sourceOwner == (output == FGOutput::DLSSG), "StarRail latency receipt requires successful matching provider");
            Check(detours.calls == totalCalls && detours.commits == 1 && detours.aborts == 0,
                  "normal provider hook selection and one transaction preserved");
            const auto beforeCalls = detours.calls;
            Check(FGHooks::SetFGSwapchain(&chain, reinterpret_cast<HWND>(1)) == S_OK &&
                  detours.calls == beforeCalls, "successful install reused without duplicate Detours");
        }
    }
    ++scenarios;
    Reset();
    detours.failCall = 3;
    detours.abortError = ERROR_INVALID_OPERATION;
    IDXGISwapChain* out = nullptr;
    bool sourceOwner = false;
    UINT count = 0;
    Check(Create(false, &out, &sourceOwner, &count) == HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_MEMORY),
          "abort failure preserves primary failure HRESULT");
    CheckUnpublished();
    Check(errors.size() == 2 && errors.back().stage == "Abort" && errors.back().error == ERROR_INVALID_OPERATION,
          "abort error separately recorded");
    Check(FGHooks::o_FGRelease != nullptr && !FGHooks::_fgHooksInstalled && fgOwner->quarantined,
          "unknown rollback retains original storage without readiness");
    Check(detours.active, "failed Abort can leave the owned transaction active");
    Check(Create(true, &out, &sourceOwner, &count) == DXGI_ERROR_WAS_STILL_DRAWING && providerCreates == 1,
          "unknown rollback cannot retry or replace retained owner");

    ++scenarios;
    Reset();
    detours.active = true;
    detours.failCall = 1;
    detours.error = ERROR_INVALID_OPERATION;
    Check(Create(false, &out, &sourceOwner, &count) == HRESULT_FROM_WIN32(ERROR_INVALID_OPERATION),
          "Begin rejection propagates without stealing a pre-existing transaction");
    Check(detours.active && detours.aborts == 0 && detours.commits == 0,
          "Begin rejection leaves pre-existing transaction untouched");
    CheckUnpublished();

    ++scenarios;
    Reset();
    Check(FGHooks::SwapchainHookStatus() == HRESULT_FROM_WIN32(ERROR_NOT_READY), "initial status remains distinct");
    Check(FGHooks::SetFGSwapchain(nullptr, nullptr) == E_INVALIDARG, "null Set rejects");
    Check(FGHooks::HookFGSwapchain(nullptr) == E_INVALIDARG, "null Hook rejects");
    table[8] = nullptr;
    Check(FGHooks::SetFGSwapchain(&chain, nullptr) == HRESULT_FROM_WIN32(ERROR_INVALID_ADDRESS),
          "missing required vtable entry refuses before transaction");
    Check(detours.calls == 0, "invalid target never begins Detours");
    CheckUnpublished();

    ++scenarios;
    Reset();
    FGSwapchainOwner owner;
    owner.pin = &chain;
    Check(RetireFGForCreate(&owner, static_cast<FGHooks::PFN_Release>(nullptr)) == DXGI_ERROR_WAS_STILL_DRAWING &&
          addRefs == 0 && teardowns == 0 && retirements == 0, "nonquarantined null Release rejected before AddRef/teardown");
    Check(RetireFGForCreate(static_cast<FGSwapchainOwner*>(nullptr), OriginalRelease) == E_INVALIDARG,
          "null retire owner rejected");
    Check(FGHooks::SetFGSwapchain(&chain, reinterpret_cast<HWND>(1)) == S_OK, "Set success available for retirement test");
    auto* retired = new FGSwapchainOwner;
    retired->pin = &chain;
    fgOwner = retired;
    const auto beforeRelease = releases;
    const auto beforeOwners = destroyedOwners;
    ModuleLifetime::allowed = false;
    retired->retirement.execution = std::unique_lock<std::recursive_mutex>(retirementExecution);
    const auto refusedPin = ReleaseFGPin(retired, OriginalRelease);
    Check(!refusedPin.released && refusedPin.references == 1 && releases == beforeRelease &&
          destroyedOwners == beforeOwners && fgOwner == retired && retired->pin == &chain &&
          State::Instance().currentFGSwapchain == &chain && FGHooks::SwapchainHookStatus() == S_OK,
          "loader/exit refusal preserves complete owner, publication and F5 pin before any release");
    bool unlocked = false;
    std::thread([&] {
        std::unique_lock check(retirementExecution, std::try_to_lock);
        unlocked = check.owns_lock();
    }).join();
    Check(unlocked && !retired->retirement.execution.owns_lock(),
          "refused pin release drops the current-thread CPU lease before returning");
    ModuleLifetime::allowed = true;
    ReleaseFGPin(retired, OriginalRelease);
    Check(fgOwner == nullptr && State::Instance().currentFGSwapchain == nullptr &&
          FGHooks::SwapchainHookStatus() == HRESULT_FROM_WIN32(ERROR_NOT_READY), "real pin release clears source readiness");

    ++scenarios;
    Reset();
    auto* lateRefusal = new FGSwapchainOwner;
    lateRefusal->pin = &chain;
    lateRefusal->retireStage = FGSwapchainOwner::RetireStage::Ready;
    fgOwner = lateRefusal;
    State::Instance().currentFGSwapchain = &chain;
    fgHookStatus.store(S_OK);
    completeRetirement = true;
    refuseAfterRelease = 1; // Exit begins in the final balanced AddRef probe.
    Check(RetireFGForCreate(lateRefusal, OriginalRelease) == DXGI_ERROR_WAS_STILL_DRAWING,
          "Create must propagate a final pin-release refusal after successful retirement");
    Check(fgOwner == lateRefusal && lateRefusal->pin == &chain && chain.refs == 1 &&
          State::Instance().currentFGSwapchain == &chain && FGHooks::SwapchainHookStatus() == S_OK,
          "late refusal preserves the entire original publication and exactly one pin");
    unlocked = false;
    std::thread([&] {
        std::unique_lock check(retirementExecution, std::try_to_lock);
        unlocked = check.owns_lock();
    }).join();
    Check(unlocked && !lateRefusal->retirement.execution.owns_lock(),
          "Create refusal never carries a same-thread execution lease to another call");
    ModuleLifetime::allowed = true;
    refuseAfterRelease = 0;
    Check(RetireFGForCreate(lateRefusal, OriginalRelease) == S_OK && !fgOwner && chain.refs == 0,
          "a later ordinary retry can retire and release exactly the retained pin");
    std::printf("PASS %u real FG Hook/Set/Create/Retire CPU scenarios, %u checks; SDK/GPU/game untested\n",
                scenarios, checks);
    return 0;
} catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL after %u checks: %s\n", checks, error.what());
    return 1;
}

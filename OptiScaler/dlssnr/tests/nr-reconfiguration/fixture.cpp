// CPU fixture for the actual Late retirement helper and recording lifetime.
// Run prepare.py --emit before Lead compiles this file as C++20. No D3D SDK,
// driver, game, compiler subprocess, runner modification or network is used.
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <initializer_list>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using UINT64 = uint64_t;
using HRESULT = int32_t;
constexpr HRESULT S_OK = 0;
constexpr HRESULT E_FAIL = -2147467259;
inline bool FAILED(HRESULT result) { return result < 0; }
std::vector<std::string> events;
unsigned warnings = 0, resetCallbacks = 0, retiredTicks = 0, checks = 0, scenarios = 0;
bool deliverResetCallback = true, deliverResetHook = false;
#define LOG_WARN(...) (++warnings)
#define LOG_INFO(...) ((void)0)

void Require(bool condition, const char* message)
{
    ++checks;
    if (!condition) throw std::runtime_error(message);
}

// Observable CPU operands. No object is a real GPU witness. Objects stay owned
// by the scenario; intrusive references expose production RetireOwners calls.
struct IUnknown
{
    unsigned references = 1;
    unsigned AddRef() { return ++references; }
    unsigned Release()
    {
        if (references == 0) std::terminate();
        return --references;
    }
};
namespace Microsoft::WRL
{
template<class T> class ComPtr
{
    T* value_ = nullptr;
public:
    ComPtr() = default;
    ComPtr(T* value) : value_(value) { if (value_) value_->AddRef(); }
    ComPtr(const ComPtr& other) : ComPtr(other.value_) {}
    ComPtr(ComPtr&& other) noexcept : value_(std::exchange(other.value_, nullptr)) {}
    ~ComPtr() { Reset(); }
    ComPtr& operator=(T* value)
    {
        if (value) value->AddRef();
        Reset(); value_ = value; return *this;
    }
    ComPtr& operator=(const ComPtr& other) { return *this = other.value_; }
    ComPtr& operator=(ComPtr&& other) noexcept
    {
        if (this != &other) { Reset(); value_ = std::exchange(other.value_, nullptr); }
        return *this;
    }
    void Reset() { if (auto* value = std::exchange(value_, nullptr)) value->Release(); }
    T* Get() const { return value_; }
    T* operator->() const { return value_; }
    explicit operator bool() const { return value_ != nullptr; }
};
}
struct ID3D12Fence : IUnknown
{
    UINT64 completed = 0;
    unsigned polls = 0;
    UINT64 GetCompletedValue() { ++polls; return completed; }
};
struct ID3D12CommandQueue : IUnknown {};
struct ID3D12CommandList;
void FinishedPictureResetCommandList(ID3D12CommandList* list);
struct ID3D12CommandAllocator : IUnknown
{
    unsigned device = 1, resets = 0;
    HRESULT resetResult = S_OK;
    HRESULT Reset() { ++resets; events.emplace_back("allocator Reset"); return resetResult; }
};
struct ID3D12CommandList : IUnknown
{
    unsigned device = 1, resets = 0, closes = 0;
    HRESULT resetResult = S_OK, closeResult = S_OK;
    ID3D12CommandList* canonical = nullptr;
    HRESULT Reset(ID3D12CommandAllocator*, void*)
    {
        ++resets; events.emplace_back("list Reset");
        if (!FAILED(resetResult) && deliverResetHook)
            FinishedPictureResetCommandList(canonical ? canonical : this);
        return resetResult;
    }
    HRESULT Close() { ++closes; events.emplace_back("list Close"); return closeResult; }
};
namespace DetailReuse
{
// Identity adapter only. No detail-reuse algorithm or admission policy is tested.
ID3D12CommandList* RealSubmittedCommands(ID3D12CommandList* list)
{ return list && list->canonical ? list->canonical : list; }
}
namespace Late
{
struct Filter
{
    bool sourceFinished = true;
    bool DepthSourceFinished() const { return sourceFinished; }
};
struct Slot
{
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12CommandList> commands;
    Filter nrFilter;
    ID3D12CommandList* producer = nullptr; // external identity, never owned/reset here
    UINT64 done = 0;
    uintptr_t backBufferIdentity = 0;
    bool pending = false, quarantined = false, submitted = false;
};
std::array<Slot, 4> slots, freshColorSlots;
}

#include "production.inc"

struct Retired
{
    DlssNrGpuLifetime::Uses uses;
    unsigned id = 0;
};
std::vector<Retired> g_nrRetired;
std::vector<unsigned> released;
void TickNrRetired()
{
    ++retiredTicks;
    DlssNrGpuLifetime::CollectRetired(g_nrRetired, [](const Retired& entry) {
        released.push_back(entry.id); events.emplace_back("release parked generation");
    });
}
void FinishedPictureResetCommandList(ID3D12CommandList* list)
{
    ++resetCallbacks; events.emplace_back("Reset callback");
    if (deliverResetCallback) g_nrGpuWork.ResetRecording(list);
}

using Retire = void (*)(const Late::Slot*);
struct World
{
    std::array<ID3D12CommandList, 10> lists;
    std::array<ID3D12CommandAllocator, 10> allocators;
    std::array<ID3D12Fence, 10> slotFences, executionFences;
    std::array<IUnknown, 10> resources;
    std::array<ID3D12CommandQueue, 2> queues;
    World()
    {
        events.clear(); released.clear(); warnings = resetCallbacks = retiredTicks = 0;
        deliverResetCallback = true; deliverResetHook = false;
    }
    ~World()
    {
        // Clear every host reference before backing objects leave scope.
        g_nrRetired.clear(); g_nrGpuWork.Clear();
        Late::slots = {}; Late::freshColorSlots = {};
    }
    DlssNrGpuLifetime::Use Token(unsigned index)
    {
        auto use = std::make_shared<DlssNrGpuLifetime::Token>();
        use->generation = index + 100;
        use->listOwner = &lists[index]; use->submitted = true;
        use->executions[0].queue = &queues[0];
        use->executions[0].fence = &executionFences[index];
        use->executions[0].target = 7; executionFences[index].completed = 7;
        Require(use->Retain(&resources[index]), "scenario must retain the real synthetic owner");
        g_nrGpuWork.Seed(&lists[index], use);
        return use;
    }
    Late::Slot& Slot(unsigned index)
    { return index < 4 ? Late::slots[index] : Late::freshColorSlots[index - 4]; }
    DlssNrGpuLifetime::Use Private(unsigned index)
    {
        auto& slot = Slot(index);
        slot.commands = &lists[index]; slot.allocator = &allocators[index];
        slot.fence = &slotFences[index]; slot.done = 11; slotFences[index].completed = 11;
        slot.submitted = true; slot.backBufferIdentity = index + 1000;
        auto use = Token(index);
        g_nrRetired.push_back({ {use}, index });
        return use;
    }
    void Retained(const DlssNrGpuLifetime::Use& use, unsigned index)
    {
        Require(!use->closed && !use->Complete(), "blocked recording must stay open and incomplete");
        Require(use->listOwner.Get() == &lists[index] && resources[index].references == 2,
                "blocked recording must retain list and resource owners");
        Require(g_nrGpuWork.Watches(&lists[index]) && !g_nrRetired.empty() && released.empty(),
                "blocked generation must remain watched and parked");
    }
};

void IdlePools(Retire retire)
{
    ++scenarios; World w;
    std::vector<DlssNrGpuLifetime::Use> uses;
    for (unsigned i = 0; i < 8; ++i)
    {
        uses.push_back(w.Private(i));
        Require(uses.back()->GpuComplete() && !uses.back()->Complete(),
                "GPU completion alone must not close an idle executable generation");
    }
    retire(nullptr);
    Require(g_nrRetired.empty() && released.size() == 8, "both idle private pools must unblock model retirement");
    for (unsigned i = 0; i < 8; ++i)
    {
        Require(uses[i]->closed && !uses[i]->discarded && uses[i]->Complete(), "submitted closure must keep GPU completion semantics");
        Require(!uses[i]->listOwner && uses[i]->owners.empty() && w.resources[i].references == 1,
                "only completed and closed generations release retained owners");
        Require(w.allocators[i].resets == 1 && w.lists[i].resets == 1 && w.lists[i].closes == 1,
                "each idle private list must undergo one real Reset and Close");
        Require(w.Slot(i).backBufferIdentity == 0 && !w.Slot(i).quarantined,
                "successful closure clears the bound buffer identity");
    }
    const auto callbacks = resetCallbacks;
    retire(nullptr);
    Require(resetCallbacks == callbacks && released.size() == 8, "repeated poll must not reset or release twice");
}

void ExactCompletion(Retire retire)
{
    ++scenarios; World w; auto use = w.Private(0);
    // The slot's own final signal and queue zero are complete. Another actual
    // execution is still behind: neither Ready nor the unrelated fence suffices.
    auto& second = w.executionFences[8];
    second.completed = 22;
    use->executions[1].queue = &w.queues[1];
    use->executions[1].fence = &second; use->executions[1].target = 23;
    for (unsigned poll = 0; poll < 64; ++poll) retire(nullptr);
    Require(w.allocators[0].resets == 0 && w.lists[0].resets == 0, "every actual queue/fence must complete before Reset");
    w.Retained(use, 0);
    second.completed = 23; retire(nullptr);
    Require(use->Complete() && g_nrRetired.empty(), "exact second execution completion must allow progress without focus or resize");
}

void SlotAndDepthCompletion(Retire retire)
{
    for (unsigned mode = 0; mode < 3; ++mode)
    {
        ++scenarios; World w; auto use = w.Private(0);
        if (mode == 0) w.slotFences[0].completed = 10;
        if (mode == 1) w.slotFences[0].completed = UINT64_MAX;
        if (mode == 2) w.Slot(0).nrFilter.sourceFinished = false;
        retire(nullptr);
        Require(w.allocators[0].resets == 0, "slot final signal, device health and depth source must all permit Reset");
        w.Retained(use, 0);
        w.slotFences[0].completed = 11; w.Slot(0).nrFilter.sourceFinished = true;
        retire(nullptr);
        Require(use->Complete() && g_nrRetired.empty(), "healthy completion makes an idle slot reclaimable");
    }
}

void Eligibility(Retire retire)
{
    using Change = std::function<void(World&, Late::Slot&)>;
    const std::vector<Change> changes {
        [](World&, Late::Slot& s) { s.pending = true; },
        [](World&, Late::Slot& s) { s.quarantined = true; },
        [](World&, Late::Slot& s) { s.submitted = false; },
        [](World&, Late::Slot& s) { s.backBufferIdentity = 0; },
        [](World&, Late::Slot& s) { s.commands.Reset(); },
        [](World&, Late::Slot& s) { s.allocator.Reset(); },
        [](World&, Late::Slot& s) { s.fence.Reset(); },
        [](World&, Late::Slot& s) { s.done = 0; },
        [](World& w, Late::Slot&) { w.allocators[0].device = 2; },
    };
    for (const auto& change : changes)
    {
        ++scenarios; World w; auto use = w.Private(0); change(w, w.Slot(0)); retire(nullptr);
        Require(w.allocators[0].resets == 0 && w.lists[0].resets == 0, "ineligible private slot must not be reset");
        w.Retained(use, 0);
    }
    ++scenarios; World w; auto selected = w.Private(0); auto idle = w.Private(4);
    retire(&w.Slot(0));
    Require(!selected->closed && w.lists[0].resets == 0, "next recording endpoint must be excluded");
    Require(idle->Complete() && w.lists[4].resets == 1 && released.size() == 1,
            "excluding the selected endpoint must not prevent other idle slots from retiring");
}

void UncertainExecutions(Retire retire)
{
    for (unsigned mode = 0; mode < 6; ++mode)
    {
        ++scenarios; World w; auto use = w.Private(0);
        if (mode == 0) use->submitted = false;
        if (mode == 1) use->quarantined = true;
        if (mode == 2) use->discarded = true;
        if (mode == 3) use->executions[0].fence.Reset();
        if (mode == 4) use->executions[0].target = 0;
        if (mode == 5) w.executionFences[0].completed = UINT64_MAX;
        retire(nullptr);
        Require(w.allocators[0].resets == 0 && w.lists[0].resets == 0, "uncertain actual execution must not reach private Reset");
        w.Retained(use, 0);
    }
}

void ExternalProducer(Retire retire)
{
    ++scenarios; World w; auto own = w.Private(0); auto external = w.Token(8);
    w.Slot(0).producer = &w.lists[8];
    g_nrRetired[0].uses.push_back(external); // same production shared snapshot semantics
    retire(nullptr);
    Require(own->Complete() && !external->closed && !g_nrRetired.empty() && released.empty(),
            "private closure cannot retire a model still referenced by an external producer");
    Require(w.lists[8].resets == 0 && w.lists[8].closes == 0 && w.allocators[8].resets == 0,
            "external producers must never be reset or closed by this helper");
    Require(g_nrGpuWork.Watches(&w.lists[8]) && w.resources[8].references == 2,
            "completed external executable generation remains owned until its owner closes it");
    g_nrGpuWork.ResetRecording(&w.lists[8]); // separate explicit external-owner notification
    retire(nullptr);
    Require(external->Complete() && g_nrRetired.empty() && released.size() == 1,
            "external owner's real closure can finally release the shared parked model");
}

void OldGenerationAndCanonicalIdentity(Retire retire)
{
    {
        ++scenarios; World w; auto current = w.Private(0);
        // A previously reset generation of this same list may still have an
        // actual execution pending. Inspect every matching snapshot use.
        auto older = std::make_shared<DlssNrGpuLifetime::Token>();
        older->generation = 9; older->listOwner = &w.lists[0];
        older->closed = true; older->submitted = true;
        older->executions[0].queue = &w.queues[1];
        older->executions[0].fence = &w.executionFences[8];
        older->executions[0].target = 50; w.executionFences[8].completed = 49;
        g_nrGpuWork.TrackOldGeneration(older); g_nrRetired[0].uses.push_back(older);
        retire(nullptr);
        Require(w.allocators[0].resets == 0 && !current->closed && released.empty(),
                "a completed current use cannot hide an older matching execution still in flight");
        w.executionFences[8].completed = 50; retire(nullptr);
        Require(current->Complete() && older->Complete() && g_nrRetired.empty(),
                "all matching generations complete before the private allocator is reset");
    }
    {
        ++scenarios; World w;
        w.Private(0);
        // Host proxy owns the actual Reset, while the production registry and
        // callback name its canonical list. No borrowed producer is substituted.
        w.lists[8].canonical = &w.lists[0]; w.Slot(0).commands = &w.lists[8];
        retire(nullptr);
        Require(w.lists[8].resets == 1 && w.lists[0].resets == 0 && g_nrRetired.empty(),
                "proxy Reset must close the matching canonical recording identity");
    }
    {
        ++scenarios; World w;
        auto& slot = w.Slot(0);
        slot.commands = &w.lists[0]; slot.allocator = &w.allocators[0];
        slot.fence = &w.slotFences[0]; slot.done = 11; w.slotFences[0].completed = 11;
        slot.submitted = true; slot.backBufferIdentity = 1000;
        retire(nullptr);
        Require(w.allocators[0].resets == 0 && resetCallbacks == 0,
                "an idle slot without an open tracked NR generation grants no Reset authority");
    }
}

void ResetFailures(Retire retire)
{
    for (unsigned mode = 0; mode < 2; ++mode)
    {
        ++scenarios; World w; auto use = w.Private(0);
        if (mode == 0) w.allocators[0].resetResult = E_FAIL;
        else w.lists[0].resetResult = E_FAIL;
        retire(nullptr);
        w.Retained(use, 0);
        Require(w.Slot(0).quarantined && w.Slot(0).backBufferIdentity != 0 && warnings != 0,
                "failed allocator/list Reset must quarantine and retain the old recording identity");
        Require(resetCallbacks == 0 && w.lists[0].closes == 0, "failed Reset must not fabricate closure");
        Require(w.lists[0].resets == mode, "allocator failure must short-circuit the list Reset");
        const auto attempts = w.allocators[0].resets;
        retire(nullptr);
        Require(w.allocators[0].resets == attempts, "quarantined Reset failure must not be retried by polling");
    }
}

void ClosureFailuresAndHooks(Retire retire)
{
    {
        ++scenarios; World w; auto use = w.Private(0); deliverResetCallback = false;
        retire(nullptr);
        w.Retained(use, 0);
        Require(w.Slot(0).quarantined && w.lists[0].closes == 0,
                "missing registry closure must quarantine instead of retiring or continuing");
    }
    {
        ++scenarios; World w; auto use = w.Private(0); w.lists[0].closeResult = E_FAIL;
        retire(nullptr);
        Require(w.Slot(0).quarantined && use->Complete() && g_nrRetired.empty(),
                "Close failure quarantines the new empty list; already-complete, actually-reset old work can retire");
    }
    {
        ++scenarios; World w; auto use = w.Private(0); deliverResetHook = true;
        retire(nullptr);
        Require(resetCallbacks == 2 && use->Complete() && released.size() == 1 && !w.Slot(0).quarantined,
                "real Reset hook plus explicit fallback must be idempotent");
        Require(events == std::vector<std::string>{"allocator Reset", "list Reset", "Reset callback",
                    "Reset callback", "list Close", "release parked generation"},
                "release follows actual Reset, callback closure and final collector");
    }
}

int main(int argc, char** argv)
{
    Retire retire = Late::RetireCompletedPrivateRecordings;
    if (argc == 2)
    {
        const std::string control = argv[1];
        if (control == "--negative-gpu") retire = Late::IgnoreGpuCompletion;
        else if (control == "--negative-pending") retire = Late::IgnorePending;
        else if (control == "--negative-reset") retire = Late::IgnoreResetFailure;
        else { std::fprintf(stderr, "unknown negative control\n"); return 2; }
    }
    else if (argc != 1) return 2;
    try
    {
        IdlePools(retire); ExactCompletion(retire); SlotAndDepthCompletion(retire);
        Eligibility(retire); UncertainExecutions(retire); ExternalProducer(retire);
        OldGenerationAndCanonicalIdentity(retire);
        ResetFailures(retire); ClosureFailuresAndHooks(retire);
        if (argc != 1) { std::fprintf(stderr, "FAIL: unsafe negative control escaped detection\n"); return 2; }
        std::printf("PASS NR reconfiguration actual production bodies: %u scenarios, %u checks; CPU operands only\n", scenarios, checks);
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "FAIL scenario=%u check=%u: %s\n", scenarios, checks, error.what());
        return 1;
    }
}

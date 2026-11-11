// SPDX-License-Identifier: GPL-3.0-or-later
// CPU-only tests of the production helper; no Streamline DLL is loaded.
// Only fake SDK getters mint tokens. There is no guessed token ring or copy.
#include "../../framegen/DlssgPacingLifetime.h"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>

namespace
{
using Domain = framegen::DlssgTokenDomain;
using Lifetime = framegen::DlssgPacingLifetime;
constexpr auto Ok = sl::Result::eOk;
constexpr auto Rejected = sl::Result::eErrorInvalidState;
unsigned checks = 0;

void Check(bool condition, const char* message)
{
    ++checks;
    if (!condition) throw std::runtime_error(message);
}

// A deadlock fails the process instead of hanging Lead's runner. This generous
// deadline is not a performance assertion. Never detach a fixture's worker.
template<class Work>
auto OnWorker(Work&& work)
{
    using Result = std::invoke_result_t<Work>;
    std::packaged_task<Result()> task(std::forward<Work>(work));
    auto result = task.get_future();
    std::thread worker(std::move(task));
    if (result.wait_for(std::chrono::seconds(2)) != std::future_status::ready)
    {
        std::fputs("FAIL: SDK worker blocked instead of returning promptly\n", stderr);
        std::fflush(stderr);
        std::_Exit(2);
    }
    worker.join();
    return result.get();
}

// Real abstract interface from external/streamline/sl_core_types.h. Deleting
// copy/move makes accidental test-side token cloning a compile error.
struct Token final : sl::FrameToken
{
    std::atomic<uint32_t> value {0};
    Token() = default;
    Token(const Token&) = delete;
    Token& operator=(const Token&) = delete;
    Token(Token&&) = delete;
    Token& operator=(Token&&) = delete;
    operator uint32_t() const override { return value.load(); }
};
static_assert(std::is_abstract_v<sl::FrameToken>);
static_assert(std::is_base_of_v<sl::FrameToken, Token>);
static_assert(!std::is_copy_constructible_v<Token>);

sl::Result GetOriginal(sl::FrameToken*&, const uint32_t*);
sl::Result GetOther(sl::FrameToken*&, const uint32_t*);
sl::Result SleepOriginal(const sl::FrameToken&);
sl::Result MarkerOriginal(sl::PCLMarker, const sl::FrameToken&);
std::shared_ptr<void> PinModule(const void*) noexcept;
static_assert(std::is_same_v<decltype(&GetOriginal), Domain::GetToken>);
static_assert(std::is_same_v<decltype(&SleepOriginal), Domain::SleepApi>);
static_assert(std::is_same_v<decltype(&MarkerOriginal), Domain::MarkerApi>);
static_assert(std::is_same_v<decltype(&PinModule), Domain::Pin>);

struct PinCounts
{
    // Exported getter, sleep, marker, and the token's actual vtable module.
    std::array<std::atomic<unsigned>, 4> acquired {};
    std::array<std::atomic<unsigned>, 4> released {};
    bool Balanced() const
    {
        for (size_t i = 0; i != acquired.size(); ++i)
            if (acquired[i].load() != released[i].load()) return false;
        return true;
    }
    bool Live(size_t index) const
    { return acquired[index].load() == released[index].load() + 1; }
};

struct ModulePin
{
    std::shared_ptr<PinCounts> counts;
    size_t index;
    ModulePin(std::shared_ptr<PinCounts> owner, size_t slot)
        : counts(std::move(owner)), index(slot) { ++counts->acquired[index]; }
    ~ModulePin() { ++counts->released[index]; }
    ModulePin(const ModulePin&) = delete;
    ModulePin& operator=(const ModulePin&) = delete;
};

struct InjectedSdkException {};
struct FakeSdk;
FakeSdk* sdk = nullptr; // Fixtures run serially; all workers join before teardown.

struct FakeSdk
{
    Domain domain;
    std::shared_ptr<Lifetime> lifetime = std::make_shared<Lifetime>();
    std::shared_ptr<PinCounts> pins = std::make_shared<PinCounts>();
    Token original, other;
    std::atomic<unsigned> originalGets {0}, otherGets {0}, sleeps {0}, markers {0};
    std::atomic<const sl::FrameToken*> sleptToken {nullptr};
    std::atomic<uint32_t> sleptFrame {0};
    bool failGet = false, nullToken = false, mismatch = false;
    bool throwGet = false, throwSleep = false;
    int missingPin = -1;
    std::atomic<bool> unknownPinAddress {false};
    sl::Result sleepResult = Ok;
    std::function<void()> duringGet, duringSleep;

    FakeSdk()
    {
        if (sdk) throw std::runtime_error("nested SDK fixture");
        sdk = this;
        lifetime->OpenAdmission();
    }
    ~FakeSdk() { sdk = nullptr; }
    FakeSdk(const FakeSdk&) = delete;
    FakeSdk& operator=(const FakeSdk&) = delete;

    FGPacingLeasePtr Begin(uint32_t frame, sl::FrameToken*& token)
    {
        return domain.Begin(lifetime, &GetOriginal, &SleepOriginal, &MarkerOriginal,
                            token, &frame, &PinModule);
    }
    const void* VtableAddress() const
    {
        // Observe the vtable using the same Windows/MSVC ABI as the helper.
        // Neither this observation nor any caller constructs a token copy.
        return *reinterpret_cast<const void* const*>(&original);
    }
    void RequireAllPinsLive() const
    {
        for (size_t i = 0; i != pins->acquired.size(); ++i)
            Check(pins->Live(i), "all four module pins must outlive the SDK call");
        Check(!unknownPinAddress.load(), "pin the API/vtable module, never the token heap address");
    }
};

sl::Result GetOriginal(sl::FrameToken*& token, const uint32_t* frame)
{
    ++sdk->originalGets;
    if (sdk->throwGet) throw InjectedSdkException {};
    const uint32_t requested = frame ? *frame : 0;
    sdk->original.value = sdk->mismatch ? requested + uint32_t {1} : requested;
    token = sdk->nullToken ? nullptr : &sdk->original;
    if (sdk->duringGet) sdk->duringGet();
    // An SDK failure may still have written an output pointer.
    return sdk->failGet ? Rejected : Ok;
}

sl::Result GetOther(sl::FrameToken*& token, const uint32_t* frame)
{
    ++sdk->otherGets;
    sdk->other.value = frame ? *frame : 0;
    token = &sdk->other;
    return Ok;
}

sl::Result SleepOriginal(const sl::FrameToken& token)
{
    ++sdk->sleeps;
    sdk->sleptToken = &token;
    sdk->sleptFrame = static_cast<uint32_t>(token);
    if (sdk->duringSleep) sdk->duringSleep();
    if (sdk->throwSleep) throw InjectedSdkException {};
    return sdk->sleepResult;
}

sl::Result MarkerOriginal(sl::PCLMarker, const sl::FrameToken&)
{
    ++sdk->markers;
    return Ok;
}

std::shared_ptr<void> PinModule(const void* address) noexcept
{
    const std::array<const void*, 4> modules {
        reinterpret_cast<const void*>(&GetOriginal),
        reinterpret_cast<const void*>(&SleepOriginal),
        reinterpret_cast<const void*>(&MarkerOriginal), sdk->VtableAddress()};
    for (size_t i = 0; i != modules.size(); ++i)
    {
        if (address != modules[i]) continue;
        if (sdk->missingPin == static_cast<int>(i)) return {};
        try { return std::make_shared<ModulePin>(sdk->pins, i); }
        catch (...) { return {}; }
    }
    sdk->unknownPinAddress = true;
    return {};
}

void AdmissionStartsClosedAndDrainsAllEntrants()
{
    Lifetime lifetime;
    Check(lifetime.Idle(), "a new lifetime is idle");
    Check(!lifetime.TryEnter(), "a new lifetime starts closed");
    Check(lifetime.OpenAdmission() && lifetime.OpenAdmission(), "idle open is idempotent");
    Check(lifetime.TryEnter() && lifetime.TryEnter(), "both admitted users count");
    Check(!lifetime.Idle(), "admitted users prevent idle");
    lifetime.CloseAdmission();
    lifetime.CloseAdmission();
    Check(!lifetime.TryEnter(), "close refuses new admissions");
    Check(!lifetime.OpenAdmission(), "close cannot reopen across an active user");
    lifetime.Leave();
    Check(!lifetime.Idle(), "one remaining user still prevents idle");
    lifetime.Leave();
    Check(lifetime.Idle(), "only the last leave establishes CPU idle");
    Check(!lifetime.TryEnter(), "draining does not implicitly reopen");
    Check(lifetime.OpenAdmission() && lifetime.TryEnter(), "explicit reopen after drain succeeds");
    lifetime.Leave();
}

void MissingPrerequisitesHaveNoSideEffects()
{
    for (unsigned missing = 0; missing != 7; ++missing)
    {
        FakeSdk f;
        const uint32_t frame = 41;
        if (missing == 6) f.lifetime->CloseAdmission();
        sl::FrameToken* token = &f.other;
        auto lease = f.domain.Begin(missing == 0 ? nullptr : f.lifetime,
            missing == 1 ? nullptr : &GetOriginal, missing == 2 ? nullptr : &SleepOriginal,
            missing == 3 ? nullptr : &MarkerOriginal, token,
            missing == 4 ? nullptr : &frame, missing == 5 ? nullptr : &PinModule);
        Check(!lease && !token, "invalid prerequisites must refuse and clear stale output");
        Check(f.lifetime->Idle() && f.pins->Balanced(), "refusal leaves no admission or pins");
        Check(f.originalGets == 0 && f.sleeps == 0, "refusal never calls the SDK");
        for (const auto& acquired : f.pins->acquired)
            Check(acquired.load() == 0, "invalid prerequisites never attempt module retention");
    }
}

void OriginalPointerAndPinsSurviveSharedLeaseOwnership()
{
    FakeSdk f;
    sl::FrameToken* token = nullptr;
    auto lease = f.Begin(57, token);
    Check(lease && lease->Valid() && lease->Frame() == 57, "Begin returns the requested lease");
    Check(token == &f.original && f.originalGets == 1, "Begin returns the original SDK object");
    Check(!f.lifetime->Idle(), "a retained lease is admitted");
    f.RequireAllPinsLive();
    auto alias = lease;
    lease.reset();
    Check(!f.lifetime->Idle(), "dropping one shared reference cannot release the lease");
    f.RequireAllPinsLive();
    alias.reset();
    Check(f.lifetime->Idle() && f.pins->Balanced(), "last reference cancels and unpins exactly once");
    Check(f.sleeps == 0 && f.markers == 0, "abandonment never sleeps or sends a marker");
    const uint32_t next = 58;
    Check(f.domain.Get(&GetOriginal, token, &next) == Ok && f.originalGets == 2,
          "abandonment releases the original API reservation");
}

void ReservationPrecedesOriginalGetter()
{
    FakeSdk f;
    bool observed = false;
    f.duringGet = [&] {
        observed = true;
        Check(!f.lifetime->Idle(), "CPU admission exists inside the original getter");
        for (size_t i = 0; i != 3; ++i)
            Check(f.pins->Live(i), "all exported API modules are pinned before original Get");
        OnWorker([&] {
            const uint32_t competingFrame = 61;
            sl::FrameToken* competing = &f.other;
            Check(f.domain.Get(&GetOriginal, competing, &competingFrame) != Ok && !competing,
                  "reservation prevents recycling before the original Get returns");
            auto nested = f.Begin(competingFrame, competing);
            Check(!nested && !competing, "a worker cannot acquire a second reservation inside Get");
            Check(!f.lifetime->Idle(), "refused competition cannot drop the original admission");
            // Distinguish a reservation from merely holding the domain mutex:
            // an independent API must still progress while the original Get runs.
            Check(f.domain.Get(&GetOther, competing, &competingFrame) == Ok && competing == &f.other,
                  "an independent API remains available while original Get holds a reservation");
        });
        Check(f.originalGets == 1, "reservation predates any possible second original getter");
    };
    sl::FrameToken* token = nullptr;
    auto lease = f.Begin(60, token);
    Check(observed && lease && token == &f.original, "original getter completed under its reservation");
    lease.reset();
    Check(f.lifetime->Idle() && f.pins->Balanced(), "getter reservation cleans up with its lease");
}

void SameExplicitFrameReusesOriginalAndOthersRefuse()
{
    FakeSdk f;
    uint32_t frame = 101, different = 102;
    sl::FrameToken* original = nullptr;
    auto lease = f.Begin(frame, original);
    Check(lease && original == &f.original, "original reservation created");
    for (unsigned repeat = 0; repeat != 3; ++repeat)
    {
        sl::FrameToken* reused = nullptr;
        Check(f.domain.Get(&GetOriginal, reused, &frame) == Ok && reused == original,
              "same thread and explicit frame reuse the exact original pointer");
    }
    sl::FrameToken* refused = &f.other;
    Check(f.domain.Get(&GetOriginal, refused, &different) != Ok && !refused,
          "same API with a different frame refuses");
    refused = &f.other;
    Check(f.domain.Get(&GetOriginal, refused, nullptr) != Ok && !refused,
          "implicit frame cannot borrow an explicit-frame reservation");
    OnWorker([&] {
        for (const auto candidate : {frame, different})
        {
            sl::FrameToken* result = &f.other;
            Check(f.domain.Get(&GetOriginal, result, &candidate) != Ok && !result,
                  "another thread refuses even the same explicit frame without blocking");
        }
    });
    Check(f.originalGets == 1, "neither reuse nor refusal reacquires the SDK token");
    lease.reset();
    Check(f.domain.Get(&GetOriginal, refused, &different) == Ok && refused == &f.original,
          "another frame is accepted after lease release");
    Check(f.originalGets == 2 && f.lifetime->Idle(), "release removes reservation and admission");
}

void OtherApiRemainsIndependentWhileReserved()
{
    FakeSdk f;
    sl::FrameToken* original = nullptr;
    auto lease = f.Begin(201, original);
    Check(lease && original == &f.original, "original API lease exists");
    const uint32_t unrelatedFrame = 701;
    sl::FrameToken* unrelated = nullptr;
    Check(f.domain.Get(&GetOther, unrelated, &unrelatedFrame) == Ok && unrelated == &f.other,
          "an independent API can obtain its own original token");
    OnWorker([&] {
        const uint32_t workerFrame = 702;
        sl::FrameToken* token = nullptr;
        Check(f.domain.Get(&GetOther, token, &workerFrame) == Ok && token == &f.other,
              "an independent API is also allowed on another thread");
    });
    Check(f.otherGets == 2 && f.originalGets == 1 && static_cast<uint32_t>(*original) == 201,
          "other API requests cannot recycle the reserved API's token");
    auto otherLifetime = std::make_shared<Lifetime>();
    otherLifetime->OpenAdmission();
    auto second = f.domain.Begin(otherLifetime, &GetOther, &SleepOriginal, &MarkerOriginal,
                                 unrelated, &unrelatedFrame, &PinModule);
    Check(!second && otherLifetime->Idle(), "a second Begin cannot overwrite a live reservation");
    Check(!f.lifetime->Idle(), "second-owner refusal preserves the first admission");
}

void SleepIsOneShotAndKeepsOriginalPinsUntilReturn()
{
    FakeSdk f;
    sl::FrameToken* token = nullptr;
    auto lease = f.Begin(301, token);
    Check(lease && token == &f.original, "sleep lease uses original SDK storage");
    f.duringSleep = [&] {
        Check(f.sleptToken == token && f.sleptFrame == 301, "Sleep receives the exact original object");
        Check(!lease->Valid() && !f.lifetime->Idle(), "consumed does not mean the SDK call returned");
        f.RequireAllPinsLive();
        Check(lease->Sleep() != Ok, "recursive Sleep is rejected");
        OnWorker([&] { Check(lease->Sleep() != Ok, "concurrent second Sleep is rejected"); });
        Check(f.sleeps == 1, "only one SDK Sleep can execute");
    };
    Check(lease->Sleep() == Ok, "original SDK Sleep succeeds");
    Check(!lease->Valid() && f.lifetime->Idle(), "CPU admission ends after actual return");
    Check(lease->Sleep() != Ok && f.sleeps == 1 && f.originalGets == 1,
          "finished lease cannot sleep again or reacquire a token");
    lease.reset();
    Check(f.pins->Balanced(), "all successful pins release exactly once");
}

void ActiveSleepAllowsExternalLocksAndDefersOffAndReplacement()
{
    FakeSdk f;
    std::mutex continuity, fg;
    sl::FrameToken* token = nullptr;
    FGPacingLeasePtr lease;
    {
        std::scoped_lock providerLocks(continuity, fg);
        lease = f.Begin(401, token);
    }
    Check(lease && token == &f.original, "provider retains lease before dropping external locks");
    unsigned offCalls = 0, replacements = 0;
    // Caller-side policy probe, not a fake implementation of provider teardown.
    // CPU idle is necessary here, but NEVER proves GPU queue/fence completion.
    const auto tryOff = [&] { if (f.lifetime->Idle()) ++offCalls; };
    const auto tryReplace = [&] { if (f.lifetime->Idle()) ++replacements; };
    f.duringSleep = [&] {
        OnWorker([&] {
            std::unique_lock continuityLock(continuity, std::try_to_lock);
            std::unique_lock fgLock(fg, std::try_to_lock);
            Check(continuityLock.owns_lock() && fgLock.owns_lock(),
                  "both external mutexes are AVAILABLE to another thread during SDK Sleep");
            f.lifetime->CloseAdmission();
            Check(!f.lifetime->Idle(), "closing admission cannot hide an active Sleep");
            Check(!f.lifetime->OpenAdmission(), "active closed admission cannot reopen");
            tryOff();
            tryReplace();
            Check(offCalls == 0 && replacements == 0, "Idle policy defers Off and provider replacement");
            sl::FrameToken* denied = &f.other;
            auto refused = f.Begin(402, denied);
            Check(!refused && !denied && !f.lifetime->Idle(),
                  "closed Begin refuses promptly while the old SDK call remains active");
            const uint32_t next = 402;
            Check(f.domain.Get(&GetOriginal, denied, &next) != Ok && !denied,
                  "Sleep keeps token-recycling exclusion while external locks are available");
            Check(f.domain.Get(&GetOther, denied, &next) == Ok && denied == &f.other,
                  "the domain mutex is not held across SDK Sleep");
            f.RequireAllPinsLive();
        });
    };
    Check(lease->Sleep() == Ok && f.lifetime->Idle(), "only actual Sleep return permits CPU idle");
    tryOff();
    tryReplace();
    Check(offCalls == 1 && replacements == 1, "deferred CPU policy actions can progress after return");
    Check(!f.lifetime->TryEnter(), "close remains effective after Sleep returns");
    lease.reset();
    Check(f.pins->Balanced(), "closed admission does not leak SDK module pins");
}

void PinFailuresIncludingMarkerAndVtableReleaseEverything()
{
    for (int failedPin = 0; failedPin != 4; ++failedPin)
    {
        FakeSdk f;
        f.missingPin = failedPin;
        sl::FrameToken* token = nullptr;
        auto lease = f.Begin(501, token);
        Check(!lease && f.lifetime->Idle(), "any failed module pin leaves no admitted lease");
        Check(f.pins->Balanced(), "pin failure releases every earlier successful module pin");
        Check(!f.unknownPinAddress, "token storage must be pinned through its vtable module");
        Check(f.originalGets == (failedPin == 3 ? 1u : 0u),
              "missing getter/sleep/MARKER pin refuses before SDK Get; vtable pin follows Get");
        Check(f.sleeps == 0 && f.markers == 0, "failed acquisition performs no SDK work after Get");
        f.missingPin = -1;
        lease = f.Begin(502, token);
        Check(lease && token == &f.original, "failed pin leaves no stale token reservation");
        lease.reset();
        Check(f.lifetime->Idle() && f.pins->Balanced(), "retry releases pins and admission");
    }
}

void FailedNullAndMismatchedGettersLeaveNoReservation()
{
    for (unsigned failure = 0; failure != 3; ++failure)
    {
        FakeSdk f;
        f.failGet = failure == 0;
        f.nullToken = failure == 1;
        f.mismatch = failure == 2;
        sl::FrameToken* token = nullptr;
        auto lease = f.Begin(601, token);
        Check(!lease && f.originalGets == 1, "failed/null/mismatched getter cannot mint a lease");
        Check(f.lifetime->Idle() && f.pins->Balanced(), "failed getter drops admission and every pin");
        Check(f.sleeps == 0, "invalid SDK token never reaches Sleep");
        f.failGet = f.nullToken = f.mismatch = false;
        lease = f.Begin(602, token);
        Check(lease && lease->Frame() == 602 && f.originalGets == 2,
              "failed getter leaves no reservation or sticky SDK reentrancy guard");
        lease.reset();
        Check(f.lifetime->Idle() && f.pins->Balanced(), "successful retry fully releases");
    }
}

void GetterExceptionsUnwindAdmissionPinsAndSdkScope()
{
    for (const bool throughBegin : {false, true})
    {
        FakeSdk f;
        f.throwGet = true;
        const uint32_t frame = 701;
        sl::FrameToken* token = nullptr;
        FGPacingLeasePtr lease;
        bool refused = false;
        try
        {
            if (throughBegin) { lease = f.Begin(frame, token); refused = !lease; }
            else refused = f.domain.Get(&GetOriginal, token, &frame) != Ok;
        }
        catch (const InjectedSdkException&) { refused = true; }
        Check(refused && !lease, "getter exception cannot return an admitted lease or success");
        Check(f.lifetime->Idle() && f.pins->Balanced(), "getter exception unwinds all CPU ownership");
        f.throwGet = false;
        lease = f.Begin(702, token);
        Check(lease && token == &f.original, "getter exception restores mutex and thread-local SDK guard");
        lease.reset();
        Check(f.lifetime->Idle() && f.pins->Balanced(), "post-exception retry cleans up");
    }
}

void SleepErrorAndExceptionStillReleaseAfterReturn()
{
    for (const bool throws : {false, true})
    {
        FakeSdk f;
        f.throwSleep = throws;
        f.sleepResult = Rejected;
        sl::FrameToken* token = nullptr;
        auto lease = f.Begin(801, token);
        Check(lease != nullptr, "sleep failure test acquired a valid lease");
        f.duringSleep = [&] {
            Check(!f.lifetime->Idle(), "failure-path SDK call retains admission until return");
            f.RequireAllPinsLive();
        };
        Check(lease->Sleep() != Ok, "SDK failure/exception reports failed Sleep without escaping noexcept");
        Check(f.lifetime->Idle() && !lease->Valid() && f.sleeps == 1,
              "failed Sleep still consumes once and releases CPU admission");
        Check(lease->Sleep() != Ok && f.sleeps == 1, "failed Sleep is also one-shot");
        lease.reset();
        Check(f.pins->Balanced(), "failed Sleep releases all module pins");
        f.throwSleep = false;
        f.sleepResult = Ok;
        f.duringSleep = {};
        lease = f.Begin(802, token);
        Check(lease && lease->Sleep() == Ok, "Sleep exception cannot leave a sticky reservation or SDK guard");
    }
}

void CallerExceptionCancelsUnconsumedLease()
{
    FakeSdk f;
    try
    {
        sl::FrameToken* token = nullptr;
        auto lease = f.Begin(901, token);
        Check(lease && !f.lifetime->Idle(), "caller owns an admitted lease before unwinding");
        throw InjectedSdkException {};
    }
    catch (const InjectedSdkException&) {}
    Check(f.lifetime->Idle() && f.pins->Balanced() && f.sleeps == 0,
          "caller exception cancels an unconsumed lease and unpins its modules");
    sl::FrameToken* token = nullptr;
    auto next = f.Begin(902, token);
    Check(next && token == &f.original, "caller unwinding removes the token reservation");
}

void WrongThreadSleepFailsWithoutCallingSdk()
{
    FakeSdk f;
    sl::FrameToken* token = nullptr;
    auto lease = f.Begin(1001, token);
    Check(lease != nullptr, "thread-affinity test acquired a lease");
    const auto result = OnWorker([&] { return lease->Sleep(); });
    Check(result != Ok && f.sleeps == 0, "another thread must not submit the source thread's Sleep");
    Check(!lease->Valid() && f.lifetime->Idle(), "rejected Sleep relinquishes its spent admission");
    lease.reset();
    Check(f.pins->Balanced(), "wrong-thread refusal releases pins exactly once");
    lease = f.Begin(1002, token);
    Check(lease && lease->Sleep() == Ok, "wrong-thread refusal leaves no stale reservation");
}

void ChangedSdkTokenIsRejectedWithoutManualCopyOrRelabel()
{
    FakeSdk f;
    const uint32_t frame = 1101;
    sl::FrameToken* token = nullptr;
    auto lease = f.Begin(frame, token);
    Check(lease != nullptr, "token-integrity test acquired a lease");
    // Fault injection in SDK-owned storage, never a caller copy or repair.
    f.original.value = frame + uint32_t {1};
    sl::FrameToken* reused = &f.other;
    Check(f.domain.Get(&GetOriginal, reused, &frame) != Ok && !reused,
          "a recycled/mutated SDK object cannot be reused as the reserved frame");
    Check(lease->Sleep() != Ok && f.sleeps == 0 && f.originalGets == 1,
          "invalid original object is rejected, never reacquired or relabelled for Sleep");
    Check(f.lifetime->Idle(), "token-integrity refusal still releases CPU admission");
    lease.reset();
    Check(f.pins->Balanced(), "token-integrity refusal releases module references");
}

void FinishedLeaseCannotCancelANewerReservation()
{
    FakeSdk f;
    sl::FrameToken* token = nullptr;
    auto first = f.Begin(1201, token);
    Check(first && first->Sleep() == Ok && f.lifetime->Idle(), "first lease completed");
    auto second = f.Begin(1202, token);
    Check(second && !f.lifetime->Idle(), "new reservation starts before old object destruction");
    first.reset();
    const uint32_t conflicting = 1203;
    sl::FrameToken* refused = &f.other;
    Check(f.domain.Get(&GetOriginal, refused, &conflicting) != Ok && !refused,
          "old destructor cannot cancel the newer reservation");
    Check(!f.lifetime->Idle(), "old destructor cannot release the newer admission");
    Check(second->Sleep() == Ok && f.sleptFrame == 1202, "new lease still sleeps its original frame");
    second.reset();
    Check(f.lifetime->Idle() && f.pins->Balanced(), "both serials release exactly their own ownership");
}

void ExplicitFrameWraparoundUsesExactIdentity()
{
    FakeSdk f;
    constexpr auto max = (std::numeric_limits<uint32_t>::max)();
    constexpr std::array<uint32_t, 6> frames {max - 1, max, 0, 1, 0, max};
    unsigned calls = 0;
    for (const auto frame : frames)
    {
        sl::FrameToken* token = nullptr;
        auto lease = f.Begin(frame, token);
        ++calls;
        Check(lease && lease->Frame() == frame && token == &f.original,
              "zero and UINT32_MAX are explicit frame identities, not unset sentinels");
        sl::FrameToken* reused = nullptr;
        Check(f.domain.Get(&GetOriginal, reused, &frame) == Ok && reused == token,
              "wraparound still reuses exact original identity");
        const uint32_t different = frame + uint32_t {1};
        Check(f.domain.Get(&GetOriginal, reused, &different) != Ok && !reused,
              "adjacent frames are distinct across UINT32_MAX to zero");
        Check(f.originalGets == calls, "wraparound never mints an extra getter call");
        Check(lease->Sleep() == Ok && f.sleptToken == token && f.sleptFrame == frame,
              "Sleep preserves original pointer and full uint32 frame value");
        Check(f.lifetime->Idle(), "each wrapped frame retires its CPU admission");
        lease.reset();
        Check(f.pins->Balanced(), "each wrapped frame releases its module pins");
    }
    Check(f.sleeps == calls, "exactly one SDK Sleep per successful frame, independent of any ring size");
}

void ReentrantGettersAndBeginAreRejected()
{
    FakeSdk f;
    bool callbackRan = false;
    f.duringGet = [&] {
        callbackRan = true;
        const uint32_t nestedFrame = 1302;
        for (const auto api : {&GetOriginal, &GetOther})
        {
            sl::FrameToken* token = &f.other;
            Check(f.domain.Get(api, token, &nestedFrame) != Ok && !token,
                  "getter callback cannot recursively enter either SDK API");
        }
        sl::FrameToken* token = &f.other;
        auto recursive = f.Begin(nestedFrame, token);
        Check(!recursive && !token && !f.lifetime->Idle(),
              "recursive Begin refuses without releasing the original getter admission");
    };
    sl::FrameToken* token = nullptr;
    auto lease = f.Begin(1301, token);
    Check(callbackRan && lease && f.originalGets == 1 && f.otherGets == 0,
          "reentrant callbacks never call a second real getter");
    f.duringGet = {};
    lease.reset();
    const uint32_t nextFrame = 1302;
    Check(f.domain.Get(&GetOriginal, token, &nextFrame) == Ok && f.originalGets == 2,
          "outer SDK return clears the reentrancy guard");
}

void ApiChangeTransactionExcludesBeginAndBothGetters()
{
    FakeSdk f;
    auto change = f.domain.TryChangeApi();
    Check(change.owns_lock(), "idle domain permits an API detach/replace transaction");
    OnWorker([&] {
        auto competingChange = f.domain.TryChangeApi();
        Check(!competingChange.owns_lock(), "another API change refuses without blocking");
        const uint32_t frame = 1401;
        for (const auto api : {&GetOriginal, &GetOther})
        {
            sl::FrameToken* token = &f.other;
            Check(f.domain.Get(api, token, &frame) != Ok && !token,
                  "API change guard excludes Get for every API");
        }
        sl::FrameToken* token = &f.other;
        auto refused = f.Begin(frame, token);
        Check(!refused && !token, "API change guard excludes Begin without blocking");
        Check(f.lifetime->Idle() && f.pins->Balanced(),
              "Begin rejected by API change leaves no admission or module pins");
        Check(f.originalGets == 0 && f.otherGets == 0,
              "no SDK getter runs during a detach/replace transaction");
    });
    change.unlock();
    sl::FrameToken* token = nullptr;
    const uint32_t frame = 1402;
    Check(f.domain.Get(&GetOther, token, &frame) == Ok && token == &f.other,
          "Get resumes after API transaction releases its guard");
    auto lease = f.Begin(frame, token);
    Check(lease && token == &f.original, "Begin resumes after API transaction releases its guard");
    lease.reset();
    Check(f.lifetime->Idle() && f.pins->Balanced(), "API transaction does not leave sticky admission");
}

void ApiChangesRefuseUntilLeaseAndActiveSleepRelease()
{
    for (const bool consume : {false, true})
    {
        FakeSdk f;
        sl::FrameToken* token = nullptr;
        auto lease = f.Begin(1501, token);
        Check(lease && token == &f.original, "API-change test retains the original SDK token");
        {
            auto refused = f.domain.TryChangeApi();
            Check(!refused.owns_lock(), "live reservation refuses a same-thread API change");
        }
        OnWorker([&] {
            auto refused = f.domain.TryChangeApi();
            Check(!refused.owns_lock(), "live reservation refuses another thread's API change promptly");
        });
        f.duringSleep = [&] {
            Check(!f.lifetime->Idle(), "active Sleep retains lifecycle admission");
            OnWorker([&] {
                auto refused = f.domain.TryChangeApi();
                Check(!refused.owns_lock(),
                      "token/API replacement stays refused until the actual SDK Sleep returns");
                f.RequireAllPinsLive();
            });
        };
        if (consume)
        {
            Check(lease->Sleep() == Ok && f.lifetime->Idle(), "SDK return releases API reservation");
            // Keep the spent shared object alive: exclusion follows the active
            // reservation, not the lifetime of an already-consumed lease object.
            auto afterReturn = f.domain.TryChangeApi();
            Check(afterReturn.owns_lock(), "API change succeeds after Sleep while spent lease still exists");
        }
        lease.reset();
        auto afterRelease = f.domain.TryChangeApi();
        Check(afterRelease.owns_lock(), "API change succeeds after consumed or abandoned lease destruction");
        Check(f.lifetime->Idle() && f.pins->Balanced(), "lease release balances admission and all pins");
        Check(f.sleeps == (consume ? 1u : 0u), "abandonment does not fabricate an SDK Sleep");
    }
}
void ResolutionAndAdmissionAreAtomicWithApiReplacement()
{
    FakeSdk f;uint32_t frame=1601;sl::FrameToken* token=nullptr;
    Domain::GetToken current=&GetOriginal;unsigned resolutions=0;
    const auto resolve=[&] {
        ++resolutions;OnWorker([&]{auto change=f.domain.TryChangeApi();
            Check(!change.owns_lock(),"API replacement cannot cross callable resolution before admission");});
        return current;
    };
    f.duringGet=[&]{OnWorker([&]{auto change=f.domain.TryChangeApi();
        Check(!change.owns_lock(),"ordinary resolved getter retains API admission through SDK return");});};
    Check(f.domain.GetResolved(resolve,token,&frame)==Ok&&token==&f.original,"resolved ordinary getter uses actual target");
    Check(resolutions==1,"ordinary getter resolves once inside domain");
    {auto change=f.domain.TryChangeApi();Check(change.owns_lock(),"API may replace after ordinary return");current=&GetOther;}
    Check(f.domain.GetResolved(resolve,token,&frame)==Ok&&token==&f.other,"post-change getter resolves new target rather than stale callable");
    current=&GetOriginal;f.duringGet={};
    auto lease=f.domain.BeginResolved(f.lifetime,[&]{return Domain::Apis{resolve(),&SleepOriginal,&MarkerOriginal};},token,&frame,&PinModule);
    Check(lease&&token==&f.original,"resolved Begin reserves original target before SDK entry");
    Check(!f.domain.TryChangeApi().owns_lock(),"resolved lease excludes detach through Sleep");
    Check(lease->Sleep()==Ok,"resolved lease executes exact original token");lease.reset();
    auto change=f.domain.TryChangeApi();Check(change.owns_lock(),"detachment allowed after resolved lease return");
    Check(f.lifetime->Idle()&&f.pins->Balanced(),"resolved path balances admission and module pins");
}
} // namespace

int main()
{
    struct Case { const char* name; void (*run)(); };
    const std::array cases {
        Case {"atomic callable resolution and admission", &ResolutionAndAdmissionAreAtomicWithApiReplacement},
        Case {"closed admission and drain", &AdmissionStartsClosedAndDrainsAllEntrants},
        Case {"missing prerequisites", &MissingPrerequisitesHaveNoSideEffects},
        Case {"original pointer and shared module pins", &OriginalPointerAndPinsSurviveSharedLeaseOwnership},
        Case {"reservation before original getter", &ReservationPrecedesOriginalGetter},
        Case {"same-frame reuse and nonblocking refusals", &SameExplicitFrameReusesOriginalAndOthersRefuse},
        Case {"independent API while reserved", &OtherApiRemainsIndependentWhileReserved},
        Case {"one-shot original-token Sleep", &SleepIsOneShotAndKeepsOriginalPinsUntilReturn},
        Case {"external mutex availability and deferred policy", &ActiveSleepAllowsExternalLocksAndDefersOffAndReplacement},
        Case {"all pin failure paths", &PinFailuresIncludingMarkerAndVtableReleaseEverything},
        Case {"getter error/null/mismatch cleanup", &FailedNullAndMismatchedGettersLeaveNoReservation},
        Case {"getter exception cleanup", &GetterExceptionsUnwindAdmissionPinsAndSdkScope},
        Case {"Sleep error and exception cleanup", &SleepErrorAndExceptionStillReleaseAfterReturn},
        Case {"caller exception cleanup", &CallerExceptionCancelsUnconsumedLease},
        Case {"Sleep thread affinity", &WrongThreadSleepFailsWithoutCallingSdk},
        Case {"SDK token mutation refusal", &ChangedSdkTokenIsRejectedWithoutManualCopyOrRelabel},
        Case {"old lease cannot cancel new reservation", &FinishedLeaseCannotCancelANewerReservation},
        Case {"explicit frame wraparound", &ExplicitFrameWraparoundUsesExactIdentity},
        Case {"reentrant getter rejection", &ReentrantGettersAndBeginAreRejected},
        Case {"API change transaction excludes Get and Begin", &ApiChangeTransactionExcludesBeginAndBothGetters},
        Case {"API changes refuse until lease and Sleep release", &ApiChangesRefuseUntilLeaseAndActiveSleepRelease}};
    unsigned failures = 0;
    for (const auto& test : cases)
    {
        try { test.run(); std::printf("PASS: %s\n", test.name); }
        catch (const std::exception& error)
        { ++failures; std::fprintf(stderr, "FAIL: %s: %s\n", test.name, error.what()); }
        catch (...)
        { ++failures; std::fprintf(stderr, "FAIL: %s: unexpected exception\n", test.name); }
    }
    std::printf("DlssgPacingLifetime: %zu cases, %u checks, %u failures (CPU only)\n",
                cases.size(), checks, failures);
    return failures ? 1 : 0;
}

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "GenshinFsrUiObservation.h"
#include <array>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <mutex>

// Runtime owners publish sparse real events. UI reads a copy, never calls an
// initializer, polls a GPU fence, cancels work, or changes an admission gate.
namespace GenshinLoading
{
enum class Stage : unsigned char
{
    Detected, WaitingFsr, Hooking, ProducerResources, InteropAdapterQueue,
    DlssOn12Provider, FirstSrEvaluate, OutputCopyback, NrGuides, NrProvider,
    FirstNrExecute, FgInputs, FgProvider, FirstFgExecute, Count
};
enum class State : unsigned char { Unknown, Running, Complete, Failed, Skipped };
enum class Reason : unsigned char
{
    None, WaitingObservation, FsrNotDetected, HookNotMatched, HookInstallFailed,
    ProducerResourcesUnknown, AdapterQueueUnknown, ProviderPending, AdmissionUnknown,
    Disabled, Unsupported, NativeFailure, Recovery, Reset
};
enum class NrMode : unsigned char { Unknown, Requested, Disabled, Unsupported };
enum class Authority : unsigned char { RuntimeOwner, UiObservation };
struct Entry
{
    State state = State::Unknown;
    Reason reason = Reason::WaitingObservation;
    std::int64_t errorCode = 0;
    std::uint64_t startedTick = 0, finishedTick = 0, updatedTick = 0;
};
struct Snapshot
{
    GenshinLegacy::Ui::Session session {};
    std::uint64_t attempt = 0, startedTick = 0, updatedTick = 0, revision = 0;
    Stage current = Stage::Detected;
    NrMode nrMode = NrMode::Unknown, fgMode = NrMode::Unknown;
    Authority authority = Authority::RuntimeOwner;
    std::array<Entry, static_cast<std::size_t>(Stage::Count)> stages {};
    bool known = false;
};
constexpr std::size_t Index(Stage stage) noexcept { return static_cast<std::size_t>(stage); }
constexpr bool IsNr(Stage stage) noexcept { return stage >= Stage::NrGuides && stage <= Stage::FirstNrExecute; }

constexpr bool IsFg(Stage stage) noexcept { return stage >= Stage::FgInputs && stage < Stage::Count; }

class Store
{
    std::mutex mutex_;
    Snapshot value_ {};
public:
    bool Begin(const GenshinLegacy::Ui::Session& session, std::uint64_t attempt,
               NrMode nr, std::uint64_t now, NrMode fg = NrMode::Unknown,
               Authority authority = Authority::RuntimeOwner) noexcept
    {
        if (!session.Known() || !attempt || !now) return false;
        std::lock_guard lock(mutex_);
        // Repeat publications for the same attempt cannot erase actual progress.
        if (value_.known && value_.authority == Authority::RuntimeOwner && authority == Authority::UiObservation)
            return false; // A UI observation can never replace a real runtime generation.
        if (value_.known && value_.session == session && value_.attempt == attempt)
        {
            if (authority == Authority::RuntimeOwner && value_.authority == Authority::UiObservation)
            { value_.authority = authority; ++value_.revision; }
            return true;
        }
        value_ = {}; value_.known = true; value_.session = session; value_.attempt = attempt;
        value_.startedTick = now; value_.updatedTick = now; value_.revision = 1; value_.nrMode = nr; value_.fgMode = fg; value_.authority = authority;
        if (nr == NrMode::Disabled || nr == NrMode::Unsupported)
            for (std::size_t i = Index(Stage::NrGuides); i <= Index(Stage::FirstNrExecute); ++i)
                value_.stages[i] = { State::Skipped, nr == NrMode::Disabled ? Reason::Disabled : Reason::Unsupported,
                                     0, 0, now, now };
        if (fg == NrMode::Disabled || fg == NrMode::Unsupported)
            for (std::size_t i = Index(Stage::FgInputs); i < Index(Stage::Count); ++i)
                value_.stages[i] = { State::Skipped, fg == NrMode::Disabled ? Reason::Disabled : Reason::Unsupported,
                                     0, 0, now, now };
        return true;
    }
    bool ConfigureFeature(const GenshinLegacy::Ui::Session& session, std::uint64_t attempt,
                          Stage first, NrMode mode, std::uint64_t now,
                          Authority authority = Authority::RuntimeOwner) noexcept
    {
        if ((first != Stage::NrGuides && first != Stage::FgInputs) || !now) return false;
        std::lock_guard lock(mutex_);
        if (!value_.known || value_.authority != authority || value_.session != session ||
            value_.attempt != attempt || now < value_.startedTick)
            return false;
        auto& current = first == Stage::NrGuides ? value_.nrMode : value_.fgMode;
        if (current == mode) return true;
        current = mode;
        const auto end = first == Stage::NrGuides ? Index(Stage::FirstNrExecute) + 1 : Index(Stage::Count);
        for (auto i = Index(first); i < end; ++i)
        {
            value_.stages[i] = {};
            if (mode == NrMode::Disabled || mode == NrMode::Unsupported)
                value_.stages[i] = { State::Skipped, mode == NrMode::Disabled ? Reason::Disabled : Reason::Unsupported,
                                     0, 0, now, now };
        }
        value_.updatedTick = (std::max)(value_.updatedTick, now); ++value_.revision;
        return true;
    }
    bool Publish(const GenshinLegacy::Ui::Session& session, std::uint64_t attempt,
                 Stage stage, State state, Reason reason, std::int64_t errorCode,
                 std::uint64_t now, Authority authority = Authority::RuntimeOwner) noexcept
    {
        if (Index(stage) >= Index(Stage::Count) || state == State::Unknown || !now) return false;
        std::lock_guard lock(mutex_);
        if (!value_.known || value_.authority != authority || value_.session != session || value_.attempt != attempt ||
            now < value_.startedTick || (IsNr(stage) && value_.nrMode != NrMode::Requested) ||
            (IsFg(stage) && value_.fgMode != NrMode::Requested)) return false;
        auto& entry = value_.stages[Index(stage)];
        if (entry.updatedTick && now < entry.updatedTick) return false;
        // Success is monotonic within an attempt; a new device/resize/config
        // generation must Begin a new identity, never reuse an old success.
        if (entry.state == State::Complete || entry.state == State::Skipped) return state == entry.state;
        if (entry.state == state && entry.reason == reason && entry.errorCode == errorCode) return true;
        if (!entry.startedTick) entry.startedTick = now;
        entry.state = state; entry.reason = reason; entry.errorCode = errorCode; entry.updatedTick = now;
        entry.finishedTick = (state == State::Complete || state == State::Failed || state == State::Skipped) ? now : 0;
        if (state == State::Running || state == State::Failed ||
            (state == State::Complete && Index(stage) >= Index(value_.current))) value_.current = stage;
        value_.updatedTick = (std::max)(value_.updatedTick, now); ++value_.revision;
        return true;
    }
    Snapshot Read() noexcept
    {
        std::unique_lock lock(mutex_, std::try_to_lock);
        return lock.owns_lock() ? value_ : Snapshot {};
    }
    void Clear(const GenshinLegacy::Ui::Session& session, std::uint64_t attempt) noexcept
    {
        std::lock_guard lock(mutex_);
        if (value_.session == session && value_.attempt == attempt) value_ = {};
    }
};
inline Store& Runtime() noexcept { static Store value; return value; }
inline Snapshot Read() noexcept { return Runtime().Read(); }

// Diagnostic UI budgets only. Hook/method scanning and provider cold starts
// get 30s; resource/queue and guide setup 15s; first GPU-visible success 20s.
// Waiting for the user to select FSR gets 15s before showing troubleshooting.
// These conservative defaults are not measured performance targets or ETAs.
constexpr std::uint64_t TimeoutMs(Stage stage) noexcept
{
    constexpr std::array<std::uint64_t, Index(Stage::Count)> budget {
        10000, 15000, 30000, 15000, 15000, 30000, 20000, 20000, 15000, 30000, 20000, 15000, 30000, 20000
    };
    return Index(stage) < budget.size() ? budget[Index(stage)] : 0;
}
struct Progress
{
    unsigned complete = 0, applicable = 0, skipped = 0, unknown = 0;
    bool srReady = false, nrReady = false, fgReady = false, timedOut = false;
    std::uint64_t totalMs = 0, currentMs = 0;
    float fraction = 0;
    Stage stage = Stage::Detected;
    State state = State::Unknown;
};
inline Progress Evaluate(const Snapshot& snapshot, std::uint64_t now) noexcept
{
    Progress out;
    if (!snapshot.known || !snapshot.session.Known() || !snapshot.attempt ||
        now < snapshot.startedTick || Index(snapshot.current) >= Index(Stage::Count)) return out;
    out.totalMs = now - snapshot.startedTick;
    bool srReady = true, nrReady = snapshot.nrMode == NrMode::Requested;
    bool fgReady = snapshot.fgMode == NrMode::Requested;
    for (std::size_t i = 0; i < snapshot.stages.size(); ++i)
    {
        const auto& entry = snapshot.stages[i];
        if (entry.state == State::Skipped) ++out.skipped;
        else { ++out.applicable; if (entry.state == State::Complete) ++out.complete; }
        if (entry.state == State::Unknown) ++out.unknown;
        if (i < Index(Stage::NrGuides)) srReady = srReady && entry.state == State::Complete;
        else if (i <= Index(Stage::FirstNrExecute)) nrReady = nrReady && entry.state == State::Complete;
        else fgReady = fgReady && entry.state == State::Complete;
    }
    out.srReady = srReady; out.nrReady = nrReady; out.fgReady = fgReady;
    if (out.applicable) out.fraction = float(out.complete) / out.applicable;
    out.stage = snapshot.current;
    if (snapshot.stages[Index(out.stage)].state == State::Complete ||
        snapshot.stages[Index(out.stage)].state == State::Skipped)
        for (std::size_t i = 0; i < snapshot.stages.size(); ++i)
            if (snapshot.stages[i].state != State::Complete && snapshot.stages[i].state != State::Skipped)
            { out.stage = static_cast<Stage>(i); break; }
    const auto& current = snapshot.stages[Index(out.stage)];
    out.state = current.state;
    // An unwired next phase stays Unknown, with a diagnostic wait deadline
    // starting at the preceding real event. It never becomes a fake Running.
    const auto start = current.startedTick ? current.startedTick : snapshot.updatedTick;
    if (start && now >= start)
    {
        const auto stop = current.finishedTick ? current.finishedTick : now;
        out.currentMs = stop >= start ? stop - start : 0;
        out.timedOut = (current.state == State::Running || current.state == State::Unknown) &&
            out.currentMs >= TimeoutMs(out.stage);
    }
    if (out.complete == out.applicable && !out.unknown && snapshot.updatedTick >= snapshot.startedTick)
        out.totalMs = snapshot.updatedTick - snapshot.startedTick;
    return out;
}
}

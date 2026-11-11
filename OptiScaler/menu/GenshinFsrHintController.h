// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "CriticalErrorQueue.h"
#include "GenshinNrWritebackNotice.h"
#include <genshin/GenshinLoadingProgress.h>

namespace CriticalUi
{
class GenshinFsrHintController
{
    GenshinFsrHintPolicy policy_;
    Queue queue_;
    GenshinLoading::Snapshot snapshot_ {};
    GenshinLegacy::Ui::Session nrSession_{};
    std::uint64_t nrAttempt_=0, nrRequestedAfter_=0;
    bool nrRequested_=false;
    NrWriteback::Confirmation nrConfirmation_;
public:
    void Update(const GenshinLegacy::Ui::Observation& value, std::uint32_t pid,
                std::uint64_t processStart, std::uint64_t now, std::uint64_t frame,
                std::uint32_t width, std::uint32_t height, bool routeEnabled,
                GenshinLoading::NrMode nrMode) noexcept
    {
        using namespace GenshinLoading;
        using LoadingState = GenshinLoading::State;
        using GenshinLegacy::Ui::HookPhase;
        const auto notice = policy_.Update(value, pid, processStart, now, frame, width, height, routeEnabled);
        const bool valid = value.session.Known() && value.session.processId == pid &&
            value.session.processStart == processStart && value.attempt && value.enabled && routeEnabled &&
            value.phase != HookPhase::Unknown && value.observedTick && value.observedTick <= now &&
            now - value.observedTick <= GenshinFsrHintPolicy::FreshMs;
        snapshot_ = {};
        if (!valid) { nrRequested_=false; queue_.Set(Key::GenshinFsr, Notice::None); return; }
        const bool requested = nrMode == NrMode::Requested;
        if (requested && (!nrRequested_ || nrSession_ != value.session || nrAttempt_ != value.attempt))
        {
            nrSession_=value.session; nrAttempt_=value.attempt; nrRequestedAfter_=now;
            nrConfirmation_.Reset();
        }
        nrRequested_=requested;
        const auto visibleNotice = [&](Notice fallback) {
            // No model-loaded, provider-ready, mode or SR progress shortcuts.
            GenshinSubstage::Snapshot current;
            return requested && GenshinSubstage::Runtime().Read(current) && current.known &&
                nrConfirmation_.Observe(NrWriteback::Runtime(), value, now, nrRequestedAfter_, current.key)
                ? Notice::None : fallback;
        };
        auto& events = Runtime();
        const auto runtime = events.Read();
        if (runtime.known && runtime.authority == Authority::RuntimeOwner)
        {
            // Runtime owns session/attempt and all stage writes. A stale UI
            // sample cannot Begin a competing generation or erase real events.
            if (runtime.session != value.session || runtime.attempt != value.attempt)
            { queue_.Set(Key::GenshinFsr, notice != Notice::None ? notice : Notice::GenshinLoading); return; }
            snapshot_ = runtime;
            queue_.Set(Key::GenshinFsr, visibleNotice(notice != Notice::None ? notice : Notice::GenshinLoading));
            return;
        }
        const auto began = value.hookStartedTick && value.hookStartedTick <= now ? value.hookStartedTick : now;
        if (!events.Begin(value.session, value.attempt, nrMode, began, NrMode::Unknown, Authority::UiObservation))
        { queue_.Set(Key::GenshinFsr, notice != Notice::None ? notice : Notice::GenshinLoading); return; }
        const auto previous = events.Read();
        // UI owns the explicit user enable switch; a runtime Unsupported result
        // is preserved while that switch remains enabled. Never reset SR/FG.
        if (previous.known && (previous.nrMode != NrMode::Unsupported || nrMode == NrMode::Disabled))
            events.ConfigureFeature(value.session, value.attempt, Stage::NrGuides, nrMode, now, Authority::UiObservation);
        const auto publish = [&](Stage stage, LoadingState state, Reason reason) {
            events.Publish(value.session, value.attempt, stage, state, reason, 0, now, Authority::UiObservation);
        };
        events.Publish(value.session, value.attempt, Stage::Detected, LoadingState::Complete, Reason::None, 0, began, Authority::UiObservation);
        events.Publish(value.session, value.attempt, Stage::Hooking, LoadingState::Running, Reason::None, 0, began, Authority::UiObservation);
        const auto hookFinished = value.hookFinishedTick >= began && value.hookFinishedTick <= now
            ? value.hookFinishedTick : now;
        if (value.phase == HookPhase::Initializing)
            publish(Stage::Hooking, LoadingState::Running, Reason::None);
        else if (value.phase == HookPhase::Failed)
            events.Publish(value.session, value.attempt, Stage::Hooking, LoadingState::Failed,
                value.failure == GenshinLegacy::Ui::HookFailure::EntryNotMatched ? Reason::HookNotMatched :
                Reason::HookInstallFailed, 0, hookFinished, Authority::UiObservation);
        else if (value.phase == HookPhase::Installed)
        {
            events.Publish(value.session, value.attempt, Stage::Hooking, LoadingState::Complete, Reason::None, 0, hookFinished, Authority::UiObservation);
            const bool rendered = value.render.calls && value.render.generation && value.render.captureTick &&
                value.render.captureTick <= now && now - value.render.captureTick <= GenshinFsrHintPolicy::FreshMs;
            publish(Stage::WaitingFsr, rendered ? LoadingState::Complete : LoadingState::Running,
                    rendered ? Reason::None : Reason::WaitingObservation);
        }
        // Producer/bridge, DLSS DX12, SR/output, NR and FG need their own typed
        // owner publications; an installed hook never completes those stages.
        snapshot_ = events.Read();
        if (!snapshot_.known || snapshot_.session != value.session || snapshot_.attempt != value.attempt)
        { snapshot_ = {}; queue_.Set(Key::GenshinFsr, notice != Notice::None ? notice : Notice::GenshinLoading); return; }
        queue_.Set(Key::GenshinFsr, visibleNotice(notice != Notice::None ? notice : Notice::GenshinLoading));
    }
    const Queue& Notices() const noexcept { return queue_; }
    const GenshinLoading::Snapshot& ProgressSnapshot() const noexcept { return snapshot_; }
};
}

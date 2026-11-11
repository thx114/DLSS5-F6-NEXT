// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>

namespace GenshinLegacy::Ui
{
// UI telemetry only. No field authorizes capture, dispatch, NR or a provider.
enum class HookPhase : unsigned char { Unknown, Initializing, Installed, Failed };
enum class HookFailure : unsigned char { None, EntryNotMatched, InstallFailed };
struct Session
{
    std::uint32_t processId = 0;
    std::uint64_t processStart = 0, deviceEpoch = 0, controlsEpoch = 0;
    bool operator==(const Session&) const = default;
    bool Known() const noexcept { return processId && processStart && deviceEpoch && controlsEpoch; }
};
struct RenderObservation
{
    std::uint64_t calls = 0, generation = 0, captureTick = 0;
    std::uint32_t instanceCount = 0;
    std::uint32_t renderWidth = 0, renderHeight = 0, displayWidth = 0, displayHeight = 0;
};
struct Observation
{
    Session session {};
    HookPhase phase = HookPhase::Unknown;
    HookFailure failure = HookFailure::None;
    std::uint64_t observedTick = 0, attempt = 0, hookStartedTick = 0, hookFinishedTick = 0;
    bool enabled = false;
    RenderObservation render {};
};

// Nonblocking native-owner snapshot; contention/unknown ownership returns Unknown.
// The initialization phase is readable without taking the initializing owner's lock.
Observation ReadObservation() noexcept;
std::uint64_t ProcessStartIdentity() noexcept;
}

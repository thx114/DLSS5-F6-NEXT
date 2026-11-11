// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "GenshinFsrUiObservation.h"
#include <cstdint>
#include <mutex>

// CPU witnesses only: draw-list creation, actual overlay submission and accepted
// non-test/non-occluded display Present are distinct. No waits, threads or GPU work.
// The runtime owner must schedule the scan at a later safe boundary after Ready.
namespace GenshinLegacy::Ui
{
struct HintKey
{
    Session session {};
    std::uint64_t attempt = 0;
    bool operator==(const HintKey&) const = default;
    bool Known() const noexcept { return session.Known() && attempt; }
};
struct HintFrame
{
    std::uint64_t swapchain = 0, serial = 0;
    std::uint32_t thread = 0;
    bool operator==(const HintFrame&) const = default;
    bool Known() const noexcept { return swapchain && serial && thread; }
};
class HintPresentation
{
    std::mutex mutex_;
    HintKey key_ {};
    HintFrame frame_ {};
    bool drawn_ = false, submitted_ = false, ready_ = false;
    std::uint64_t presentedTick_ = 0;
public:
    bool Request(const HintKey& key) noexcept
    {
        if (!key.Known()) return false;
        std::lock_guard lock(mutex_);
        if (key_ != key) { key_ = key; frame_ = {}; drawn_ = submitted_ = ready_ = false; presentedTick_ = 0; }
        return true;
    }
    bool BeginFrame(const HintKey& key, const HintFrame& frame) noexcept
    {
        if (!frame.Known()) return false;
        std::lock_guard lock(mutex_);
        if (key_ != key || !key.Known()) return false;
        frame_ = frame; drawn_ = submitted_ = false;
        return true;
    }
    bool DrawListBuilt(const HintKey& key, std::uint32_t thread) noexcept
    {
        std::unique_lock lock(mutex_, std::try_to_lock);
        if (!lock.owns_lock() || key_ != key || !key.Known() || !frame_.Known() || frame_.thread != thread)
            return false;
        drawn_ = true; return true;
    }
    bool Submitted(const HintFrame& frame) noexcept
    {
        std::lock_guard lock(mutex_);
        if (!frame.Known() || frame_ != frame || !drawn_) return false;
        submitted_ = true; return true;
    }
    bool CompletePresent(const HintFrame& frame, bool acceptedDisplayPresent, std::uint64_t now) noexcept
    {
        std::lock_guard lock(mutex_);
        if (!frame.Known() || frame_ != frame || !drawn_ || !submitted_) return false;
        // Failure/test/occlusion must pass false. Completion consumes this frame,
        // so a later success cannot reuse a failed submission acknowledgement.
        drawn_ = submitted_ = false;
        if (!acceptedDisplayPresent || !now) return false;
        ready_ = true; presentedTick_ = now; return true;
    }
    bool Ready(const HintKey& key) noexcept
    {
        std::unique_lock lock(mutex_, std::try_to_lock);
        return lock.owns_lock() && key.Known() && key_ == key && ready_ && presentedTick_;
    }
    void Revoke(const HintKey& key) noexcept
    {
        std::lock_guard lock(mutex_);
        if (key_ == key) { key_ = {}; frame_ = {}; drawn_ = submitted_ = ready_ = false; presentedTick_ = 0; }
    }
};
inline HintPresentation& HookHintPresentation() noexcept { static HintPresentation value; return value; }
}

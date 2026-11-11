// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <genshin/GenshinFsrUiObservation.h>
#include <genshin/GenshinFsrSubstageProgress.h>
#include <cstdint>
#include <mutex>

namespace CriticalUi::NrWriteback
{
// Observation only: the native owner must publish AFTER successful final-picture
// copyback ordering, using the exact ticket key. NGX-return/Ready is insufficient.
struct Receipt
{
    GenshinLegacy::Ui::Session session{};
    std::uint64_t attempt=0, ownerEpoch=0, context=0, serial=0;
    std::uint64_t pictureSerial=0, presentEpoch=0, copybackFence=0, copybackValue=0, tick=0;
    bool modelProcessed=false, screenCopyOrdered=false;
    bool Known() const noexcept
    {
        return session.Known() && attempt && ownerEpoch && context && serial && pictureSerial && presentEpoch &&
               copybackFence && copybackValue && tick && modelProcessed && screenCopyOrdered;
    }
};
class Store final
{
    std::mutex mutex_;
    Receipt value_{};
public:
    bool Publish(const Receipt& value) noexcept
    {
        if (!value.Known()) return false;
        std::lock_guard lock(mutex_);
        if (value_.session == value.session && value_.attempt == value.attempt &&
            (value.tick < value_.tick || (value.ownerEpoch == value_.ownerEpoch &&
             value.context == value_.context && value.presentEpoch <= value_.presentEpoch))) return false;
        value_ = value;
        return true;
    }
    bool Matches(const GenshinLegacy::Ui::Observation& observation, std::uint64_t now,
                 std::uint64_t requestedAfter, const GenshinSubstage::Key& currentKey) noexcept
    {
        std::unique_lock lock(mutex_, std::try_to_lock);
        return lock.owns_lock() && value_.Known() && value_.session == observation.session &&
               value_.attempt == observation.attempt && currentKey.Known() && currentKey.session == value_.session &&
               currentKey.ownerEpoch == value_.ownerEpoch && currentKey.context == value_.context &&
               currentKey.serial >= value_.serial && value_.tick > requestedAfter &&
               value_.tick <= now && now - value_.tick < 1000;
    }
};
// A success is freshness-checked when consumed, then retained for that owner/context.
// Progress does not reappear merely because rendering pauses after a successful copy.
// Session/attempt, re-enable, device/config and owner/context changes invalidate it.
class Confirmation final
{
    GenshinSubstage::Key confirmed_{};
    std::uint64_t attempt_=0;
public:
    void Reset() noexcept { confirmed_={}; attempt_=0; }
    bool Observe(Store& store, const GenshinLegacy::Ui::Observation& observation,
                 std::uint64_t now, std::uint64_t requestedAfter,
                 const GenshinSubstage::Key& currentKey) noexcept
    {
        if (!currentKey.Known() || currentKey.session != observation.session) return false;
        if (confirmed_.Known() && attempt_ == observation.attempt &&
            confirmed_.session == currentKey.session && confirmed_.ownerEpoch == currentKey.ownerEpoch &&
            confirmed_.context == currentKey.context && currentKey.serial >= confirmed_.serial) return true;
        Reset();
        if (!store.Matches(observation, now, requestedAfter, currentKey)) return false;
        confirmed_=currentKey; attempt_=observation.attempt; return true;
    }
};
inline Store& Runtime() noexcept { static Store value; return value; }
}

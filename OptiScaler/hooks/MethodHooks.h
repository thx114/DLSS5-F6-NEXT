#pragma once
#include "HookOwnership.h"
#include <array>
#include <vector>

namespace HookOwnership {
// Each actual method implementation has its own detour and original. Indexed
// thunks avoid routing a proxy method through another device's trampoline.
template<class Function, size_t Capacity = 8> class MethodHooks final {
    std::array<Own::Slot, Capacity> slots;
public:
    static constexpr size_t capacity = Capacity;
    class Call final {
        Own::Slot* slot = nullptr;
        Function original = nullptr;
    public:
        explicit Call(Own::Slot& value) noexcept : slot(&value) {
            std::lock_guard lock(Own::PublicationMutex);
            original = Own::Original<Function>(value); ++value.readers;
        }
        ~Call() { std::lock_guard lock(Own::PublicationMutex); --slot->readers; }
        Call(const Call&) = delete;
        Call& operator=(const Call&) = delete;
        Function Get() const noexcept { return original; }
    };
    Call Acquire(size_t index) noexcept { return Call(slots[index]); }
    Function Original(size_t index) noexcept {
        std::lock_guard lock(Own::PublicationMutex);
        return Own::Original<Function>(slots[index]);
    }
    size_t Find(PVOID entry) const noexcept {
        std::lock_guard lock(Own::PublicationMutex);
        const auto canonical = entry ? DetourCodeFromPointer(entry, nullptr) : nullptr;
        for (size_t i = 0; i < Capacity; ++i) {
            const auto& slot = slots[i];
            if (slot.live && slot.transaction && slot.detachReady &&
                (slot.entry == entry || slot.detachReceipt.entry == entry ||
                 slot.detachReceipt.entry == canonical || slot.hook == canonical)) return i;
        }
        return Capacity;
    }
    bool Covers(PVOID entry) const noexcept { return Find(entry) != Capacity; }
    LONG Add(Own::Batch& batch, PVOID entry, const std::array<Function, Capacity>& thunks) noexcept {
        std::lock_guard lock(Own::PublicationMutex);
        if (!entry) return batch.Fail(ERROR_INVALID_PARAMETER);
        if (Covers(entry)) return NO_ERROR;
        for (size_t i = 0; i < Capacity; ++i) if (!slots[i].live && !slots[i].executed) {
            auto& slot = slots[i];
            return batch.Add(slot, &slot.transaction, nullptr, entry, reinterpret_cast<PVOID>(thunks[i]));
        }
        return batch.Fail(ERROR_NOT_ENOUGH_MEMORY);
    }
    void AppendSlots(std::vector<Own::Slot*>& output) noexcept {
        std::lock_guard lock(Own::PublicationMutex);
        for (auto& slot : slots) if (slot.live) output.push_back(&slot);
    }
};
}

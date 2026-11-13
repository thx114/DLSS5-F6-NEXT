// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <span>

#include <Windows.h>
#include <detours/detours.h>

#include <algorithm>
#include <array>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <source_location>
#include <initializer_list>
#include <mutex>

namespace HookOwnership
{
enum class InstallRequirement
{
    Mandatory,
    Optional
};

inline LONG BeginDetourTransaction() noexcept
{
    const auto result = DetourTransactionBegin();
    // Detours acquires transaction ownership before making trampoline pages writable.
    // ERROR_INVALID_OPERATION alone rejects Begin without acquiring ownership.
    if (result != NO_ERROR && result != ERROR_INVALID_OPERATION)
        DetourTransactionAbort();
    return result;
}

inline LONG Begin(InstallRequirement = InstallRequirement::Mandatory) noexcept
{
    return BeginDetourTransaction();
}

inline LONG Abort() noexcept
{
    return DetourTransactionAbort();
}

inline LONG Commit() noexcept
{
    return DetourTransactionCommit();
}

inline void EndTransaction(bool) noexcept {}
inline void Revoke() noexcept {}

namespace detail
{
template <class T> inline T Read(const unsigned char* p) noexcept
{
    T v {};
    std::memcpy(&v, p, sizeof(v));
    return v;
}

inline bool Memory(const void* p, void* out, size_t n) noexcept
{
    SIZE_T got = 0;
    return p && ReadProcessMemory(GetCurrentProcess(), p, out, n, &got) && got == n;
}

inline bool Equal(const void* p, const void* expected, size_t n) noexcept
{
    std::array<unsigned char, 4096> bytes {};
    const auto* address = static_cast<const unsigned char*>(p);
    const auto* source = static_cast<const unsigned char*>(expected);
    while (n)
    {
        const auto take = (std::min)(n, bytes.size());
        if (!Memory(address, bytes.data(), take) || std::memcmp(bytes.data(), source, take) != 0)
            return false;
        address += take;
        source += take;
        n -= take;
    }
    return true;
}

struct Patch
{
    void** original = nullptr;
    void** executed = nullptr;
    void** mirror = nullptr;
    unsigned char* entry = nullptr;
    unsigned char* trampoline = nullptr;
    void* hook = nullptr;
    unsigned size = 0;
    std::array<unsigned char, 30> edit {};
    std::array<unsigned char, 96> code {};
    std::array<unsigned char, 32> ownEntry {};
};

inline bool PatchIntact(const Patch& p) noexcept
{
    void* current = nullptr;
    if (!Memory(p.original, &current, sizeof(current)) || current != p.trampoline)
        return false;
    for (auto* slot : { p.executed, p.mirror })
    {
        if (slot && (!Memory(slot, &current, sizeof(current)) || current != p.trampoline))
            return false;
    }
    return Equal(p.entry, p.edit.data(), p.size) &&
           Equal(p.trampoline, p.code.data(), p.code.size()) &&
           Equal(p.hook, p.ownEntry.data(), p.ownEntry.size());
}

inline bool FinalizeOwnPatch(Patch& p) noexcept
{
    const auto displacement = reinterpret_cast<intptr_t>(p.trampoline + 88) - reinterpret_cast<intptr_t>(p.entry + 5);
    if (displacement < INT32_MIN || displacement > INT32_MAX)
        return false;
    p.edit.fill(0xcc);
    p.edit[0] = 0xe9;
    const auto relative = static_cast<int32_t>(displacement);
    std::memcpy(p.edit.data() + 1, &relative, 4);
    p.code[88] = 0xff;
    p.code[89] = 0x25;
    const int32_t relay = -14;
    std::memcpy(p.code.data() + 90, &relay, 4);
    const auto priorRemainder = Read<uintptr_t>(p.code.data() + 72);
    if (priorRemainder != reinterpret_cast<uintptr_t>(p.entry + p.size) ||
        Read<uintptr_t>(p.code.data() + 80) != reinterpret_cast<uintptr_t>(p.hook))
        return false;
    return PatchIntact(p);
}
} // namespace detail

namespace Own
{
inline std::recursive_mutex PublicationMutex;
struct Slot final
{
    PVOID transaction = nullptr, entry = nullptr, hook = nullptr;
    PVOID* executed = nullptr;
    PVOID* mirror = nullptr;
    bool live = false, detachReady = false;
    unsigned readers = 0; // guarded by PublicationMutex; detachment never waits
    detail::Patch detachReceipt {};
    constexpr Slot() noexcept = default;
    Slot(const Slot&) = delete;
    Slot& operator=(const Slot&) = delete;
};

template <class Function> inline Function& Original(Slot& slot) noexcept
{
    static_assert(sizeof(Function) == sizeof(PVOID));
    return reinterpret_cast<Function&>(slot.transaction);
}

class Batch final
{
    std::unique_lock<std::recursive_mutex> publication_{PublicationMutex};
    struct Item
    {
        Slot* slot = nullptr;
        PVOID prior = nullptr;
    };
    std::array<Item, 32> items_ {};
    unsigned count_ = 0;
    LONG error_ = NO_ERROR;
    bool begun_ = false, finished_ = false;

    void Restore() noexcept
    {
        for (unsigned i = 0; i < count_; ++i)
        {
            auto& s = *items_[i].slot;
            InterlockedExchangePointer(s.executed, items_[i].prior);
            s.transaction = items_[i].prior;
            s.entry = nullptr;
            s.hook = nullptr;
            s.executed = nullptr;
            s.mirror = nullptr;
        }
    }

    void AbortBatch() noexcept
    {
        DetourTransactionAbort();
        Restore();
    }

  public:
    explicit Batch(InstallRequirement requirement = InstallRequirement::Mandatory) noexcept
    {
        error_ = Begin(requirement);
        begun_ = error_ == NO_ERROR;
        if (begun_)
            error_ = DetourUpdateThread(GetCurrentThread());
    }

    ~Batch()
    {
        if (begun_ && !finished_)
            AbortBatch();
    }

    Batch(const Batch&) = delete;
    Batch& operator=(const Batch&) = delete;

    LONG Fail(LONG error) noexcept
    {
        if (error_ == NO_ERROR) error_ = error;
        return error_;
    }

    LONG Add(Slot& slot, PVOID* executed, PVOID* mirror, PVOID entry, PVOID hook,
             HMODULE = nullptr, const char* = nullptr, bool = true,
             const std::source_location& = std::source_location::current()) noexcept
    {
        if (error_ != NO_ERROR)
            return error_;
        if (!executed || !entry || !hook || count_ == items_.size())
            return error_ = ERROR_INVALID_PARAMETER;
        if (slot.live)
        {
            if (slot.executed != executed || slot.mirror != mirror || slot.entry != entry || slot.hook != hook ||
                (mirror && *mirror != *executed))
                return error_ = ERROR_INVALID_OPERATION;
            return NO_ERROR;
        }
        for (unsigned i = 0; i < count_; ++i)
            if (items_[i].slot == &slot)
                return error_ = ERROR_INVALID_PARAMETER;

        items_[count_++] = { &slot, *executed };
        slot.entry = entry;
        slot.hook = hook;
        slot.executed = executed;
        slot.mirror = mirror;
        *executed = entry;

        PDETOUR_TRAMPOLINE trampoline = nullptr;
        PVOID target = nullptr, detour = nullptr;
        error_ = DetourAttachEx(executed, hook, &trampoline, &target, &detour);

        auto& p = slot.detachReceipt;
        p = {};
        p.original = executed;
        p.executed = executed;
        p.mirror = mirror;
        p.entry = static_cast<unsigned char*>(target);
        p.trampoline = reinterpret_cast<unsigned char*>(trampoline);
        p.hook = hook;
        slot.detachReady = error_ == NO_ERROR && detour == hook && target && trampoline &&
                           detail::Memory(trampoline, p.code.data(), p.code.size()) &&
                           detail::Memory(hook, p.ownEntry.data(), p.ownEntry.size());
        if (slot.detachReady)
        {
            p.size = p.code[62];
            slot.detachReady = p.size >= 5 && p.size <= 24 && detail::Equal(target, p.code.data() + 32, p.size);
        }
        return error_;
    }

    LONG Commit() noexcept
    {
        if (!begun_)
            return error_;
        finished_ = true;
        if (error_ != NO_ERROR)
        {
            AbortBatch();
            return error_;
        }
        error_ = DetourTransactionCommit();
        if (error_ == NO_ERROR)
        {
            for (unsigned i = 0; i < count_; ++i)
            {
                auto& s = *items_[i].slot;
                // Both Install's internal slot and AddMethod's external original
                // publish the same committed trampoline for coverage checks.
                s.transaction = *s.executed;
                if (s.mirror && s.mirror != s.executed)
                    InterlockedExchangePointer(s.mirror, *s.executed);
                s.live = true;
                s.detachReady = s.detachReady && detail::FinalizeOwnPatch(s.detachReceipt);
            }
        }
        else
        {
            Restore();
        }
        return error_;
    }
};

inline LONG Install(Slot& slot, PVOID* mirror, PVOID entry, PVOID hook,
                    HMODULE module = nullptr, const char* symbol = nullptr, bool enroll = true,
                    const std::source_location& caller = std::source_location::current()) noexcept
{
    Batch batch;
    batch.Add(slot, &slot.transaction, mirror, entry, hook, module, symbol, enroll, caller);
    return batch.Commit();
}

inline LONG Detach(Slot& slot) noexcept
{
    std::lock_guard publication(PublicationMutex);
    if (slot.readers) return ERROR_BUSY;
    if (!slot.live)
        return NO_ERROR;
    if (!slot.detachReady || !detail::PatchIntact(slot.detachReceipt))
        return ERROR_INVALID_OPERATION;

    auto result = Begin();
    if (result != NO_ERROR)
        return result;
    result = DetourUpdateThread(GetCurrentThread());
    if (result == NO_ERROR)
        result = DetourDetach(slot.executed, slot.hook);
    if (result != NO_ERROR)
    {
        Abort();
        return result;
    }
    result = Commit();
    if (result != NO_ERROR)
        return result;

    if (slot.mirror && slot.mirror != slot.executed)
        InterlockedExchangePointer(slot.mirror, *slot.executed);
    slot.transaction = *slot.executed;
    slot.live = false;
    slot.detachReady = false;
    slot.detachReceipt = {};
    slot.entry = nullptr;
    slot.hook = nullptr;
    slot.executed = nullptr;
    slot.mirror = nullptr;
    return NO_ERROR;
}

inline LONG DetachAll(std::span<Slot* const> slots) noexcept
{
    std::lock_guard publication(PublicationMutex);
    for (const auto* slot : slots) if (slot->readers) return ERROR_BUSY;
    for (const auto* slot : slots)
        if (slot->live && (!slot->detachReady || !detail::PatchIntact(slot->detachReceipt)))
            return ERROR_INVALID_OPERATION;
    auto result = Begin();
    if (result != NO_ERROR) return result;
    result = DetourUpdateThread(GetCurrentThread());
    for (auto* slot : slots)
        if (result == NO_ERROR && slot->live) result = DetourDetach(slot->executed, slot->hook);
    if (result != NO_ERROR) { Abort(); return result; }
    result = Commit();
    if (result != NO_ERROR) return result;
    for (auto* slot : slots)
    {
        if (!slot->live) continue;
        if (slot->mirror && slot->mirror != slot->executed)
            InterlockedExchangePointer(slot->mirror, *slot->executed);
        slot->transaction = *slot->executed;
        slot->live = slot->detachReady = false;
        slot->entry = slot->hook = nullptr;
        slot->detachReceipt = {};
        slot->executed = slot->mirror = nullptr;
    }
    return NO_ERROR;
}
inline LONG DetachAll(std::initializer_list<Slot*> slots) noexcept
{ return DetachAll(std::span<Slot* const>(slots.begin(), slots.size())); }
} // namespace Own
inline LONG AddMethod(HookOwnership::Own::Batch& hooks, HookOwnership::Own::Slot& slot,
                      PVOID* original, void** table, unsigned index, PVOID hook,
                      const std::source_location& caller = std::source_location::current()) noexcept
{
    return hooks.Add(slot, original, nullptr, table[index], hook, nullptr, nullptr, false, caller);
}
} // namespace HookOwnership

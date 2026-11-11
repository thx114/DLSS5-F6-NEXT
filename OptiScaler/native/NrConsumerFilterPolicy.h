#pragma once
#include <cstdint>

namespace native::nrfilter
{
// CPU admission only. Neither this key nor a successful comparison mints GPU
// readiness or a natural-runtime binding.
struct Key
{
    uintptr_t child = 0, producer = 0, consumer = 0;
    uint64_t epoch = 0;
    unsigned long thread = 0;
};
inline bool Matches(const Key& owner, const Key& caller) noexcept
{
    return owner.child && owner.producer && owner.consumer && owner.epoch && owner.epoch != UINT64_MAX &&
        owner.thread && owner.child == caller.child && owner.producer == caller.producer &&
        owner.consumer == caller.consumer && owner.epoch == caller.epoch && owner.thread == caller.thread;
}
inline bool ReadyValueValid(uint64_t value, uint64_t completed) noexcept
{
    // Pending work is allowed only through its real queue Wait; device removal
    // is never a completed fence, and zero/saturated values grant no readiness.
    return value && value != UINT64_MAX && completed != UINT64_MAX;
}
// Both values belong to the wrapper's copy timeline, checked immediately
// after its optional native overwrite and BEFORE any lifecycle drain advances it.
inline bool FinalCopyReceiptValid(uint64_t issued, uint64_t published) noexcept
{ return issued && issued != UINT64_MAX && issued == published; }
template<class Ring> bool HasUncertainCopy(const Ring& values) noexcept
{
    for (auto value : values) if (value == UINT64_MAX) return true;
    return false;
}

enum class Decision { Continue, Stop };
inline Decision AfterAttempt(bool uncertain) noexcept
{
    return uncertain ? Decision::Stop : Decision::Continue;
}
// The disabled branch must not invoke even source capture, GetDevice or Signal.
template<class Access, class Selected, class Work>
bool BeforeRead(Access&& access, Selected&& selected, Work&& work)
{
    // Persistent physical-content denial is independent of the menu setting.
    // In particular an uncertain earlier recording cannot be consumed by
    // switching the stage off. Selection/work are never queried when denied.
    if (!access()) return false;
    return !selected() || work();
}
}

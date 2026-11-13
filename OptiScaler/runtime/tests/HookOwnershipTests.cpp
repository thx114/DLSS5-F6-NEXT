// SPDX-License-Identifier: GPL-3.0-or-later
#include <hooks/HookOwnership.h>
#include <cassert>
#include <cstdio>

namespace
{
__declspec(noinline) int __cdecl Target(int input)
{
    volatile int value = input;
    return value * 3 + 7;
}
using Function = int(__cdecl*)(int);
Function original = nullptr;
Function mirrored = nullptr;
unsigned forwards = 0;
__declspec(noinline) int __cdecl Hook(int input)
{
    ++forwards;
    return original(input) + 100;
}
}

int main()
{
    using namespace HookOwnership;
    Own::Slot slot;
    const auto entry = reinterpret_cast<PVOID>(&Target);
    const auto hook = reinterpret_cast<PVOID>(&Hook);
    const auto add = [&](Own::Batch& batch) {
        return batch.Add(slot, reinterpret_cast<PVOID*>(&original), reinterpret_cast<PVOID*>(&mirrored), entry, hook);
    };
    for (unsigned install = 0; install < 2; ++install)
    {
        Own::Batch batch;
        assert(add(batch) == NO_ERROR && batch.Commit() == NO_ERROR);
        assert(slot.live && slot.detachReady && slot.transaction == reinterpret_cast<PVOID>(original));
        assert(mirrored == original && Target(4) == 119 && forwards == install + 1);
        {
            Own::Batch duplicate;
            assert(add(duplicate) == NO_ERROR && duplicate.Commit() == NO_ERROR);
            assert(original(4) == 19 && mirrored(4) == 19);
        }
        const auto trampoline = original;
        original = &Target;
        assert(Own::Detach(slot) == ERROR_INVALID_OPERATION && slot.live);
        original = trampoline;
        assert(Own::DetachAll({&slot}) == NO_ERROR);
        assert(!slot.live && !slot.executed && !slot.mirror && Target(4) == 19);
        assert(original == &Target && mirrored == original && slot.transaction == entry);
    }
    {
        Own::Batch rollback;
        assert(add(rollback) == NO_ERROR);
        assert(rollback.Add(slot, nullptr, nullptr, entry, hook) != NO_ERROR);
        assert(rollback.Commit() != NO_ERROR);
        assert(!slot.live && Target(4) == 19 && original == &Target);
    }
    Own::Slot internal;
    assert(Own::Install(internal, reinterpret_cast<PVOID*>(&original), entry, hook) == NO_ERROR);
    assert(internal.transaction == reinterpret_cast<PVOID>(original) && Target(4) == 119);
    assert(Own::Detach(internal) == NO_ERROR && !internal.live && Target(4) == 19);
    std::puts("PASS production Detours external/internal originals, coverage, duplicate install, rollback, detach and reinstall");
}

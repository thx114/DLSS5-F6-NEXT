// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "HookOwnership.h"

namespace HookTransactions
{
inline bool Enabled() noexcept
{
    return false;
}

class Transaction final
{
    struct Mirror
    {
        PVOID* executed = nullptr;
        PVOID* alias = nullptr;
    };
    std::array<Mirror, 16> mirrors_ {};
    unsigned mirrorCount_ = 0;
    LONG error_ = NO_ERROR;
    bool begun_ = false, finished_ = false;

    bool StageMirror(PVOID* original, PVOID* mirror) noexcept
    {
        if (!mirror || mirror == original)
            return true;
        if (!original || mirrorCount_ == mirrors_.size() || *mirror != *original)
            return false;
        for (unsigned i = 0; i < mirrorCount_; ++i)
            if (mirrors_[i].alias == mirror || mirrors_[i].executed == mirror || mirrors_[i].alias == original)
                return false;
        mirrors_[mirrorCount_++] = { original, mirror };
        return true;
    }

  public:
    Transaction() noexcept
    {
        error_ = HookOwnership::BeginDetourTransaction();
        begun_ = error_ == NO_ERROR;
        if (begun_)
            error_ = DetourUpdateThread(GetCurrentThread());
    }

    ~Transaction()
    {
        if (begun_ && !finished_)
            DetourTransactionAbort();
    }

    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;

    template <class Function>
    LONG Attach(PVOID* original, Function hook, PVOID* mirror = nullptr,
                const std::source_location& = std::source_location::current()) noexcept
    {
        if (error_ != NO_ERROR)
            return error_;
        if (!StageMirror(original, mirror))
            return error_ = ERROR_INVALID_PARAMETER;
        error_ = DetourAttach(original, reinterpret_cast<PVOID>(hook));
        return error_;
    }

    template <class Function>
    LONG AttachInputMethod(PVOID* original, Function hook, IUnknown*, unsigned,
                           const std::source_location& = std::source_location::current()) noexcept
    {
        if (error_ != NO_ERROR)
            return error_;
        error_ = DetourAttach(original, reinterpret_cast<PVOID>(hook));
        return error_;
    }

    template <class Function>
    LONG Detach(PVOID* original, Function hook, PVOID* mirror = nullptr) noexcept
    {
        if (error_ != NO_ERROR)
            return error_;
        if (!StageMirror(original, mirror))
            return error_ = ERROR_INVALID_PARAMETER;
        error_ = DetourDetach(original, reinterpret_cast<PVOID>(hook));
        return error_;
    }

    LONG Commit() noexcept
    {
        if (!begun_ || finished_)
            return error_ == NO_ERROR ? ERROR_INVALID_OPERATION : error_;
        finished_ = true;
        if (error_ != NO_ERROR)
        {
            DetourTransactionAbort();
            return error_;
        }
        error_ = DetourTransactionCommit();
        if (error_ == NO_ERROR)
        {
            for (unsigned i = 0; i < mirrorCount_; ++i)
                InterlockedExchangePointer(mirrors_[i].alias, *mirrors_[i].executed);
        }
        return error_;
    }
};
} // namespace HookTransactions

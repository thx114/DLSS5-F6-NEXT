// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "D3D11On12FactoryBoundary.h"
#include "MethodHooks.h"

namespace D3D11On12FactoryRoutes
{
using Function = HRESULT(WINAPI*)(UINT, REFIID, void**);
inline HookOwnership::MethodHooks<Function> routes;
template<size_t Index> HRESULT WINAPI Thunk(UINT flags, REFIID iid, void** output)
{
    auto call = routes.Acquire(Index);
    if (!call.Get()) return E_UNEXPECTED;
    return D3D11On12FactoryBoundary::Returned(call.Get()(flags, iid, output), flags, iid, output);
}
inline LONG Install(PVOID entry) noexcept
{
    constexpr auto thunks = []<size_t... Index>(std::index_sequence<Index...>) {
        return std::array<Function, sizeof...(Index)> { &Thunk<Index>... };
    }(std::make_index_sequence<decltype(routes)::capacity> {});
    HookOwnership::Own::Batch batch;
    routes.Add(batch, entry, thunks);
    return batch.Commit();
}
}

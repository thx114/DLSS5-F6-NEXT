// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <unknwn.h>
#include <wrl/client.h>
#include <cstdint>

namespace ComIdentity
{
inline uintptr_t Get(IUnknown* object) noexcept
{
    Microsoft::WRL::ComPtr<IUnknown> identity;
    return object && SUCCEEDED(object->QueryInterface(IID_PPV_ARGS(&identity)))
        ? reinterpret_cast<uintptr_t>(identity.Get()) : 0;
}
}

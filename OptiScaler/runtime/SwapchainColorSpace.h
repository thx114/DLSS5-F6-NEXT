#pragma once
#include <dxgi1_4.h>
#include <optional>

namespace SwapchainColorSpace
{
// An explicit declaration on this exact chain. Buffer format and another
// chain's output state do not establish the transfer of these pixels.
inline constexpr GUID Key = {0x34a31e7b, 0x84c5, 0x44ef,
    {0xa7, 0x4d, 0x6b, 0xd3, 0x60, 0x8c, 0xe5, 0x22}};

inline std::optional<DXGI_COLOR_SPACE_TYPE> Read(IDXGIObject* chain) noexcept
{
    if (!chain) return std::nullopt;
    DXGI_COLOR_SPACE_TYPE value = DXGI_COLOR_SPACE_CUSTOM;
    UINT bytes = sizeof(value);
    if (FAILED(chain->GetPrivateData(Key, &bytes, &value)) || bytes != sizeof(value))
        return std::nullopt;
    return value;
}

inline bool ExplicitSdr(IDXGIObject* chain) noexcept
{
    return Read(chain) == DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
}

inline HRESULT Write(IDXGIObject* chain, DXGI_COLOR_SPACE_TYPE value) noexcept
{
    return chain ? chain->SetPrivateData(Key, sizeof(value), &value) : E_INVALIDARG;
}
}

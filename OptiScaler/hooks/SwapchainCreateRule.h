#pragma once

#include <Windows.h>

namespace SwapchainCreate
{
// Successful DXGI status codes can still return a real chain. Never publish a missing output.
template <typename T> constexpr bool Created(HRESULT result, T* const* swapChain)
{
    return SUCCEEDED(result) && swapChain != nullptr && *swapChain != nullptr;
}
} // namespace SwapchainCreate

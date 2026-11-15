// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Only the opt-in fixture DLL contains this ABI. It borrows the very resource
// passed to FG; it never locates private members or calls the wrapper GetBuffer.
#include <d3d12.h>
#include <dxgi1_5.h>
#include <cstdint>

namespace F6PresentationProbe
{
enum class Stage : uint32_t { RawCopy, AfterNr, FgInput };
struct Sample
{
    Stage stage;
    IDXGISwapChain4* chain;
    ID3D12Resource* color;
    ID3D12CommandQueue* queue;
    ID3D12Fence* fence;
    uint64_t value;
    uint32_t dx11Index, fgIndex, nrResult;
};
using Observe = void (WINAPI*)(const Sample*);
using Install = void (WINAPI*)(Observe);
}

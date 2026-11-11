#pragma once

#include "SysUtils.h"
#include <climits>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>

namespace MenuOverlayDx
{
ID3D12GraphicsCommandList* MenuCommandList();
// A shader-visible descriptor from the menu's own heap, for showing a texture with ImGui::Image (the GPU handle is the
// ImTextureID). SrvHeap() changes when the menu is torn down and rebuilt: handles from the old one are then dead.
ID3D12DescriptorHeap* SrvHeap();
bool AllocSrv(D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu);
void FreeSrv(D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE gpu);
void CleanupRenderTarget(bool clearQueue, HWND hWnd);
// Written means a draw was issued/executed for this call, even if its later fence
// Signal failed. It never claims GPU completion or successful display.
struct PresentResult
{
    bool OverlayWritten = false;
    bool ImmutableOutput = false;
    const char* Decision = "not-called";
    UINT BackbufferIndex = UINT_MAX;
    UINT ReplaySlot = UINT_MAX, BusySlots = 0;
    const char* RouteReason = "unchecked";
    const char* RefreshReason = "none";
    UINT64 Generation = 0, SnapshotGeneration = 0, Epoch = 0;
    UINT64 SubmittedFence = 0, CompletedFence = 0, BackbufferFence = 0, DrainFence = 0;
    IUnknown* Queue = nullptr;
    int CompletionState = 0;
    bool DrainPending = false, WaitAttempted = false;
};
void Present(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags,
             const DXGI_PRESENT_PARAMETERS* pPresentParameters, IUnknown* pDevice, HWND hWnd, bool isUWP,
             bool waitForOutputMenu = false, PresentResult* result = nullptr);
void ApplyThemeStyle();
} // namespace MenuOverlayDx

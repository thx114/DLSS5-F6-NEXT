#pragma once

#include "pch.h"

#include "GenericDepth_Select.h"

// Finds the scene's depth buffer in a Direct3D 11 game by watching how the game uses its depth-stencil views, with no ReShade
// and no help from an upscaler call: the D3D11 adapter of the native input producer (see docs/NATIVE-INPUT-ADAPTERS.md), built on
// the same native::DepthFinderCore as the D3D12 one (resource_tracking/GenericDepth_Dx12.h).
//
// D3D11 makes this simpler than D3D12 in two ways this file relies on: a depth-stencil view answers GetResource() directly (no
// descriptor tracking is needed), and almost every game draws through one context (the immediate context; deferred contexts are
// not watched yet, so a game that draws its scene only through one is not seen -- a per-game limitation, recorded as such).
//
// Off unless [DlssNr] NativeDepthFinder is set (checked at the device's first use, since OptiScaler attaches to a D3D11 game
// later than it does a D3D12 one). When on, the hooks observe ClearDepthStencilView, OMSetRenderTargets(AndUnorderedAccessViews),
// RSSetViewports and the draw calls on the immediate context, and copy the picked buffer into the finder's own textures there
// (native/DepthCopyDx11.h: the game's compute bindings it uses for that are put back; nothing else of the game's is changed).
// The hooks patch the functions, which every context of the kind shares: only the watched context's calls are counted.
namespace GenshinQueryScope { class CreationWitness; }
namespace GenericDepthDx11
{
// Called only with the private factory witness, before exposing the new device.
// Commits the full context hook set and seeds only an absent context state; failure
// cancels the witness and never blocks the ordinary device/SR initialization.
bool PrepareCreatedContextQueries(ID3D11Device*, GenshinQueryScope::CreationWitness&);

// R587: exact SDK CreateDeferredContext[1..3] return notification. Records only
// context-owned birth metadata after checking type and canonical device owner.
// Deferred replay is not covered by the immediate-context hook set: no fresh
// query state/certificate is created, and existing UnknownPast is untouched.
HRESULT NoteDeferredContextBirth(ID3D11Device*, ID3D11DeviceContext*, unsigned apiVersion,
    uint64_t entryTick, uint64_t returnTick, const void* caller, const void* executed) noexcept;

// Hooks the immediate context once a D3D11 device is known. A later bridge request
// may add its query/state hooks to the same common targets without reattaching
// them. Neither this late entry nor a completed upgrade supplies a birth witness.
void Install(ID3D11Device* device, bool genshinBridge = false);
// Complete the retained optional CPU binding-cache request only after the
// calling device initializer's owned shader/sampler hook transactions return.
// Does not seed query history or refresh an installed coverage generation.
void CompletePsStateCacheInstall(ID3D11Device* device) noexcept;

// Once per presented frame: closes the frame's counts, picks, and logs. The swap chain gives the picture's size, and its
// device the context watched: a device the game makes again is followed once it presents (a D3D12 swap chain, as under
// Dx11wDx12, says nothing, and the context Install found is kept).
void OnPresent(IDXGISwapChain* swapChain);
// Native D3D11 outer Present owner only, before its global PRE/overlay. Does
// only existing MOD installation/preparation/retirement; no depth-frame close,
// new hook, draw, POST or original Present. The real native chain must expose
// the exact captured device/context; D3D12/provider/generated routes refuse.
void PrepareModPresent(IDXGISwapChain* swapChain) noexcept;

GenericDepthSelect::Pick CurrentPick();

// Copies of the picked depth buffer from the frame just presented, for the producer's adapter. One context means one copy (no
// split-scene case as D3D12 has across many command lists), so this always has at most one entry when it has any.
struct Snapshot
{
    bool valid = false;
    // A plain (non-shared) D3D11 copy; the adapter shares it across to D3D12 itself. Holds a reference, so the copy outlives
    // a replacement of it made while the adapter is still using this one.
    Microsoft::WRL::ComPtr<ID3D11Resource> resource;
    DXGI_FORMAT typelessFormat = DXGI_FORMAT_UNKNOWN; // the copy's own (typeless) format, for making a shared copy of it
    DXGI_FORMAT viewFormat = DXGI_FORMAT_UNKNOWN; // a typed format that reads its depth
    uint32_t width = 0;
    uint32_t height = 0;
    bool reversed = false; // near is 1.0
};

Snapshot BestSnapshot();

bool Installed();
// Existing internal suppression, shared with the statically integrated MOD adapter.
bool ModInternal() noexcept;
void BindModRuntime(ID3D11Device* device) noexcept;
// Preserve early creation observation after the initializer's shader hooks;
// Present uses the same per-request claim after following its actual device.
void CompleteModCreationHookInstall(ID3D11Device* device) noexcept;
// Existing D3D11 hook owner calls before StopAccepting/native revocation.
// Revokes MOD admission; common process-lifetime context hooks stay installed.
void RevokeModStreamOutputCoverage() noexcept;

// The key was on at this start but Install() gave up (the log says why).
bool InstallFailed();

// Called when the game creates an upscaler feature of its own: a game with one needs no depth finder, so it stands down for
// good (the same rule and wake-up as the D3D12 finder).
void NoteUpscalerCall();
bool GameCallsUpscaler();
bool Armed();

// ImGui: the menu's one depth line (off, needs a restart, could not start, stood down, watching, picked), no checkbox.
// Call inside the menu, on the Optical F5Low page (dlssnr/DlssNr_Menu.cpp).
void DrawStatus();

// ImGui: the "Use the game's depth" checkbox and its tooltip. [DlssNr] NativeDepthFinder is also turned on and off by
// the menu's mode selector (dlssnr/DlssNr_NativeMode.h); this is for setting it on its own. Call inside the menu's
// Advanced section.
void DrawAdvancedUi();
} // namespace GenericDepthDx11

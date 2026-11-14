# D3D11On12 internal factory regression

Run `pwsh -NoProfile -File OptiScaler/hooks/tests/on12-factory/run.ps1` from the repository root. Outputs remain in `build/genshin-reshade-20261114/on12-factory`; the installed ReShade core and user settings are read only.

The host copies and hashes the pinned ReShade 6.8.0.2155 core, creates a native WARP D3D12 device before loading it, and calls its real D3D11On12 export. Production `D3D11On12FactoryRoutes`, `MethodHooks`, COM return normalization and `HoYoShade::AcquireExistingFactoryOwner` execute directly.

- `negative`: reproduces the DXGI `0x80000003` breakpoint while ReShade wraps the private adapter.
- `system-only-negative`: the system hook is installed before ReShade, which then wraps its return; this must still reproduce the breakpoint.
- `before` / `after`: install the two real export routes in both orders; repeat On12 creation and actual GPU buffer readback. Public factories retain their proxies, and a concurrent thread does not inherit the internal scope.

Both positive runs also check the loaded-core owner receipt and reject an incorrect in-memory ImageBase before restoring the isolated host's header. The installed DLL is never changed. Separate CommonCore suite `D3D11On12FactoryBoundary` checks ownership, adjusted interface pointers, failure cleanup, cycles, thread-local nesting and an actual Detours path from a D3D11 factory into On12 with nested proxy/native calls.

These hosts do not execute a game, NVIDIA NR/SR/FG or scanout. Game acceptance remains separate.

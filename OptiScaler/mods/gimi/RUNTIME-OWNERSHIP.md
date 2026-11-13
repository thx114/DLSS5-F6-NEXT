# F6 built-in GIMI capture ownership

The built-in runtime uses the existing public DX11 hook owner. It does not
load a second GIMI renderer, wrap another device or add a queue. GIMI remains
off by default. This document describes capture routing; it does not extend
the INI/shader language or qualify an entire MOD package.

## Device admission

Before the first selected native Present, `DeviceCreated(device, context,
hooks, coverage)` registers up to four qualified provisional captures. Their
real creation observations and writes retain separate identity/loss slots;
none runs the interpreter. The first callback need not belong to the game.
CPU Update/Copy returns and Map/Unmap publish to their still-current candidate
route, so selecting a later candidate preserves its already-completed contents.
The public hook owner must register each device and report its actual creation
hook coverage before those resources are created. A later Present does not
reconstruct missed observations. Capacity exhaustion is explicit, with no
invented resource history.

`PresentOwner` promotes an existing capture without changing its generation,
resource registry or write/loss history, then clears the other provisional
routes. After confirmation, another device's ordinary creation notification
cannot register a candidate, park the interpreter or replace the owner.

The public hook owner may explicitly select another pair by calling:

```cpp
gimi::runtime::DeviceCreated(device, context, hooks, coverage,
                            gimi::runtime::CaptureSource::PresentOwner);
```

This argument is an ownership decision made after the existing game Present
selection/`FollowPresentDevice`/`UseContext` decision. It must not be passed by
arbitrary device creation, auxiliary swap-chain callbacks or state-swap hook
reinstallation. The shared `GenericDepth_Dx11.cpp` integration belongs to the
F6 main session. The default argument preserves source compatibility for its
existing ordinary notifications.

Activation still verifies the real context's device and immediate type.
On migration the explicit selected Present first saves the old owner's dirty
semantic state, on its original serial thread and outside a native/raster
pair. A failed save leaves that owner in place. It preserves native PRE/POST,
packet retention, fence and quarantine rules. A capture is published only after its retained
device/context and unique generation are ready. Reusing the same native
pointers does not reuse that generation. Previously unobserved resources
remain unknown; activation does not invent creation coverage for them.

## Event routing

| Ingress | Runtime behavior |
|---|---|
| Current context and generation | Process the event under the existing owner mutex; preserve thread and coverage checks |
| Another device | Ignore it before registry locking; do not modify the primary capture or interpreter |
| Same device, another context | Keep captured-resource writes and unknown/deferred replay invalidating; do not replace the primary SO binding snapshot |
| Stale resource/map/return generation | Preserve required old-owner writeback and cleanup; do not invalidate a replacement capture |
| Another device's Present | Poll retained packets through each session's original owner; do not save/advance active semantic state or consume its reload revision |

`GimiCaptureRouting.h` publishes a non-owning identity snapshot. Its pointer
keys are compared, never dereferenced as leases. The retained `Capture` and
its mutex still authorize registry access. A callback checks the generation
again after obtaining the mutex. Contexts matching the published immediate
context need no device getter. Only a different context requires `GetDevice`
to distinguish an unrelated device from a possible same-device writer.

The fixed resource-key prefilter includes the authentic capture generation.
An old process-lifetime key cannot authorize a current-generation lookup or
turn a delayed old write/Map into a failure of the new owner. Ordinary
unregistered resource writes also avoid the owner mutex.

Coverage failure records its generation monotonically within its stable slot.
Promoting an earlier candidate cannot inherit a later auxiliary candidate's
failure. A delayed old callback
cannot clear a newer failure or poison a later capture. Reader paths already
holding the retained capture check this latch with one atomic load. This is
CPU routing only: it is not GPU completion or permission to release packets.
Disabling observation invalidates every provisional capture, preventing a
quick re-enable from accepting content across an unobserved Map interval.

## Verification

Run from the worktree root with the existing MSVC/Windows SDK installation:

```powershell
python -B OptiScaler/mods/gimi/tests/test_capture_routing.py
pwsh -NoProfile -File tools/tests/Run-CommonCoreTests.ps1 `
  -SuiteName CpuMappingTransactions,Dx11RasterBindings -SkipSharedRegression
```

The dedicated test extracts the production ingress/return bodies and uses
real WARP immediate/deferred contexts, buffers and two devices. Interpreter
loading and GPU-packet retirement are counters; content cases exercise CPU
publication/writeback. It also stresses concurrent routing publication and
failure recording. It does not run a game, MOD package, NGX or FG SDK.

`--negative` accepts `opaque`, `present`, `device`, `provisional`,
`provisional-return`, `provisional-unmap`, `stale-map` or `stale-return`.
Each removes one protection from the extracted production code and must fail a runtime assertion,
not compilation. Build and test outputs stay in `build/gimi-owner-routing/`.
Actual hook selection, skeleton/LOD/shadow compatibility, pixels and net
performance still require the main session's integration and user-run games.

The adjacent `research-gimi-lead-20261108` and
`research-gimi-review-20261108` are historical external integration references.
Their GIMI native-device/hosted-ReShade design and acceptance do not establish
F6's resource retirement or game acceptance. The shared source registry is
`D:/CODE/DLSS5_NEXT/docs/REFERENCE-REPOSITORIES.md`; no external implementation
was copied for this routing change.

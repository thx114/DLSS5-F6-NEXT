# Genshin FSR Bridge upstream update guide

As of 2026-10-18 (Asia/Shanghai). Status: maintainer procedure only; no
product source, build, deployment, or configuration change was made. The
current internal FSR candidate remains observation-only and `NativeDiscovery`
semantics are not proven. This document does not authorize loading the Bridge
as a second injected DLL.

## Pinned source and current upstream status

The local checkout is `D:\CODE\genshin_fsr_brigde` (spelling preserved).
Its remotes are:

```powershell
git -C 'D:\CODE\genshin_fsr_brigde' remote -v
# origin   https://github.com/thx114/genshin_fsr_brigde.git
# upstream https://github.com/AizawaHikaru233/genshin_fsr_brigde.git
```

The local branch is `frame-generation`, HEAD is
`366e778221f5b0521ba9c95838f1e731d9db26db`, commit date
`2026-10-09T05:28:18+08:00`, subject `Release 2.3.5: depth provider and skin guide diagnostics`.
The checkout is dirty; do not use it as an update base without first recording
the dirty file list. Fixed source hashes used by the R26 review are:

| file | SHA256 |
|---|---|
| `Dx11FsrBridge/Dx11FsrBridge.cpp` | `0716B1B7AE9850B212B811B50B269C16E54F58819548A2FA631B614F50D43AE2` |
| `Fsr2TranslationLayer.cpp` | `E769A32538FD0FD4CBB42C2BFBEDD9DC98C2F52820FBBC41A7275C29291D0ECA` |
| `Fsr2TranslationLayer.h` | `978B37D4F80810B1DBC68589F70C03BBE552A6F6C228D5D883C6490C4AE951E5` |
| `Il2CppCallSiteHook.cpp` | `3A3560C6341CA704F0470D60A50B4A4FB82FD5812D2E2F2611CC4A8F38187C9E` |
| `Il2CppCallSiteHook.h` | `9CD89F2F2C8765E80B3D90B413B8D2E2BD801B08460A7E9EF63DB09BAB77094C` |

On this round `git ls-remote` was attempted against `origin` and failed with
`Recv failure: Connection was aborted`. An official GitHub page check against
`https://github.com/AizawaHikaru233/genshin_fsr_brigde` was reachable and
confirms the repository and its visible default branch `main`; the page did
not expose a machine-readable latest commit/ref in this run. The requested
`frame-generation` latest ref therefore remains `UNKNOWN` (the visible
default branch is not substituted for it), and no hook change is claimed from
the page alone.
When network is available, verify without changing the local branch:

```powershell
git -C 'D:\CODE\genshin_fsr_brigde' ls-remote --symref upstream HEAD refs/heads/main refs/heads/master 'refs/tags/*'
git -C 'D:\CODE\genshin_fsr_brigde' fetch --prune upstream
git -C 'D:\CODE\genshin_fsr_brigde' log -1 --format='%H%n%ad%n%s' --date=iso-strict upstream/main
```

Do not fetch into or reset the dirty `frame-generation` branch. Create a
temporary detached inspection worktree or inspect `upstream/<branch>` after
fetch; retain the exact ref and commit in the compatibility manifest.

## What to compare

The Bridge's operational hook surface is in
`D:\CODE\genshin_fsr_brigde\Dx11FsrBridge\Dx11FsrBridge.cpp`:

- device/swapchain and D3D11 context hooks: `hooked_create_*`,
  `hooked_ps_set_shader_resources`, `hooked_*_set_constant_buffers`,
  `hooked_om_set_render_targets`, `hooked_om_set_render_targets_and_uavs`,
  and `hooked_dispatch`;
- render-token transport: `Il2CppCallSiteHook.cpp/.h`, including the inline
  `FFX_FSR2::Render` patch, pending token generation and render/display sizes;
- candidate binding and MRT scoring: the resource/CB/shader tracking around
  `OMGetRenderTargets`, `PSGetShaderResources`, reflection and the configured
  `color_slot`, `depth_slot`, `motion_slot`, `output_rtv_slot`;
- backend and copyback: `Fsr2TranslationLayer.h/.cpp`,
  `Fsr2TranslationFrame`, `dispatch_fsr2_translation`, input preparation,
  depth inversion/Y policy, motion decode/scale, exposure selection and
  output encoding.

The internal adapter is under
`D:\DATA\Documents\ChatGPT\DLSS F5 NEXT\work\full-source-f5-v11-candidate-20261018\OptiScaler`:
`genshin/GenshinLegacyCapture.cpp`, `genshin/legacy/Il2CppCallSiteHook.*`,
`inputs/GenshinFsrContract.*`, `inputs/GenshinFsrDx11.*`,
`genshin/GenshinTypedGuides.*`, and `genshin/GenshinEngineProducer.*`.
Existing internal translation/preparation and output copyback are the only
implementation to reuse. Do not reintroduce `Dx11FsrBridge.dll` beside
OptiScaler.

| Upstream change | Internal mapping | Triage |
|---|---|---|
| IL2CPP Render signature/RVA/field offsets, prologue or token layout | `genshin/legacy/Il2CppCallSiteHook.*`, token matching in `GenshinLegacyCapture.cpp` | High: update only with new source TU compile and negative token controls. |
| Hooked D3D11 API signature or vtable index | `GenshinFsrDx11.*`, `GenshinLegacyCapture.cpp` resource capture | High; ABI/lifetime change, never mechanical. |
| Stable profile values such as jitter mode/delay, reverse-Z/Y flag, motion formula/scale, exposure mode | `GenshinTypedGuides.*`, `GenshinFsrContract.*`, existing `PrepareShader` inputs | Mechanical only after the field is proven for the same generation; keep as versioned profile data. |
| Resource slots, CB range/layout, shader signature or bytecode constants | `GenshinLegacyCapture.cpp` bindings/`PrepareShader`, `GenshinFsrContract.*` | High; CB ABI or resource semantic change. Unknown remains fail-closed. |
| MRT count, companion output role, later consumer/readback | `GenshinLegacyCapture.cpp` native-draw and copyback policy | High; preserve native draw and every MRT until same-generation consumer evidence exists. |
| FSR translation/backend internals | `inputs/GenshinFsrDx11.*` and existing internal FSR2 translation | High and compatibility review; do not import provider ABI or device lifecycle. |
| UI, logging, diagnostics, or menu-only changes | no GPU contract mapping | Review separately; never treat as FSR proof. |

Stable profile parameters must be separated from mandatory synchronized hook
fields. Do not synchronize external Bridge device ownership, provider
creation/destruction, `DllMain` behavior, Present teardown, or other lifecycle
code into OptiScaler. There must be one injected implementation and one owner
of device/provider lifetime.

## Fast update procedure

Use exactly one of these three paths after the targeted diff:

1. **Signature-only:** update the pinned Render hook/signature/RVA and token
   parser mapping; run the real source-TU compile plus token positive/negative
   controls. Do not change CB, slots, MRT, or consumer policy.
2. **CB ABI or slot change:** update the typed binding/CB/slot contract only
   after shader reflection or same-generation capture proves it. Treat as a
   high implementation change; retain native draw and fail closed on unknown.
3. **MRT or consumer change:** update output ownership and consumer mapping
   only after same-generation later-read evidence. Preserve every native MRT
   write until that evidence exists; this path cannot be a mechanical profile
   edit.

1. Record `git status --short`, local HEAD, remote URLs, file hashes and the
   current compatibility manifest. Preserve dirty files; do not reset them.
2. Fetch upstream in read-only coordination work, then compare only the files
   above:

   ```powershell
   git diff --stat <old-ref>..<new-ref> -- Dx11FsrBridge Il2CppCallSiteHook*
   git diff --word-diff=plain <old-ref>..<new-ref> -- Dx11FsrBridge/Il2CppCallSiteHook.cpp Dx11FsrBridge/Dx11FsrBridge.cpp
   git show --format=fuller --stat <new-ref> -- Dx11FsrBridge Fsr2TranslationLayer.cpp
   ```

3. Classify the diff. Hook-only changes can update a review patch and profile
   manifest. Any Render-token, slot, CB, shader-bytecode, MRT, output consumer,
   or resource-generation change requires high implementation review.
4. Update only the necessary internal adapter/hook. Keep native draw and all
   MRT writes on every unknown, stale, aliasing or contradictory observation.
   A mechanical profile update must not promote `semanticsProven` by itself.
5. Run `git diff --check`, source-TU compile using the real MSVC input, and
   contract positive/negative controls. Regenerate shaders only if the actual
   shader source or ABI changes; use the repository `regenerate-look-shaders.py`
   recipe, never an ad-hoc DXC command.
6. Build exactly one low-authorized candidate after high review, clean stale
   intermediates according to `AGENTS.md`, then record artifact SHA256. Before
   deployment, back up active DLL/helper/INI, verify source/destination hashes,
   and use the single official `OptiScaler\\official\\DLSS F5` directory.
7. A fresh PID/session must show token match, submitted, FSR/NGX dispatch,
   output copyback, final consumer read and native fallback behavior. Use the
   active `[Log] LogFileName`, then bind `session.txt`, PID/start time, active
   DLL/helper/config hashes and matching game logs. Old Bridge logs and fixtures
   are not runtime validation.

Quick rollback is the prior manifest's pinned internal adapter/profile and its
backed-up deployment set. Restore the prior version only after hash checks;
preserve native draw and all MRTs while investigating. Never force-kill a game
or launcher to unlock a file.

## Compatibility manifest proposal

This is a proposal, not an existing automatic synchronizer. R26's native-MRT
work is still in implementation/review and is not an already integrated
upstream mapping:

```json
{
  "schema": "genshin-bridge-compat-v1",
  "upstream": {"repo": "https://github.com/AizawaHikaru233/genshin_fsr_brigde.git", "ref": "UNKNOWN", "commit": "UNKNOWN", "date": "UNKNOWN", "localDirty": true},
  "local": {"repoPath": "D:\\CODE\\genshin_fsr_brigde", "branch": "frame-generation", "commit": "366e778221f5b0521ba9c95838f1e731d9db26db"},
  "hooks": [{"name": "il2cpp.Render", "signature": "pinned-or-UNKNOWN", "rva": "pinned-or-UNKNOWN", "sourceHash": "...", "status": "review"}],
  "profile": {"jitter": "typed/UNKNOWN", "depthInverted": "typed/UNKNOWN", "depthFlipY": "typed/UNKNOWN", "motion": "typed/UNKNOWN", "exposure": "typed/UNKNOWN"},
  "resources": {"colorSlot": "...", "depthSlot": "...", "motionSlot": "...", "outputRtvSlot": "...", "cbLayout": "...", "mrtConsumer": "UNKNOWN"},
  "internalMapping": [{"bridgeFile": "...", "internalFile": "...", "symbol": "...", "status": "migrated|pending-high|excluded"}],
  "tests": {"sourceTU": "...", "contractNegative": "...", "shaderReceipt": "...", "build": "...", "deploy": "...", "runtime": "..."}
}
```

The manifest must carry source hashes and receipts, not only a version string.
An automatic sync tool may later generate the comparison and fail-closed
classification, but no such tool exists in this round.

## Acceptance boundary

Source inspection and a clean build do not establish deployment. Deployment
does not establish that the same game PID loaded the artifact. Runtime success
requires one fresh session proving the full chain: accepted current token,
resource roles and CB/bytecode contract, `token_matches`, `submitted`, FSR/NGX
dispatch, output copyback, subsequent consumer/presentation read and native
fallback on negative controls. Until that evidence exists, report
`runtimeVerified=false` and do not call the path DLSS-enabled.

References: `audit/r26-genshin-bridge-contract-review-low-20261018/REPORT.md`,
`audit/r25-genshin-real-dispatch-high-20261018/REPORT.md`,
`audit/r17-build-preflight-low-20261018/REPORT.md`,
`BUILD-ENVIRONMENT.md`, and the fixed Bridge hashes above.

# R852 animated-character motion repair

2026-10-25. **IMPLEMENTED / INTEGRATED-BUT-NOT-BUILT.** This is the single handoff record for the bounded genshin-only repair. Product TU/full build, embedded regeneration, deployment, game launch and visual acceptance are **NOT_RUN / UNKNOWN**. No UI, shared provider/hook, configuration, original Bridge or global state/audit document was changed. The user's latest R852 observation is that floor jelly is resolved and camera-rotation character trailing remains; this fixture does not prove that all remaining trailing has the same cause.

## Decision and product patch

`../../GenshinFgGameMotion_Hlsl.h` previously unprojected the selected current depth through `currentClipToWorld`, projected that world point through `previousWorldToClip`, and rejected motion if the resulting camera-only expected Z differed from the previous depth at the game-motion donor. That point is the current surface position. For an animated object/bone, the previous position need not equal it. Correct game motion was therefore replaced with `(65504,65504)`.

The patch removes this **Opt-added static-world Z comparison**, including its camera-only intermediate validity checks. It does not increase a depth tolerance, invent camera motion for characters or label game motion invalid merely because a surface moved in Z. The upload keeps the 192-byte ABI and matrix fields; reflection confirms all offsets unchanged, with the matrices now unused in this shader. Existing upstream camera/owner/transaction/queue admission remains untouched.

Retained byte-for-byte computational paths: the five-candidate nearest-depth cross; finite/non-clear `[0,1]` current depth; finite native motion; real per-frame correction difference; display-pixel scaling and 4096 bound; output-history bounds; the **same selected donor** `previousNative=nativeUv+nativeMotion` and its independent bounds; finite/non-clear/in-range previous depth; initial sentinel; R847 row reflection; R852 final vector-Y negation; true current device-Z output. No previous depth is rewritten to make a comparison pass.

`product.patch` is the product-only unified diff. `R852_GenshinFgGameMotion_Hlsl.h` is an exact byte preimage, SHA256 `460a96895d6235608da084a7c380aecd35a6b284452d8e2e626158439b43f658`. The product source retains UTF-8 without BOM and LF. `preimage.json`, `checks.json` and `final/receipt.json` pin inputs, protected files and results.

## Static occlusion versus dynamic geometry

The fixture distinguishes a static plane, lateral-only object motion, motion toward/away from the camera, independently moving body/limb rectangles, and same-ray radial object motion. It uses 3D ray/rectangle intersections and previous model positions, not the rejected camera-only Z rule, to generate depth and motion. Four signed yaw/pitch camera combinations, actual perspective projection, two depth conventions, unequal producer/display grids, distinct current/previous corrections and real RG16 input packing are exercised.

There is **no reliable static/dynamic classifier in these inputs**. Same-ray radial animation changes previous Z while its projected previous XY exactly matches camera-only reprojection. Therefore a proposed "keep the Z test whenever game motion agrees with camera motion" still rejects legitimate animated geometry. The final fixture contains 1,320 such interior witnesses per adapter.

A separate paired counterexample labels identical current depth, valid previous depth and zero XY motion as either an animated surface moving along Z or a newly visible static surface over previous background. R852 rejects both; the candidate forwards both with the original device-Z. A valid prior depth is not a surface-identity proof. **Static disocclusion with valid but different previous Z is consequently no longer pre-rejected by this adapter.** Clear/missing/out-of-bounds history remains sentinel. DLSSG receives unchanged current depth and actual game motion for its own occlusion handling; the fixture does not execute or validate DLSSG's internal handling. Actual static-edge quality and character trailing remain game-test requirements. Do not claim that this adapter now independently solves disocclusion.

## Actual original Bridge comparison

Read `BRIDGE-SOURCE-REFERENCE.md`, then reread actual original source at `D:/CODE/genshin_fsr_brigde/Dx11FsrBridge/Ffx12Backend.cpp` and `Dx11FsrBridge.cpp`, plus current `genshin/GenshinLegacyCapture.cpp` before editing. The original scalar extraction at lines 391-394 and signed-square decode at 427-428 copy depth and decode native motion; the compared preparation path does not apply this camera-only previous-Z refusal. Original motion scaling at `Dx11FsrBridge.cpp:11360` onward retains the recipe's decode/direction/render-scale convention. Current `PrepareShader` copies scalar depth and uses the corresponding signed-square motion decode. This patch does not change that recipe. The five-donor display normalization and previous-history validity checks are Opt's FG adaptation; their exact checks are not relabeled as original-Bridge facts. Original dispatch is SR, not evidence of an original Bridge FG implementation. Original source hashes are pinned and unchanged.

## Final hardware and WARP evidence

Run from the product tree with a fresh phase name:

```powershell
D:\APPS\conda\python.exe genshin/tests/fg-animated-character/run.py --phase rerun
```

`run.py` compiles only a standalone fixture with the actual product HLSL and exact R852 source preimage, `/O2 /fp:fast /W4`. D3DCompile produces fixture-only CS5 DXBC; it never writes `native/F5LowShaderBytecode.h`. Debug-layer hardware and WARP devices execute both shaders, then actual output textures are read back.

| Final result per adapter | RTX 4060 Laptop GPU | Microsoft WARP |
| --- | ---: | ---: |
| Candidate checks / failures | 1,672,684 / 0 | 1,672,684 / 0 |
| D3D debug errors / warnings | 0 / 0 | 0 / 0 |
| Animated interior output witnesses | 6,376 | 6,376 |
| R852 falsely rejected animated witnesses recovered | 5,056 | 5,056 |
| Same-ray animated witnesses | 1,320 | 1,320 |
| R852 negative-control failures (expected) | 36,445 | 36,445 |

Negative-control mode executes the exact R852 source as the candidate against the same animated expectations. It must fail, and recovers zero rejected animated vectors; this proves the new fixture detects the source defect instead of merely agreeing with the replacement. The candidate's output depth is bit-exact against R852 for every tested scene pixel; already-admitted static vectors remain bit-exact. Targeted checks retain invalid donor fallback, both independently necessary history bounds, exact UV=1 exclusion, nonfinite/out-of-range/clear depth, nonfinite/oversized motion, sentinel handling and Control192 reflection.

Initial `candidate-v1/v2` failures were fixture-only exact FP16-output comparisons: actual R16 UAV output can choose the other adjacent representable value rather than the CPU helper's nearest rounding. The product was not changed to accommodate them. The final oracle accepts **only the two adjacent FP16 values bracketing the exact float result** (exactly representable results must match exactly). This is narrower than a symmetric one-ULP-plus-epsilon allowance and still rejects sign, scale, sentinel and any out-of-bracket value. `candidate-v3` passed; `final` additionally includes targeted invalid-donor and independent-bounds checks. No depth-test tolerance was changed.

## Integration boundary and next action

`native/F5LowShaderBytecode.h`, `GenshinFgGameMotion.h`, capture/preparation, original Bridge and prior camera fixture are hash-unchanged. **Embedded shader is intentionally stale against this new source.** The existing source-hash freshness guard remains fail-closed. Root must regenerate the authorized embedded bytecode from this source before its separate complete product build/review/deployment round. Reuse this fixture receipt, then check actual camera-rotation character trailing and static disocclusion in-game. The previous `fg-depth-camera` fixture's unconditional wrong-surface-reject expectation and source/embedded equality are historical R852 assumptions; they are deliberately not claimed as passing against this source-only patch.

No new DLL, model or loader was added. No game/Hub process was controlled. No memory was written. No untracked/concurrent work was reset or discarded.

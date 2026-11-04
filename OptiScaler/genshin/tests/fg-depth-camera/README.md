# FG prepared-depth camera ray repair

2026-10-25. **IMPLEMENTED / INTEGRATED-BUT-NOT-BUILT.** Only `genshin/` was written. No product TU/full build, deployment, game launch, UI/native/config changes or desktop input. Actual moving-camera jelly acceptance remains **UNKNOWN**.

## Source-proven defect and implementation

The admitted original-Bridge profile forbids `depthFlipY`. `GenshinLegacyCapture.cpp`'s `PrepareShader` therefore copies the selected input depth scalar at the same integer texel. `GenshinFgGameMotion_Hlsl.h` chooses a depth/motion donor and computes its normalized texel center. However, it subtracts `currentCorrection` from that center before applying the actual jittered-camera inverse. `GenshinTypedGuides.cpp` defines this correction as native-output/history to accepted-SR reference displacement; it is not a displacement of the already selected depth texel. Consequently the selected scalar Z is combined with another raster ray.

This mismatch is invisible with identity/affine camera tests whose projected Z does not depend on XY. Under perspective camera rotation it changes the expected previous Z and rejects genuine visible predecessor surfaces. The defect is independent of R849's previous-depth donor-versus-output-center error.

`genshin/GenshinFgDepthCamera.h` adds a pure, finite-checked upload adaptation. `genshin/GenshinFgGameMotion.h` calls it immediately after inverting the current raster camera. The existing shader still unprojects `donorUv-currentCorrection`; prepend clip translation `T(2*dx,-2*dy,0)` to its inverse so that this subtraction cancels exactly in homogeneous clip space:

```
[2*(u-dx)-1, 1-2*(v-dy), z, 1] * T
    = [2*u-1, 1-2*v, z, 1]
uploadedCurrentInverse = T * actualRasterInverse
```

The previous camera matrix is unchanged. This is an explicit adaptation to the existing shader ABI, not a change to the observed camera or to the native-to-SR motion formula. There is no vector/sign/Y inversion, depth-tolerance change, gate removal, runtime compilation or shader regeneration.

R847's source/destination row reflection and R849's `previousNative=nativeUv+nativeMotion` same-donor query are byte-for-byte unchanged. The original Bridge `Ffx12Backend.cpp` scalar extraction/signed-square recipe and live capture preparation were reread. No original-Bridge file was edited. `GenshinTypedGuides.cpp` was reviewed and remains unchanged: its native-history-to-SR correction is retained. FG transport current/previous resources still come from the distinct exact transactions' `Prepared()` copies, ordered by their existing fences. Owner/epoch/generation/dimension gates remain unchanged. The dormant `Semantics` sampler is not called by this FG admission path and was not edited.

## Reproduction and results

Run from the coordination root (use a fresh phase name):

```powershell
D:\APPS\conda\python.exe work/r294-r266-build/OptiScaler/genshin/tests/fg-depth-camera/run.py --phase rerun
```

`fixture.cpp` includes the actual product adaptation header, HLSL source and embedded bytecode. It constructs world-plane depth by ray/plane intersection with real right-handed perspective cameras, four signed yaw/pitch combinations, camera translation, both Z conventions, and a distinct predecessor camera. The world plane is frontoparallel only in the PREVIOUS camera, making previous device-Z constant: fractional history-tap depth interpolation cannot conceal or cause this mismatch. Current device-Z varies across the rotated view. Motion comes from projecting each world point into the predecessor, then round-tripping the real RG16 input representation. The fixture compares pre-fix and corrected matrix uploads with the SAME actual embedded shader and checks actual source/embedded agreement.

The mode-3 example uses raw token `.5/render`, accepted SR jitter zero and native-to-SR correction `.5/render`. Thus a nonzero correction does not imply a shifted prepared-depth raster. No actual game jitter or motion convention is inferred from the fixture.

Authoritative `final/receipt.json`, compiled with `/O2 /fp:fast`:

| Evidence | RTX 4060 Laptop | WARP |
| --- | ---: | ---: |
| Checks / failures / D3D debug errors | 205634 / 0 / 0 | 205634 / 0 / 0 |
| Pre-fix false motion rejections recovered | 37030 / 37030 | 37030 / 37030 |
| Already-valid motion values preserved exactly | 148 | 148 |

Checks also preserve every output depth value/row/donor, signed display-pixel displacement, same-donor history bounds, missing-history and wrong-surface refusal, Control192 reflection/offsets, source-FNV freshness, zero-correction identity, nonfinite fail-closed behavior, and the actual product's aliased matrix input/output usage. Native shader bytes and source remain unchanged, so prior R847/R849 shader regression evidence is reused by exact hashes, not described as newly rerun.

Intermediate `perspective-v1/v2` retain failed fixture-oracle logs: an arbitrary 96-pixel display extent introduced a half-output rounding ambiguity in an overly exact scalar comparison. The final perspective fixture uses power-of-two extents, making input-half to display-pixel scaling exact without widening the oracle tolerance or changing product code. `perspective-v3` and `release-fp-fast` are passing predecessors; `final` adds aliasing checks. These intermediate failures are not product-build or game failures.

## Provenance and limits

`GenshinFgGameMotion.before.h` is the exact product preimage (SHA256 `6ade62f4cba505895dffde3548cbf2e1620675e210cb423386c8c98c7a27eda3`). Its original BOM absence/LF are preserved. `inputs.json`, `checks.json` and `final/receipt.json` pin product, fixture, runner, tool-environment script and output evidence. Source preimage/current `git diff --no-index --check` has no whitespace diagnostics.

Untouched pins:

- `GenshinFgGameMotion_Hlsl.h`: `499b386e07d5240456b7612d98fdad703a59a8bbd13ad5fca32681c082ddec15`.
- `GenshinTypedGuides.cpp`: `d8310d2f568f664cdddfa6d9b8ae53d429e99d0eabb9f19e2a7ebb1b89a87119`.
- `native/F5LowShaderBytecode.h`: `11e67abcc4a283afb73a68b5c4a50fbfcf5f015c83003e2102d6e10468193267`.
- Recompiled/embedded NormalizeNative DXBC: `9682ee60d280cca9a37f1043de44e5df754aba184d7566e4c0afbe71f7476494`; source FNV `3344643877562361707`.

No game data or current runtime semantic authority was added. Actual selected-depth/camera correspondence, moving objects, arbitrary nonlinear surfaces, actual motion-writer phase and final DLSSG behavior remain outside this synthetic proof. This repair must not be described as proving the sole cause or final elimination of the user's severe jelly. Root still needs its separate authorized product build and runtime process; the latest instruction explicitly excludes those actions here.

Only obsolete reproducible fixture EXE/OBJ files from four superseded attempts were deleted after checking their absolute paths remain under this `genshin/tests` directory. Released 4,267,538 bytes; unique logs/receipts and the final executable remain. The exact list is in `checks.json`. No active/deployed artifact was touched. This report is the sole local handoff record; global state/audit documents were not edited under the current bounded-file instruction.

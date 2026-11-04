# GIMI static shader tools

The upstream static tool code is pinned to `SpectrumQT/XXMI-Libs-Package` commit `065dc746c21c71ef48bbc93cb6b90b3dcdf766e6`. The additional `texfx-outline` directory records the exact user-provided local template and its own license/provenance; it is not claimed to be from that commit. No external DLL, loader, ReShade core, generated binary, or upstream project file is used.

## PCRE2

The interpreter is PCRE2 10.30 from `pcre2-10.30`, licensed under its included BSD-style `LICENCE`. `include/pcre2.h` is `src/pcre2.h.generic`; `include/config.h` is the pinned `build64/config.h` with JIT deliberately undefined. `src/pcre2_chartables.c` is the pinned `src/pcre2_chartables.c.dist` distribution table. The 20 compiled C units are `auto_possess`, `chartables`, `compile`, `context`, `error`, `find_bracket`, `jit_compile`, `match`, `match_data`, `newline`, `ord2utf`, `pattern_info`, `string_utils`, `study`, `substitute`, `substring`, `tables`, `ucd`, `valid_utf`, and `xclass`. `pcre2_jit_match.c` and `pcre2_jit_misc.c` remain include-only files used by `jit_compile.c`; neither is a separate translation unit. Unicode and JIT are disabled. No bundled tests, grep utility, generator, prebuilt library, or DLL is included.

## DXBC tools

`dxbc/Assembler.cpp`, `dxbc/SignatureParser.cpp`, `dxbc/stdafx.h`, and `dxbc/targetver.h` are from the same pinned commit's `D3D_Shaders` directory. `dxbc/shader.h` comes from that repository's root `shader.h`. `dxbc/COPYING.txt` and `dxbc/LICENSE.GPL.txt` carry the upstream GPLv3 notices from the repository root. `dxbc/log.h` is a local no-op adapter for the vendor logging calls; it does not import the upstream global file logger. `reference/ShaderRegex.cpp` and `.h` retain the pinned `DirectX11` algorithm reference only; they are not compiled.

The translation units and their old global symbols are isolated in `gimi_dxbc_vendor`. The two vendor C++ units and the bridge use a private C++14 setting, no PCH, `MIGOTO_DX=11`, and precise floating point. They use the existing Windows SDK D3D compiler API and the existing `d3dcompiler.lib` dependency.

The bridge accepts only bounded, well-formed PS 5.0 containers with monotonically ordered chunks. It serializes the upstream mutable instruction map, initializes that map from the exact original code for each operation, and clears it on scope exit. The unused LUT writer is disabled. Parsed operand-vector accesses use bounds-checked access, unknown minimum precision is an error, and disassembler blobs use RAII so failed parsing does not leak the compiler blob. The string splitter's end checks and assembler's empty-program path fail safely; parse errors use the vendor's fail-fast exception path. Only the original executable SHDR/SHEX chunk is replaced. Signature, resource reflection, and other metadata chunks remain byte-for-byte from the original container. `SignatureParser.cpp` is compiled as part of the pinned tool closure but its signature-manufacturing entry point is not used by this no-new-output consumer.

PCRE2 translation units are C, use the private 8-bit static configuration, and do not use the product PCH. The adapter keeps the PCRE2 interpreter off draw callbacks and applies explicit pattern, match, recursion, heap, and output bounds.

## Supported boundary

The replacement path is limited to an exact PS 5.0 shader whose original input and output signatures are preserved. It rejects any original `t120` declaration, any new output or other resource declaration, and any nonliteral or out-of-range t120 load. Every original executable instruction is retained except that an original `mov oN.xyzw, rM.xyzw` may become an adjacent direct t120 load into a newly allocated scratch, `mul scratch.xyzw, rM.xyzw, scratch.xyzw`, and `mov oN.xyzw, scratch.xyzw`. The output must be an original reflected float4 `SV_Target` at an existing register 0 through 5. The scratch must lie beyond the original temp count. Both proposed text and final assembled/disassembled DXBC are compared against this exact contract. This supplies an actual parameter-to-output instruction path without adding `o6`, an MRT, or a new texture/sampler ABI. Original control flow, discard, and every other instruction stay exact.

A candidate is published only after these checks, the final executable's permitted `Texture1D<float4> t120` declaration and direct bounded `ld_indexable` load, actual D3D reflection matching the captured original signatures, and authentic device creation of the replacement shader. This is a source implementation with a runtime preflight contract; no compilation, actual shader creation, game execution, visible pixel, or performance result has been observed for this candidate.

M5 adds a separate content-qualified Outline path. The complete local Pattern/Replace/temps/declaration contract must match `GimiOutlineContract.h`; unrelated t69/t70/s15 declarations stay refused. The actual final disassembly must equal the proposed exact-template instruction stream (literal bit values compared, not decimal formatting). Every physical raster in the complete tape preflights actual immutable, device-qualified sampled Texture2D t69/t70 views and retains their static owner; the actual getter-derived s15 object or real null state is rebound/restored and retained by the packet. No texture or sampler is fabricated. Game-produced views, arrays, multisampling, integer/depth/typeless resources and comparison/reduction samplers are outside this first material path. Null materials are legal for the real parameter consumer, while they do not certify sampled material functionality.

M5b supersedes the earlier source refusal: fixed XXMI ShaderRegex preserves `${ini69}_abs.x`, and its actual assembler uses the numeric register prefix without setting ABS. Preparation sends the original spelling unchanged to that assembler, then compares the final executable against this exact no-ABS source contract for this one pinned template operand; no suffix stripping or inferred absolute-value correction is performed. Author confirmation is not a gate. The known Vodyanitsa INIs provide no t69/t70 bindings, but those materials are optional in this template: unbound resinfo dimensions are zero, sample branches are skipped, and actual t120-driven threshold/discard/fallback instructions remain. All-null resources are legal without being reported as sampled material. Nonnull resources still require exact view/device/owner qualification; sampler filter checks follow their possible sample path. No runtime or pixel result has been observed. See third_party/texfx-outline/SOURCE.md and audit m5b/RESULT.md; the older m5 report remains a frozen historical delivery. Full Diffuse/MRT/Present transparency work is separate.

## R1045 SoftwareMouse

`mouse/MouseSource.h` embeds the exact installed-package source retained at
`audit/r1043-f6-gimi-software-mouse-20261104/inputs/Core/GIMI/Libraries/Shaders/Mouse.hlsl`,
originally read from `D:/APPS/XXMI/GIMI/Core/GIMI/Libraries/Shaders/Mouse.hlsl`.
This input is NOT claimed to be part of the separately pinned XXMI-Libs-Package
commit above. Load normalizes CRLF to LF, compares the full text without hashing,
then compiles that exact text twice with VERTEX_SHADER/PIXEL_SHADER, main,
vs_5_0/ps_5_0. It reflects actual t100/t101 and VS t120 float texture contracts,
stage versions, interface/CB absence and the VS/PS signature link before any
library publication. No generated binary, new TU, external loader or DLL.

The input/hide/bypass semantic reference is the separately pinned primary
`SpectrumQT/XXMI-Libs-Package` commit `065dc746c21c71ef48bbc93cb6b90b3dcdf766e6`,
`DirectX11/cursor.cpp`, `cursor.h`, `CommandList.cpp` and `IniHandler.cpp`.
R1043 SOURCES.md records the original cursor source retrieval; R1045 SOURCES.md
records the precise F6 adaptation and actual candidate callsites.

# GIMI configuration and command core — R478

This document describes the command core and its resource/executor integration. `GimiRuntime` owns the existing opt-in hook path; source integration and a successful build do not establish MOD rendering. No provider/proxy/second-loader dependency is introduced.

The bounded IniParams path creates real immutable `Texture1D<float4>` pages at load/Present. `GimiIniParams.h` defines the numeric page state; `GimiResources` proves its finite closure and creates all GPU pages; `GimiDx11Executor` validates and binds pages in tape order; `GimiRuntime` commits the final state only after a successful transaction and retains pages through the existing real fence/quarantine owner. This is source implementation, not game or visual acceptance.

Current original VS/PS must be creation-reflected **unused at t120**. An original shader already consuming that slot remains Unsupported. R1006 adds a separate authentic PS replacement capability for the exact output-modulation contract below; it never rewrites that original unused-slot fact. Binding an unused slot alone is parameter transport. R990 model-only links and ordered entry scheduling remain; duplicate ShaderOverride merging and complete package dependencies are not silently dropped to load Vodyanitsa.

## Explicit original host import (R1055)

`Selection.hostRoot` / `[GIMI] HostRoot` is an explicit absolute original root
INI, not a dependency library and not the persisted-state-only `importRoot`.
`Ini` remains one selected MOD INI or one selected MOD directory. For the user
package these are `D:/APPS/XXMI/GIMI/d3dx.ini` and
`D:/APPS/XXMI/GIMI/Mods/Summer Bikini Vodyanitsa no toggle`, respectively.
No installed path or costume variable is inferred from a role/character name.

Load preserves every original section/entry/source. The root is parsed first
in its authentic empty namespace. Its real Constants (including costume_mods)
then participate in the existing declaration/static-initializer order before
the original included core/MOD Constants. Nothing injects an enabled value.
Fixed core keys bind to the actual imported root declaration (finite 0/1,
nonpersistent), not a filename-only substitute or a manufactured default.
The root's original `Include` graph is consumed in the existing sorted waves.
Its explicit core file has its own parent-directory IO authority. Each
recursive host container is narrowed to the explicit MOD selection; siblings
are not enumerated. Selecting the entire recursive Mods container as a
directory is rejected. Original root glob exclusions apply to selected
ancestors, descendants and INI filenames; explicit includes retain upstream
semantics, including explicitly named DISABLED core files. No user resource
is granted the union of root/core/MOD directories. Conflicting package roots,
overlapping host ingresses, cycles, missing inputs and original limits still
reject. Original default namespaces are relative to HostRoot's parent.
An explicit `NamespaceRoot` must equal that directory in host mode; standalone
mode retains the separate naming-only contract without expanding authority.

Host sections are not deleted or made optional. `HostImport.settings` records
each accepted setting with its source, original value and one explicit role:

* Native mappings: authentic Genshin/DX11 admission, existing Present-only
  initialization, HWND-bound focus/input, save interval, actual-client Mouse
  policy, swapchain resolution, existing shader/texture identity and t120 ABI,
  creation-time resize, disabled stereo/global raster modifications. Other
  runtime values need the corresponding native owner and reject precisely.
* Original-tool provenance: launcher executable/privilege requests, original
  proxy/device-creation preferences, hunting/debug-host controls and disk-tool
  paths. The F6 import contract does **not** run those original host tools or
  install their shortcuts. Existing F6 menu controls own enabling/reloading and
  diagnostics. This is explicit in the HostRoot hover text and the successful
  Load status tooltip; it is not identical d3dx host emulation.
* Inactive: explicitly disabled original cache/export capabilities and
  unselected recursive containers. They never become automatic discovery or
  write authority. Empty Profile is admitted; any actual unknown driver-profile
  setting is rejected at its file/line.

Known original screen fallback dimensions are provenance only: no source
configuration dimension replaces a real HWND, cursor snapshot, DPI, RTV or
swapchain sample. The original INI's own PassWindowInfo variable fallback
remains intact. Native shader policy accepts only the existing bounded exact
transformations; an accepted assembler preference never expands shader grammar.
Unsupported required CustomShader/ShaderRegex/Key/command/resource declarations
still fail their original compiler/executor admission. Adaptation is only for
the exact imported root's individually admitted host settings, never a blanket
skip of all settings or dependencies.

`Input.input` initializes the native key-disable state; it never enables the
selected MOD. The root Ctrl+Alt+Shift+End toggle consumes the existing same-owner
KeyboardFrame rising edge, subject to the existing focus/menu suppression.
`input_disable_mode=mods` preserves the fixed core-library cycles, since these
are outside the recursive MOD path. Required MOD-local Key grammars remain
unsupported, rather than falsely enabled. `all` suppresses the core cycles;
the host toggle itself remains available. There is no second input/cursor owner.
`settings_auto_save_interval` maps to native safe-Present saves (0..86400 seconds,
or -1 to suppress timed saves); initial seeding and forced semantic-owner save
retain existing behavior. Original XXMI clear-unknown maintenance is not applied
to F6's independent append-only state.

Only after immutable resource and CommandState initialization does state import
apply. The already parsed original `user_config` path (or authentic default
d3dx_user.ini) supplies first-time read-only values; it is not a compiler input
which can override declarations or introduce commands. An existing F6 sidecar
is authoritative. `ImportRoot`, if explicitly set in host mode, must name the
same original HostRoot; root authority is not reopened/reselected later.
Original package/user files are never written. Runtime variables are initialized
from real declarations before those explicit persistent-value overlays.

The menu accepts `MaxComponentSelections` (64), including the package's 33
actionable entries. It preserves HostRoot/NamespaceRoot/ImportRoot across apply,
enable/disable and existing configuration save/reload, while remaining off by
default. The 8192-byte qualified list, exact target/name matching, 32 ingresses
and 32 requirements per physical draw remain separate bounded contracts.
Slot identity, full Load, creation coverage, skeleton/LOD/shadow/FSR coexistence,
pixels and net performance require real execution. The source implementation
does not claim any of those outcomes.

## Ownership and module boundaries

| File | Responsibility | Runs when |
| --- | --- | --- |
| `GimiConfig.h/.cpp` | Ordered UTF-8 INI, include dependency graph, namespace and source locations | Manual Load |
| `GimiPlan.h/.cpp` | Resolve exact entry/dependency closures; compile expressions, branches, resources and overrides into immutable IR | Manual Load |
| `GimiExpression.h` | One float32 arithmetic definition for constant and runtime evaluation | Load and bounded IR evaluation |
| `GimiIniParams.h` | Bit-exact finite page layout, initial values and allocation-free scalar transitions | Load proof and tape preflight |
| `GimiCommands.h/.cpp` | Preallocated mutable semantic state; resolve commands into a transactional action tape | Eligible draw or other explicitly selected invocation |
| `INTEGRATION.md` | Existing FSR reuse, missing resource/DX11 hooks and draw-cache contract | Next-stage integration |

The core uses floating-point `from_chars`; paths support C++17 and C++20 UTF-8 string types. The current patch adds no translation unit or project definition. Existing source file objects, BOMs and line endings are preserved. Build/deployment belongs to the integration owner.

`LoadConfig` publishes `shared_ptr<const Config>` only on success. `CompilePlan` publishes `shared_ptr<const Plan>` only on success; its constructor and mutable state are private. `CommandState` is the separate single-owner mutable state. IDs are local to one Plan generation. Never reuse a state, lease, candidate, action pointer or program ID after swapping plans/devices.

## Supported language

INI command lines retain their original order, duplicate keys, raw spelling, entry-specific namespace, and absolute file/line. ASCII name case folds; file paths retain case. UTF-8 with an optional BOM is accepted. UTF-16/ANSI is explicitly unsupported. Whole-line `;` comments are supported; inline comments are not interpreted as comments.

An entirely empty `CustomShader` is retained as `EmptyCustomShader`, including
its source/dependency and normal program/run budgets. Its PRE and POST contain
no commands, state overrides or GPU actions, so it can be called from both the
ordinary transactional interpreter and CPU-only/native Present. This handles
the original legacy Help `CustomShaderFormatText` dummy without manufacturing a
compute program. Any nonempty entry still requires the complete supported shader
contract. R1045 implements both original SoftwareMouse declarations in native
PRE, including the existing input owner's actual game-cursor snapshot, eight
parameters, original VS/PS, nested blend/Draw(4,0), complete state restoration
and the same final-reader owner. The coordinate adaptation and resource
boundaries are detailed below. No required declaration is dropped; full package
Load/pixels are UNKNOWN.

R1043 adds stage-qualified vs-t0..127 reference transport alongside existing
ps-t transport (including the original Mouse reference spelling as a synonym
of ref). The target enum appends VS SRVs without changing old IDs. Their
transactional pipeline cache, real getter checks, alias snapshots/previous-new
owner journal, explicit setter and exact VS/PS restoration are separate from IA
vertex buffers. Resource use propagates through alias chains into the same
immutable preload; explicit VS/PS t120 commands/filters cannot bypass IniParams
ownership. At a raster, the original reflected VS must admit a command-bound
float Texture2D, backed by a real immutable DDS or already-initialized private
texture; a different game view is not promoted into a new VS reader. Undeclared
slots are transport only. This uses the existing fully preflighted/reserved
raster packet, retained COM set and post-final-reader Signal/fence/quarantine.
R1045 separately connects the complete scoped native Mouse contract below;
arbitrary native VS reference tapes remain unsupported.

The first Load file is the root namespace unless a `namespace = ...` preamble renames it. Other explicitly supplied or included files default to their package-relative filename namespace. File-local `namespace` overrides do not change relative include/resource paths. Namespaced categories use the upstream category insertion rule: `[CommandListFix]` under `global\ORFix` becomes `commandlist\global\orfix\fix`. References try the caller's namespace, then the unqualified/explicit name. Variables use `$\namespace\name` with root-global fallback. Exact canonical entry names are exposed in `Config::sections` for selection.

`[Include...] include` and `include_recursive` resolve relative to the declaring INI. Include sections are consumed in case-insensitive canonical section order in waves, preserving duplicate directives within each section. Recursive loading reads sorted files before sorted directories, as upstream does. Repeated canonical physical paths load once; a real include dependency cycle fails with the originating line. Root `[Include] exclude_recursive` supports filename `*` and `?`, including `DISABLED*`/`desktop.ini`; no implicit disabled-name rule is invented. An explicit include may therefore load `DISABLED_Includes.ini`. Bracket/path globs, conditional preambles, user_config and included-file exclusion changes are Unsupported. Dependencies escaping the selected root are Unsupported. Missing files and limits fail the Load; no partial Config is published.

Global Constants/Present sections append across included namespaces, retaining each entry's namespace. Duplicate namespaced sections and duplicate resource/identity metadata fail rather than changing upstream first-section semantics silently. Other ambiguous duplicate section cases are Unsupported.

| Construct | Compiled behavior |
| --- | --- |
| `global $name`, `global persist $name` | Float32 initial value; optional initializer; persistent flag exposed for out-of-draw save/restore |
| `local $name` | Static lexical slot, shared across its section's pre/post; assignment executes at its position; sibling scopes may reuse a name, visible parent locals cannot be redeclared |
| `$name = expression` | Resolved variable ID; no runtime name lookup |
| Numeric/variable/parameter expressions | Float32, decimal/scientific literals (leading zero remains decimal), `true/false`, `+ - !`, `**`, `* / // %`, `+ -`, comparisons, `&& ||`, parentheses |
| `vs`, `ps`, `vbN`, `ib`, `ps-tN`, Resource/`this` expressions | Cached identity/filter query through a typed callback; known-null is negative zero, unknown is an execution error |
| Draw expressions | `FIRST_INDEX`, `INDEX_COUNT`, `VERTEX_COUNT`, `INSTANCE_COUNT`, `FIRST_VERTEX`, `FIRST_INSTANCE` |
| `if/elif/else if/else/endif` | Forward branch IR for both phases, original statement order and lexical scope |
| `run = CommandList...` | Implicit: PRE invokes callee PRE and POST invokes callee POST |
| `pre/post run = CommandList...` | At that explicit point, callee PRE then POST together |
| `checktextureoverride = target` | Same phase behavior as run; all matched PRE lists then all matched POST lists for an explicit check; nested `this` restored on return |
| Resource/binding assignment | `ref`, `unless_null`, explicit `null`; slots `vb0..31`, `ib`, `ps-t0..127`, custom Resources and `this` |
| Implicit resource copy defaults | Pipeline assignment from Resource or same target kind may reference as upstream; custom-resource destination defaults to COPY upstream, so it requires explicit `ref` here. GPU COPY is Unsupported |
| `handling = skip` | PRE decision only, held until a complete transaction is ready; never calls/blocks the original hook itself |
| `drawindexed = count, firstIndex, baseVertex` | Three compiled expressions, bounded action emission; runtime rejects nonfinite/fractional/out-of-range arguments |
| `x/y/z/w` parameter assignments, optional numbered suffix | Typed ordered parameter actions and simulated reads; index <4096. finite immutable pages provide GPU transport, subject to the reserved-slot/consumer contract below |
| TextureOverride identity | Exact 8-digit hash, optional exact `match_first_index`/`match_index_count`, signed `match_priority`, float filter_index |
| ShaderOverride identity | Exact 16-digit hash, original filter_index and `allow_duplicate_hash=overrule` metadata; duplicate-hash merging execution remains unsupported |
| ShaderRegex | Coherent namespaced group and ordered commands; exact VS/PS models for model-only links. Pattern/Replace/temps/InsertDeclarations additionally support only the exact PS5.0 consumer below. Unsupported models, members and declarations reject with their source line |
| Resource declaration | Empty alias or file-backed Buffer/Texture2D, filename, format and positive stride; existence/root checks at Load. Format/decode/bind capability is validated by the resource preload stage |

Arithmetic is eager, including `&&` and `||`, matching the selected XXMI evaluator. Unary binds above right-associative exponent. No expression parser or name resolution runs in a draw. Global initializers are restricted to constant arithmetic/earlier initialized globals in original Constants order; forward initializers and dynamic Constants commands are Unsupported. Constants `run` additionally admits a bounded startup-only CommandList closure containing explicit `ResourceAlias = ref Resource` / `null`, optional `unless_null`, and nested unqualified `run`. Each alias assignment snapshots the source's current canonical immutable resource (or known null) in merged Constants order; later writes to that source alias do not retarget earlier copies. Calls are subject to the existing command/instruction/depth budgets and cycle diagnostics. Phase-prefixed commands, branches, variable/parameter writes inside these calls, pipeline references, writable GPU outputs and GPU copy/create commands are refused. The whole required graph is still validated. After full immutable creation, the startup alias shares the actual source COM lease/identity in both CommandState and Session ownership; no draw-time initializer or extra GPU object is introduced. Startup origins participate in preload binding-use propagation, and initializer metadata counts against the existing total-byte budget. Static `x/y/z/w` assignments in Constants accept constant arithmetic and earlier initialized non-persistent globals, in original merged-entry order. Key/Present scheduling and persistence IO belong to the external runtime, not a hidden thread here.

Unknown reachable sections, commands, targets, fields and dependencies return a diagnostic with a file/line and publish no Plan. Syntax errors, missing dependencies, cycles and configured limits have distinct codes. Unreferenced library commandlists are not executed or advertised as supported. Every explicitly loaded file/include contributes its TextureOverride/ShaderOverride/ShaderRegex/Present declarations to the required closure. This is declaration validation, never permission to execute all triggers. `triggerFiles` additionally requires explicit mandatory files to have been loaded. This prevents compiling an ORFix run while silently dropping the shader/resource captures in the same library file.

## ORFix 0.9.19 dependency scope

The implementation is based on reading the **local fixed snapshots**, not on executing or porting their loader:

- `references/gimi-package/SilentNightSound-GIMI-Package-83e75c0/GIMI/Core/GIMI/Libraries/ORFix.ini`: namespace, ORFix/NNFix, shader/resource filters, nested branches, Resource captures, null/unless_null, `x70` and post cleanup.
- The same package's `Libraries/ORFixAPI.ini`: `GIMI` Resource aliases used by the real ORFix Clear/GIMIReference commands. Supply this real file, not fabricated placeholders for missing Resource sections.
- The package root `d3dx.ini`: actual root `$costume_mods` declaration. No implicit `$costume_mods=1` is injected by this module. A selected dependency slice must supply its real declaration.
- `references/gimi-package/xxmi-core-065dc74/DirectX11/IniHandler.cpp`: namespace helpers, recursive include ordering, Constants/metadata, pre/post routing.
- `DirectX11/CommandList.cpp`: `ParseRunExplicitCommandList`, `ParseCheckTextureOverride`, `AddCommandToList`, local-variable scope, arithmetic precedence/eagerness, resource-copy defaults and null/unless_null.

ORFix's character names, hashes, shader filter numbers and remap branches are data, not C++ cases. Updating that INI can change those tables without editing this interpreter. **Unmodified ORFix rendering is NOT established**. R990 prepares an immutable link at Present for each qualified original shader identity/generation: its exact ShaderOverride first, followed by matching model-only groups in canonical group-name order. Original explicit filter_index wins; an otherwise unclassified linked shader uses 1, known bound/unmatched uses +0, and null PS uses -0.

The single transactional tape runs VS PRE, PS PRE, original draw (once or skipped), VS POST, PS POST. An explicit draw inside VS PRE stays before PS PRE. TextureOverride lists run only when an actual source-ordered check reaches the current binding, preserving priority/name siblings and the interpreter's explicit/implicit run/check phases. The selected component is only an eligibility constraint and must actually be reached by the completed tape. Missing selection, unknown matching, or tape/preflight failure rolls back before submission. No draw-time group scan, regex, bytecode reflection, IO, Map, upload or growing allocation is added.

M8 permits an explicit `Selection.component` list of 1..32 distinct canonical
TextureOverride names separated by `|`, within the existing 255-byte component
field. All parts use the same exact `Selection.trigger`; multiple shader roots
and mixed shader/resource selection reject. Parsing, compilation, immutable
preload and sorting occur at Load only. The immutable selected metadata reuses
the Plan's hash/priority/name order. A draw considers only its captured original
binding identity and exact index constraints; no truncated matches or fabricated
per-part callback is allowed. Skip/filter/resize-only entries cannot serve as
multi-part eligibility roots, but remain ordinary ordered Check siblings.

Every selected part matching that physical draw is passed to `PrepareDraw` as a
bounded `OverrideRequirement`. Each must be reached by a real PRE Check of the
same target and full original numeric binding lease (object/view/generation,
stride/offset/format/size). A direct Run, POST-only reach or another same-hash
resource cannot satisfy it. Any missing part rolls back the complete tape before
GPU effects. Existing Check sibling order, PRE/POST sequencing, resource hazards,
shader/material ABI qualification and fence/quarantine ownership remain in force.
This is an execution/admission slice for explicitly selected parts, not full
package/animation/LOD/skin/shadow compatibility. The original Vodyanitsa and
Mermaid graphs still require all of their genuine dependencies; never drop an
unsupported required include to make a partial selection look loadable.

Bound HS/DS and unknown GS remain unsupported. The existing qualified GS/SO passthrough additionally requires no GS override entry; parameter transport keeps its existing no-GS restriction. These boundaries do not prove full character/animation/LOD/shadow/FSR compatibility.

`CommandListClear` saves x70, writes `x70 = ps`, and restores it in POST. The finite page analysis covers literal values, parameter/variable dependencies and the compiled filter-index set, including +0/-0 and the filter fallback. It overapproximates all branches and uses the same float32 operators as runtime; it never caches a branch or action tape. Unknown draw-varying inputs, persistence, a complete image union above 512 pages or proof-budget exhaustion reject the plan. All required written scalar IDs (including singleton domains) are retained within the unchanged 4096-float4 ABI. Raster pages cover the complete finite product of raster writes/restores; native overlay pages project that product onto the real reader contract z=0/w=1/x70=1 or 2, without multiplying native w across unrelated raster x70 values. Shader/resource filter domains follow the actual query kind; an unresolved this conservatively uses both. Images use full bit-exact keys and binary lookup; native lookup also checks every unwritten default, so un-restored dynamic text parameters cannot enter overlay pages. All 4096 float4 entries are initialized to the fresh private +0 defaults plus ordered static Constants writes; external loader state is not imported. The maximum immutable GPU payload remains 32 MiB; the complete key table also counts against the existing preload byte budget. The original proof budget covers image enumeration/comparison/transposition as well as expression closure. No draw-time page creation, upload or cached action tape is introduced.

The executor reads from the exact committed page, validates every parameter action and reserves the complete page owner before issuing GPU work. PRE and POST bind existing VS/PS t120 SRVs with no Map, upload, readback, allocation or page mutation. It restores both actual previous bindings on every execution exit. Cancelled/preflight failures keep the committed state; partial GPU work quarantines real owners and cannot replay the original draw. The original-shader unused-slot contract deliberately does not certify the TexFx ShaderRegex consumer, t69/t70/s15 resources, its IniParams[69] flags, or any character rendering.

The complete `main.ini` brings model-only ShaderRegex plus GUI CustomShaders, API resources, Key bindings and full TexFx beyond this slice. Loading a whole package does not authorize omitting these dependencies. Selecting it as a required trigger root fails explicitly at unsupported sections. R478 is one module stage of the full compatibility route in R477; it is not a reduced definition of GIMI.

## Consuming the product interfaces

1. At manual Load, call `LoadConfig` with a bounded root and the actual INI dependency files; resolve the exact entry selection from Config. Call `CompilePlan`. On either failure display its diagnostic and leave the entire selected component disabled.
2. At resource/device preparation, inspect Plan resources, override metadata, expression targets and touch masks. Decode/validate formats and construct immutable GPU objects/lease table. Create a `CommandState` and initialize every required file resource; apply persisted variables only to the flagged slots. Reject unsupported GPU/identity/parameter capabilities before publication. No GPU readiness flag is produced here.
3. For an eligible immediate-context DrawIndexed, the identity/cache layer returns an exact ordered `ProgramInvocation` sequence: required shader captures and component roots, with valid `thisTarget`s. Root identity eligibility is the caller's responsibility, not a draw-count assumption. Provide bounded cached `CommandQueries` for the actual draw and lease generation.
4. Prefer `PrepareDraw`: it simulates all root PRE lists, inserts `OriginalDrawBoundary(skipOriginal)`, then all POST lists into **one** preallocated tape. Unknown/missing query, dynamic recursion, argument or budget failure rolls back semantic changes, returns no batch, and leaves the original draw eligible for unchanged forwarding. It requires stable known identities across the draw; GPU-written/unknown identity routes must be rejected upstream.
5. Validate the whole returned tape and pin every needed lease before its first effect. Save the exact touched pipeline/hazard state. Execute actions in order using existing internal-draw suppression. `SetResource` includes previous/new bindings so retention does not require scanning all aliases. At the explicit boundary call the original draw exactly once unless skip is true. Preserve pre/post positions and restore pipeline state on every exit. `SetParameter` must be genuinely supported, not ignored. Explicit MOD draw actions never recurse through public hooks.
6. After complete successful submission call `Accept`; after rejection before effects call `Discard`. Batch pointers are borrowed only until Accept/Discard/next preparation. If GPU effects partially happened, restore/quarantine/disable through the real owner; never forward the original as if nothing occurred. CPU acceptance is not GPU completion.

The phase-by-phase `Prepare/PrepareSequence` APIs retain pre/post invocation identity and context checks for external scheduling. Accept PRE, process the actual boundary, prepare/accept the same POST sequence, then `FinishInvocation`. They do not preflight future POST queries; use `PrepareDraw` for first-slice fail-closed replacement. Failed POST requires recovery/retirement, not forced Finish or replay. Do not use a phase API and a combined API concurrently on one state.

Match queries return compiled **program IDs**, not override-array indices, in Plan override order. Overflow/unknown returns false, never a shortened successful list. The VM checks IDs, resource-vs-shader kind, ordering and exact index metadata. The provider still proves hash algorithm, descriptor/content generation, collision eligibility and actual resource ownership. `filterIndex` must return upstream priority behavior: known null -0, known bound without override +0, highest-priority matching filter_index, or upstream bound-match fallback; unknown is false.

## Hot-path cost and limits

Only `CommandState` construction allocates runtime storage. Actions, match scratch per depth, expression stack, journal, variable/resource epochs, parameter cache and binding cache are sized there. Execution uses numeric IDs, bounded forward IR, O(1) alias/variable access and lazy O(1) per-batch binding/parameter caching. A journal records each changed semantic slot once, so rollback/commit does not scan every variable/resource. No per-draw file IO/hash/readback/upload/log/string/path copying or vector growth is performed by the core. Query providers and action executors must preserve these rules too.

Default limits: 32768 instruction/token steps, 1024 actions (including alias/parameter/boundary effects), 32 call depth, 128 matches/check and 128 expression stack. Exhaustion rejects the entire prepared tape; no truncation. Unchanged configuration/plan publication is external. No performance number or cache hit rate is claimed from this source implementation. Larger GIMI libraries can require different limits, but increasing them is not proof of acceptable draw cost.

## Upstream updates

Keep the package INIs and interpreter code separate. To update: replace the read-only upstream reference snapshot, read its real include closure, and compare the semantic entry points above. Hash/filter/character data changes should be data-only; grammar changes belong in Config/Plan, arithmetic in the shared Expression helper, and execution in Commands. Resource formats/copy/identity and D3D context ownership remain their own next-stage modules. Unknown syntax fails visibly rather than enabling by version number.

The bounded interpreter is independently written from observed semantics. The R1006 private shader tools import the pinned PCRE2 and DXBC static source closure documented in third_party/SOURCES.md with its original licenses. No HackerContext/HackerDevice, foreign proxy, loader, executable or DLL is imported. These notices must accompany any distribution of the corresponding source or binaries.

Not supported yet: general ShaderRegex bytecode changes beyond the exact R1006 output modulation contract, CustomShader, GPU copies or pools, dispatch/auto/instanced/indirect commands, deferred context replay, arbitrary D3D stages/UAV/RTV/CB operations, ShaderOverride duplicate merging execution, keyboard/preset services, the full built-in expression/function set, complete main.ini/TexFx/GUI or real-game rendering acceptance. The exact consumer below is a source implementation; it does not establish full package/material support.

## R1006: retained original source and signature preparation (F6 candidate)

`GimiShaderSource.h/.cpp` is an internal preparation contract. `ShaderCreated`
uses its existing real reflection to copy supported original VS/PS bytes once,
with the exact original lease/generation/model and owned signature names. Each
byte array is at most 1 MiB; each Capture has a separate 32 MiB retained payload
budget, including bounded input/output signature arrays (64 parameters each).
Dynamic linkage, unknown models, missing reflection, oversized bytes or budget
exhaustion have no source capability. A source-budget miss does not revoke a
model-only R990 entry or consume the existing resource CPU budget.

`runtime::AcquireShaderSource(device, originalShader, originalIdentity)` is only
for Present/toolchain preparation: it takes the owner lock, requires the actual
current device and exact lease/generation/hash, and returns a shared immutable
snapshot. It is not a draw query. Pipeline owners already retain their original
Shader owner; the source follows that lifetime without altering GPU retirement.
Late attachment cannot recover creation bytes from the shader COM object.

`PreparePixelTarget6Signature` constructs the expected signature of the known
TexFx PS5.0 float4 output-6 transformation. It preserves original signature
order, names, indices, registers, stream, component type, masks and precision;
existing output register 6 or SV_Target6 is refused. Its result is a requested
signature, not an assembled DXBC. `MatchesPixelTarget6Signature` compares the
final candidate's authentic reflection with that contract before future shader
creation/publication. Neither function establishes actual t120 instructions,
MRT attachments, a GPU submission or pixel correctness. That target6 request
helper is not used by the supported consumer; new outputs remain refused.

## R1006 M2-M4: exact output consumer (isolated source, not built)

The fixed PCRE2 interpreter and DXBC source closure is imported privately; see
`third_party/SOURCES.md` for the pinned commit, files, adaptations and licenses.
`Load` compiles immutable bounded PCRE2 rules once. Existing Present preparation
uses each original PS source snapshot, matches coherent groups in canonical
order, applies one extended substitute per matched Pattern, allocates named
temps, inserts permitted t120 declarations, and assembles actual DXBC. No JIT,
disk shader cache, second core or loader is added.

The supported instruction delta is exact: an original full float4
`mov oN.xyzw, rM.xyzw` may be replaced by a direct bounded t120 float4 load into
a fresh scratch, `mul scratch.xyzw, rM.xyzw, scratch.xyzw`, and the same output
MOV from that scratch. N must be an original reflected float4 SV_Target at
register 0 through 5. All other instructions, original declaration interfaces,
control flow and discard stay exact. Both proposed text and the final assembled
code are checked. A dead load alone, dynamic load, new output/resource/sampler,
arbitrary patch, or original t120 use is rejected. The immutable page contains
all 4096 float4 values; direct indices/component masks come from actual final
code. Its output signatures must equal the retained original descriptors.

The authentic device creates the replacement only after those checks. The
immutable capability owns device, original PS, replacement PS and source, plus
original identity, exact plan/device generation and a unique replacement lease.
The executor preflights this capability, actual original bindings, no classes,
original unused-t120/no-GS fact, and actual immutable parameter page owner.
After reserving the packet and entering existing internal suppression, it binds
the replacement before the first real raster/Before callback. The observer sees
the actual PS. It restores the original PS and exact touched resource slots on
all execution exits. Partial submission remains consumed/quarantined. The
packet owns the capability and pages until existing real fence retirement;
UINT64_MAX never means completion.

Pattern-only matches can link commands without producing a replacement. A pure
shader patch does not invent a command root/classification. The existing selected
component must still be reached by its real automatic shader/check tape. This
slice does not change selection, defaults, INI state or UI. No installed TexFx
Diffuse/Outline group is advertised as supported: their resource/MRT contracts
remain missing. Actual compilation, shader creation, game execution, intended
pixels, package compatibility and performance are UNKNOWN.


## M5a exact Outline material continuation (source only)

This candidate contains a separate exact-content Outline template contract and actual t69/t70/s15 material preflight/bind/restore/packet-retention path. It does not authorize arbitrary shader resources or change the original output signature. Only actual immutable user DDS Texture2D leases with a typed float sample view and device SHADER_SAMPLE support qualify; each real raster sees the command tape state in source order. The actual current s15 object (including observed null) is retained and used unchanged, never synthesized. Every required lease is reserved through the existing real GPU retirement/quarantine path before setters; no new queue, fence, Present hook, core or loader is introduced.

M5b supersedes the earlier source refusal: fixed XXMI ShaderRegex preserves `${ini69}_abs.x`, and its actual assembler uses the numeric register prefix without setting ABS. Preparation sends the original spelling unchanged to that assembler, then compares the final executable against this exact no-ABS source contract for this one pinned template operand; no suffix stripping or inferred absolute-value correction is performed. Author confirmation is not a gate. The known Vodyanitsa INIs provide no t69/t70 bindings, but those materials are optional in this template: unbound resinfo dimensions are zero, sample branches are skipped, and actual t120-driven threshold/discard/fallback instructions remain. All-null resources are legal without being reported as sampled material. Nonnull resources still require exact view/device/owner qualification; sampler filter checks follow their possible sample path. No runtime or pixel result has been observed. See third_party/texfx-outline/SOURCE.md and audit m5b/RESULT.md; the older m5 report remains a frozen historical delivery. Full Diffuse/MRT/Present transparency work is separate.

M5b also qualifies the existing ordinary command-bound DDS consumer against the actual original PS creation reflection. Complete tape simulation checks each real Draw/DrawIndexed, including explicit VS PRE rasters; immutable typed float Texture2D SRVs must match a declared original PS Texture2D float slot. No Diffuse-to-slot mapping is inferred: the real SetTextures/ORFix command tape supplies it. The packet retains original getter-derived samplers, the static DDS resource/view and alias owners through existing GPU retirement; only touched SRVs are set/restored. Unchanged game resources remain under the existing serial owner contract. The first ordinary color-DDS slice conservatively refuses any original comparison sampler or reduction filter because texture-to-sampler pairing is not reflected; it does not synthesize a sampler. An undeclared bound slot is transport only and provides no consumer claim. No replacement shader, new output, core, Present hook or user-config edit is needed for this ordinary path. HeadA/whole-package/runtime success remains unknown.


## R1006 M9: explicit multi-ingress character scheduling (private source)

The runtime accepts up to 32 distinct exact canonical resource override names,
each with an explicit ingress: `ib`, `vb0..7`, or `ps-t0..127`. Legacy
`Selection.component = "name|name"` still uses one `Selection.trigger` within
255 bytes. A qualified list instead uses `target@name|target@name`, up to 8192
bytes; qualified and unqualified tokens cannot mix. For example, the declared
Head inputs can be selected explicitly as:

```
vb0@TextureOverrideVodyanitsaHeadPosition|vb1@TextureOverrideVodyanitsaHeadBlend|vb1@TextureOverrideVodyanitsaHeadTexcoord|ib@TextureOverrideVodyanitsaHeadA
```

This is a declaration of exact requested bindings, not discovery or evidence
that a live game draw has those bindings. `Selection.entries` offers the same
bounded structure as `{component, trigger}` pairs, with `component` empty when
used. One program cannot be declared twice or at multiple targets. Shader and
resource entries cannot mix; one explicitly selected ShaderOverride preserves
its original vs/ps route. The UI editor still has its existing 255-byte widget;
long qualified values and structured entries have no new UI/persistence service
in this slice. Default enablement remains false. No user INI or MOD is changed.

Load resolves stable Plan pointers and deduplicated numeric ingress slots.
Every physical Draw/DrawIndexed snapshots each requested original binding once,
with the real object/view/device/content generation and exact draw range. Unknown
requested inputs reject that invocation. Known null/nonmatching inputs grant no
candidate. Nonindexed draws do not fabricate an IB or indexed match range.
The existing DrawCache qualifies each candidate ingress against the actual
original layout/shaders/index window and complete priority/name-ordered sibling
set. Requirements are compacted in Plan order; no cache result reference survives
a different direct-mapped lookup. This schedules ONE complete original automatic
VS PRE / PS PRE / original-or-skip / VS POST / PS POST tape. Each matched selected
entry must actually reach PRE Check of its captured original binding; direct Run,
POST alone, another same-hash object, or only part of the matched set rejects the
whole tape before GPU setters. Original Skin order and branch behavior remain in
the interpreter; selection never calls a TextureOverride as an automatic root.

Original game IMMUTABLE inputs retain their existing capture route. A DEFAULT
VB/IB with actual creation bytes, no CPU access, no GPU-write/shared flags and
complete existing write coverage can now retain a creation snapshot within the
same CPU/resource budget. This is `capturedInput`, not a claim that DEFAULT
storage is immutable. Existing Map/Unmap/Update/copy/opaque boundaries revoke its
content identity and cached index windows immediately; no refresh, CPU copy,
hash, upload or allocation occurs in draw. Missing/dirty/GPU-only initial bytes
cannot use this static-input route. Texture admission is unchanged.

The original qualified point-list GS/SO path still runs the authentic Draw
trampoline and unchanged original VS/GS inputs, SO declarations and hidden append
cursor. Producer packets now snapshot exact output content generations and
SO-setter revision; the serial owner checks these before Reserve and Finish.
All output bounds are validated before any generation is published. Only a
complete submitted producer advances write generations and input-cache epochs.
Partial submission/owner loss revokes output readability and retains real COM
owners in quarantine. Ordinary external SO draws with an unknown/overflowed
cursor cannot publish a readable producer; zero-work draws publish nothing.
After actual SO unbind, same-immediate-context readers can retain the ordered
output owner through the existing executor; this orders GPU work without a CPU
wait or a claim of GPU completion. Existing Present Signal/fence retirement,
UINT64_MAX/device-loss and failed-Signal ownership remain authoritative.

This is an executable scheduling/resource continuation, not complete Vodyanitsa
qualification. Arbitrary GS/HS/DS, guessed skeleton/LOD ABI, GPU-written indices,
UAV-written SO storage without its real writer contract, foreign/deferred
contexts, general CustomShader, TexFx Diffuse/MRT/o6 and unimplemented original
include dependencies remain unsupported. A conservative SO byte upper bound is
not the driver's actual emitted vertex count or pixel correctness. Original
main/include/MOD files remain intact. Feynman's keyboard/self-managed-persist
slice is independent and must be merged by Root at the overlapping Runtime
hunks. Compilation, game hits, skeleton/deformation/shadows, FSR/NR/FG coexistence,
full-package rendering and performance are UNKNOWN; this worker ran no product
build, tests, fixture, deployment or game.

## R1045 SoftwareMouse native PRE

This candidate includes the R1043 stage-qualified VS reference dependency
cumulatively and completes its formerly missing native Mouse/input boundary.
The original Mouse.ini outer and BW CustomShaders are executable contracts:
all eight scalars are backed up/restored, four VS/PS aliases have real strong
owners, and both original VS/PS stages execute Draw(4,0) with the original
colour and inverse-destination blend states inside two nested state scopes.
Original file-backed mask/color Resources (including genuine known-null
bindings) retain their real immutable DDS owners. No cursor artwork is invented.

The existing OptiInput hook transaction owns SetCursor/GetCursor/GetCursorInfo
and DefWindowProc A/W. Game requests are distinguished from this DLL's menu
requests. Active snapshots own CopyIcon/GetIconInfo-derived hotspot and bind
the actual target HWND/window generation, native PRE token, client rectangle,
real window DPI, calling DPI-awareness context and cursor revision. Visibility
comes from actual CURSORINFO flags; optional hide_cursor uses an input-owned
transparent hardware handle without changing ShowCursor's count. Menu entry,
focus loss, disable/reload, rejected PRE/cancelled POST, HWND destruction or
replacement and shutdown release that policy. An OS-owned HWND epoch property
also prevents recycled handles from reviving old snapshots when no subclass
observed destruction; the existing target validation renews that epoch. No UID/login data is
sampled. Unknown/null shape does not invent a zero hotspot or successful draw.

Coordinate adaptation is explicit: F6 has no XXMI SCREEN_UPSCALING hook and
owns actual client coordinates, equivalent to the primary bypass path in that
domain. Both boolean bypass settings use that same domain; explicit nonzero
[Device] upscaling (the primary SCREEN_UPSCALING field) is rejected at its
actual source. DPI is identity metadata,
not an extra multiplier. PassWindowInfo preserves WINDOW -> current tape RT
-> RES -> original variable fallback; Mouse's bb binding updates RT semantics.

Unlike a finite raster page, cursor position is dynamic. At serial native PRE
preflight, each reached Mouse reader gets one INITIALIZED IMMUTABLE full
4096-float4 texture from the entire tape-position image, before Reserve and
before any setter/draw. It is never patched/uploaded in a draw. Static finite
pages remain unchanged: 512-state/4Mi proof, 512Mi preload/256Mi per-file.
Static plus live native parameter textures share the 512-page/32Mi payload
ceiling and original preload byte headroom. Capacity exhaustion rejects the
whole packet. Abort before submission can reclaim pages; submitted pages,
sprites, aliases, saved state and cursor copy remain in the same native packet
until a NEW post-final-reader Signal completes. Partial submission, failed
Signal/device loss and UINT64_MAX keep the whole real owner quarantined.

Source/candidate only. No build, deployment, full package Load, game pixel or
performance result is claimed. Root alone integrates/builds/deploys this patch.

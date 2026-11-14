# DetailHistory production runtime lifecycle fixture

The common test runner compiles and runs **28 isolated WARP lifecycle cases** with
`-SuiteName NrDetailHistoryRuntime`. Three optional hardware cases run embedded
production DXIL and FP16 history through the same real module: 40 sources of
detail carry, 40 sources of signed noise, and 48 sources of changing appearance
with long detail and short stabilization enabled together. Neither path executes
NGX/model layers or validates game output.

`fixture.cpp` directly includes the entire current `DlssNr_DetailHistory.inl`,
not an extracted or copied implementation. It also directly includes production
`DlssNr_DetailHistoryPolicy.h`, `DlssNr_OrderedLifetime.h` (actual `OrderedHistory`
and `CollectRetired`), `DlssNr_RecordingLifetime.h` (actual `Registry` and `Token`),
`DlssNr_WatchedLists.h`, frame info and shader constant layouts. Changes to those
files are picked up at the next compilation without preparation/generation.

Real WARP devices, DIRECT queues, textures, command allocators/lists, GPU Wait,
queue Signal, CPU fence observations, Execute and successful list Reset supply
the lifecycle operands. A deliberately blocked queue makes incomplete GPU work
deterministic. Post-Execute callbacks call registry then DetailHistory, matching
  the production hook order. Reset callbacks run only after a real successful
Reset. Pending old allocators stay alive; list Reset uses a fresh allocator.

From an initialized x64 MSVC environment, Lead can compile this one TU from the
repository root (the existing `build/temp` directory is suitable):

```powershell
cl /nologo /utf-8 /std:c++20 /EHsc /W4 /Od /IOptiScaler/shaders/dlssnr/tests/detail-history-runtime/stubs /IOptiScaler /Fo:build/temp/DetailHistoryRuntime.obj /Fe:build/temp/DetailHistoryRuntime.exe OptiScaler/shaders/dlssnr/tests/detail-history-runtime/fixture.cpp /link d3d12.lib dxgi.lib
pwsh -NoProfile -File OptiScaler/shaders/dlssnr/tests/detail-history-runtime/Run.ps1 -Executable build/temp/DetailHistoryRuntime.exe
```

The script launches **no compiler** and writes no files. Each case uses a fresh
process, so faulted/quarantined production state remains pinned until process
exit; it is never made safe by clearing flags between tests. Direct flags:

```powershell
build/temp/DetailHistoryRuntime.exe --list
build/temp/DetailHistoryRuntime.exe --case adjacent
build/temp/DetailHistoryRuntime.exe --case duration-zero
build/temp/DetailHistoryRuntime.exe --case finished-native-dxil
# Or include it in the script's case selection with -Hardware.
```

The 17 cases cover:

- `fresh`: complete fresh capture, resolve/capture bindings and modes, no
  pre-Execute publication, ordered post-signal publication while GPU-blocked,
  refused early Release, Reset without completion, then actual retirement.
- `adjacent`: true authenticated same-queue advancement while the previous GPU
  recording is pending; immutable published ordinal/time, real source delta
  and previous-minus-current jitter; generation publication idempotence; old
  owners retained after logical advance and after GPU completion. A parked
  shared input uses production `CollectRetired` and requires **both owning GPU
  completions and both successful Resets**, retiring exactly once.
- `pending`: recorded but never submitted generation blocks the next source;
  actual Reset discards it, and refused source observation prevents inheritance.
- `completion-only`: a queue hint lacks ordered authority; completed work can
  publish logically but still cannot retire an executable generation.
- `wrong-queue`, `history-signal`, `registry-signal`, `replay`: actual wrong-queue
  Execute, missing actual submission receipt (legacy `history-signal` case name),
  failed registry Signal, and actual second Execute of
  the same closed generation. History invalidates, further work blocks, owners
  remain pinned even after queue drain and successful Reset.
- `reconfigure-strength`, `reconfigure-revision`, `reconfigure-encoding`,
  `reconfigure-mapping`, `reconfigure-duration-zero`: incompatible settings do
  not bypass pending GPU/Reset ownership or replace current allocations; after
  closure the changed contract records fresh with previous history disabled.
- `duration-zero`: even compatible, adjacent, already published history stays
  explicitly fresh-only with no implicit legacy feedback weight.
- `strength-zero`: adjacent history contributes nothing when the strength is zero.
- `finished-native-route`: adjacent canonical finished-picture NGX guides record
  and publish the previous source's residual, with completion and Reset still
  required for retirement. This constructs consumer operands; it does not prove
  the actual game producer/call-site metadata.
- `dispatch-failure`: successful resolve followed by failed capture never hands
  out a partial answer or publishes it; its recorded GPU use still needs closure.

Scope and limitations:

- The surrounding `DetailReuse::Frame`, scalar Config access, ledger and
  identity-unwrapping adapters are minimal test seams. Frame info, constants,
  encoding byte count, policy and all history/recording state machines are real.
- The 28 WARP lifecycle cases use a named dispatch seam observing bindings and
  constants. `finished-native-dxil` instead uses a physical D3D12 adapter, the
  exact embedded shader header used by the DLL, real FP16/FP32 texture formats,
  upload/copy barriers, production history selection/publication and pixel
  readback. It requires nonzero bounded carry for 40 frames after fresh model
  residuals disappear; generated model answers are explicit fixture inputs.
  A physical adapter is intentionally optional: creating the complete production
  DXIL pipeline on this machine's WARP returned E_OUTOFMEMORY. The separate
  `runtime/tests/NrDetailHistoryWarpTests.cpp` still runs production HLSL on WARP.
- `--old-dxil-negative PATH.cso` runs the hardware pixel case with an explicitly
  supplied previous deployed shader. Exit 1 with the continuous-contribution
  assertion is the expected negative result. The current shader must pass first;
  a device/pipeline creation failure is not an accepted negative result.
- `stability-appearance-native-dxil` seeds one strong detail, then supplies weaker
  alternating edits while raw illumination changes. The short filter must keep
  its bounded lag without consulting the older long-detail birth RGB. A fixture
  that continually replaces the birth cannot detect this defect. The matching
  `--old-appearance-negative PATH.cso` accepts a shader with only the old appearance
  read restored; require failure at the adjacent-appearance assertion, not an
  unrelated device/compile error. The common HLSL/WARP suite also runs this
  mutation and the independent rolling-birth mutation.
- `--benchmark-history current` or `--benchmark-history PATH.cso` measures only
  the history dispatch/barriers at 2144x1344 with 1536x960 guides. It uses the
  actual DIRECT queue's timestamp frequency, 20 warmup sources and 80 samples;
  upload, CPU drain and readback are outside the interval. Flat synthetic inputs
  and no model/SR/FG make it a stage comparison, never a game FPS or total Reflex
  result. Both versions retain the same inputs, resources and history settings.
  Normal history-expiry branches remain included, and ordered source samples are
  printed alongside the distribution. The CPU debug layer checks every run;
  GPU-based validation is disabled. `tools/tests/Measure-NrHistoryStage.ps1`
  runs current/old/current and binds each log to executable and CSO SHA256 values,
  checked before and after execution. Earlier ad-hoc measurements using a CPU
  clear descriptor from a shader-visible heap are invalid and must be discarded.
- Parking copies the host's snapshot/ownership-transfer adapter; actual
  completion and retirement decisions are production `CollectRetired`/tokens.
  The private-native retirement helper is an explicit no-op: this fixture owns
  pre-SR recording operands and supplies successful Reset notifications itself.
  Actual global hook interception, wrapped COM normalization, Config parsing,
  private Late pool selection, failed real Reset, device removal, NVIDIA SDK,
  game integration, displayed pixels and performance are outside its scope.
- Source serials represent the producer/submission clock, not model cadence;
  capture epoch is stable for these pre-SR cases. This does not validate the
  external producer's assignment of `SourceCaptureSerial = submissionEpoch`.
- Conventions were checked against Guide WARP fixtures and `nr-reconfiguration`.
  A `tests/nr-hot-rebuild` directory was not present in the inspected checkout.

Exit 0 means the selected case passed, 1 means assertion/D3D failure, and 2 means
invalid arguments. No runtime correctness or passing result is claimed until
Lead compiles and executes this fixture against the current production files.

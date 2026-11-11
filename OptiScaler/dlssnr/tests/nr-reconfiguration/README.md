# NR reconfiguration retirement fixture

Lead owns compilation and execution. No runner is changed. This fixture does
not require Windows/D3D headers, run the SDK/game, or establish GPU correctness.

`prepare.py` extracts the **current production bodies**, rather than a second
implementation of the retirement policy:

- `Late::RetireCompletedPrivateRecordings` and `Late::Finished`;
- `Token`, aggregate `Complete`, and registry `Watches`, `ResetRecording`,
  `Collect`, `Snapshot`;
- `CollectRetired`, also used by production `TickNrRetired`.

The host supplies observable intrusive references, fences, allocator/list Reset
and Close results, source-depth completion, canonical list identity, and device
identity. Registry setup represents already-observed submissions. It does not
exercise real `Recording`/`Submitted`, device querying, global hook delivery,
filter internals, call-site eligibility, or pixel/display behavior. The Reset
callback adapter invokes the actual registry closure method; an optional
synthetic Reset hook checks duplicate-notification idempotence.

From the repository root, preparation performs **no compilation**:

```powershell
python OptiScaler/dlssnr/tests/nr-reconfiguration/prepare.py --emit
python OptiScaler/dlssnr/tests/nr-reconfiguration/prepare.py --check
```

`production.inc` stays beside the fixture, contains source SHA256 identities,
and must be regenerated after production edits. `--check` fails on absent or
stale extraction. Default invocation only checks extraction without writing.
All script output is confined to this test directory, with no compiler launch.

For Lead, from an initialized MSVC environment, choose an existing output
directory and keep compiler products outside the source directory:

```powershell
cl /nologo /std:c++20 /EHsc /W4 /WX /Od /Fo:build/temp/nr-reconfiguration.obj /Fe:build/temp/nr-reconfiguration.exe OptiScaler/dlssnr/tests/nr-reconfiguration/fixture.cpp
& build/temp/nr-reconfiguration.exe
```

Three **test-only unsafe mutations** run the same assertions; each must return
**1**, demonstrating detection of removed actual-fence, idle-slot and
failed-Reset-retention guards. A return of 2 means a negative control escaped
detection or its arguments were invalid.

```powershell
& build/temp/nr-reconfiguration.exe --negative-gpu
& build/temp/nr-reconfiguration.exe --negative-pending
& build/temp/nr-reconfiguration.exe --negative-reset
```

Coverage includes both private pools, indefinite polling without Reset while a
second actual execution fence is pending, independent slot/depth completion,
device removal, all slot eligibility gates, the selected endpoint exclusion,
uncertain tokens, shared retirement snapshots with an external producer,
allocator/list Reset failure retention, missed closure callbacks, Close failure
quarantine, and Reset-hook/fallback idempotence. A Close failure follows an
already successful Reset of completed old work: the empty new list stays
quarantined, while retirement of that old generation is valid.

The fixture intentionally does not assert which game may call the helper. Lead
can generalize call sites independently without turning this into a game-name
or string-wiring test.

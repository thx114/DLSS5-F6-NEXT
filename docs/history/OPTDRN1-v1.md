# OPTDRN1 version 1 — format and read-only support tool

## Standalone usage

The supported repository entry point is
scripts/diagnostics/opt_log_drain_decoder.py. Requires Python 3.9+ and its standard
library only: no compiler, audit directory, Conda installation, packages or game
process. Python is a prerequisite, not bundled here. The reader uses portable
Python; current test execution evidence is on Windows.

From the repository root:

~~~text
python -B scripts/diagnostics/opt_log_drain_decoder.py /evidence/drain.bin --pretty
python -B scripts/diagnostics/opt_log_drain_decoder.py /evidence/drain.bin --expect-pid 44236 --expect-process-start-filetime 134375411436773822
~~~

On Windows, py -3 may replace python. The script may also be copied and executed
alone by absolute path from any directory. Example identity values belong to a
native-host fixture, NOT an accepted game session; substitute independently
captured values. Both expected-identity options must be supplied together as exact
unsigned decimal integers, not floating point, exponential, hex or date strings.

The tool opens only the chosen regular file, read-only, and reads at most 8193
bytes. No process-memory access, process identity lookup, network, upload,
injection, game launch, input changes, ZIP export or session discovery occurs.
JSON goes to stdout; callers choose redirection/storage. The tool itself creates
no output files. -B suppresses Python bytecode caches for import/tests. --help
prints usage text; handled errors and normal results are JSON.

No Hub repository/API integration or second exporter is delivered. This tool is
an input for a later unified support bundle, not a claim that Hub already uses it.

## Return codes and exact process identity

| Exit | Meaning |
| --- | --- |
| 0 | Structurally decoded; if expected identity was supplied, both integers match. Without it, no session match is asserted. |
| 2 | Invalid arguments, non-file/missing/unreadable input, malformed/truncated evidence or unsupported format/version. No partial records. |
| 3 | Valid file, but expected PID or exact creation FILETIME mismatches. Do not attach to that session. |
| 4 | Valid file, expected identity supplied, but actual creation FILETIME is unknown (zero) and PID does not already disprove a match. |

JSON ok=true means PARSED, not drain-success or identity-match. Check
identity.status and exit code. Status is match, mismatch, unknown, or
not_requested. No expectations yields not_requested if time is known, otherwise
unknown. A known PID mismatch still returns mismatch if creation time is zero.
There is no fallback session selection on mismatch.

Association authority is the pair (processId, processStartFileTime).
GetProcessTimes creation FILETIME is an unsigned 64-bit count of 100-nanosecond
intervals since 1601-01-01T00:00:00Z. Capture OS PID and exact creation FILETIME
with Hub session identity and loaded Opt DLL hash during controlled collection;
retain sidecar bytes/hash. Compare both integers exactly. PID alone, filename,
mtime, rounded ISO date, current reused PID or uptime is NOT exact association.
Supplying identity copied from this sidecar is not independent matching evidence.

The reader provides processStartUtc display with all seven fractional digits;
out-of-display-range becomes null, with raw integer preserved. Raw FILETIME is
always authoritative. Zero means unavailable and cannot be an expected identity.
The sidecar contains no executable path, DLL hash or Hub session ID; external
same-process evidence is still needed. Missing independent creation FILETIME
leaves session association unverified.

## Binary v1 layout

Little-endian Windows x64, fixed-width fields, no native pointers. All integers
are unsigned except explicit i32. Only version 1 is accepted; a future version
must be independently documented/reviewed, never guessed by matching sizes.

- Exact file size: 8192 bytes; truncation AND trailing bytes reject.
- Header size: 48; record size: 88; slot count: 64.
- Record region: offsets 48..5679. Bytes 5680..8191 are unused padding, ignored
  even if nonzero; they cannot serve as an unversioned extension.
- Magic: eight bytes 4f 50 54 44 52 4e 31 00 (OPTDRN1 plus NUL).

### Header

Absolute offsets; Python struct layout <8s6IQ2i.

| Offset | Bytes/type | Field | Contract |
| --- | --- | --- | --- |
| 0 | 8 bytes | magic | Exact eight bytes above. |
| 8 | 4/u32 | version | Exactly 1. |
| 12 | 4/u32 | headerBytes | Exactly 48. |
| 16 | 4/u32 | recordBytes | Exactly 88. |
| 20 | 4/u32 | slots | Exactly 64. |
| 24 | 4/u32 | processId | Nonzero producer PID, not decoder PID. |
| 28 | 4/u32 | reserved | Zero in v1. |
| 32 | 8/u64 | processStartFileTime | Exact process creation time; zero means unavailable. |
| 40 | 4/i32 | attempts | Failure-record attempts, not drain calls. Negative/overflowed values reject. |
| 44 | 4/i32 | omitted | Concurrent/nested guard or full slot-budget omissions; negative values reject. |

### Record

Offsets relative to 48 + slot * 88; Python layout <8I7Q. Slots are published once
and never reused. Slot order is physical storage, not guaranteed attempt or GPU
ordering under concurrency.

| Offset | Bytes/type | Field | Meaning |
| --- | --- | --- | --- |
| 0 | 4/i32 | committed | 0 unused/in progress: ignore payload; 1 published; other values reject. |
| 4 | 4/u32 | status | 1 queue-capacity precheck refused drain; 2 marker-wait deadline elapsed. No success records. |
| 8 | 4/u32 | context | 1 close, 2 exit, 3 crash-handler pre-dump, 4 post-dump-attempt. |
| 12 | 4/u32 | mode | 0 sync, 1 async drop-oldest, 2 async block. Not queue-health proof. |
| 16 | 4/u32 | threadId | Nonzero reporting thread. |
| 20 | 4/u32 | waitBudgetMs | Requested marker-wait budget, NOT measured elapsed time. |
| 24 | 4/u32 | sampleKnown | Boolean cached sample availability. |
| 28 | 4/u32 | capacityCheckKnown | Boolean; must equal (status == 1). |
| 32 | 8/u64 | sequence | Nonzero attempt sequence; gaps/out-of-order slots allowed; duplicates reject. |
| 40 | 8/u64 | tickMs | GetTickCount64 uptime, NOT wall-clock timestamp or creation time. |
| 48 | 8/u64 | queueCapacity | Nonzero configured capacity in items. |
| 56 | 8/u64 | sampledItems | Cached periodic size, NOT remaining backlog at failure. |
| 64 | 8/u64 | sampledOverruns | Cached cumulative spdlog item overruns, including control messages; NOT exact lost log lines. |
| 72 | 8/u64 | sampleAgeMs | Cached sample age, NOT drain duration. |
| 80 | 8/u64 | itemsAtCapacityCheck | Size at block precheck, NOT post-failure remaining count. |

Unknown flags must be 0/1; unknown enums and invalid required fields reject.
Unknown sample binary values must all be zero; JSON exposes null instead of fake
known-zero values. Known sampledItems cannot exceed capacity. Capacity-known must
match status=1, mode=async_block and size >= capacity-1 (two control messages need
room). Unknown capacity-check size must be zero. Contradictory committed payloads
reject the whole result, not silently repair or partially salvage data.

Header counters can precede record publication or reflect interrupted recording.
attempts != omitted + committedCount, or sequence > attempts, produces
counters_not_reconciled rather than invented records or automatic corruption
classification. Positive omissions generate a warning. Reconciliation itself is
not proof of capture completeness or durability. Negative signed counters cannot
be interpreted safely and reject.

## JSON API — schemaVersion 1

Public standalone Python functions (optional parameters are keyword-only Python
integers):

~~~python
decode_bytes(data, *, expected_pid=None, expected_process_start_filetime=None)
read_and_decode(path, *, expected_pid=None, expected_process_start_filetime=None)
~~~

Returns JSON-compatible dict or raises DecodeError (code, offset, message).
File reads may also raise OSError; CLI converts handled errors to JSON/exit 2.
Neither function needs an audit path, sibling module or compiler.

Success keys:

- schemaVersion=1, tool="opt-log-drain-decoder", ok=true.
- format: name, version, fileBytes, headerBytes, recordBytes, slots.
- source: bytes and SHA256 of the exact decoded snapshot; path only for file
  input. Hash identifies bytes, NOT authorship/loaded DLL identity. No mtime/session guessing.
- identity: status, reason, actual (pid, processStartFileTime, processStartUtc),
  expected (pid, processStartFileTime or null). Mismatch remains valid parsed data.
- snapshot: attempts, omitted, committedRecords, uncommittedOrUnusedSlots,
  countersReconciled. Uncommitted payloads are never exposed.
- records: slot order, each with slot, committed=true, sequence, status/context/mode
  objects {code,name}, threadId, waitBudgetMs, tickMs, queueCapacity,
  queueSample {known,items,overruns,ageMs}, capacityCheck {known,items}.
- warnings: {code,message} objects; currently creation_filetime_unavailable,
  filetime_display_out_of_range, failure_records_omitted, counters_not_reconciled.

Every binary u64 is a DECIMAL JSON STRING (including FILETIME, sequence, tickMs,
queue capacity, optional queue counts/ages), preserving integers beyond 2^53 in
JavaScript consumers. Unknown optional values are null. Flags are booleans;
header i32/u32, codes and slot indices are JSON numbers. Never round-trip identity
through floating point. JSON is key-sorted and ASCII-safe with no generated
timestamp; identical bytes/path/arguments produce identical JSON. --pretty only
changes whitespace.

Error envelope: schemaVersion, tool, ok=false, error {code,message,offset}. Offset
is a byte offset or null; NO partial records are returned. Automate on code,
schema and identity, not prose message text. Error codes:

- invalid_arguments, invalid_identity_arguments, input_not_file, read_error;
- truncated_header, invalid_magic, unsupported_version, invalid_header_size,
  invalid_record_size, invalid_slot_count, truncated_records, invalid_file_size;
- invalid_header, invalid_counters, invalid_commit_marker, invalid_record_enum,
  invalid_record_flags, invalid_record, duplicate_sequence,
  inconsistent_capacity, inconsistent_sample.

Binary format version and JSON schemaVersion are separate. Incompatible JSON
changes require a schemaVersion bump. Additive keys/warnings can be documented
without changing version: consumers should tolerate new keys but fail closed on
unknown schemaVersion, binary version or identity status. No future layout is
accepted just because its dimensions resemble v1.

## Limits / collection

The frozen product candidate prepares the mapping outside DllMain beside the
configured log as OptiScaler_log_drain_<PID>.bin, using CREATE_NEW. A leftover
PID/reload file can cause init failure instead of overwriting evidence. Do not
delete or rename old evidence to manufacture a session match. Decoder does not
change producer behavior or deploy/enable the candidate.

Prefer an immutable preserved snapshot after normal process exit. Live file reads
are not transactional or a CPU acquire relationship with a different process;
committed=1 is necessary but not a universal live-capture guarantee. Counters
can lag/torn snapshots can occur. No lock/retry/repair is performed. Malformed
snapshots reject, uncommitted records skip; retain raw evidence for review.

Mapped writes can page fault; no durable-storage, crash completeness or hard
real-time guarantee. Counts may be stale/unknown; records may be omitted. Failure
does not prove fatality, worker death or disk failure. Marker success does not
prove durable storage, no drops or completion of all multiworker sink work.
Existing synchronous/block-policy operations can still block. No failure records
does NOT establish logging/game/NR/FG/rendering success.

## Portable tests

~~~text
python -B -m unittest discover -s scripts/diagnostics/tests -v
~~~

unittest only, no audit/runtime/compiler dependency. An unmodified actual MSVC
native-host mapped sidecar is included under scripts/diagnostics/tests/fixtures/
with SHA256/provenance. Its origin path is metadata, not a runtime dependency.
This is real OS/spdlog test evidence, NOT a game crash/product DLL acceptance.

Tests cover exact PID+FILETIME/one-tick PID reuse, zero and out-of-display-range
FILETIME, every truncation length 0..8191, magic/version/layout/bounds/enum/flag
corruption, incomplete records, unknown samples, counter/omission/concurrency
semantics, input preservation, stable JSON/exit contracts and standalone use.
Fixture PIDs/thread/timing are native test data, not user gameplay/memory data.

# Genshin baseline recovery candidate — 2026-11-05

Base: `5a61e97df4fd880c41fd37090f5f9d6b31c1622b`, branch
`codex/f6-genshin-baseline-recovery`. Product changes are confined to
`with_dx12/GenshinVisibleChild.h` and `inputs/FG/GenshinFgTicket.h`.
No shared SR/provider, StarRail adamfg/F5-DS, ZZZ routing, NR algorithm,
profile, product build, deployment, game or UI changes.

## Evidence and causality

Authoritative input: `D:/CODE/DLSS5_NEXT-workers/f6-integration/build/genshin-64796-OptiScaler.log`,
33,974,207 bytes / 164,379 lines, SHA256
`25A991216035C0178367F4626F830C4BF1CF100C9CC9547603C5A149290E971A`.
Rehashed for this candidate. Adjacent `genshin-64796-evidence.json` records
PID64796 / loaded version5a61e97; this does not establish loaded-memory SHA.
Line numbers below refer to this fixed log, not the overwritten public log.

| Observation | Evidence | Meaning / limit |
|---|---|---|
| Two selected SR creations, both DLSS_on12 | 11065–11066, 45801–45802; DLSS initialization success11477/45959 | Not continuous selected-SR fallback/rebuild; 797 DX11 Evaluate records |
| New camera/FSR instance before second SR create | 45516/45796, instance7697188897632 vs7696695107168 | Actual instance transition; cause and first SR destruction remain unknown |
| No ChangeFeature or observed DX11 ReleaseFeature | whole fixed log | No logged selected-backend change or release witness; absence is not proof no destruction occurred |
| 43 raw refusals | 30 OwnerRevealDrainPending,10 UnauthenticatedOutputWaitingForOwnerHide,3 OwnerOrParentBoundaryBusy | Normal scheduling/visibility state was converted to DXGI failure |
| 86 SDK asynchronous failures and86 deferred host errors | whole fixed log | Error propagation is explicit in SDK messages; logs may include several subframes per refusal |
| 44 FG teardown requests and44 completed frees | whole fixed log;46 FrameGeneration NGXCubinD3D12 Init records including startup | Real repeated FG lifecycle churn; not46 selected SR recreations |
| Final selected SR copyback summary | 164307:798 candidates,789 token matches/returned/submitted/copyback | Copies were queued to selected game RTV; final backbuffer consumption and pixels remain UNKNOWN |
| Late NR depth absent | 128617:picture8662/modelCalls403/modelDepth0x0/rect0x0/feedbackDepth=false | Agrees with user report; three early nonzero-depth samples do not establish steady-state depth |

The clearest directly observed cycle is19707 `OwnerRevealDrainPending` →19708
SDK worker `Async present failed (0x887A000A)` → next host error →19748
`Deactivate` →20589 owned-queue-fence completion and `slFreeResources=eOk` →
SDK initialization/warmup again. Teardown is doing its job after a failed SDK
Present. Removing teardown or accepting a failed raw would conceal the cause.

The first cycle has a second admission defect. Sequence30 was shown after its
actual physical tail at15720. The next input at15733 reports309ms since the
previous SDK input, exceeding the250ms **input** lease despite recently shown
output. Dispatch20 succeeds at15744 but BeforePresent hides/revokes at15745;
15749 records `SdkStreamPostvalidationRefused` with saved SDK S_OK. Worker31
is refused before hidden acknowledgement at15754;15755 explicitly defers that
error;15876 reports it to the host;15886/16730 tear down/free FG. The precise
ArmStream refusal operand is not logged, so expired input lease is a
source-supported explanation of this first refusal, not a measured branch ID.

Display epoch9→10 is recorded at15745 while camera guide epoch remains3,
cut2, instance unchanged at15736/15750. Display epochs, FG `_inputOwnerEpoch`
(advanced by Deactivate), and camera/guide epoch are different clocks.
The44 FG resets cannot be assigned to camera epoch changes.36 bounded
`NoAuthenticatedCameraOrGuides` records also cover startup, missing unconsumed
render tokens and real predecessor gaps; they are not SR-recreate evidence.
Camera transitions can legitimately pause/reset temporal history, but a
bidirectional camera-epoch/backend-rebuild loop is not established here.

`FSR2X_DX11` is the intercepted input API assigned in `inputs/FSR2_Dx11.cpp`,
not the selected output backend. Actual fallback constructors are in
`FeatureProvider_Dx11::GetFeature` (unsupported/missing DLSS/module paths) and
ChangeFeature initialization failure handling; this session instead records
successful DLSS_on12 creation. Thus the perceived FSR restarting is plausibly
the directly observed FG teardown/warmup/fallback-to-natural-parent churn;
continuous selected FSR fallback has not been demonstrated.

Read-only F5 comparison:
`D:/DATA/Documents/ChatGPT/DLSS F5 NEXT/work/r294-r266-build/OptiScaler`.
Its child PresentPermit allows an authenticated input to complete across the
SDK call without the later asynchronous stream admission machinery. This
candidate restores that lifecycle property while retaining F6's exact owner,
physical binding, real queue/fence, hidden bootstrap and completed-tail reveal
requirements. No F5 DLL or whole source file is restored.

## Candidate behavior and lock audit

The existing reveal window now waits for the HWND owner instead of returning
an immediate synthetic SDK failure. Timed display acquisition, reveal wait,
hide acknowledgement and pre-raw checks share one250ms scheduling budget.
Timeout alone grants neither hidden state nor any GPU completion. A queued
worker on the HWND owner's own thread skips waiting on itself; it cancels only
the CPU input-yield window and still passes the original physical checks.
Stalled owners, real revocation and wrong queue/epoch/device still fail.

There are exactly two AwaitOwner sites, both inside PhysicalOutput::Bind.
The only production caller is FinalOutputEndpoint::PresentImpl, entered from
wrapped_swapchain's retained Genshin child branch. It holds endpoint state,
but no DisplayGate lock. Bind acquires its sole display unique_lock once.
At the reveal wait no recursive helper is live. At the hide wait,
RequestHide's recursive display lock and snapshot lock have both returned.
All waiting paths exclude OnOwnerThread. Same-thread HWND reentry from
ShowWindow/SetWindowPos is therefore non-waiting. Both actual waits have
recursive depth **exactly1**, not an assumption that condition_variable_any
unlocks every recursive level.

Known lock order is endpoint → display → snapshot. Input Arm/Accept, owner
Publish/Hide/ApplyShow use display → snapshot; ReadSnapshot/RequestReveal
release snapshot before any later display operation. Owner reveal checks
captured fence completion directly, never enters endpoint/provider state or
waits for a GPU. While AwaitOwner releases display, owner publication can
proceed without endpoint state. The owner retirement callback runs before
OwnerCommand takes display; Endpoint::TryRetire uses try-locks and refuses
in-flight activity. No reverse display → endpoint wait exists in this source
graph. External game/driver callbacks and actual HWND timing still require
runtime validation; the CPU fixture does not prove their lock behavior.

SdkStreamLease::Begin opens only after the exact current parent/dispatch and
creation owner are authenticated. A fresh input may renew an elapsed display
lease, and an open real SDK call survives display timeout. Accept requires the
saved original S_OK and all post-call owner/source checks. Only accepted
streams plus completed physical tails can reveal. Output-only expiry hides
old pixels without bumping epoch/revoking a current SDK call; focus, geometry,
configuration and actual stream retirement still use ordinary revocation.

PresentPermit destruction ends the same captured stream lease under display
lock on every normal/early-return/C++-unwind path. Failed/unaccepted End
revokes its deadline too. RevokeStream clears open/accepted state immediately
on epoch/owner loss. A late destructor compares streamScope identity before
End and both scope/epoch before Hide, so it cannot close or hide a replacement scope.
Begin's allocation happens before opening; assignment after Begin is noexcept.
AwaitOwner catches recoverable wait errors at the noexcept hook boundary and
admission rechecks its predicates. Fatal process termination or failed mutex
relocking is not a recoverable C++ unwind path. No teardown/GPU-retirement
proof is manufactured by Begin, Accept, End or a condition-variable wake.

## Validation and remaining work

MSVC14.44.35207, standalone C++20 fixtures: contract127 checks, threaded
handoff15 checks. Thread tests exercise the actual OutputRevealWindow wait:
spurious publication, actual owner notification, expiry without hidden ACK,
retirement wake, and endpoint-held wait with returned recursive hide scopes.
The last case proves the depth-one display release permits owner publication.
This fixture models endpoint/snapshot locks; it does not execute HWND or SDK.
Failure and revoked-call lease transitions use the actual SdkStreamLease;
PresentPermit exception cleanup is established by RAII/source wiring audit,
not an injected real-SDK exception.

`python OptiScaler/genshin/tests/async-output/check-wiring.py`:64 PASS.
The same command with `--revision 5a61e97` fails at the expected normal-reveal
synthetic-error regression. Existing native/drain production and exact
queue/fence retirement checks remain in that suite. Compile fixtures from a
VS2022 x64 developer prompt with `/std:c++20 /EHsc /W4
/D_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR`, placing obj/exe under ignored build.

Five production translation units PASS MSVC `/Zs /Y-` semantic syntax checks:
dx11_with_dx12_sc.cpp, DLSSG_Dx12.cpp, FinalOutputEndpoint.cpp, FG_Hooks.cpp,
wrapped_swapchain.cpp, with the project's Release include paths. No product compilation/link,
prebuild version generation or deployment is performed. Existing C4858 at
GenshinReturnedRaster.h222 is unrelated. Final results are in ignored
`build/baseline-checks/*-syntax.log` and the delivery message.

This candidate addresses two evidenced FG admission/lifecycle causes, not all
reported symptoms. Whether selected DLSS reaches the visible backbuffer, why
the camera/FSR instance changed, why late NR depth is absent, unlogged camera
qualification failures, actual generated cadence and performance remain open.
User-visible output/SDK stream/shown-after-tail records do not overturn the
user's observation. Lead owns independent review, integration, product build
and any later controlled game acceptance; CPU/syntax PASS is not game PASS.

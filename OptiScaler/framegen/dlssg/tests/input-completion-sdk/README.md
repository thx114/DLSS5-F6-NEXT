# Standalone SDK input-completion probe

This host loads the existing Streamline runtime in its own process, creates an
NVIDIA D3D12 device, queue and swapchain, and submits owned color, depth and
motion inputs with matching constants, frame tokens, Reflex and PCL markers.
The calling Present thread immediately queries the active `slDLSSGGetState`
function and takes an independent reference to the returned D3D12 fence. Its
device, target progress, actual Presented count and status are recorded.

The default window is hidden. No game, launcher, F6 DLL, ASI, user preset or
configuration is launched or modified. The existing product's application ID
is used; this probe does not bypass SDK, driver or feature support checks.
Installed DLL hashes are checked before and after; the actual state-function
module path and hash are recorded separately because NVIDIA's runtime can
select a cached plugin override.

From the repository root, run with PowerShell 7:

```powershell
pwsh -NoProfile -File OptiScaler/framegen/dlssg/tests/input-completion-sdk/run.ps1 `
  -RuntimeDirectory 'D:/APPS/test/TEST/HoYoShadeHub_Portable_1.4.5.0_x64_Full/OptiScaler/local/F6-ASI/OptiScaler/streamline'
```

An optional **manual** focused test adds `-Visible`. The probe shows its own
window without activating it, then allows 45 seconds for the user to click it.
It does not change foreground focus or synthesize input. The focused mode
samples 120 source frames with a synthetic 16 ms cadence; default hidden mode
samples 24. Neither measures game FPS or end-to-end Reflex latency.

Exit codes are preserved by `run.ps1`:

| Exit | Meaning |
|---|---|
| 0 | At least one successful Present returned a successful state, same-device owning fence and advancing nonzero target |
| 1 | Compilation, SDK call or other probe failure; see logs |
| 3 | SDK capability could not be established; the recorded result gives the reason |
| 4 | No consumed-input proof, including no manual focus or inactive FG; this is not a passing queue-separation test |
| 5 | Standalone GPU drain or SDK cleanup failed; process termination avoids ordinary owner release |

A successful CPU queue Wait only orders future work. This probe does not grant
F6 resource retirement, map every input to a production lifecycle, or activate
separate queues. It uses the SDK's D3D default presenting-queue blocking mode
and actually drains that queue before resetting and releasing its resources.
Repeated/retrograde targets, zero, device removal and wrong-device fences do
not count as a new input observation. `numFramesActuallyPresented` remains a
raw observation, not a game-display rate or proof of visual quality.

The bundled SDK contract is `external/streamline/sl_dlss_g.h:170`: query on the
Present thread and retain the actual SDK fence before modifying tagged inputs
from a non-presenting queue. This executable does not replace the production
ABI/forwarding/queue fixtures or user-started game acceptance.

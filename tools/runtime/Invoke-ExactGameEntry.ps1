[CmdletBinding(DefaultParameterSetName = 'Resolve')]
param(
    [Parameter(Mandatory)][ValidateRange(1, [int]::MaxValue)][int]$ExpectedPid,
    [Parameter(Mandatory)][ValidateRange(1, [long]::MaxValue)][long]$ExpectedCreationFileTime,
    [Parameter(Mandatory, ParameterSetName = 'Explicit')][ValidateRange(1, [long]::MaxValue)][long]$ExpectedWindow,
    [Parameter(ParameterSetName = 'Resolve')][switch]$ResolveWindow,
    [switch]$RestoreHiddenWindow,
    [switch]$DelegateForeground,
    # Explicit opt-in: only the exact no-input foreground-retention rejection can authorize one activation click.
    [switch]$AllowActivationClick,
    [double]$X = 0.5,
    [double]$Y = 0.949,
    # Required: every launcher CLI call uses exactly this --biz value; no implicit default game.
    [Parameter(Mandatory)][ValidateSet('hk4e_cn', 'hkrpg_bilibili', 'nap_cn')][string]$Biz,
    # Bounded wait for one launcher CLI child. A timeout is UNKNOWN: no retry, no kill, no ExitCode read.
    [ValidateRange(1, 120)][int]$CliTimeoutSeconds = 60,
    [string]$LauncherPath = 'D:\APPS\test\TEST\HoYoShadeHub_Portable_1.4.5.0_x64_Full\HoYoShadeHub.exe'
)

$ErrorActionPreference = 'Stop'
$principal = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
if (!$principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run this script from an already elevated PowerShell session; no game action was requested.'
}
if (![double]::IsFinite($X) -or ![double]::IsFinite($Y) -or $X -lt 0 -or $X -ge 1 -or $Y -lt 0 -or $Y -ge 1) {
    throw 'Coordinates must be finite normalized values in [0, 1); no clamping is performed.'
}
[void][DateTime]::FromFileTimeUtc($ExpectedCreationFileTime)
if (![IO.Path]::IsPathFullyQualified($LauncherPath) -or !(Test-Path -LiteralPath $LauncherPath -PathType Leaf)) {
    throw 'LauncherPath must identify an existing absolute launcher EXE path.'
}
$recordDirectory = Join-Path ([IO.Path]::GetTempPath()) ('hys-exact-entry-' + [Guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($recordDirectory)
Write-Information "CLI records: $recordDirectory" -InformationAction Continue

if (!('HoYoShadeHub.CliInspection.Windows' -as [type])) {
    Add-Type -LiteralPath (Join-Path $PSScriptRoot 'ExactGameWindowInspection.cs')
}
$gameProcess = [Diagnostics.Process]::GetProcessById($ExpectedPid)
function Assert-ExactProcess {
    if ($gameProcess.HasExited -or $gameProcess.StartTime.ToUniversalTime().ToFileTimeUtc() -ne $ExpectedCreationFileTime `
        -or $gameProcess.MainModule.FileName -ne $gameImage) {
        throw 'Bound game process exited or its creation time/executable identity does not match.'
    }
}

function Assert-SelectedWindow([switch]$BeforeActivation) {
    Assert-ExactProcess
    $state = [HoYoShadeHub.CliInspection.Windows]::Inspect($script:selectedWindow)
    $eligible = $state.Candidate
    if ($RestoreHiddenWindow) {
        $eligible = $state.RecoverableMain -and ($BeforeActivation -or $state.Visible)
    }
    if ($state.Pid -ne $ExpectedPid -or !$eligible -or (!$BeforeActivation -and $state.Minimized)) {
        $state | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $recordDirectory 'rejected-window.json') -Encoding utf8
        throw 'Selected HWND failed identity/main-window/visibility checks; see rejected-window.json. Hidden recovery requires the updated launcher DLL and explicit -RestoreHiddenWindow.'
    }
}

function Invoke-ExactStep([string]$Name, [hashtable]$Step, [switch]$ReturnActivationRejection) {
    $jsonPath = Join-Path $recordDirectory ($Name + '.json')
    @{ steps = @($Step) } | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $jsonPath -Encoding utf8
    $stdoutPath = Join-Path $recordDirectory ($Name + '.stdout.log')
    $stderrPath = Join-Path $recordDirectory ($Name + '.stderr.log')
    # Hide each child console so a new console does not take focus between the two calls.
    $foregroundBefore = [HoYoShadeHub.CliInspection.Windows]::Foreground()
    $child = Start-Process -FilePath $LauncherPath -WorkingDirectory (Split-Path -LiteralPath $LauncherPath) `
        -ArgumentList @('run', '--biz', $Biz, '--file', ('"' + $jsonPath + '"')) `
        -WindowStyle Hidden -PassThru -RedirectStandardOutput $stdoutPath -RedirectStandardError $stderrPath
    $grant = $null
    # Bounded wait only: the child is never killed and never waited on without a limit.
    $timedOut = $false
    $exitCode = $null
    try {
        try {
            if ($DelegateForeground -and $Name -eq 'activate') {
                if ($child.HasExited) { throw 'Activation CLI exited before foreground permission could be delegated; no retry.' }
                $grant = [HoYoShadeHub.CliInspection.ForegroundPermission]::Grant($child.Id)
                [ordered]@{
                    utc = [DateTime]::UtcNow.ToString('O'); helperPid = $PID; cliPid = $child.Id
                    foregroundBefore = $foregroundBefore; accepted = $grant.Accepted; win32Error = $grant.Error
                } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $recordDirectory 'foreground-grant.json') -Encoding utf8
            }
        } finally {
            # No kill/retry: even permission denial waits (bounded) for this activation-only child.
            if ($child.WaitForExit($CliTimeoutSeconds * 1000)) {
                $exitCode = $child.ExitCode
            } else {
                $timedOut = $true
            }
            [ordered]@{
                utc = [DateTime]::UtcNow.ToString('O'); cliPid = $child.Id
                waitOutcome = $(if ($timedOut) { 'UNKNOWN_TIMEOUT' } else { 'EXITED' })
                exitCode = $exitCode; timeoutSeconds = $CliTimeoutSeconds
                foregroundBefore = $foregroundBefore; foregroundAfter = [HoYoShadeHub.CliInspection.Windows]::Foreground()
            } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $recordDirectory ($Name + '.result.json')) -Encoding utf8
        }
        if ($timedOut) {
            # The child may still be running: its ExitCode is not read, and nothing is retried or killed.
            throw "$Name CLI did not exit within $CliTimeoutSeconds s; exit status UNKNOWN (no ExitCode read), no retry, child left running. Records: $recordDirectory"
        }
        # Only the activation caller may inspect a rejection, with explicit authorization.
        # Click failure/partial input is never retried or converted into a second click.
        if ($exitCode -ne 0 -and !($Name -eq 'activate' -and $ReturnActivationRejection)) {
            throw "$Name failed (exit $exitCode); input status is in CLI stdout/stderr; stopping without retry. Records: $recordDirectory"
        }
        # This is the only success-stream object; child stdout/stderr stay in their files.
        return [pscustomobject]@{ ExitCode = $exitCode; StdoutPath = $stdoutPath; StderrPath = $stderrPath }
    } finally {
        [void]$child.Dispose()
    }
}

try {
    # Keep a process handle through both CLI calls; never follow a reused PID.
    # The launcher independently checks the executable against GameCatalog.
    $gameHandle = $gameProcess.SafeHandle
    $gameImage = $gameProcess.MainModule.FileName
    if ([string]::IsNullOrWhiteSpace($gameImage)) { throw 'Cannot resolve the bound process executable.' }
    Assert-ExactProcess
    $states = @([HoYoShadeHub.CliInspection.Windows]::ForProcess($ExpectedPid))
    [ordered]@{ pid = $ExpectedPid; creationFileTime = $ExpectedCreationFileTime; image = $gameImage; windows = $states } `
        | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $recordDirectory 'windows.json') -Encoding utf8
    if ($PSCmdlet.ParameterSetName -eq 'Explicit') {
        $script:selectedWindow = $ExpectedWindow
    } else {
        $candidates = @($states | Where-Object {
            $_.Pid -eq $ExpectedPid -and (
                ($RestoreHiddenWindow -and $_.RecoverableMain) -or (!$RestoreHiddenWindow -and $_.Candidate)
            )
        })
        if ($candidates.Count -ne 1) {
            throw "Expected exactly one eligible game main window; found $($candidates.Count). See windows.json. No activation or click requested."
        }
        $script:selectedWindow = $candidates[0].Window
    }
    Assert-SelectedWindow -BeforeActivation
    Write-Information "Selected HWND: $script:selectedWindow" -InformationAction Continue
    $activationStep = @{
        action = 'activate_game'; expectedPid = $ExpectedPid
        expectedCreationFileTime = $ExpectedCreationFileTime; expectedWindow = $script:selectedWindow
    }
    if ($RestoreHiddenWindow) { $activationStep.restoreHiddenWindow = $true }
    $activationResult = Invoke-ExactStep 'activate' $activationStep -ReturnActivationRejection:$AllowActivationClick
    if ($activationResult -is [array] -or $activationResult -isnot [pscustomobject]) {
        throw "Activation CLI did not return one structured result; no click requested. Records: $recordDirectory"
    }
    Assert-SelectedWindow
    $activationClick = $false
    if ($activationResult.ExitCode -ne 0) {
        # Match the existing final no-input foreground-retention summary, not a generic failure,
        # logging exception, or API boolean alone. API acceptance does not prove retained foreground.
        $rejection = '^\s*\u2717 activate_game \u88AB\u62D2\u7EDD\uFF1AGame foreground activation was not retained: requestedWindow=' `
            + [regex]::Escape([string]$script:selectedWindow) `
            + ' actualForeground=[0-9]+ actualForegroundPid=[0-9]+ apiAccepted=(?:True|False); target identity and visibility passed; no input was sent\.\s*$'
        $exactRejections = @(Get-Content -LiteralPath $activationResult.StdoutPath -Encoding utf8 | Where-Object { $_ -cmatch $rejection })
        if (!$AllowActivationClick -or $activationResult.ExitCode -ne 1 -or $exactRejections.Count -ne 1) {
            throw "Activation failed outside the explicitly authorized exact foreground-retention rejection; no click requested. Records: $recordDirectory"
        }
        $activationClick = $true
        Write-Information ($exactRejections[0].Trim()) -InformationAction Continue
        Write-Information 'Exact no-input foreground-retention rejection matched; requesting one exact-hit activation click, with no second click.' -InformationAction Continue
    } elseif ([HoYoShadeHub.CliInspection.Windows]::Foreground() -ne $script:selectedWindow) {
        throw 'Selected window lost foreground after activation; no click requested.'
    }
    $clickStep = @{
        action = 'click_game'; expectedPid = $ExpectedPid
        expectedCreationFileTime = $ExpectedCreationFileTime; expectedWindow = $script:selectedWindow; x = $X; y = $Y
    }
    if ($activationClick) { $clickStep.activationClick = $true }
    Assert-SelectedWindow
    if ($activationClick) {
        # Recheck eligibility without changing the HWND; explicit selection also needs one visible target.
        $visibleCandidates = @([HoYoShadeHub.CliInspection.Windows]::ForProcess($ExpectedPid) | Where-Object {
            $_.Pid -eq $ExpectedPid -and $_.Candidate -and !$_.Minimized
        })
        if ($visibleCandidates.Count -ne 1 -or $visibleCandidates[0].Window -ne $script:selectedWindow) {
            throw 'Activation click requires the bound HWND to remain the unique eligible visible game main window; no click requested.'
        }
    }
    $clickResult = Invoke-ExactStep 'click' $clickStep
    if ([HoYoShadeHub.CliInspection.Windows]::Foreground() -ne $script:selectedWindow) {
        throw "Click CLI returned exit 0, but the exact HWND is no longer foreground; input was already sent, consumption/world entry UNKNOWN; no retry. Records: $recordDirectory"
    }
    Write-Information "CLI accepted one left-button click and verified foreground (activationClick=$activationClick); consumption/world entry UNKNOWN. Records: $recordDirectory" -InformationAction Continue
} catch {
    $_ | Out-String | Set-Content -LiteralPath (Join-Path $recordDirectory 'failure.txt') -Encoding utf8
    throw
} finally {
    $gameProcess.Dispose()
}

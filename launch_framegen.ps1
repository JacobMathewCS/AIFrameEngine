<#
.SYNOPSIS
    Launches AIFrameEngineCompositor with the validated frame-generation configuration.
.DESCRIPTION
    This script starts the compositor with the settings that produced consistent
    ~28-30 generated frames/sec in testing:
      - FP16 inference (AIFRAME_RIFE_FP16=1)
      - Display caching (AIFRAME_DISPLAY_CACHE=1)
      - Full-resolution reconstruction (default)
      - Correct client-area crop (default)
      - Adaptive presentation delay (worst capture-to-completion + 10 ms, 66.7 ms ceiling)

    Usage:
        .\launch_framegen.ps1 -SelectWindow -Fsr1 # pick a running game's main window
        .\launch_framegen.ps1 -ListWindows       # list accessible windows, no launch
        .\launch_framegen.ps1 -TargetProcessId 1234 # use a listed PID
        .\launch_framegen.ps1 -Fsr1      # experimental reference EASU + RCAS upscaler
        .\launch_framegen.ps1            # normal launch
        .\launch_framegen.ps1 -Debug     # Debug build
        .\launch_framegen.ps1 -NoFp16    # FP32 inference
        .\launch_framegen.ps1 -NoCache   # disable display caching
        .\launch_framegen.ps1 -RepeatCopyProbe 1 # diagnostic: ADD one copy on repeat-only cache hits
        # Opt-in evidence bundle; waits for clean compositor exit (use a NEW absolute directory):
        .\launch_framegen.ps1 -WorkerCpuProbe -DiagnosticDirectory 'C:\Temp\afe-trial-001' -TrialLabel '1080p gameplay'
.NOTES
    RepeatCopyProbe is OFF by default (0); accepts 0..4 extra copies, requires caching.
    This adds diagnostic load, not a performance fix. Keep other options identical
    between baseline and probe runs. It does not measure GPU-active time.
    Requires Witcher 3 (or target game) running at 720p windowed, 30 FPS capped.
    Press F9 to enable frame generation, F10/F11 for upscaling/sharpening,
    Ctrl+Shift+Up/Down for +/-0.05 sharpening (clamped 0..1),
    F8 to cycle comparison modes, Ctrl+Shift+F12 to quit cleanly.
#>

param(
    [switch]$Debug,
    [switch]$NoFp16,
    [switch]$NoCache,
    [switch]$Fsr1,
    [switch]$TracePresentations,
    [switch]$WorkerCpuProbe,
    [switch]$TraceRifeAttempts,
    [string]$DiagnosticDirectory,
    [string]$TrialLabel,
    [ValidateRange(0, 4)]
    [int]$RepeatCopyProbe = 0,
    [switch]$SelectWindow,
    [switch]$ListWindows,
    [int]$TargetProcessId = 0
)

$root = $PSScriptRoot
if ($TraceRifeAttempts -and -not $DiagnosticDirectory) { throw 'TraceRifeAttempts requires DiagnosticDirectory.' }
if ($DiagnosticDirectory -and [string]::IsNullOrWhiteSpace($TrialLabel)) {
    throw 'DiagnosticDirectory requires a nonempty TrialLabel describing this trial.'
}
if ($DiagnosticDirectory -and $ListWindows) { throw 'DiagnosticDirectory cannot be used with ListWindows.' }
if ($RepeatCopyProbe -and $NoCache) {
    throw 'RepeatCopyProbe requires display caching; remove -NoCache.'
}
$cfg = if ($Debug) { 'Debug' } else { 'Release' }
$exe = "$root\build\$cfg\AIFrameEngineCompositor.exe"

if (-not (Test-Path $exe)) {
    Write-Error "Executable not found: $exe"
    exit 1
}

# Offer accessible main windows only; never choose an arbitrary same-name process.
$selected = $null
if ($SelectWindow -or $ListWindows -or $TargetProcessId) {
    $windows = @(Get-Process | ForEach-Object {
        try {
            if ($_.MainWindowHandle -ne 0 -and $_.ProcessName -ne 'AIFrameEngineCompositor') {
                [pscustomobject]@{ Id = $_.Id; Process = $_.ProcessName;
                    Title = $_.MainWindowTitle; Handle = $_.MainWindowHandle.ToInt64();
                    Created = $_.StartTime.ToUniversalTime().ToFileTimeUtc() }
            }
        } catch { } # Protected/exited processes cannot be selected safely.
    } | Sort-Object Process, Id)
    $windows | Select-Object Id, Process, Title | Format-Table -AutoSize | Out-Host
    if ($ListWindows) { return }
    if ($SelectWindow -and -not $TargetProcessId) {
        $answer = Read-Host 'Enter the game PID from this list (blank cancels)'
        if ([string]::IsNullOrWhiteSpace($answer)) { return }
        if (-not [int]::TryParse($answer, [ref]$TargetProcessId) -or $TargetProcessId -le 0) {
            throw 'Invalid PID. Run the picker again.'
        }
    }
    $selected = $windows | Where-Object Id -eq $TargetProcessId | Select-Object -First 1
    if (-not $selected) { throw 'No accessible main window for that PID. Restore the game window and retry.' }
    Write-Host "Selected: $($selected.Title) (PID $($selected.Id))"
}

$existing = Get-Process -Name AIFrameEngineCompositor -ErrorAction SilentlyContinue
if ($existing) {
    throw 'Close the existing compositor with Ctrl+Shift+F12 before launching another.'
}

# Preserve previous env values so we can restore them
$previous = @{}
$diagnostic = $null
$vars = @('AIFRAME_RIFE_FP16', 'AIFRAME_DISPLAY_CACHE', 'AIFRAME_LEGACY_RIFE', 'AIFRAME_CPU_PREPROCESS', 'AIFRAME_FSR1', 'AIFRAME_TARGET_HWND', 'AIFRAME_TARGET_PID', 'AIFRAME_TARGET_CREATED', 'AIFRAME_PRESENT_TRACE', 'AIFRAME_REPEAT_COPY_PROBE', 'AIFRAME_WORKER_CPU_PROBE')
$vars += 'AIFRAME_RIFE_ATTEMPT_TRACE'
if ($TraceRifeAttempts) { $vars += 'AIFRAME_FG_TRACE' }

try {
    foreach ($v in $vars) {
        $previous[$v] = [Environment]::GetEnvironmentVariable($v, 'Process')
    }

    # Apply validated configuration
    [Environment]::SetEnvironmentVariable('AIFRAME_RIFE_ATTEMPT_TRACE', $null, 'Process')
    if ($TraceRifeAttempts) {
        # New-DiagnosticSession exclusively creates this directory before launch.
        $bundlePath = [IO.Path]::GetFullPath($DiagnosticDirectory)
        [Environment]::SetEnvironmentVariable('AIFRAME_RIFE_ATTEMPT_TRACE', (Join-Path $bundlePath 'attempts.raw.csv'), 'Process')
        [Environment]::SetEnvironmentVariable('AIFRAME_FG_TRACE', (Join-Path $bundlePath 'fg.raw.csv'), 'Process')
    }
    [Environment]::SetEnvironmentVariable('AIFRAME_PRESENT_TRACE', $null, 'Process')
    if ($TracePresentations) {
        $traceDir = Join-Path $root 'scratch\presentation_traces'
        New-Item -ItemType Directory -Force -Path $traceDir | Out-Null
        $trace = Join-Path $traceDir ('identities-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff') + '.csv')
        [Environment]::SetEnvironmentVariable('AIFRAME_PRESENT_TRACE', $trace, 'Process')
        Write-Host "Identity CSV (close compositor cleanly to flush): $trace"
    }
    # Clear inherited selections for the backward-compatible default Witcher path.
    foreach ($v in @('AIFRAME_TARGET_HWND', 'AIFRAME_TARGET_PID', 'AIFRAME_TARGET_CREATED')) {
        [Environment]::SetEnvironmentVariable($v, $null, 'Process')
    }
    if ($selected) {
        [Environment]::SetEnvironmentVariable('AIFRAME_TARGET_HWND', [string]$selected.Handle, 'Process')
        [Environment]::SetEnvironmentVariable('AIFRAME_TARGET_PID', [string]$selected.Id, 'Process')
        [Environment]::SetEnvironmentVariable('AIFRAME_TARGET_CREATED', [string]$selected.Created, 'Process')
    }
    [Environment]::SetEnvironmentVariable('AIFRAME_RIFE_FP16', $(if ($NoFp16) { '0' } else { '1' }), 'Process')
    [Environment]::SetEnvironmentVariable('AIFRAME_DISPLAY_CACHE', $(if ($NoCache) { '0' } else { '1' }), 'Process')
    [Environment]::SetEnvironmentVariable('AIFRAME_REPEAT_COPY_PROBE', [string]$RepeatCopyProbe, 'Process')
    [Environment]::SetEnvironmentVariable('AIFRAME_WORKER_CPU_PROBE', $(if ($WorkerCpuProbe) { '1' } else { '0' }), 'Process')
    [Environment]::SetEnvironmentVariable('AIFRAME_LEGACY_RIFE', '', 'Process')
    [Environment]::SetEnvironmentVariable('AIFRAME_CPU_PREPROCESS', '', 'Process')
    [Environment]::SetEnvironmentVariable('AIFRAME_FSR1', $(if ($Fsr1) { '1' } else { '0' }), 'Process')

    Write-Host "Starting $cfg build with:"
    Write-Host "  FP16 inference:       " -NoNewline; Write-Host $(if (-not $NoFp16) { 'ON' } else { 'OFF' }) -ForegroundColor Cyan
    Write-Host "  Display caching:      " -NoNewline; Write-Host $(if (-not $NoCache) { 'ON' } else { 'OFF' }) -ForegroundColor Cyan
    Write-Host "  Full-res reconstruction: ON (default)"
    Write-Host "  Correct crop:         ON (default)"
    Write-Host "  Upscaler:             $(if ($Fsr1) { 'FSR1 EASU + RCAS (experimental)' } else { 'legacy bicubic + sharpen' })"
    Write-Host "  Presentation buffer:  adaptive, smoothly adjusted (66.7 ms ceiling)"
    Write-Host "  Repeat-copy probe:    $RepeatCopyProbe extra copies (0 = OFF; diagnostic load, not a fix)"
    Write-Host "  Worker CPU probe:     $([bool]$WorkerCpuProbe) (calling-thread accounting, not GPU/wait time)"
    Write-Host ""

    if ($DiagnosticDirectory) {
        . (Join-Path $root 'scratch\diagnostic_session.ps1')
        $options = [ordered]@{ configuration = $cfg; no_fp16 = [bool]$NoFp16; no_cache = [bool]$NoCache
            fsr1 = [bool]$Fsr1; trace_presentations = [bool]$TracePresentations
            worker_cpu_probe = [bool]$WorkerCpuProbe; repeat_copy_probe = $RepeatCopyProbe
            trace_rife_attempts = [bool]$TraceRifeAttempts
            selected_target = $selected; executable_arguments = @(); working_directory = (Split-Path $exe) }
        $diagnostic = New-DiagnosticSession -Root $root -Executable $exe -Directory $DiagnosticDirectory `
            -LogPath 'C:\Temp\aiframe_compositor.log' -Label $TrialLabel -Options $options
        Write-Host "Diagnostic evidence: $($diagnostic.directory)"
        Write-Host 'This shell will wait for a normal compositor exit; it will not terminate it.'
        try {
            $child = Start-Process -FilePath $exe -WorkingDirectory (Split-Path $exe) -PassThru -ErrorAction Stop
            Register-DiagnosticProcess $diagnostic $child
            Write-Host 'Compositor launched. F9 enables FG; Ctrl+Shift+F12 exits cleanly. Keep this shell open.'
            while (-not $child.WaitForExit(250)) { } # Only the returned process; never kill or wait by reused PID.
            Complete-DiagnosticSession $diagnostic $child
            Write-Host "Session closed. Integrity checks passed: $($diagnostic.integrity_checks_passed). Analysis still required."
        } catch {
            Write-DiagnosticJson (Join-Path $diagnostic.directory 'launch_or_finalize_failure.json') @{
                error = $_.Exception.Message; observed_utc = [DateTime]::UtcNow.ToString('o')
                process_may_still_be_running = $true; execution_visibility_verified = $false }
            throw
        }
    } else {
        Start-Process -FilePath $exe -WorkingDirectory (Split-Path $exe)
    }

    if (-not $DiagnosticDirectory) { Write-Host "Compositor started. Press F9 to enable frame generation." }
    Write-Host "Sharpen: F11 presets; Ctrl+Shift+Up/Down adjusts by 0.05. Ctrl+Shift+F6 shows status."
    Write-Host "Log: C:\Temp\aiframe_compositor.log"
}
finally {
    foreach ($v in $vars) {
        [Environment]::SetEnvironmentVariable($v, $previous[$v], 'Process')
    }
}
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
.NOTES
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
    [switch]$SelectWindow,
    [switch]$ListWindows,
    [int]$TargetProcessId = 0
)

$root = $PSScriptRoot
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
$vars = @('AIFRAME_RIFE_FP16', 'AIFRAME_DISPLAY_CACHE', 'AIFRAME_LEGACY_RIFE', 'AIFRAME_CPU_PREPROCESS', 'AIFRAME_FSR1', 'AIFRAME_TARGET_HWND', 'AIFRAME_TARGET_PID', 'AIFRAME_TARGET_CREATED', 'AIFRAME_PRESENT_TRACE')

try {
    foreach ($v in $vars) {
        $previous[$v] = [Environment]::GetEnvironmentVariable($v, 'Process')
    }

    # Apply validated configuration
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
    Write-Host ""

    Start-Process -FilePath $exe -WorkingDirectory (Split-Path $exe)

    Write-Host "Compositor started. Press F9 to enable frame generation."
    Write-Host "Sharpen: F11 presets; Ctrl+Shift+Up/Down adjusts by 0.05. Ctrl+Shift+F6 shows status."
    Write-Host "Log: C:\Temp\aiframe_compositor.log"
}
finally {
    foreach ($v in $vars) {
        [Environment]::SetEnvironmentVariable($v, $previous[$v], 'Process')
    }
}
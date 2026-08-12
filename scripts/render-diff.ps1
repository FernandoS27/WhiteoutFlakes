# Draw-path regression runner — gates G1 (draw trace) and G2 (golden image)
# from REFACTOR_PLAN.md §2. Mirrors particle-diff.ps1: same corpus file, same
# progressive-baseline model, same -Record / -Check shape.
#
# Workflow for each phase of the refactor:
#   .\scripts\render-diff.ps1 -Record        # from the known-good build
#   ... make the phase's changes, rebuild ...
#   .\scripts\render-diff.ps1 -Check         # must report ALL MATCH
#
# Baselines compare against the *immediately preceding* state, not a frozen
# ancient one — every phase is accountable for its own delta and nothing else.
# They are build artifacts; regenerate rather than commit them.
#
# -Perturb is GATE P0's determinism arm and the reason this script is not just
# "record twice". An unseeded rand() and a fixed unordered_map insertion
# sequence are both deterministic *within a process*, so a repeat recording
# agrees with the bug live. Seeding the first actor handle differently moves
# every actor's hash bucket, which is what actually exposes a draw path that
# follows container iteration order:
#
#   .\scripts\render-diff.ps1 -Record
#   .\scripts\render-diff.ps1 -Check -Perturb 5000     # must also be ALL MATCH
#
# Neither gate runs in CI: the corpus is extracted retail data that is not in
# the repo, and every hook sits inside the device path. G0 (ctest) is the only
# CI gate; these are developer-machine gates keyed to a documented corpus root.

[CmdletBinding()]
param(
    [switch]$Record,
    [switch]$Check,
    # Also record / compare the 512x512 offscreen readback. Off by default
    # because ReadbackTexture has no Metal implementation, so a macOS developer
    # has G1 but not G2.
    [switch]$Golden,
    # SD is the default mode; -Hd records the HD profile's baselines instead.
    # A full gate run does both — they are different draw paths.
    [switch]$Hd,
    [int]$Frames = 30,
    [int]$Perturb = 0,
    [int]$CameraDistance = 350,
    # Escape hatch for a phase that legitimately perturbs sqDist. Using it must
    # be justified in the commit message, and CompareTraces refuses to combine
    # it with the exact constant-buffer hash.
    [double]$DistanceTolerance = 0,
    [string]$CorpusRoot = 'C:/Projects/WhiteoutLib/Corpus/MDL',
    [string]$CorpusFile = "$PSScriptRoot/../tools/particle_diff/corpus.txt",
    [string]$BaselineDir = "$PSScriptRoot/../build/render-baselines",
    [string]$Exe = "$PSScriptRoot/../build/standalone/RelWithDebInfo/WhiteoutFlakes.exe"
)

if (-not $Record -and -not $Check) {
    Write-Error 'Pass -Record or -Check.'
    exit 2
}
if (-not (Test-Path $Exe)) {
    Write-Error "Viewer not built: $Exe"
    exit 2
}
if (-not (Test-Path $BaselineDir)) {
    New-Item -ItemType Directory -Force $BaselineDir | Out-Null
}

$mode = if ($Hd) { 'hd' } else { 'sd' }

$entries = Get-Content $CorpusFile |
    ForEach-Object { ($_ -split '#')[0].Trim() } |
    Where-Object { $_ -ne '' }

$pass = 0
$fail = 0
$failed = @()

foreach ($rel in $entries) {
    $model = Join-Path $CorpusRoot $rel
    if (-not (Test-Path $model)) {
        Write-Host "SKIP (missing): $rel" -ForegroundColor DarkYellow
        continue
    }

    # Flatten the relative path into a single baseline filename.
    $key = (($rel -replace '[\\/ ]', '_') -replace '\.mdx$', '') + "_$mode"
    $trace = Join-Path $BaselineDir "$key.txt"
    $image = Join-Path $BaselineDir "$key.raw"

    $argv = @('--draw-trace', $model, '--trace-frames', $Frames,
              '--draw-trace-camera-distance', $CameraDistance)
    if ($Hd) { $argv += '--draw-trace-hd' }
    if ($Perturb -gt 0) { $argv += @('--draw-trace-perturb', $Perturb) }
    if ($Golden) { $argv += @('--draw-trace-golden', $image) }

    if ($Record) {
        $argv += @('--draw-trace-record', $trace)
    } else {
        if (-not (Test-Path $trace)) {
            Write-Host "SKIP (no baseline): $rel" -ForegroundColor DarkYellow
            continue
        }
        $argv += @('--draw-trace-check', $trace)
        if ($DistanceTolerance -gt 0) {
            $argv += @('--draw-trace-distance-tol', $DistanceTolerance)
        }
    }

    $out = & $Exe @argv 2>&1 | Select-String -Pattern '^\[dtrace\]'
    if ($LASTEXITCODE -eq 0) {
        $pass++
        Write-Host "PASS  $rel" -ForegroundColor Green
    } else {
        $fail++
        $failed += $rel
        Write-Host "FAIL  $rel" -ForegroundColor Red
        $out | ForEach-Object { Write-Host "      $_" -ForegroundColor Red }
    }
}

Write-Host ''
if ($Record) {
    Write-Host "Recorded $pass $mode baseline(s) into $BaselineDir"
} else {
    $arm = if ($Perturb -gt 0) { " (perturb $Perturb)" } else { '' }
    Write-Host "ALL MATCH$($arm): $pass passed, $fail failed"
}
if ($fail -gt 0) {
    Write-Host 'Failing models:'
    $failed | ForEach-Object { Write-Host "  $_" }
    exit 1
}
exit 0

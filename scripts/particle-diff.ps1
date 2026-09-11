# Particle refactor validation runner — see PARTICLE_TYPES_DESIGN.md.
#
# Workflow for each step of the refactor:
#   .\scripts\particle-diff.ps1 -Record         # from the known-good build
#   ... make the step's changes, rebuild ...
#   .\scripts\particle-diff.ps1 -Check          # must report ALL MATCH
#
# Baselines compare against the *immediately preceding* state, not a frozen
# ancient one, which is what makes the refactor progressive: every step is
# accountable for its own delta and nothing else. They are build artifacts —
# regenerate rather than commit them.
#
# -CurveTolerance switches to step 5's declared budget (colour within 1/255,
# scale within 1e-5) instead of demanding a bit-identical match.

[CmdletBinding()]
param(
    [switch]$Record,
    [switch]$Check,
    [switch]$CurveTolerance,
    [int]$Frames = 60,
    [string]$CorpusRoot = 'C:/Projects/WhiteoutLib/Corpus/MDL',
    [string]$CorpusFile = "$PSScriptRoot/../tools/particle_diff/corpus.txt",
    [string]$BaselineDir = "$PSScriptRoot/../build/particle-baselines",
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

$entries = Get-Content $CorpusFile |
    ForEach-Object { ($_ -split '#')[0].Trim() } |
    Where-Object { $_ -ne '' }

$pass = 0
$fail = 0
$skip = 0
$failed = @()
$skipped = @()

foreach ($rel in $entries) {
    $model = Join-Path $CorpusRoot $rel
    if (-not (Test-Path $model)) {
        $skip++
        $skipped += "$rel  (missing)"
        Write-Host "SKIP (missing): $rel" -ForegroundColor DarkYellow
        continue
    }

    # Flatten the relative path into a single baseline filename.
    $key = ($rel -replace '[\\/ ]', '_') -replace '\.mdx$', ''
    $trace = Join-Path $BaselineDir "$key.txt"

    $argv = @('--particle-diff', $model, '--trace-frames', $Frames)
    if ($Record) { $argv += @('--trace-record', $trace) }
    if ($Check) {
        if (-not (Test-Path $trace)) {
            $skip++
            $skipped += "$rel  (no baseline)"
            Write-Host "SKIP (no baseline): $rel" -ForegroundColor DarkYellow
            continue
        }
        $argv += @('--trace-check', $trace)
        if ($CurveTolerance) { $argv += '--trace-curve-tol' }
    }

    $out = & $Exe @argv 2>&1 | Select-String -Pattern '^\[ptrace\]'
    $ok = ($LASTEXITCODE -eq 0)
    if ($ok) {
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
    Write-Host "Recorded $pass baseline(s) into $BaselineDir"
} else {
    Write-Host "ALL MATCH: $pass passed, $fail failed, $skip skipped"
}
# A skipped entry is a line of this corpus that did not run. Left unsaid, it
# reads as coverage: two `|seq=` lines sat here unresolvable (Join-Path cannot
# make a path out of `foo.mdx|seq=Spell`) and the summary still said ALL MATCH.
# They are duplicates of the bare lines above them, so nothing was lost — but
# nothing said so either.
if ($skip -gt 0) {
    Write-Host 'Skipped entries (these did NOT run):' -ForegroundColor DarkYellow
    $skipped | ForEach-Object { Write-Host "  $_" -ForegroundColor DarkYellow }
}
if ($fail -gt 0) {
    Write-Host 'Failing models:'
    $failed | ForEach-Object { Write-Host "  $_" }
    exit 1
}
exit 0

# Three-way Warcraft III -> StarCraft II gate (WC3_TO_SC2_COMPLETION_PLAN.md §3, L2).
#
# For every corpus line: export the classic `.mdx` with `--export-m3`, then
# render (a) the source natively as Warcraft III, (b) our `.m3` as StarCraft II
# and (c) Blizzard's own conversion of the same model, from the same camera.
# `scripts/war3_diff_report.py` tables the draw counts and the pixel distance of
# every pair and lays the three frames side by side. With -ParticleStats it
# also dumps `--particle-diff --trace-dump` for the native model and the export
# at several frame counts -- per emitter position, vertex count, bounds and
# mean colour -- because two games' random streams never land the same
# particles and a pixel diff of an emitter is noise (§3).
#
#   .\scripts\war3-diff.ps1 -Out c0
#   .\scripts\war3-diff.ps1 -Out c4 -Yaws 0,0.785,1.571,2.356,3.142,3.927,4.712,5.498
#   .\scripts\war3-diff.ps1 -Out c5 -ParticleStats -Frames 120
#
# (a)<->(b) is fidelity to the source, (b)<->(c) agreement with Blizzard, and
# (a)<->(c) how far Blizzard themselves drifted -- they re-exported from the
# original scenes (§A1), so (c) is a direction check, not a target.
#
# Headless throughout: every run is a `--draw-trace`, `--particle-diff` or a
# hidden-window export. Run through the PowerShell tool with `&`, never through
# a nested shell (`$PSScriptRoot` comes up empty there).

[CmdletBinding()]
param(
    [string]$Out = 'run',
    [string]$CorpusFile = "$PSScriptRoot/../tools/particle_diff/corpus_war3_m3.txt",
    [int]$Frames = 60,
    [int]$CameraDistance = 350,
    # Camera yaws in radians. Empty renders the one default view (0.7); a list
    # renders each and names the files `_y<index>`.
    [double[]]$Yaws = @(),
    [switch]$ParticleStats,
    [int[]]$StatFrames = @(30, 60, 120),
    [switch]$NoOracle,
    # Reuse the export already in the output folder rather than exporting again.
    [switch]$NoExport,
    [string[]]$ExportArgs = @(),
    [string]$Only = '',
    [string]$OracleRoot = 'C:/Projects/WhiteoutLib/Corpus/Sc2M3',
    [string]$OutRoot = "$PSScriptRoot/../build/war3-diff",
    [string]$Exe = "$PSScriptRoot/../build/standalone/RelWithDebInfo/WhiteoutFlakes.exe",
    [string]$Python = 'python'
)

if (-not (Test-Path $Exe)) {
    Write-Error "Viewer not built: $Exe"
    exit 2
}
$runDir = Join-Path $OutRoot $Out
New-Item -ItemType Directory -Force $runDir | Out-Null

# `<wc3 path>|War3_<Stem>.m3[|seq=<wc3 sequence>][|seqm3=<exported sequence>]`
$entries = Get-Content $CorpusFile |
    ForEach-Object { ($_ -split '#')[0].Trim() } |
    Where-Object { $_ -ne '' } |
    ForEach-Object {
        $parts = $_ -split '\|'
        $e = [ordered]@{ Path = $parts[0].Trim(); Oracle = ''; Seq = ''; SeqM3 = '' }
        for ($t = 1; $t -lt $parts.Count; $t++) {
            $p = $parts[$t].Trim()
            if ($p -match '^seq=(?<v>.+)$') { $e.Seq = $Matches.v }
            elseif ($p -match '^seqm3=(?<v>.+)$') { $e.SeqM3 = $Matches.v }
            elseif ($p -ne '') { $e.Oracle = $p }
        }
        [pscustomobject]$e
    }

# One entry per view; '' is the default camera. (A lone $null in an array
# collapses to nothing in PowerShell, which renders no view at all.)
$views = @('')
if ($Yaws.Count -gt 0) { $views = @($Yaws | ForEach-Object { [string]$_ }) }
$counts = [ordered]@{ exported = 0; exportFailed = 0; rendered = 0; renderFailed = 0; noOracle = 0; skipped = 0 }

function Invoke-Trace([string]$model, [string]$game, [string]$base, $yaw, [string]$seq) {
    $argv = @('--draw-trace', $model, '--draw-trace-game', $game, '--trace-frames', $Frames,
              '--draw-trace-camera-distance', $CameraDistance,
              '--draw-trace-record', "$base.txt", '--draw-trace-golden', "$base.raw")
    if ($yaw -ne '') { $argv += @('--draw-trace-camera-yaw', $yaw) }
    if ($seq) { $argv += @('--draw-trace-anim', $seq) }
    $text = & $Exe @argv *>&1 | Out-String
    $text | Out-File "$base.log" -Encoding utf8
    return ($LASTEXITCODE -eq 0 -and (Test-Path "$base.raw"))
}

foreach ($entry in $entries) {
    $stem = [IO.Path]::GetFileNameWithoutExtension($entry.Path)
    if ($Only -and $stem -notlike $Only) { $counts.skipped++; continue }
    $dir = Join-Path $runDir $stem
    New-Item -ItemType Directory -Force $dir | Out-Null
    $m3 = Join-Path $dir "$stem.m3"

    if (-not $NoExport) {
        if (Test-Path $m3) { Remove-Item $m3 -Force }
        $argv = @($entry.Path, '--export-m3', $m3) + $ExportArgs
        & $Exe @argv *>&1 | Out-File (Join-Path $dir 'export.log') -Encoding utf8
    }
    if (Test-Path $m3) { $counts.exported++ } else {
        $counts.exportFailed++
        Write-Host "EXPORT FAILED  $($entry.Path)" -ForegroundColor Red
    }

    for ($v = 0; $v -lt $views.Count; $v++) {
        $yaw = $views[$v]
        $suffix = if ($yaw -ne '') { "_y$v" } else { '' }
        $ok = Invoke-Trace $entry.Path 'wc3' (Join-Path $dir "native$suffix") $yaw $entry.Seq
        # `seqm3` names the sequence in both StarCraft II files: the export
        # renames to the StarCraft II convention Blizzard's conversions follow.
        $seqM3 = if ($entry.SeqM3) { $entry.SeqM3 } else { '' }
        if (Test-Path $m3) {
            $ok = (Invoke-Trace $m3 'sc2' (Join-Path $dir "ours$suffix") $yaw $seqM3) -and $ok
        }
        if (-not $NoOracle) {
            if ($entry.Oracle -and (Test-Path (Join-Path $OracleRoot $entry.Oracle))) {
                $ok = (Invoke-Trace (Join-Path $OracleRoot $entry.Oracle) 'sc2' (Join-Path $dir "oracle$suffix") $yaw $seqM3) -and $ok
            } else {
                $counts.noOracle++
            }
        }
        if ($ok) { $counts.rendered++ } else {
            $counts.renderFailed++
            Write-Host "RENDER FAILED  $stem$suffix" -ForegroundColor Red
        }
    }

    if ($ParticleStats) {
        foreach ($f in $StatFrames) {
            & $Exe --particle-diff $entry.Path --trace-dump --trace-frames $f *>&1 |
                Out-File (Join-Path $dir "pstats_native_$f.txt") -Encoding utf8
            if (Test-Path $m3) {
                & $Exe --particle-diff $m3 --trace-dump --trace-frames $f *>&1 |
                    Out-File (Join-Path $dir "pstats_ours_$f.txt") -Encoding utf8
            }
        }
    }
    Write-Host "done  $stem"
}

& $Python "$PSScriptRoot/war3_diff_report.py" $runDir
$summary = ($counts.GetEnumerator() | ForEach-Object { "$($_.Key) $($_.Value)" }) -join ', '
Write-Host "war3-diff $Out`: $summary"
Write-Host "report: $(Join-Path $runDir 'report.md')"
if ($counts.exportFailed -gt 0 -or $counts.renderFailed -gt 0) { exit 1 }
exit 0

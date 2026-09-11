<#
.SYNOPSIS
    SC2 particle draw coverage: does every PAR_ carrier actually reach the screen?

.DESCRIPTION
    SC2_PARTICLE_PLAN.md X7. Runs `--draw-trace` over the -M3Particle corpus and
    the -M3 arm's particle carriers and reads the `sc2par=` count off the
    `[dtrace]` summary line: the draws the M3 producer shaded as SC2 particles.
    Zero means a carrier proved nothing in the scenario its corpus line names,
    however stable a golden recorded from it would be.

    Records nothing and compares against nothing, so it runs on any build and
    moves no baseline. Exits 1 when any carrier draws no SC2 particle, when a
    named sequence does not resolve, or when the viewer fails.

    Corpus lines are the -M3Particle arm's grammar: a path, then whitespace
    separated `key=value` tokens. `seq=` takes a sequence index or a name with
    `+` for its spaces; `globals=off` drops the global loops.
#>
param(
    [string]$Exe = "$PSScriptRoot/../build/standalone/RelWithDebInfo/WhiteoutFlakes.exe",
    [string]$CorpusRoot = 'C:/Projects/WhiteoutLib/Corpus',
    [string[]]$CorpusFile = @("$PSScriptRoot/../tools/particle_diff/corpus_m3_particle.txt"),
    # The -M3 arm's models that carry a PAR_ (the X2 survey), so the sweep
    # covers the standing arm's carriers too. Thor and the Ultralisk squirt only
    # on a sequence the default pose never plays, so they name the one that
    # fires them.
    [string[]]$Extra = @('Sc2M3/Zealot.m3', 'Sc2M3/SCV.m3', 'Sc2M3/Thor.m3 seq=Spell',
                         'Sc2M3/Ultralisk.m3 seq=Spell+A', 'Sc2M3/BattleCruiser.m3',
                         'HotSM3/Storm_Hero_Brightwing_Base.m3'),
    # The -M3Particle arm's capture length, so the two agree about what fires.
    [int]$Frames = 120,
    # Where each run's trace lands. A draw trace refuses to run without a
    # record or check target, and this one is thrown away.
    [string]$WorkDir = (Join-Path ([IO.Path]::GetTempPath()) 'sc2-particle-coverage')
)
if (-not (Test-Path $WorkDir)) {
    New-Item -ItemType Directory -Force $WorkDir | Out-Null
}

if (-not (Test-Path $Exe)) {
    Write-Error "Viewer not built: $Exe"
    exit 2
}

$lines = @()
foreach ($f in $CorpusFile) {
    if (-not (Test-Path $f)) {
        Write-Error "Corpus file not found: $f"
        exit 2
    }
    $lines += Get-Content $f | ForEach-Object { ($_ -split '#')[0].Trim() } | Where-Object { $_ -ne '' }
}
$lines += $Extra

$rows = @()
$bad = 0
foreach ($line in $lines) {
    $tok = $line -split '\s+'
    $rel = $tok[0]
    $seq = ''
    $noGlobals = $false
    for ($t = 1; $t -lt $tok.Count; $t++) {
        if ($tok[$t] -notmatch '^(?<k>[a-z]+)=(?<v>.+)$') {
            Write-Error "Malformed scenario token '$($tok[$t])' in: $line"
            exit 2
        }
        switch ($Matches.k) {
            'seq'     { $seq = $Matches.v }
            'globals' { $noGlobals = ($Matches.v -eq 'off') }
            default   { Write-Error "Unknown scenario key '$($Matches.k)' in: $line"; exit 2 }
        }
    }
    $model = Join-Path $CorpusRoot $rel
    if (-not (Test-Path $model)) {
        Write-Host "SKIP (missing): $rel" -ForegroundColor DarkYellow
        continue
    }

    # `--draw-trace` always leads: the list and scenario flags are modifiers of
    # it, and without it the viewer opens its window. Late assets are allowed
    # because this counts draws and records no trace a timing could spoil.
    $scratch = Join-Path $WorkDir ((($rel -replace '[\\/ ]', '_') -replace '\.m3$', '') + '.txt')
    $argv = @('--draw-trace', $model, '--trace-frames', $Frames, '--draw-trace-allow-late-assets',
              '--draw-trace-record', $scratch)
    if ($seq) { $argv += @('--draw-trace-anim', $seq) }
    if ($noGlobals) { $argv += '--draw-trace-anim-no-globals' }

    $out = & $Exe @argv 2>&1 | ForEach-Object { "$_" } | Where-Object { $_ -like '`[dtrace`]*' }
    $code = $LASTEXITCODE
    $sc2par = -1
    $draws = -1
    $summary = $out | Where-Object { $_ -match 'sc2par=(\d+)' } | Select-Object -Last 1
    if ($summary -match '\] (\d+) draw\(s\).*sc2par=(\d+)') {
        $draws = [int]$Matches[1]
        $sc2par = [int]$Matches[2]
    }
    $unresolved = [bool]($out | Where-Object { $_ -match 'no sequence matching' })

    $scenario = @($(if ($seq) { "seq=$seq" }), $(if ($noGlobals) { 'globals=off' })) -join ' '
    $status = if ($code -ne 0 -or $sc2par -lt 0) { 'FAIL' }
              elseif ($unresolved) { 'NOSEQ' }
              elseif ($sc2par -eq 0) { 'ZERO' }
              else { 'OK' }
    if ($status -ne 'OK') { $bad++ }
    $rows += [pscustomobject]@{ Status = $status; Sc2Par = $sc2par; Draws = $draws; Carrier = $rel; Scenario = $scenario }
    $colour = if ($status -eq 'OK') { 'Green' } else { 'Red' }
    Write-Host ("{0,-5} sc2par={1,-6} draws={2,-6} {3} {4}" -f $status, $sc2par, $draws, $rel, $scenario) -ForegroundColor $colour
}

Write-Host ''
$ok = ($rows | Where-Object { $_.Status -eq 'OK' }).Count
Write-Host "$ok of $($rows.Count) carrier(s) drew SC2 particles over $Frames frames"
if ($bad -gt 0) {
    Write-Host 'Carriers that proved nothing:'
    $rows | Where-Object { $_.Status -ne 'OK' } | ForEach-Object { Write-Host "  $($_.Status)  $($_.Carrier) $($_.Scenario)" }
    exit 1
}
exit 0

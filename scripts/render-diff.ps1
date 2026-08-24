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
    # Multi-model arm: route every odd geoset through UnlitShading so one
    # frame contains draws from two shading models. Its baselines are separate
    # (`_unlit` suffix) because the toggle changes what the frame draws — the
    # byte-identical gate is the arm WITHOUT this.
    [switch]$Unlit,

    # The `.m2` arm: a different corpus, a different root, and its own
    # baselines. Needs a build configured with -DWDX_ENABLE_M2=ON — without it
    # the viewer will not load an `.m2` at all and every model reports a miss.
    [switch]$M2,

    # `.m2` streaming arm: parse without the `.anim` siblings and read one the
    # first time a sequence plays. Deliberately shares the -M2 baselines rather
    # than recording its own — the claim being gated is that deferring the reads
    # leaves the frame byte-identical, so:
    #
    #   .\scripts\render-diff.ps1 -Record -M2 -Golden
    #   .\scripts\render-diff.ps1 -Check  -M2 -Golden -LazyAnim   # must ALL MATCH
    [switch]$LazyAnim,

    # `.m2` client-database arm. A creature model leaves its skin blank for the
    # game to fill (see WowReplaceableTextures); a bare -M2 run fills it from
    # the model's `.blp` siblings, and this arm fills it from CreatureDisplayInfo
    # instead. That needs two things the bare run has not got: a listfile,
    # because the tables key on fileDataID and a corpus is path-addressed, and a
    # disk root, because `dbfilesclient/` sits above the models rather than
    # beside them. The two routes can pick different files, so this records its
    # own baselines (`m2_skins`) rather than sharing.
    [string]$Listfile,

    # The `.m3` arm. Same shape as -M2, and needs -DWDX_ENABLE_M3=ON. Its
    # golden is *tonemapped* white rather than #FFFFFF: Sc2HeroesProfile reuses
    # the HD frame, and a linear 1.0 through the tonemap and bloom chain does
    # not land back at 1.0.
    [switch]$M3,

    # GATE G5 — the `.m3` *animation* arm (M3_ANIMATION_PLAN.md §2.2). Same
    # corpus root as -M3 and a separate corpus file whose lines carry a scripted
    # scenario per model, because "spawn and let it run" cannot reach the parts
    # of M3 playback that only exist once something asks for a second sequence.
    #
    # Its baselines are PROGRESSIVE by design and this is the one arm where that
    # is true: the wc3/wow baselines assert "nothing moved", and each animation
    # phase legitimately changes what M3 draws, so re-recording is the expected
    # outcome of a phase rather than an admission. Which phase last re-recorded
    # is what the commit message has to say.
    [switch]$M3Anim,

    # GATE G6 — the `.m3` *material* arm (M3_SIMPLE_MATERIAL_DESIGN.md §7).
    # Same corpus root as -M3, its own corpus file and baselines (`sc2mat`).
    # Like -M3Anim these baselines are PROGRESSIVE: each material phase
    # legitimately changes what M3 draws, and the vacuity anchor is the -M3
    # arm's unlit golden — a phase whose sc2mat golden still matches the unlit
    # one proved nothing.
    [switch]$Sc2Mat,

    # The deferred-light sub-arm of -Sc2Mat: a scripted debug point light at a
    # fixed offset (the values live in test_main, not here, so every model
    # gets the identical light). Its own baselines (`sc2mat_lit`) — the claim
    # is that it differs from the light-off run.
    [switch]$DebugLight,

    # The solver arm of G5: same corpus and scenarios, but with the pose stages
    # on and a ground plane installed, so terrain IK and the turret are visible
    # to a golden at all. Its own baselines (`m3ik`) — the whole point is that
    # it differs from the solvers-off run, which is what the plain -M3Anim arm
    # keeps asserting stays put.
    [switch]$Solvers,
    # Ground plane height for -Solvers, in model units. The default of 0 puts it
    # at the scene origin; a model standing on it already needs no correction,
    # so a non-zero value is what makes the solve do work.
    [double]$GroundZ = 0,
    # Aim target for turrets, model space, as three components.
    [double[]]$Aim,

    # GATE G7 — the Diablo III arm. Same shape as -M3: its own corpus root and
    # corpus file, and its own baselines (`d3`). The corpus is the extracted
    # `.app` tree, not the install, so this needs no CASC.
    #
    # Its baselines are PROGRESSIVE, for -M3Anim's reason: every D3 phase
    # legitimately changes what D3 draws, so re-recording is the expected
    # outcome of a phase rather than an admission — and which phase last
    # re-recorded is what the commit message has to say. The wc3/wow/sc2
    # baselines are NOT progressive and are never re-recorded here.
    #
    # A green -D3 run is not enough on its own: if two different actors'
    # goldens come out byte-identical, the arm is proving nothing. That is the
    # trap that made the first -M3Anim recording vacuous.
    [switch]$D3,

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
# The perturbation arm reseeds actor ids, and particle::MixSeed mixes the actor
# id into every emitter's RNG stream. So a perturbed run draws *the same
# decisions* (the trace matches exactly) from *different particle positions*
# — the golden image legitimately differs on every particle-carrying model,
# 11 of the 17 in the corpus. Combining the two reports a total failure that
# means nothing, with `MATCH: identical` printed directly above each one.
#
# -Perturb answers "does the draw path depend on hash order". -Golden answers
# "did pixels move". Run them separately.
if ($Perturb -gt 0 -and $Golden) {
    Write-Error ('-Perturb and -Golden are not compatible: reseeding actor ids ' +
                 'changes the particle RNG stream, so the image differs by design ' +
                 'while the trace stays identical. Run them as separate arms.')
    exit 2
}

# -LazyAnim is a check-only arm by construction: it is compared against the
# baselines the eager run recorded, so recording with it on would compare the
# streaming path against itself and prove nothing.
if ($LazyAnim -and $Record) {
    Write-Error ('-LazyAnim is a -Check arm: record the baselines with the eager parse, ' +
                 'then check them with -LazyAnim.')
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
if ($Unlit) { $mode += '_unlit' }
if ((@($M2, $M3, $M3Anim, $Sc2Mat, $D3) | Where-Object { $_ }).Count -gt 1) {
    Write-Error '-M2, -M3, -M3Anim, -Sc2Mat and -D3 are separate arms: pass one of them.'
    exit 2
}
if ($Solvers -and -not $M3Anim) {
    Write-Error '-Solvers is a -M3Anim arm: the pose stages only exist for `.m3`.'
    exit 2
}
if ($DebugLight -and -not $Sc2Mat) {
    Write-Error '-DebugLight is a -Sc2Mat arm: only the M3 material frame consumes it.'
    exit 2
}
if ($Listfile -and -not $M2) {
    Write-Error '-Listfile is a -M2 arm: no other product reads a fileDataID listfile.'
    exit 2
}
if ($Listfile -and -not (Test-Path $Listfile)) {
    Write-Error "Listfile not found: $Listfile"
    exit 2
}
if ($M2) {
    $mode = if ($Listfile) { 'm2_skins' } else { 'm2' }
    # Only defaulted when the caller did not name their own; an explicit
    # -CorpusRoot / -CorpusFile still wins.
    if (-not $PSBoundParameters.ContainsKey('CorpusRoot')) {
        $CorpusRoot = 'C:/Projects/WhiteoutLib/Corpus/WoW'
    }
    if (-not $PSBoundParameters.ContainsKey('CorpusFile')) {
        $CorpusFile = "$PSScriptRoot/../tools/particle_diff/corpus_m2.txt"
    }
}
if ($M3 -or $M3Anim) {
    $mode = if ($M3Anim) { if ($Solvers) { 'm3ik' } else { 'm3anim' } } else { 'm3' }
    if (-not $PSBoundParameters.ContainsKey('CorpusRoot')) {
        $CorpusRoot = 'C:/Projects/WhiteoutLib/Corpus'
    }
    if (-not $PSBoundParameters.ContainsKey('CorpusFile')) {
        $CorpusFile = if ($M3Anim) { "$PSScriptRoot/../tools/particle_diff/corpus_m3_anim.txt" }
                      else { "$PSScriptRoot/../tools/particle_diff/corpus_m3.txt" }
    }
    # A 30-frame capture at 60 Hz is half a second — long enough for a static
    # pose and far too short for a cross-fade to start and settle. The scenario
    # frames in the corpus file are written against this default.
    if ($M3Anim -and -not $PSBoundParameters.ContainsKey('Frames')) {
        $Frames = 120
    }
}
if ($D3) {
    $mode = 'd3'
    if (-not $PSBoundParameters.ContainsKey('CorpusRoot')) {
        $CorpusRoot = 'C:/Projects/WhiteoutLib/Corpus/D3'
    }
    if (-not $PSBoundParameters.ContainsKey('CorpusFile')) {
        $CorpusFile = "$PSScriptRoot/../tools/particle_diff/corpus_d3.txt"
    }
}
if ($Sc2Mat) {
    $mode = if ($DebugLight) { 'sc2mat_lit' } else { 'sc2mat' }
    if (-not $PSBoundParameters.ContainsKey('CorpusRoot')) {
        $CorpusRoot = 'C:/Projects/WhiteoutLib/Corpus'
    }
    if (-not $PSBoundParameters.ContainsKey('CorpusFile')) {
        $CorpusFile = "$PSScriptRoot/../tools/sc2_material_corpus.txt"
    }
}

# One entry per corpus line. The animation corpus adds whitespace-separated
# `key=value` scenario tokens after the path; every other corpus is a bare path
# per line and MUST stay that way — the WC3 corpus has directories with spaces
# in them ("HoTS Maiev High Priestess Upgraded/Maiev.mdx"), so tokenising those
# lines would split a path into a path plus garbage.
$entries = Get-Content $CorpusFile |
    ForEach-Object { ($_ -split '#')[0].Trim() } |
    Where-Object { $_ -ne '' } |
    ForEach-Object {
        $line = $_
        $e = [ordered]@{ Path = $line; Seq = ''; Switch = ''; Layer = ''; Blend = ''; Weight = '' }
        if ($M3Anim) {
            $tok = $line -split '\s+'
            $e.Path = $tok[0]
            for ($t = 1; $t -lt $tok.Count; $t++) {
                if ($tok[$t] -notmatch '^(?<k>[a-z]+)=(?<v>.+)$') {
                    Write-Error "Malformed scenario token '$($tok[$t])' in: $line"
                    exit 2
                }
                $k = $Matches.k; $v = $Matches.v
                switch ($k) {
                    'seq'    { $e.Seq = $v }
                    'switch' { $e.Switch = $v }
                    'layer'  { $e.Layer = $v }
                    'blend'  { $e.Blend = $v }
                    'weight' { $e.Weight = $v }
                    default  { Write-Error "Unknown scenario key '$k' in: $line"; exit 2 }
                }
            }
        }
        [pscustomobject]$e
    }

$pass = 0
$fail = 0
$failed = @()

foreach ($entry in $entries) {
    $rel = $entry.Path
    $model = Join-Path $CorpusRoot $rel
    if (-not (Test-Path $model)) {
        Write-Host "SKIP (missing): $rel" -ForegroundColor DarkYellow
        continue
    }

    # Flatten the relative path into a single baseline filename. The scenario's
    # start sequence joins the key so one model can appear in the corpus more
    # than once — different sequences of the same unit are different baselines,
    # not a collision.
    $key = (($rel -replace '[\\/ ]', '_') -replace '\.(mdx|m2|m3|app)$', '') + "_$mode"
    if ($entry.Seq) { $key += '_' + ($entry.Seq -replace '[^A-Za-z0-9]', '') }
    $trace = Join-Path $BaselineDir "$key.txt"
    $image = Join-Path $BaselineDir "$key.raw"

    $argv = @('--draw-trace', $model, '--trace-frames', $Frames,
              '--draw-trace-camera-distance', $CameraDistance)
    if ($entry.Seq)    { $argv += @('--draw-trace-anim', $entry.Seq) }
    if ($entry.Switch) {
        $s = $entry.Switch -split ':', 2
        $argv += @('--draw-trace-anim-switch', $s[0], $s[1])
    }
    if ($entry.Layer) {
        $l = $entry.Layer -split ':', 2
        $argv += @('--draw-trace-anim-layer', $l[0], $l[1])
    }
    if ($entry.Blend)  { $argv += @('--draw-trace-anim-blend', $entry.Blend) }
    if ($entry.Weight) { $argv += @('--draw-trace-anim-weight', $entry.Weight) }
    if ($Solvers) {
        $argv += @('--draw-trace-solvers', '--draw-trace-ground', $GroundZ)
        if ($Aim -and $Aim.Count -eq 3) {
            $argv += @('--draw-trace-aim', $Aim[0], $Aim[1], $Aim[2])
        }
    }
    if ($Hd) { $argv += '--draw-trace-hd' }
    if ($DebugLight) { $argv += '--draw-trace-debug-light' }
    if ($Unlit) { $argv += '--draw-trace-unlit' }
    if ($LazyAnim) { $argv += '--draw-trace-lazy-anim' }
    if ($Listfile) { $argv += @('--listfile', $Listfile, '--content-root', $CorpusRoot) }
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

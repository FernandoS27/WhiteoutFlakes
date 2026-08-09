# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Fernando Sahmkow
#
# Publish the Rust crates to crates.io, in dependency order.
#
# Order matters and cannot be parallelised: crates.io must have indexed
# each crate before the one that depends on it will resolve. The script
# waits between steps for that reason.
#
#   scripts\publish-rust.ps1 -DryRun     # package + verify, upload nothing
#   scripts\publish-rust.ps1             # for real
#
# Run scripts\vendor-rust.ps1 first — without the staged trees the
# published crates cannot build. -DryRun catches that, because
# `cargo package` runs a verification build.

param(
    [switch]$DryRun,
    [int]$IndexWaitSeconds = 45
)

$ErrorActionPreference = "Stop"

# True if this exact name+version is already on crates.io. Releases move one
# crate at a time — a shader pack only changes when the shaders do — so the
# ones that did not move are skipped rather than failing the run on "crate
# version already uploaded".
function Test-AlreadyPublished([string]$name, [string]$version) {
    $prefix = switch ($name.Length) {
        1 { "1" }
        2 { "2" }
        3 { "3/" + $name.Substring(0, 1) }
        default { $name.Substring(0, 2) + "/" + $name.Substring(2, 2) }
    }
    try {
        $body = Invoke-WebRequest -UseBasicParsing -TimeoutSec 20 `
            -Uri "https://index.crates.io/$prefix/$name"
    } catch {
        return $false   # unpublished crates 404 here; so does a network blip
    }
    foreach ($line in ($body.Content -split "`n")) {
        if ($line.Trim() -and (ConvertFrom-Json $line).vers -eq $version) { return $true }
    }
    return $false
}

$repoRoot = Split-Path -Parent $PSScriptRoot
$rustDir  = Join-Path $repoRoot "bindings/rust"

# Dependency order: shader packs, then sys, then the safe wrapper.
$order = @(
    "whiteoutflakes-shaders-d3d11",
    "whiteoutflakes-shaders-d3d12",
    "whiteoutflakes-shaders-vulkan",
    "whiteoutflakes-shaders-metal",
    "whiteoutflakes-shaders-webgpu",
    "whiteoutflakes-sys",
    "whiteoutflakes"
)

& (Join-Path $PSScriptRoot "vendor-rust.ps1") -Check
if ($LASTEXITCODE -ne 0) {
    throw "sources not staged — run scripts\vendor-rust.ps1 first"
}

Push-Location $rustDir
try {
    # crates.io rejects a .crate over 10 MiB. Check every package before
    # uploading any of them, so a size failure doesn't leave a half-published
    # set behind (crates.io versions are immutable and cannot be re-uploaded).
    Write-Host "== packaging ==" -ForegroundColor Cyan
    foreach ($crate in $order) {
        # cargo reports progress on stderr, and under Windows PowerShell
        # 5.1 redirecting a native command's stderr wraps each line in an
        # ErrorRecord — which `$ErrorActionPreference = "Stop"` then treats
        # as fatal. The exit code is the only thing worth believing here.
        # pwsh 7 (what CI uses) does not do this, so it only bites locally.
        $prevEap = $ErrorActionPreference
        $ErrorActionPreference = "Continue"
        $out = & cargo package -p $crate --allow-dirty 2>&1 | ForEach-Object { "$_" }
        $ErrorActionPreference = $prevEap
        $out | Write-Host

        if ($LASTEXITCODE -ne 0) {
            # `cargo package` rewrites path dependencies to registry ones and
            # resolves them, so a crate whose dependencies are not yet on
            # crates.io cannot be packaged at all. On a first publish that is
            # every crate above the shader packs, and it resolves itself as
            # the run proceeds — so it is expected, not a failure, in a real
            # run as much as a dry one. Deferring costs nothing here: the
            # crates that cannot be measured yet are the small ones, and
            # `cargo publish` packages each again at upload time anyway. The
            # shader packs are what sit near the cap, they have no
            # dependencies, and they are always measured before anything is
            # uploaded — which is what this pre-check exists for.
            # Cargo words this two ways: "no matching package named" when the
            # dependency has never been published, and "candidate versions
            # found which didn't match" when an older version exists and the
            # new one does not yet. A release that bumps sys and the wrapper
            # together hits the second on the wrapper.
            if (($out -match "no matching package named") -or
                ($out -match "candidate versions found which didn't match")) {
                Write-Host ("  {0,-32} deferred (dependencies not yet on crates.io)" -f $crate) `
                    -ForegroundColor DarkGray
                continue
            }
            throw "cargo package failed for $crate"
        }

        $file = Get-ChildItem (Join-Path $rustDir "target/package") -Filter "$crate-*.crate" |
                Sort-Object LastWriteTime | Select-Object -Last 1
        $mib = $file.Length / 1MB
        if ($mib -gt 10) {
            Write-Host ("  {0,-32} {1,6:N2} MiB  OVER LIMIT" -f $crate, $mib) -ForegroundColor Red
            throw "$crate exceeds the 10 MiB crates.io limit. Either split the " +
                  "pack further (by shader stage) or ask the crates.io team to " +
                  "raise the cap for it."
        }
        # DXIL and SPIR-V barely compress, so those packs sit close to the
        # cap already — a handful of new shaders would push them over.
        $verdict = if ($mib -gt 9) { "ok (tight)" } else { "ok" }
        $colour  = if ($mib -gt 9) { "Yellow" } else { "Gray" }
        Write-Host ("  {0,-32} {1,6:N2} MiB  {2}" -f $crate, $mib, $verdict) -ForegroundColor $colour
    }

    if ($DryRun) {
        Write-Host "`nDry run — nothing uploaded." -ForegroundColor Green
        return
    }

    Write-Host "== publishing ==" -ForegroundColor Cyan
    $uploaded = @()
    foreach ($crate in $order) {
        $ver = ((cargo metadata --no-deps --format-version 1 | ConvertFrom-Json).packages |
            Where-Object { $_.name -eq $crate }).version
        if (Test-AlreadyPublished $crate $ver) {
            Write-Host ("  {0,-32} {1} already on crates.io - skipped" -f $crate, $ver) `
                -ForegroundColor DarkGray
            continue
        }
        Write-Host "  $crate $ver" -ForegroundColor Cyan
        # `--allow-dirty` is required, not lax. What these crates ship is
        # generated and gitignored on purpose — the shader `pack/` trees and
        # `whiteoutflakes-sys/vendor/` are staged by vendor-rust.ps1, and
        # committing tens of megabytes of build output to keep cargo quiet
        # would be the wrong trade. Cargo counts untracked files inside a
        # package as uncommitted changes, so it refuses without this even
        # when `git status` is clean. The check above runs vendor-rust.ps1's
        # staging and verifies it is present, which is the guarantee that
        # actually matters here.
        # Same Windows PowerShell 5.1 stderr trap the packaging loop above
        # guards against, and it bites harder here: cargo announces "Updating
        # crates.io index" on stderr, which became a fatal ErrorRecord and
        # aborted the run mid-upload. The exit code is the only signal worth
        # believing.
        $prevEap = $ErrorActionPreference
        $ErrorActionPreference = "Continue"
        & cargo publish -p $crate --allow-dirty 2>&1 | ForEach-Object { Write-Host "$_" }
        $code = $LASTEXITCODE
        $ErrorActionPreference = $prevEap
        if ($code -ne 0) { throw "cargo publish failed for $crate" }
        $uploaded += $crate
        if ($crate -ne $order[-1]) {
            Write-Host "    waiting ${IndexWaitSeconds}s for the index" -ForegroundColor DarkGray
            Start-Sleep -Seconds $IndexWaitSeconds
        }
    }
    if ($uploaded.Count -eq 0) {
        Write-Host "`nNothing to publish - every version is already on crates.io." `
            -ForegroundColor Green
    } else {
        Write-Host ("`nPublished: " + ($uploaded -join ", ")) -ForegroundColor Green
    }
} finally {
    Pop-Location
}

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
        $out = & cargo package -p $crate --allow-dirty 2>&1
        $out | Write-Host

        if ($LASTEXITCODE -ne 0) {
            # `cargo package` rewrites path dependencies to registry ones and
            # resolves them, so a crate whose dependencies are not yet on
            # crates.io cannot be packaged at all. On a first publish that is
            # every crate above the shader packs, and it resolves itself as
            # the run proceeds — so in a dry run it is expected, not a failure.
            if ($DryRun -and ($out -match "no matching package named")) {
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
    foreach ($crate in $order) {
        Write-Host "  $crate" -ForegroundColor Cyan
        & cargo publish -p $crate
        if ($LASTEXITCODE -ne 0) { throw "cargo publish failed for $crate" }
        if ($crate -ne $order[-1]) {
            Write-Host "    waiting ${IndexWaitSeconds}s for the index" -ForegroundColor DarkGray
            Start-Sleep -Seconds $IndexWaitSeconds
        }
    }
    Write-Host "`nPublished." -ForegroundColor Green
} finally {
    Pop-Location
}

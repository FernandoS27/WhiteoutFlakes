# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Fernando Sahmkow
#
# Stage everything `cargo publish` needs into the crate directories.
#
# Cargo only packages files that live *inside* the crate being published —
# `include = ["../../../src/**"]` does not work, and symlinks are not
# followed into the tarball. So the C++ tree and the shader packs have to
# be physically copied in before publishing. Everything this script writes
# is gitignored and regenerated; nothing here is edited by hand.
#
#   scripts\vendor-rust.ps1              # stage
#   scripts\vendor-rust.ps1 -Check       # verify staged == source, no writes
#   scripts\vendor-rust.ps1 -Clean       # remove staged trees

param(
    [switch]$Check,
    [switch]$Clean
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
$rustDir  = Join-Path $repoRoot "bindings/rust"
$sysDir   = Join-Path $rustDir "whiteoutflakes-sys"
$vendor   = Join-Path $sysDir "vendor"

# Shader packs, split by graphics API. crates.io caps a .crate tarball at
# 10 MiB; the full 71 MB pack is 27.6 MB gzipped, so it cannot ship as one
# crate. Split this way each lands well under the cap — DXBC compresses
# ~18:1, DXIL and SPIR-V barely compress at all, which is why d3d11 is
# tiny and its siblings are not.
$shaderCrates = @(
    @{ Name = "d3d11"; Dirs = @("ps", "vs") }              # ~1.0 MB gz
    @{ Name = "d3d12"; Dirs = @("d3d12") }                 # ~8.6 MB gz
    @{ Name = "vulkan"; Dirs = @("vulkan") }               # ~9.4 MB gz
    @{ Name = "metal"; Dirs = @("mtlvs", "mtlfs") }        # ~6.5 MB gz
    @{ Name = "webgpu"; Dirs = @("webgpu") }               # ~2.0 MB gz
)

function Remove-Staged {
    # A concurrent cargo build holds the vendored headers open, and
    # Remove-Item deletes what it can before failing — leaving a tree that
    # is neither the old one nor a new one. Fail before touching anything
    # instead: partial removal is the worst outcome available here.
    if (Test-Path $vendor) {
        try {
            Remove-Item -Recurse -Force $vendor -ErrorAction Stop
        } catch {
            throw "could not clear $vendor - a build is probably using it. " +
                  "Wait for cargo/MSBuild to finish and re-run. ($($_.Exception.Message))"
        }
    }
    foreach ($c in $shaderCrates) {
        $p = Join-Path $rustDir "whiteoutflakes-shaders-$($c.Name)/pack"
        if (Test-Path $p) { Remove-Item -Recurse -Force $p }
    }
}

if ($Clean) {
    Remove-Staged
    Write-Host "staged trees removed." -ForegroundColor Green
    exit 0
}

# ── Drift check: does the vendor glob still cover the real targets? ───────
# vendor-cmake/CMakeLists.txt globs src/**/*.cpp instead of listing files,
# which only stays correct while every .cpp under those trees belongs to a
# library target. A file that no CMakeLists names is either expected
# (cornflakes, referenced through a variable) or drift — and drift here is
# not theoretical: whiteoutlib's src/whiteout/sno/sno_defs.cpp is orphaned
# upstream and does not compile, which a glob happily picks up.
# Exclusion patterns come out of vendor-cmake/CMakeLists.txt itself, so a
# file that build deliberately drops is not reported as drift and the
# exclusion list stays the single source of truth. CMake's regex flavour
# matches .NET's closely enough for the simple path patterns used there.
# Paths the vendored build drops on purpose, in at least one configuration.
# A file matching one of these is handled, not drift.
#
# KEEP IN SYNC with the list(FILTER ... EXCLUDE REGEX) calls in
# whiteoutflakes-sys/vendor-cmake/CMakeLists.txt. Scraping them out of the
# CMake source was tried and abandoned: several are applied through a
# foreach variable rather than written literally, and a looser "any quoted
# literal" match drags in prose from the FATAL_ERROR messages. Duplicating
# twelve lines is worse than nothing and better than a parser that breaks
# on a comment. Getting it wrong here costs a spurious warning, never a
# bad build.
$excludePatterns = @(
    '/src/renderer/imgui/'
    '/src/gfx/webgpu/'
    '/src/gfx/metal/'
    '/src/gfx/vulkan/'
    '/src/gfx/d3d11/'
    '/src/gfx/d3d12/'
    '/casc/'
    'casc_file_system\.cpp$'
    '/mpq/'
    'mpq_file_system\.cpp$'
    'deflate_zlibng\.cpp$'
    '/models/m2/'
    '/models/m3/'
    '/models/wem/'
    '/sno/'
)

function Test-Drift($tree, $cmakeFile, $label) {
    $root = (Resolve-Path $tree).Path
    $paths = Get-ChildItem -Recurse -Filter *.cpp $tree |
        ForEach-Object { '/' + $_.FullName.Substring($root.Length + 1).Replace('\', '/') }
    $text = Get-Content $cmakeFile -Raw

    $drift = $paths | Where-Object {
        $path = $_
        $name = Split-Path $path -Leaf
        # Named in the real build? Not drift. Anchor on a path separator or
        # start of line: a bare substring test reports `device.cpp` as named
        # because `d3d11_device.cpp` is, and silently swallows exactly the
        # new files this check exists to surface.
        if ($text -match "(^|[/\\`"'\s])$([regex]::Escape($name))") { return $false }
        # Dropped by the vendored build on purpose? Not drift either.
        foreach ($pat in $excludePatterns) {
            if ($path -match $pat) { return $false }
        }
        # cornflakes is referenced through a CMake variable, never by name.
        return ($path -notmatch '/cornflakes/')
    } | Sort-Object

    if ($drift) {
        Write-Warning "$label`: .cpp files that no CMakeLists names and the vendored build still compiles:"
        $drift | ForEach-Object { Write-Warning "  $_" }
        Write-Warning "Confirm each is wanted, or exclude it in vendor-cmake/CMakeLists.txt."
    }
}

Test-Drift (Join-Path $repoRoot "src") (Join-Path $repoRoot "CMakeLists.txt") "flakes src/"
Test-Drift (Join-Path $repoRoot "externals/WhiteoutLib/src") `
           (Join-Path $repoRoot "externals/WhiteoutLib/CMakeLists.txt") "whiteoutlib src/"

if ($Check) {
    $missing = @()
    if (-not (Test-Path $vendor)) { $missing += "vendor/" }
    foreach ($c in $shaderCrates) {
        $p = Join-Path $rustDir "whiteoutflakes-shaders-$($c.Name)/pack"
        if (-not (Test-Path $p)) { $missing += "whiteoutflakes-shaders-$($c.Name)/pack" }
    }
    if ($missing) {
        Write-Host "not staged: $($missing -join ', ')" -ForegroundColor Yellow
        exit 1
    }
    Write-Host "staged trees present." -ForegroundColor Green
    exit 0
}

Remove-Staged

# ── C++ sources ───────────────────────────────────────────────────────────
Write-Host "== vendoring C++ ==" -ForegroundColor Cyan
New-Item -ItemType Directory -Force $vendor | Out-Null

$copies = @(
    @{ From = "src";                                To = "src" }
    @{ From = "include";                            To = "include" }
    @{ From = "externals/WhiteoutLib/src";          To = "whiteoutlib/src" }
    @{ From = "externals/WhiteoutLib/include";      To = "whiteoutlib/include" }
    @{ From = "bindings/c";                         To = "c" }
)
foreach ($c in $copies) {
    $from = Join-Path $repoRoot $c.From
    if (-not (Test-Path $from)) { throw "missing source tree: $($c.From)" }
    $to = Join-Path $vendor $c.To
    New-Item -ItemType Directory -Force (Split-Path -Parent $to) | Out-Null
    Copy-Item $from $to -Recurse -Force
    Write-Host "  $($c.From) -> vendor/$($c.To)"
}

# The vendored C ABI keeps its sources but not the repo's CMakeLists —
# vendor-cmake/CMakeLists.txt is the build for this tree.
Remove-Item (Join-Path $vendor "c/CMakeLists.txt") -ErrorAction SilentlyContinue
Copy-Item (Join-Path $sysDir "vendor-cmake/CMakeLists.txt") (Join-Path $vendor "CMakeLists.txt") -Force

# ── Embedded resources ────────────────────────────────────────────────────
# Two headers the top-level build generates and the vendored build cannot:
#
#   team_glow_embedded.h   from resources/teamglow.tga — cheap, regenerate
#                          here with the same cmake script.
#   compiled_shaders.h     ~2 MB of DXBC/SPIR-V for the engine's own small
#                          shaders (blit, capture, gtao, dof, tonemap),
#                          produced by slangc from the Vulkan SDK. Copied
#                          from a build tree if there is one, else from the
#                          committed prebuilt/compiled_shaders.h — the same
#                          bargain prebuilt/shaders already makes, so this
#                          script runs from a clean clone.
Write-Host "== embedding resources ==" -ForegroundColor Cyan
$embedded = Join-Path $vendor "embedded"
New-Item -ItemType Directory -Force $embedded | Out-Null

# Prefer a live build tree (freshest), fall back to the committed copy —
# same bargain prebuilt/shaders already makes, so vendoring works from a
# clean clone with no Vulkan SDK.
$shaderHeader = $null
foreach ($tree in @("build", "build-rust", "build-winci")) {
    $candidate = Join-Path $repoRoot "$tree/shaders/compiled_shaders.h"
    if (Test-Path $candidate) { $shaderHeader = $candidate; break }
}
if (-not $shaderHeader) {
    $shaderHeader = Join-Path $repoRoot "prebuilt/compiled_shaders.h"
}
if (-not (Test-Path $shaderHeader)) {
    throw "compiled_shaders.h found neither under build*/shaders/ nor in " +
          "prebuilt/. Build the project once (needs slangc from the Vulkan " +
          "SDK) and re-run, or restore prebuilt/compiled_shaders.h."
}
Copy-Item $shaderHeader (Join-Path $embedded "compiled_shaders.h") -Force
$kb = [math]::Round((Get-Item $shaderHeader).Length / 1KB)
$origin = $shaderHeader.Substring($repoRoot.Length + 1).Replace('\', '/')
Write-Host "  compiled_shaders.h ($kb KB, from $origin)"
$script = Join-Path $repoRoot "resources/embed_binary.cmake"
if (-not (Test-Path $script)) { throw "missing $script — cannot embed teamglow.tga" }
& cmake "-DINPUT=$(Join-Path $repoRoot 'resources/teamglow.tga')" `
        "-DOUTPUT=$(Join-Path $embedded 'team_glow_embedded.h')" `
        "-DVAR_NAME=kTeamGlowTGA" `
        "-DNAMESPACE=whiteout::flakes::io" `
        -P $script
if ($LASTEXITCODE -ne 0) { throw "embed_binary.cmake failed" }
Write-Host "  team_glow_embedded.h"

# ── Shader packs ──────────────────────────────────────────────────────────
# Each crate's pack/ mirrors the layout the BLS cache expects under its
# search root, i.e. pack/shaders/<api>/<stage>/<name>.bls. The sys crate
# merges the enabled packs into one directory at build time.
Write-Host "== vendoring shaders ==" -ForegroundColor Cyan
$shaderSrc = Join-Path $repoRoot "prebuilt/shaders"
if (-not (Test-Path $shaderSrc)) { throw "missing prebuilt/shaders" }
foreach ($c in $shaderCrates) {
    $dest = Join-Path $rustDir "whiteoutflakes-shaders-$($c.Name)/pack/shaders"
    New-Item -ItemType Directory -Force $dest | Out-Null
    foreach ($d in $c.Dirs) {
        $from = Join-Path $shaderSrc $d
        if (-not (Test-Path $from)) { throw "missing shader dir: prebuilt/shaders/$d" }
        Copy-Item $from (Join-Path $dest $d) -Recurse -Force
    }
    $mb = [math]::Round((Get-ChildItem -Recurse $dest | Measure-Object Length -Sum).Sum / 1MB, 1)
    Write-Host "  whiteoutflakes-shaders-$($c.Name): $mb MB raw"
}

Write-Host "`nStaged. Verify with: cargo package --list -p whiteoutflakes-sys" -ForegroundColor Green

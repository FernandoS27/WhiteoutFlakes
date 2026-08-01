// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

//! Native library for [WhiteoutFlakes](https://github.com/Sahmkow/WhiteoutFlakes).
//!
//! Use [`whiteoutflakes`](https://docs.rs/whiteoutflakes) instead — this
//! crate exists to build and link the C++ renderer, and to say where its
//! shader pack ended up.
//!
//! # Why there are no `extern "C"` declarations here
//!
//! The usual `-sys` split puts raw declarations in the sys crate and the
//! safe API above it. Here the codegen emits both halves of a module into
//! one file (`whiteoutflakes/src/flakes.rs`), and splitting it would mean
//! an emitter change for no gain: the `links` key below still gives cargo
//! what it needs to reject two crates claiming the same native library,
//! and the extern declarations resolve against this crate's link
//! directives at final link either way.

use std::path::Path;

/// Directory holding the shader packs this crate was built with.
///
/// Every enabled `whiteoutflakes-shaders-*` pack is merged here at build
/// time, in the `shaders/<api>/<stage>/<name>.bls` layout the renderer
/// searches for. Pass it to `SceneView::set_engine_asset_root` before
/// bringing the device up — `whiteoutflakes::Renderer::use_bundled_shaders`
/// does exactly that.
///
/// # This path is build-local
///
/// It points inside `target/`, so it is valid on the machine that compiled
/// the crate and stops being valid the moment you ship the binary
/// somewhere else. Distributing an application means copying the tree
/// somewhere you control and calling `set_engine_asset_root` with that
/// path instead.
pub const ENGINE_ASSET_ROOT: &str = concat!(env!("OUT_DIR"), "/engine-assets");

/// [`ENGINE_ASSET_ROOT`] as a [`Path`].
pub fn engine_asset_root() -> &'static Path {
    Path::new(ENGINE_ASSET_ROOT)
}

/// Whether the shader pack is actually present.
///
/// False when the crate was built with every shader feature turned off, or
/// when the build directory has since been cleaned. A host that gets
/// `false` here must set its own engine asset root or device init will
/// fail on the first shader read.
pub fn has_bundled_shaders() -> bool {
    engine_asset_root().join("shaders").is_dir()
}

/// Subdirectory of `shaders/` a given backend reads its bundles from.
///
/// D3D11 is the odd one out: its DXBC lives directly under `shaders/ps`
/// and `shaders/vs`, with no API subdirectory, because it was the original
/// layout. See `BlsShaderCache::Acquire`.
fn backend_subdir(api_subdir: &str) -> std::path::PathBuf {
    let shaders = engine_asset_root().join("shaders");
    if api_subdir.is_empty() {
        shaders.join("ps")
    } else {
        shaders.join(api_subdir).join("ps")
    }
}

/// Whether bundled shaders exist for a specific backend.
///
/// Enabling the `d3d12` feature and then asking for a D3D11 device is an
/// easy mistake, and the failure it produces without this check is a
/// shader-read error deep inside device init. Takes the API's shader
/// subdirectory — `""` for D3D11, else `"d3d12"` / `"vulkan"` / `"metal"`
/// — because this crate has no dependency on the enum that names them.
pub fn has_shaders_for(api_subdir: &str) -> bool {
    backend_subdir(api_subdir).is_dir()
}

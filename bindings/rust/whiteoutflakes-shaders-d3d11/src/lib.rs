// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

//! Prebuilt BLS shader bundles for the Direct3D 11 (DXBC) backend of
//! [WhiteoutFlakes](https://github.com/Sahmkow/WhiteoutFlakes).
//!
//! Data only — no code, no dependencies. The pack ships as files rather
//! than `include_bytes!` constants because there are dozens of megabytes
//! of them across the backends, and the renderer wants a directory to
//! search anyway.
//!
//! Consumers do not use this crate directly: `whiteoutflakes-sys` takes it
//! as a build-dependency, merges the enabled packs into one tree under
//! `OUT_DIR`, and hands the path to the renderer.

use std::path::PathBuf;

/// Root of this pack. Contains `shaders/<api>/<stage>/<name>.bls`, the
/// layout the BLS cache expects beneath its search root.
///
/// Resolved from `CARGO_MANIFEST_DIR` at compile time, so it points into
/// the registry checkout for a published build and into the worktree for a
/// path dependency. Either way the files outlive the process.
pub fn pack_dir() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("pack")
}

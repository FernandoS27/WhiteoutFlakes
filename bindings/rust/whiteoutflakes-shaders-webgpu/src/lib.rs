// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

//! Prebuilt BLS shader bundles for the WebGPU (WGSL) backend of
//! [WhiteoutFlakes](https://github.com/Sahmkow/WhiteoutFlakes).
//!
//! Data only — no code, no dependencies. See
//! `whiteoutflakes-shaders-d3d11` for the layout rationale.

use std::path::PathBuf;

/// Root of this pack. Contains `shaders/webgpu/<stage>/<name>.bls`.
pub fn pack_dir() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("pack")
}

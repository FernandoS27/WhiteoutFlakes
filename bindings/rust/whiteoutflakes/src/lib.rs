// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

//! Rust bindings for the [WhiteoutFlakes](https://github.com/Sahmkow/WhiteoutFlakes)
//! renderer service library — a Warcraft III model renderer with SD and
//! HD (Reforged PBR) material paths.
//!
//! # Shape of the API
//!
//! [`Renderer`] owns everything. Sub-services are reached through small
//! views ([`PipelineView`], [`CameraView`], [`SettingsView`], …) that the
//! renderer hands out, and per-model state through [`ActorView`]:
//!
//! ```no_run
//! use whiteoutflakes::{GfxApi, Renderer};
//!
//! let mut r = Renderer::new();
//! // Before init_device: tell the renderer where its shaders are.
//! assert!(r.use_bundled_shaders());
//! r.pipeline().unwrap().init_device(GfxApi::D3D11);
//! let actor = r.loader().unwrap().spawn_unit("Units/Human/Footman/Footman.mdx");
//! loop {
//!     r.tick(1.0 / 60.0);
//!     # break;
//! }
//! # let _ = actor;
//! ```
//!
//! # Shaders come from a directory, and you have to say which
//!
//! Device bring-up reads the BLS shader pack through the renderer's
//! content provider, whose default search root is the *executable's*
//! directory — which is never right for a library. Call
//! [`Renderer::use_bundled_shaders`] (or
//! [`SceneView::set_engine_asset_root`] with your own path) before
//! [`PipelineView::init_device`], or init fails.
//!
//! # Views borrow the renderer
//!
//! A view is a raw pointer into the renderer's internals and dangles once
//! the renderer is dropped. The accessors take `&mut self` and the views
//! carry no lifetime, so the compiler does **not** stop you from outliving
//! the owner — keep views scoped to the call site rather than storing
//! them. Tying the lifetime properly needs an emitter change; until then
//! this is the one unenforced invariant in the crate.
//!
//! # `init_device` is a precondition, not an option
//!
//! [`Renderer::tick`], every [`AssetsView`] method, and [`ReplaceablesView`]
//! reach subsystems the C++ side only builds inside
//! [`PipelineView::init_device`]. Calling them on a device-less renderer
//! dereferences a null pointer and crashes the process. Bring the device
//! up first, always — including in headless use, where an off-screen
//! target replaces the swap chain.
//!
//! # Threading
//!
//! The renderer is single-threaded by design. Handles are [`Send`] so you
//! may move a renderer to another thread, but they are deliberately not
//! [`Sync`]: no part of the C++ side documents concurrent access.
//!
//! # Errors
//!
//! The underlying C++ library does not throw. Operations that can fail
//! report it the way the C++ API does — a zero handle from
//! [`LoaderView::spawn_unit`], `false` from an apply, `None` from a view
//! accessor on a torn-down renderer.

#![deny(unsafe_op_in_unsafe_fn)]
#![warn(missing_debug_implementations)]

// The emitter imports the full support prelude into every generated ffi
// block whether or not a given module's shapes use all of it. Scoped to
// the generated module so the hand-written code still gets the lint.
#[allow(unused_imports)]
pub mod flakes;
mod shims;
mod support;

pub use flakes::*;
pub use support::{BorrowedSlice, Bytes};

use core::fmt;

/// Errors that can cross the binding boundary.
///
/// Deliberately small: the renderer signals ordinary failure in-band
/// (zero handles, `false`, `None`), so this covers only version skew
/// between the crate and the library it linked against.
#[non_exhaustive]
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Error {
    /// The native library produced an enum discriminant this crate does
    /// not know. Indicates version skew rather than bad input.
    UnknownEnum { name: &'static str, value: i32 },
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Error::UnknownEnum { name, value } => write!(
                f,
                "the native library returned {value} for {name}, which this \
                 crate does not know — the linked library is newer"
            ),
        }
    }
}

impl std::error::Error for Error {}

// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
//
// Produce (or locate) the native library, and stage the shader packs.
//
// Three modes, in precedence order:
//
//   1. WHITEOUTFLAKES_LIB_DIR — link a library already built from the
//      repository. Skips CMake entirely; what the repo's own
//      scripts/build-rust.ps1 uses.
//   2. vendor/ present      — build it from the vendored C++ via CMake.
//      This is the published path.
//   3. pkg-config           — a system install.
//
// docs.rs gets neither a C++ toolchain nor the time to use one, so the
// build is skipped there; the crate still type-checks and documents.

use std::path::{Path, PathBuf};
use std::{env, fs};

fn main() {
    println!("cargo:rerun-if-env-changed=WHITEOUTFLAKES_LIB_DIR");
    println!("cargo:rerun-if-env-changed=WHITEOUTFLAKES_STATIC");
    // Printing any rerun-if-* switches cargo off "rerun on any change", so
    // every environment variable this script reads has to be declared —
    // DOCS_RS included. Without it a `DOCS_RS=1 cargo check` caches a run
    // that emitted no link directives, and the next real build links
    // nothing and fails on every symbol in the library.
    println!("cargo:rerun-if-env-changed=DOCS_RS");

    let out_dir = PathBuf::from(env::var("OUT_DIR").expect("OUT_DIR"));
    let shader_root = stage_shaders(&out_dir);
    // Read by the `whiteoutflakes` crate's build script as
    // DEP_WHITEOUTFLAKES_SHADER_ROOT — the `links` key is what makes that
    // metadata visible to direct dependents.
    println!("cargo:shader_root={}", shader_root.display());

    if env::var_os("DOCS_RS").is_some() {
        return;
    }

    if let Some(dir) = env::var_os("WHITEOUTFLAKES_LIB_DIR") {
        link_prebuilt(&PathBuf::from(dir));
        return;
    }

    let vendor = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("vendor");
    if vendor.join("CMakeLists.txt").is_file() {
        // Watch the vendored tree so an edit re-runs CMake. Cargo compares
        // the *directory's own* mtime for a directory path, which does not
        // change when a file is replaced several levels down — copying a
        // single .cpp into vendor/src/... left cargo relinking a stale
        // archive with no warning. Listing the immediate subdirectories
        // narrows that window; the reliable way to propagate an edit is
        // still to re-run scripts/vendor-rust.ps1, which rewrites the tree.
        println!("cargo:rerun-if-changed={}", vendor.display());
        for sub in [
            "CMakeLists.txt",
            "c",
            "src",
            "include",
            "whiteoutlib",
            "embedded",
        ] {
            println!("cargo:rerun-if-changed={}", vendor.join(sub).display());
        }
        check_cmakelists_fresh(&vendor);
        build_from_source(&vendor);
        return;
    }

    if pkg_config_probe() {
        return;
    }

    panic!(
        "no native library and no vendored sources.\n\
         \n\
         In a checkout, either stage the sources\n\
             scripts\\vendor-rust.ps1\n\
         or point at an existing build\n\
             cmake -S . -B build-rust -DWDX_BUILD_C_BINDINGS=ON\n\
             cmake --build build-rust --config Release --target whiteoutflakes_c\n\
             WHITEOUTFLAKES_LIB_DIR=build-rust/c-dist/Release cargo build\n\
         \n\
         scripts\\build-rust.ps1 does the latter."
    );
}

/// Merge every enabled shader pack into one directory under OUT_DIR.
///
/// The renderer searches a single root, so packs for several backends have
/// to be unified rather than listed. Copying is a few MB and only happens
/// when a pack crate changes.
fn stage_shaders(out_dir: &Path) -> PathBuf {
    let root = out_dir.join("engine-assets");

    // `vec![]` can't express this: each entry is behind its own cfg.
    #[allow(clippy::vec_init_then_push)]
    let packs: Vec<PathBuf> = {
        let mut packs = Vec::new();
        #[cfg(feature = "d3d11")]
        packs.push(whiteoutflakes_shaders_d3d11::pack_dir());
        #[cfg(feature = "d3d12")]
        packs.push(whiteoutflakes_shaders_d3d12::pack_dir());
        #[cfg(feature = "vulkan")]
        packs.push(whiteoutflakes_shaders_vulkan::pack_dir());
        #[cfg(feature = "metal")]
        packs.push(whiteoutflakes_shaders_metal::pack_dir());
        #[cfg(feature = "webgpu")]
        packs.push(whiteoutflakes_shaders_webgpu::pack_dir());
        packs
    };

    if packs.is_empty() {
        println!(
            "cargo:warning=no shader pack features enabled — \
             InitDevice will fail unless the host sets its own engine asset root"
        );
    }

    // The Metal pack is real; the Metal *backend* is not vendored — its
    // sources are Objective-C++ and vendor-cmake/CMakeLists.txt excludes
    // them. Staging shaders for a backend that cannot be created is worth
    // saying out loud rather than letting InitDevice discover it.
    #[cfg(feature = "metal")]
    println!(
        "cargo:warning=the `metal` feature stages Metal shaders, but the \
         Metal backend sources are not vendored — GfxApi::Metal will not \
         initialise. Build the native library from the repository instead."
    );

    for pack in &packs {
        println!("cargo:rerun-if-changed={}", pack.display());
    }

    // The build script re-runs whenever anything under vendor/ changes, but
    // the shader packs are immutable data — re-copying ~20 MB every time is
    // pure churn, and wiping the tree while another build is reading it is
    // how this managed to fail intermittently. A stamp keyed on the enabled
    // packs makes it a no-op unless the feature set actually moved.
    let stamp = root.join(".staged");
    let want: String = packs
        .iter()
        .map(|p| p.display().to_string())
        .collect::<Vec<_>>()
        .join("\n");
    if fs::read_to_string(&stamp).is_ok_and(|have| have == want) {
        return root;
    }

    let _ = fs::remove_dir_all(&root);
    fs::create_dir_all(&root).expect("create shader root");
    for pack in &packs {
        copy_tree(pack, &root).unwrap_or_else(|e| panic!("staging {}: {e}", pack.display()));
    }
    fs::write(&stamp, want).expect("write staging stamp");
    root
}

fn copy_tree(from: &Path, to: &Path) -> std::io::Result<()> {
    for entry in fs::read_dir(from)? {
        let entry = entry?;
        let dst = to.join(entry.file_name());
        if entry.file_type()?.is_dir() {
            fs::create_dir_all(&dst)?;
            copy_tree(&entry.path(), &dst)?;
        } else {
            fs::copy(entry.path(), &dst)?;
        }
    }
    Ok(())
}

/// `vendor/CMakeLists.txt` is a copy of `vendor-cmake/CMakeLists.txt` that
/// `scripts/vendor-rust.ps1` stages, and CMake only ever sees the copy. In a
/// published crate the two are identical by construction; in a checkout,
/// editing the source and rebuilding produces a build that silently ignores
/// the edit — which is how a WhiteoutLib exclusion that looked correct went
/// on compiling 68 translation units. Turn that into an error.
fn check_cmakelists_fresh(vendor: &Path) {
    let authored = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("vendor-cmake")
        .join("CMakeLists.txt");
    println!("cargo:rerun-if-changed={}", authored.display());
    let (Ok(a), Ok(b)) = (
        fs::read_to_string(&authored),
        fs::read_to_string(vendor.join("CMakeLists.txt")),
    ) else {
        return;
    };
    if a != b {
        panic!(
            "vendor/CMakeLists.txt is out of date with vendor-cmake/CMakeLists.txt.\n\
             CMake builds the staged copy, so the edit would have no effect.\n\
             Re-run: pwsh scripts/vendor-rust.ps1"
        );
    }
}

fn build_from_source(vendor: &Path) {
    let mut cfg = cmake::Config::new(vendor);
    // Release has no debug info, so a fault inside the C++ shows up in a
    // debugger as a bare module+offset. Setting WHITEOUTFLAKES_DEBUG_NATIVE
    // builds RelWithDebInfo instead — same optimisation level, plus PDBs —
    // which is what makes a native backtrace readable from a Rust host.
    println!("cargo:rerun-if-env-changed=WHITEOUTFLAKES_DEBUG_NATIVE");
    if env::var_os("WHITEOUTFLAKES_DEBUG_NATIVE").is_some() {
        cfg.profile("RelWithDebInfo");
    } else {
        cfg.profile("Release");
    }

    // Mirror the cargo features onto the CMake options. Only the backends
    // that need an SDK are switchable; D3D11/12 come free on Windows.
    cfg.define(
        "WFS_ENABLE_VULKAN",
        if cfg!(feature = "vulkan") {
            "ON"
        } else {
            "OFF"
        },
    );

    // As with Dawn below: CMake resolving `Vulkan::Vulkan` for a *static*
    // library records an interface dependency nothing downstream reads.
    // Cargo links the final binary and only knows what this script prints,
    // so the loader import library has to be named here or the link ends
    // in ~27 unresolved `vk*` symbols.
    if cfg!(feature = "vulkan") {
        println!("cargo:rerun-if-env-changed=VULKAN_SDK");
        let sdk = env::var("VULKAN_SDK").unwrap_or_else(|_| {
            panic!(
                "the `vulkan` feature needs a Vulkan SDK: set VULKAN_SDK.\n\
                 The install must include the VMA component — the default \
                 silent install omits it and vk_mem_alloc.h will be missing."
            )
        });
        let lib_dir = if cfg!(target_os = "windows") {
            format!("{sdk}/Lib")
        } else {
            format!("{sdk}/lib")
        };
        println!("cargo:rustc-link-search=native={lib_dir}");
        println!(
            "cargo:rustc-link-lib=dylib={}",
            if cfg!(target_os = "windows") {
                "vulkan-1"
            } else {
                "vulkan"
            }
        );
    }
    cfg.define(
        "WFS_ENABLE_CASC",
        if cfg!(feature = "casc") { "ON" } else { "OFF" },
    );
    cfg.define(
        "WFS_ENABLE_MPQ",
        if cfg!(feature = "mpq") { "ON" } else { "OFF" },
    );

    // WhiteoutLib: vendored copy, or the archive the `whiteoutlib` crate
    // already built and linked. In the latter case not one of its 148
    // translation units is compiled here — only its headers are used.
    if cfg!(feature = "system-whiteoutlib") {
        let include = env::var("DEP_WHITEOUT_NATIVE_INCLUDE").unwrap_or_else(|_| {
            panic!(
                "`system-whiteoutlib` is enabled but the whiteoutlib crate published \
                 no include path. It needs `links = \"whiteout_native\"` and a \
                 `cargo:include=` line in its build script (whiteoutlib >= 0.1.0)."
            )
        });
        // Both builds must agree on CASC: the headers this crate compiles
        // against are gated on it, and the code it calls lives in the other
        // archive. `casc = ["whiteoutlib?/casc"]` keeps them in step, so a
        // mismatch here means something re-configured that crate directly.
        let their_mpq = env::var("DEP_WHITEOUT_NATIVE_HAS_MPQ").unwrap_or_default() == "1";
        if their_mpq != cfg!(feature = "mpq") {
            panic!(
                "MPQ mismatch: whiteoutflakes-sys has mpq={}, the linked whiteoutlib                  was built with mpq={their_mpq}. Enable or disable the `mpq` feature                  on both, or drop `system-whiteoutlib`.",
                cfg!(feature = "mpq")
            );
        }
        let their_casc = env::var("DEP_WHITEOUT_NATIVE_HAS_CASC").unwrap_or_default() == "1";
        if their_casc != cfg!(feature = "casc") {
            panic!(
                "CASC mismatch: whiteoutflakes-sys has casc={}, the linked \
                 whiteoutlib was built with casc={their_casc}. Enable or disable \
                 the `casc` feature on both, or drop `system-whiteoutlib`.",
                cfg!(feature = "casc")
            );
        }

        // WhiteoutLib ships two archives: `whiteout_native_static` for the C
        // ABI and `whiteout_lib` for the C++ behind it. This crate's objects
        // reference the C++ directly, so it has to ask for both.
        //
        // whiteoutlib's build script names them too, but that is not enough
        // to rely on. A build script's `-l` rides on its crate's rlib and
        // only reaches the link when that rlib does; a binary that uses the
        // renderer without touching whiteoutlib's Rust API drops the unused
        // rlib and the `-l` with it, and the link fails on every
        // `whiteout::textures::Texture` symbol. `-L` is graph-wide and does
        // arrive either way, which is why the search paths are left to
        // whiteoutlib — deriving `whiteout_lib`'s directory from
        // `DEP_WHITEOUT_NATIVE_LIB_DIR` (which points at the C-ABI
        // directory) would mean hard-coding upstream's build layout here.
        // Order matters for GNU ld, which resolves static archives in
        // command-line order: the archives that *reference* whiteout_lib
        // come first. MSVC does not care, so getting this wrong would
        // survive local testing here and fail on Linux.
        println!("cargo:rustc-link-lib=static=whiteout_native_static");
        if cfg!(feature = "casc") {
            println!("cargo:rustc-link-lib=static=whiteout_casc");
        }
        if cfg!(feature = "mpq") {
            println!("cargo:rustc-link-lib=static=whiteout_mpq");
        }
        println!("cargo:rustc-link-lib=static=whiteout_lib");
        if let Ok(dir) = env::var("DEP_WHITEOUT_NATIVE_LIB_DIR") {
            println!("cargo:rustc-link-search=native={dir}");
        }

        cfg.define("WFS_EXTERNAL_WHITEOUTLIB", "ON");
        cfg.define("WFS_WHITEOUTLIB_INCLUDE", &include);
    } else {
        cfg.define("WFS_EXTERNAL_WHITEOUTLIB", "OFF");
    }

    // WebGPU needs a Dawn distribution, and this build never downloads one:
    // the repository's CMakeLists clones eliemichel/WebGPU-distribution at
    // configure time, which a published crate must not do. Point
    // WHITEOUTFLAKES_DAWN_DIR at an unpacked distribution instead.
    if cfg!(feature = "webgpu") {
        println!("cargo:rerun-if-env-changed=WHITEOUTFLAKES_DAWN_DIR");
        let dawn = env::var("WHITEOUTFLAKES_DAWN_DIR").unwrap_or_else(|_| {
            panic!(
                "the `webgpu` feature needs WHITEOUTFLAKES_DAWN_DIR pointing at \
                 an unpacked Dawn distribution (include/ + lib/).\n\
                 Prebuilt ones: https://github.com/eliemichel/WebGPU-distribution/releases\n\
                 Nothing is downloaded during this build by design."
            )
        });
        cfg.define("WFS_ENABLE_WEBGPU", "ON");
        cfg.define("WFS_DAWN_DIR", &dawn);
        // `target_link_libraries` on a *static* CMake library records an
        // interface dependency that nothing downstream reads: cargo links
        // the final binary and only knows what this script prints. Without
        // these two lines the C++ compiles and the link fails on ~80
        // unresolved wgpu* symbols.
        println!("cargo:rustc-link-search=native={dawn}/lib");
        println!("cargo:rustc-link-lib=dylib=webgpu_dawn");
        // Dawn ships as a shared library only — no static archive — so the
        // consumer's executable needs webgpu_dawn.dll/.so/.dylib beside it
        // at run time. Nothing cargo can place for them.
        println!(
            "cargo:warning=WebGPU links Dawn dynamically; copy webgpu_dawn.* \
             from {dawn} next to your executable or it will not start"
        );
    } else {
        cfg.define("WFS_ENABLE_WEBGPU", "OFF");
    }

    let dst = cfg.build();
    println!(
        "cargo:rustc-link-search=native={}",
        dst.join("lib").display()
    );
    println!("cargo:rustc-link-lib=static=whiteoutflakes_native");
    link_system_libs();
    link_cxx_runtime();
}

fn link_prebuilt(dir: &Path) {
    if !dir.is_dir() {
        panic!(
            "WHITEOUTFLAKES_LIB_DIR points at {} which is not a directory",
            dir.display()
        );
    }
    let static_link = env::var("WHITEOUTFLAKES_STATIC")
        .map(|v| v != "0" && !v.eq_ignore_ascii_case("false"))
        .unwrap_or(false);
    println!("cargo:rustc-link-search=native={}", dir.display());
    if static_link {
        println!("cargo:rustc-link-lib=static=whiteoutflakes_native_static");
        link_system_libs();
        link_cxx_runtime();
    } else {
        println!("cargo:rustc-link-lib=dylib=whiteoutflakes_native");
    }
}

/// Static linking means the consumer's binary, not the DLL, owns the
/// dependency edges the shared build resolved for us.
fn link_system_libs() {
    if env::var("CARGO_CFG_TARGET_OS").as_deref() == Ok("windows") {
        for lib in [
            "d3d11", "d3d12", "dxgi", "user32", "gdi32", "ole32", "advapi32", "shell32", "dbghelp",
            "ws2_32",
        ] {
            println!("cargo:rustc-link-lib=dylib={lib}");
        }
    }
}

fn link_cxx_runtime() {
    let target = env::var("TARGET").unwrap_or_default();
    if target.contains("msvc") {
        return; // MSVC links it from the objects' own directives.
    }
    if target.contains("apple") {
        println!("cargo:rustc-link-lib=dylib=c++");
    } else {
        println!("cargo:rustc-link-lib=dylib=stdc++");
    }
}

fn pkg_config_probe() -> bool {
    // Shelling out rather than taking a pkg-config crate dependency: this
    // is a fallback path and the dependency budget is better spent
    // elsewhere.
    let out = std::process::Command::new("pkg-config")
        .args(["--libs", "--silence-errors", "whiteoutflakes"])
        .output();
    match out {
        Ok(o) if o.status.success() => {
            let flags = String::from_utf8_lossy(&o.stdout).to_string();
            for tok in flags.split_whitespace() {
                if let Some(p) = tok.strip_prefix("-L") {
                    println!("cargo:rustc-link-search=native={p}");
                } else if let Some(l) = tok.strip_prefix("-l") {
                    println!("cargo:rustc-link-lib=dylib={l}");
                }
            }
            true
        }
        _ => false,
    }
}

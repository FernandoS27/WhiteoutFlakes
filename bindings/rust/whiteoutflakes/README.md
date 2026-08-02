# whiteoutflakes

Rust bindings for the [WhiteoutFlakes](https://github.com/Sahmkow/WhiteoutFlakes)
renderer service library — a Warcraft III model renderer with SD and HD
(Reforged PBR) material paths.

`src/flakes.rs` is generated from the annotated C++ headers in
`include/whiteout/flakes/`; `src/shims.rs` and `src/support.rs` are
hand-written. See [BINDINGS.md](../../../BINDINGS.md) for the pipeline.

## Installing

```toml
[dependencies]
whiteoutflakes = "0.1"
```

That builds the C++ renderer from source through CMake — a C++20
compiler and about three minutes, no submodules, no vcpkg, no SDKs.

| Feature | Default | Effect |
|---|---|---|
| `d3d11` | yes | D3D11 backend + shader pack (0.9 MiB) |
| `casc` | yes | Reads assets out of an installed Warcraft III |
| `d3d12` | | D3D12 backend + shader pack (8.7 MiB) |
| `vulkan` | | Vulkan backend; needs a Vulkan SDK (with the VMA component) |
| `metal` | | Metal shader pack only — backend not vendored, see the sys crate |
| `webgpu` | | WebGPU backend; Windows-only here, and needs `WHITEOUTFLAKES_DAWN_DIR` |

```toml
whiteoutflakes = { version = "0.1", features = ["d3d12"] }
```

Each backend brings its own prebuilt shader pack as a separate crate —
the combined pack is 27.6 MB compressed and crates.io caps a crate at
10 MiB. `Renderer::use_bundled_shaders()` points the renderer at whichever
packs got staged.

Turning `casc` off is possible but rarely what you want: a model whose
textures live in game storage still renders its geometry, with every
surface a placeholder magenta.

### Alongside `whiteoutlib`

The renderer is built on WhiteoutLib and ships a copy of it. If your
binary also depends on the [`whiteoutlib`] crate directly, that copy is
compiled a second time and ~9,900 identically-mangled C++ symbols end up
defined in both archives, with the linker silently picking one. Switch
this crate to the copy `whiteoutlib` already built:

```toml
whiteoutflakes = { version = "0.1", default-features = false,
                   features = ["d3d11", "casc", "system-whiteoutlib"] }
whiteoutlib    = { version = "0.1", features = ["casc"] }
```

Both must agree on `casc` — the renderer compiles against headers gated on
it and calls into the other archive. The build fails with an explanation
rather than mislinking if they disagree.

[`whiteoutlib`]: https://crates.io/crates/whiteoutlib

`webgpu` is the one backend that cannot be self-contained. Dawn is only
distributed as a shared library, and this crate never downloads it, so you
supply an unpacked distribution at build time and copy `webgpu_dawn.*`
beside your executable at run time:

```pwsh
$env:WHITEOUTFLAKES_DAWN_DIR = "C:\path	o\dawn"
cargo run --example viewer --features webgpu -- model.mdx --backend webgpu
```

### Working from a checkout

The crates only build from a clone once the C++ sources have been staged
into them — cargo cannot package files from outside a crate directory:

```pwsh
scripts\vendor-rust.ps1
```

Or skip the from-source build entirely and link a library you already
built:

```pwsh
scripts\build-rust.ps1          # cmake + fmt + clippy + test
```

| Variable | Meaning |
|---|---|
| `WHITEOUTFLAKES_LIB_DIR` | Link a prebuilt `whiteoutflakes_native` from here, skipping CMake |
| `WHITEOUTFLAKES_STATIC` | `1` to link `whiteoutflakes_native_static` instead |

## The example viewer

```pwsh
scripts\build-rust.ps1                 # builds the native lib and stages shaders
cd bindings\rust
cargo run --example viewer -- "path\to\model.mdx"
```

A winit window with the model in it: left-drag orbits, right-drag pans,
wheel zooms. `--frames N` renders N frames, prints what the last one
drew, and exits — a self-checking mode that needs nobody to close the
window.

Windows-only: `create_swap_chain_target` takes an HWND here, and the
Linux / macOS equivalents (`VkSurfaceKHR`, `NSWindow`) would mean an
`ash`/`objc` dependency the example declines to take. The bindings
themselves are platform-neutral.

The example calls `Renderer::use_bundled_shaders()` before
`init_device`, which points the renderer at the shader pack
`whiteoutflakes-sys` staged at build time. Any host has to do the
equivalent: the C++ default search root is the executable's directory,
which is never right for a library.

## Picking a backend

`Renderer::preferred_backend()` returns the best backend this build can
use here — D3D12 then D3D11 on Windows, Metal on macOS, Vulkan elsewhere,
matching the C++ viewer's own order — skipping anything not compiled in or
missing its shader pack. `None` means nothing usable, which is honest to
report rather than discovering it inside `init_device`.

```rust,no_run
# use whiteoutflakes::Renderer;
let mut r = Renderer::new();
let api = r.preferred_backend().expect("no usable gfx backend");
r.use_bundled_shaders();
r.pipeline().unwrap().init_device(api);
```

## Usage

```rust,no_run
use whiteoutflakes::{GfxApi, Renderer};

let mut r = Renderer::new();
r.pipeline().unwrap().init_device(GfxApi::D3D11);

// `hwnd` is your window handle; see PipelineView::create_swap_chain_target
// for the platform contract.
# let hwnd = 0usize;
let target = unsafe { r.pipeline().unwrap().create_swap_chain_target(hwnd, 1280, 720) };
r.pipeline().unwrap().set_primary_target(target);

let _actor = r.loader().unwrap().spawn_unit("Units/Human/Footman/Footman.mdx");

loop {
    r.tick(1.0 / 60.0);
    r.pipeline().unwrap().render_frame(target);
    r.pipeline().unwrap().present(target);
    # break;
}
```

## Caveats

**Views don't borrow.** `Renderer::pipeline()` and friends hand back a
raw pointer into the renderer's internals with no lifetime attached.
Dropping the renderer while a view is alive is undefined behaviour that
the compiler will not catch. Keep views at the call site.

**Not `Sync`.** The renderer is single-threaded by design. Handles are
`Send`, so a renderer can be moved between threads, but never shared.

**`init_device` first.** `Renderer::tick`, every `AssetsView` method and
`ReplaceablesView` reach subsystems the C++ side only builds during
device init; calling them before then crashes. This is a precondition of
the underlying library, not something the bindings add.

**Loading both this and the `whiteout` crate.** Both native libraries
export `whiteout_Bytes_free` / `whiteout_CString_free` from the same
generated common TU. The implementations are identical, so whichever the
linker picks behaves correctly — but the two libraries must be built
against the same C++ runtime, since a buffer allocated by one may be
freed through the other's copy of the function.

## Coverage

The bindings cover the public renderer surface with the exceptions listed
at the bottom of `src/flakes.rs` under "Not yet bound". Broadly: the
host-implemented interfaces (`IContentProvider`, `ISoundEmitter`,
`IModelSource`) need trampolines the generator doesn't synthesise, and
`LoaderView::update_materials` needs `std::vector<T>` parameter support.

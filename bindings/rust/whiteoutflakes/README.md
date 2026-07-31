# whiteoutflakes

Rust bindings for the [WhiteoutFlakes](https://github.com/Sahmkow/WhiteoutFlakes)
renderer service library — a Warcraft III model renderer with SD and HD
(Reforged PBR) material paths.

`src/flakes.rs` is generated from the annotated C++ headers in
`include/whiteout/flakes/`; `src/shims.rs` and `src/support.rs` are
hand-written. See [BINDINGS.md](../../../BINDINGS.md) for the pipeline.

## Building

The crate links a native library it does not build. Point it at one:

```pwsh
cmake -S . -B build-rust -DWDX_BUILD_C_BINDINGS=ON
cmake --build build-rust --config Release --target whiteoutflakes_c
$env:WHITEOUTFLAKES_LIB_DIR = "build-rust/c-dist/Release"
cargo test
```

`scripts/build-rust.ps1` does all of the above, plus `cargo fmt --check`
and clippy.

| Variable | Meaning |
|---|---|
| `WHITEOUTFLAKES_LIB_DIR` | Directory holding `whiteoutflakes_native` (required) |
| `WHITEOUTFLAKES_STATIC` | `1` to link `whiteoutflakes_native_static` instead |

Without either, `build.rs` falls back to `pkg-config --libs whiteoutflakes`
and then fails with instructions.

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

The example needs `shaders/` beside its binary — the BLS cache resolves
shader bundles against the *executable* directory. `build-rust.ps1`
stages it into `target/debug/examples/`; without it `init_device` fails.

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

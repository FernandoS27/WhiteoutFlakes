# SPDX-License-Identifier: BSD-3-Clause
"""The renderer service surface — `include/whiteout/flakes/`.

One module rather than the four sketched in BINDINGS.md §4. The C emitter
resolves cross-class references against `_known_class_short_names(module)`,
so a method returning a type bound in a *different* module is silently
dropped. Nearly every call here crosses that line — `Renderer::Camera()`
returns a `CameraView`, `ShadowView::Params()` a `ShadowParams`,
`ActorView::Sequences()` a `SequenceInfo` — so splitting the surface would
mean losing most of it. Split only if a genuinely independent subsystem
(event data, model authoring) shows up.

Deliberately absent:

  - `content_provider.h`, `sound_emitter.h`, `model_source.h` — abstract
    interfaces the host subclasses. That needs trampolines the generator
    doesn't synthesise; Rust would want a `Box<dyn Trait>` vtable shim.
  - `model_types.h` — `MaterialData` / `TextureData` reach the API only
    through `std::vector<T>` parameters, a shape neither emitter models.
  - `event_data.h` — SLK lookup tables, useful but self-contained.
"""

from tools.codegen.ir import ModuleConfig

CONFIG = ModuleConfig(
    name='flakes',
    cpp_namespace='whiteout::flakes',
    js_prefix='Flakes',
    embind_block='flakes',
    headers=[
        'include/whiteout/flakes/types.h',
        'include/whiteout/flakes/gfx_types.h',
        'include/whiteout/flakes/enums.h',
        'include/whiteout/flakes/shadow_params.h',
        'include/whiteout/flakes/display.h',
        'include/whiteout/flakes/util/replaceable_paths.h',
        'include/whiteout/flakes/views.h',
        'include/whiteout/flakes/actor_view.h',
        'include/whiteout/flakes/renderer.h',
        'include/whiteout/flakes/storage_browser.h',
    ],
    include_dirs=[
        'include',
        'externals/WhiteoutLib/include',
    ],
    c_header_output_path='bindings/c/whiteout_flakes.h',
    c_source_output_path='bindings/c/whiteout_flakes.cpp',
    # Embind / pybind11 / .d.ts aren't wired up yet; the fields are
    # required by ModuleConfig, so point them somewhere obvious rather
    # than at a path that looks like it might be real.
    output_path='bindings/wasm/flakes_bindings.cpp',
    # Explicit opt-in only. Auto-binding `whiteout::flakes` would drag in
    # every transitively-included type from the WhiteoutLib headers that
    # `types.h` re-exports.
    auto_bind=False,
)

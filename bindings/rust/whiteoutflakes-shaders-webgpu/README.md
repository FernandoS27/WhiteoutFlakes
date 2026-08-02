# whiteoutflakes-shaders-webgpu

Prebuilt WhiteoutFlakes BLS shader bundles for the WebGPU (WGSL) backend.

Data only. Pulled in as a build-dependency by `whiteoutflakes-sys`; you do
not depend on this crate directly.

The WebGPU *backend* additionally needs a Dawn distribution at build time —
see `whiteoutflakes-sys`'s `webgpu` feature. This crate is only the shaders.

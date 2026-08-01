# whiteoutflakes-shaders-d3d11

Prebuilt WhiteoutFlakes BLS shader bundles for the Direct3D 11 (DXBC) backend.

Data only. Pulled in as a build-dependency by `whiteoutflakes-sys`, which
merges the enabled packs into one search root; you do not depend on this
crate directly.

The packs are split per backend because crates.io caps a `.crate` tarball
at 10 MiB and the combined pack is 27.6 MB compressed.

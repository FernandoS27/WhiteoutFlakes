# whiteoutflakes-sys

Builds and links the native [WhiteoutFlakes](https://github.com/Sahmkow/WhiteoutFlakes)
renderer, and stages its shader pack. Use
[`whiteoutflakes`](https://crates.io/crates/whiteoutflakes) instead.

## What it builds

A static `whiteoutflakes_native` from the vendored C++, via CMake. The
vendored build is deliberately narrower than the repository's own: no Dear
ImGui, Tracy, GLFW, cubeb or Dawn, and — by default — no Vulkan, Metal,
CASC or MPQ. It needs a C++20 compiler and CMake 3.20+, and takes several
minutes.

## Features

| Feature | Effect |
|---|---|
| `d3d11` *(default)* | D3D11 backend + its shader pack (~1 MB) |
| `d3d12` | D3D12 backend + its shader pack (~9 MB) |
| `vulkan` | Vulkan backend; needs a Vulkan SDK on the build machine |
| `metal` | Metal shader pack only — the backend's Objective-C++ sources are not vendored, so `GfxApi::Metal` will not initialise. The build script warns. |
| `casc` | CASC storage, so assets resolve out of an installed Warcraft III; needs CascLib |

Without `casc` the renderer reads loose files only — models referencing
game content will come up untextured.

## Escape hatches

`WHITEOUTFLAKES_LIB_DIR` links a library you built yourself and skips
CMake entirely (add `WHITEOUTFLAKES_STATIC=1` for the static one). Failing
that it tries `pkg-config --libs whiteoutflakes`.

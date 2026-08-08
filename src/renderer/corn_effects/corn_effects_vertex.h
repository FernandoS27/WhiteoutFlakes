#pragma once

#include "renderer/types.h"

namespace whiteout::flakes::renderer::corn_effects {

// One corn-fx vertex, laid out as a superset of every popcorn VS permutation
// we select. The popcorn shader trims its input signature per perm (unlike the
// geoset shaders, which keep the full ATTR0..7 set), and a layout may provide
// more elements than the signature declares — so a single superset layout
// covers BasicUV, atlas and random-bearing perms without a layout per perm.
//
// The first 64 bytes are unchanged from the original BasicUV-only struct:
// position/color/uv0/pivot keep their offsets, and `random` occupies what used
// to be padding. modeSlot4/modeSlot5 are appended.
struct CornEffectsVertex {
    Vector3f position; // ATTR0
    f32 _pad0;
    Vector4f color;   // ATTR2
    Vector2f uv0;     // ATTR3 — BasicUV sample point, or atlas frame A
    f32 random;       // ATTR6 — AlphaRemap LUT V axis
    f32 _pad1;
    Vector4f pivot;     // ATTR8
    Vector4f modeSlot4; // ATTR4 — atlas frame B UV in .xy
    Vector4f modeSlot5; // ATTR5 — atlas blend cursor in .x
};

static_assert(sizeof(CornEffectsVertex) == 96);
static_assert(offsetof(CornEffectsVertex, position) == 0x00);
static_assert(offsetof(CornEffectsVertex, color) == 0x10);
static_assert(offsetof(CornEffectsVertex, uv0) == 0x20);
static_assert(offsetof(CornEffectsVertex, random) == 0x28);
static_assert(offsetof(CornEffectsVertex, pivot) == 0x30);
static_assert(offsetof(CornEffectsVertex, modeSlot4) == 0x40);
static_assert(offsetof(CornEffectsVertex, modeSlot5) == 0x50);

} // namespace whiteout::flakes::renderer::corn_effects

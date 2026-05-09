#pragma once

#include "renderer/types.h"

namespace whiteout::flakes::renderer::corn_effects {

struct CornEffectsVertex {
    Vector3f position;
    f32      _pad0;
    Vector4f color;
    Vector2f uv0;
    f32      _pad1[2];
    Vector4f pivot;
};

static_assert(sizeof(CornEffectsVertex) == 64);
static_assert(offsetof(CornEffectsVertex, position) == 0x00);
static_assert(offsetof(CornEffectsVertex, color)    == 0x10);
static_assert(offsetof(CornEffectsVertex, uv0)      == 0x20);
static_assert(offsetof(CornEffectsVertex, pivot)    == 0x30);

}

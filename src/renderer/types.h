#pragma once

#include "whiteout/flakes/types.h"

#include <whiteout/vector_types.h>

#include <cmath>
#include <vector>
#include <string>
#include <mutex>
#include <thread>
#include <atomic>
#include <algorithm>
#include <cstring>

using whiteout::Vector2f;
using whiteout::Vector3f;
using whiteout::Vector4f;
using whiteout::Matrix44f;
using whiteout::Quaternion;

namespace whiteout::flakes::renderer {

struct Vertex {
    Vector3f position;
    Vector3f normal;
    Vector4f color;
    Vector2f uv;
};
static_assert(sizeof(Vertex) == 48);

struct MeshVertexSD {
    Vector3f position;
    Vector3f normal;
    Vector2f uv0;
    Vector2f uv1;
};
static_assert(sizeof(MeshVertexSD) == 40);

struct BoneVertex {
    u8 weights[4];
    u8 indices[4];
};
static_assert(sizeof(BoneVertex) == 8);

struct alignas(16) CBPerFrame {
    Matrix44f world;
    Matrix44f view;
    Matrix44f projection;
    Vector4f lightDir;
    Vector4f lightColor;
    Vector4f ambientColor;
    Vector4f extraParams;
    Vector4f texAnimParams;
    Vector4f materialFlags;
};

}

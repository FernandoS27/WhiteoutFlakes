#pragma once

#include "gfx/gfx.h"
#include "types.h"
#include "whiteout/flakes/event_data.h"
#include "whiteout/flakes/model_types.h"
#include "whiteout/flakes/types.h"

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace whiteout::flakes::io {
class IContentProvider;
}
namespace whiteout::flakes::renderer::assets {
class TextureAssetManager;
}

namespace whiteout::flakes::renderer::particle {

struct Splat {

    Vector3f corners[4];

    gfx::TextureHandle texture = gfx::TextureHandle::Invalid;
    i32 blendMode = 0;
    bool isUbr = false;

    f32 t0 = 0.f, t1 = 0.f, t2 = 0.f;
    f32 total = 0.f;

    f32 c[3][4] = {{1, 1, 1, 1}, {1, 1, 1, 1}, {1, 1, 1, 1}};

    i32 columns = 1;
    i32 rows = 1;
    i32 uvLifeStart = 0, uvLifeEnd = 0, lifespanRepeat = 1;
    i32 uvDecayStart = 0, uvDecayEnd = 0, decayRepeat = 1;

    f32 age = 0.f;
};

struct SplatDrawList {
    i32 vertexOffset = 0;
    i32 vertexCount = 0;
    gfx::TextureHandle texture = gfx::TextureHandle::Invalid;
    i32 blendMode = 0;
};

class SplatService {
public:
    SplatService();
    ~SplatService();

    void Configure(gfx::IGFXDevice* gfx, assets::TextureAssetManager* textures,
                   io::IContentProvider* contentProvider);

    void Tick();

    void Clear();

    void SpawnSpl(const io::SplEntry& entry, const Vector3f& worldOrigin,
                  const Vector3f& worldRight, const Vector3f& worldForward);

    void SpawnUbr(const io::UbrEntry& entry, const Vector3f& worldOrigin,
                  const Vector3f& worldRight, const Vector3f& worldForward);

    void BuildGeometry(std::vector<Vertex>& outVertices,
                       std::vector<SplatDrawList>& outDrawLists) const;

    i32 Count() const;

private:
    gfx::TextureHandle GetOrLoadTexture(const std::string& path);

    static void BuildCorners(Vector3f corners[4], const Vector3f& origin, const Vector3f& right,
                             const Vector3f& forward);

    static void EvaluateAt(const Splat& s, f32 outColor[4], i32& outCellIdx);

    static void CellToUV(i32 cellIdx, i32 columns, i32 rows, f32& u0, f32& v0, f32& u1, f32& v1);

    mutable std::mutex mutex_;
    std::vector<Splat> splats_;

    gfx::IGFXDevice* gfx_ = nullptr;
    assets::TextureAssetManager* textures_ = nullptr;
    io::IContentProvider* content_ = nullptr;

    std::unordered_map<std::string, gfx::TextureHandle> textureCache_;

    // -1 means "no prior sample" — first Tick() seeds the timer.
    i64 lastTickNs_ = -1;
};

} // namespace whiteout::flakes::renderer::particle

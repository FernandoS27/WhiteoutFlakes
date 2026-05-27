#pragma once

#include "gfx/gfx.h"
#include "types.h"
#include "whiteout/flakes/event_data.h"
#include "whiteout/flakes/model_types.h"
#include "whiteout/flakes/types.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace whiteout::flakes::renderer::assets {
class AssetManager;
}

namespace whiteout::flakes::renderer::particle {

struct Splat {

    Vector3f corners[4];

    // Texture slot acquired from AssetManager — the GPU handle is resolved
    // at BuildGeometry time via TextureOf(slot). Slot starts as the shared
    // placeholder until the host's pump fetches the bytes and CommitPrepared
    // swaps in the real texture; the splat then renders correctly with no
    // further coordination.
    u32 textureSlot = 0; // AssetManager::kInvalidSlot
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

    void Configure(assets::AssetManager* assets);

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
    u32 AcquireTexture(const std::string& path);
    void ReleaseSplat(Splat& s);

    static void BuildCorners(Vector3f corners[4], const Vector3f& origin, const Vector3f& right,
                             const Vector3f& forward);

    static void EvaluateAt(const Splat& s, f32 outColor[4], i32& outCellIdx);

    static void CellToUV(i32 cellIdx, i32 columns, i32 rows, f32& u0, f32& v0, f32& u1, f32& v1);

    mutable std::mutex mutex_;
    std::vector<Splat> splats_;

    assets::AssetManager* assets_ = nullptr;

    // -1 means "no prior sample" — first Tick() seeds the timer.
    i64 lastTickNs_ = -1;
};

} // namespace whiteout::flakes::renderer::particle

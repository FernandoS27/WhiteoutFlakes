#pragma once

#include "gfx/gfx.h"
#include "types.h"
#include "whiteout/flakes/event_data.h"
#include "whiteout/flakes/model_types.h"
#include "whiteout/flakes/types.h"

#include <mutex>
#include <string>
#include <vector>

namespace whiteout::flakes::renderer::assets {
class AssetManager;
}

namespace whiteout::flakes::renderer::effects {

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

namespace detail {
/// @brief The sprite cell of an SPL key at nudged time @p t —
///        `CSplatKey::Interpolate` @0x141FE5BE0.
///
/// The key is built by @0x141FE73F0: a reversed range starts one past @p start
/// and sweeps one past @p end, the same sweep the particle cell uses. A
/// @p repeat of 1 sweeps once, anything else wraps `repeat * t`. The result is
/// clamped to a byte, not to the range.
i32 SplatCell(i32 start, i32 end, i32 repeat, f32 t);
} // namespace detail

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

    // `dt` is the scene's simulation delta, the same one every other tick
    // consumer gets — never wall time. See M2_PARTICLE_DESIGN.md §11.15.
    void Tick(f32 dt);

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
    /// False for a finished SPL splat whose end is all but transparent: it keeps
    /// its ring slot and draws nothing.
    static bool Draws(const Splat& s);

    static void CellToUV(i32 cellIdx, i32 columns, i32 rows, f32& u0, f32& v0, f32& u1, f32& v1);

    mutable std::mutex mutex_;
    std::vector<Splat> splats_;

    assets::AssetManager* assets_ = nullptr;

};

} // namespace whiteout::flakes::renderer::effects

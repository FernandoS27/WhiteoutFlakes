#include "renderer/particle/splat_service.h"

#include "assets/texture_asset_manager.h"
#include "gfx/gfx.h"
#include "model/model_source_utils.h"
#include "whiteout/flakes/content_provider.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>

namespace whiteout::flakes::renderer::particle {

using namespace ::whiteout::flakes::renderer::assets;
using namespace ::whiteout::flakes::renderer::model;
using namespace ::whiteout::flakes::io;

SplatService::SplatService() = default;
SplatService::~SplatService() = default;

void SplatService::Configure(gfx::IGFXDevice* gfx, TextureAssetManager* textures,
                             IContentProvider* contentProvider) {
    std::lock_guard<std::mutex> lk(mutex_);
    gfx_ = gfx;
    textures_ = textures;
    content_ = contentProvider;
}

void SplatService::Tick() {
    using namespace std::chrono;
    const i64 nowNs = duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();

    std::lock_guard<std::mutex> lk(mutex_);

    if (lastTickNs_ < 0) {
        lastTickNs_ = nowNs;
        return;
    }

    const f64 dtSec = (f64)(nowNs - lastTickNs_) * 1e-9;
    lastTickNs_ = nowNs;

    const f32 dt = (f32)std::min(dtSec, 0.5);
    if (dt <= 0.f)
        return;

    auto it = splats_.begin();
    while (it != splats_.end()) {
        it->age += dt;
        if (it->age >= it->total) {
            it = splats_.erase(it);
        } else {
            ++it;
        }
    }
}

void SplatService::Clear() {
    std::lock_guard<std::mutex> lk(mutex_);
    splats_.clear();

    if (gfx_) {
        for (auto& [path, tex] : textureCache_) {
            if (tex != gfx::TextureHandle::Invalid)
                gfx_->Destroy(tex);
        }
    }
    textureCache_.clear();
    lastTickNs_ = -1;
}

i32 SplatService::Count() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return (i32)splats_.size();
}

void SplatService::BuildCorners(Vector3f corners[4], const Vector3f& origin, const Vector3f& right,
                                const Vector3f& forward) {

    corners[0] = {origin.x + right.x + forward.x, origin.y + right.y + forward.y,
                  origin.z + right.z + forward.z};
    corners[1] = {origin.x - right.x + forward.x, origin.y - right.y + forward.y,
                  origin.z - right.z + forward.z};
    corners[2] = {origin.x - right.x - forward.x, origin.y - right.y - forward.y,
                  origin.z - right.z - forward.z};
    corners[3] = {origin.x + right.x - forward.x, origin.y + right.y - forward.y,
                  origin.z + right.z - forward.z};
}

void SplatService::SpawnSpl(const io::SplEntry& entry, const Vector3f& worldOrigin,
                            const Vector3f& worldRight, const Vector3f& worldForward) {
    Splat s;
    BuildCorners(s.corners, worldOrigin, worldRight, worldForward);
    s.texture = GetOrLoadTexture(entry.file);
    s.blendMode = entry.blendMode;
    s.isUbr = false;
    s.t0 = entry.lifespan;
    s.t1 = entry.decay;
    s.t2 = 0.f;
    s.total = s.t0 + s.t1;

    if (s.total <= 0.f)
        return;
    std::memcpy(s.c[0], entry.startC, sizeof(f32) * 4);
    std::memcpy(s.c[1], entry.midC, sizeof(f32) * 4);
    std::memcpy(s.c[2], entry.endC, sizeof(f32) * 4);
    s.columns = std::max(1, entry.columns);
    s.rows = std::max(1, entry.rows);
    s.uvLifeStart = entry.uvLifeStart;
    s.uvLifeEnd = entry.uvLifeEnd;
    s.lifespanRepeat = std::max(1, entry.lifespanRepeat);
    s.uvDecayStart = entry.uvDecayStart;
    s.uvDecayEnd = entry.uvDecayEnd;
    s.decayRepeat = std::max(1, entry.decayRepeat);
    s.age = 0.f;

    std::lock_guard<std::mutex> lk(mutex_);
    splats_.push_back(s);
}

void SplatService::SpawnUbr(const io::UbrEntry& entry, const Vector3f& worldOrigin,
                            const Vector3f& worldRight, const Vector3f& worldForward) {
    Splat s;
    BuildCorners(s.corners, worldOrigin, worldRight, worldForward);
    s.texture = GetOrLoadTexture(entry.file);
    s.blendMode = entry.blendMode;
    s.isUbr = true;
    s.t0 = entry.birthTime;
    s.t1 = entry.pauseTime;
    s.t2 = entry.decay;
    s.total = s.t0 + s.t1 + s.t2;

    if (s.total <= 0.f)
        return;
    std::memcpy(s.c[0], entry.c[0], sizeof(f32) * 4);
    std::memcpy(s.c[1], entry.c[1], sizeof(f32) * 4);
    std::memcpy(s.c[2], entry.c[2], sizeof(f32) * 4);

    s.columns = s.rows = 1;
    s.uvLifeStart = s.uvLifeEnd = 0;
    s.uvDecayStart = s.uvDecayEnd = 0;
    s.lifespanRepeat = s.decayRepeat = 1;
    s.age = 0.f;

    std::lock_guard<std::mutex> lk(mutex_);
    splats_.push_back(s);
}

namespace {
inline f32 ClampF(f32 v, f32 lo, f32 hi) {
    return std::max(lo, std::min(hi, v));
}

void Lerp4(f32 out[4], const f32 a[4], const f32 b[4], f32 t) {
    out[0] = a[0] + (b[0] - a[0]) * t;
    out[1] = a[1] + (b[1] - a[1]) * t;
    out[2] = a[2] + (b[2] - a[2]) * t;
    out[3] = a[3] + (b[3] - a[3]) * t;
}
} // namespace

void SplatService::EvaluateAt(const Splat& s, f32 outColor[4], i32& outCellIdx) {
    outCellIdx = -1;
    const f32 age = ClampF(s.age, 0.f, s.total);

    if (s.isUbr) {

        if (age < s.t0 && s.t0 > 0.f) {
            Lerp4(outColor, s.c[0], s.c[1], age / s.t0);
        } else if (age < s.t0 + s.t1) {
            std::memcpy(outColor, s.c[1], sizeof(f32) * 4);
        } else if (s.t2 > 0.f) {
            Lerp4(outColor, s.c[1], s.c[2], (age - s.t0 - s.t1) / s.t2);
        } else {
            std::memcpy(outColor, s.c[2], sizeof(f32) * 4);
        }
        return;
    }

    auto cellOf = [](i32 start, i32 end, i32 repeat, f32 t) {
        const f32 nudge = t * 0.99f + 0.005f;
        const f32 r = (repeat < 1) ? 1.0f : (f32)repeat;
        const i32 delta = (end >= start) ? (end - start + 1) : (end - start - 1);
        const f32 effT = (r == 1.0f) ? nudge : std::fmod(nudge * r, 1.0f);
        const f32 val = (f32)start + (f32)delta * effT;
        return (i32)val;
    };

    if (age < s.t0 && s.t0 > 0.f) {
        const f32 t = age / s.t0;
        Lerp4(outColor, s.c[0], s.c[1], t);
        outCellIdx = cellOf(s.uvLifeStart, s.uvLifeEnd, s.lifespanRepeat, t);
    } else {
        const f32 t = (s.t1 > 0.f) ? ((age - s.t0) / s.t1) : 1.f;
        Lerp4(outColor, s.c[1], s.c[2], t);
        outCellIdx = cellOf(s.uvDecayStart, s.uvDecayEnd, s.decayRepeat, t);
    }
}

void SplatService::CellToUV(i32 cellIdx, i32 columns, i32 rows, f32& u0, f32& v0, f32& u1,
                            f32& v1) {
    if (cellIdx < 0 || (columns <= 1 && rows <= 1)) {
        u0 = 0.f;
        v0 = 0.f;
        u1 = 1.f;
        v1 = 1.f;
        return;
    }
    columns = std::max(1, columns);
    rows = std::max(1, rows);
    const i32 cells = columns * rows;
    const i32 idx = ((cellIdx % cells) + cells) % cells;
    const i32 cx = idx % columns;
    const i32 cy = idx / columns;
    const f32 du = 1.0f / (f32)columns;
    const f32 dv = 1.0f / (f32)rows;
    u0 = cx * du;
    v0 = cy * dv;
    u1 = u0 + du;
    v1 = v0 + dv;
}

void SplatService::BuildGeometry(std::vector<Vertex>& outVertices,
                                 std::vector<SplatDrawList>& outDrawLists) const {
    std::lock_guard<std::mutex> lk(mutex_);
    outVertices.reserve(outVertices.size() + splats_.size() * 6);
    outDrawLists.reserve(outDrawLists.size() + splats_.size());

    for (const auto& s : splats_) {
        f32 color[4];
        i32 cellIdx = -1;
        EvaluateAt(s, color, cellIdx);

        f32 u0, v0, u1, v1;
        CellToUV(cellIdx, s.columns, s.rows, u0, v0, u1, v1);

        const Vector3f n{0.f, 0.f, 1.f};
        const Vector4f c{color[0], color[1], color[2], color[3]};

        const i32 base = (i32)outVertices.size();
        outVertices.push_back({s.corners[0], n, c, {u0, v0}});
        outVertices.push_back({s.corners[1], n, c, {u0, v1}});
        outVertices.push_back({s.corners[2], n, c, {u1, v1}});
        outVertices.push_back({s.corners[0], n, c, {u0, v0}});
        outVertices.push_back({s.corners[2], n, c, {u1, v1}});
        outVertices.push_back({s.corners[3], n, c, {u1, v0}});

        SplatDrawList dl;
        dl.vertexOffset = base;
        dl.vertexCount = 6;
        dl.texture = s.texture;
        dl.blendMode = s.blendMode;
        outDrawLists.push_back(dl);
    }
}

gfx::TextureHandle SplatService::GetOrLoadTexture(const std::string& path) {
    if (path.empty() || !gfx_ || !content_)
        return gfx::TextureHandle::Invalid;

    {
        auto it = textureCache_.find(path);
        if (it != textureCache_.end())
            return it->second;
    }

    std::string foundExt;
    auto data = content_->ReadFile(path, &foundExt);
    if (!data) {
        std::fprintf(stderr, "[splat] ERR: tex read FAIL '%s'\n", path.c_str());
#if !defined(__EMSCRIPTEN__)
        // Desktop: ReadFile is a synchronous CASC/MPQ/disk read. A miss
        // here is permanent — pin the Invalid so subsequent SpawnSpl
        // calls don't re-issue I/O per spawn (that's a ~60 FPS regression
        // on models with missing splat textures).
        textureCache_.emplace(path, gfx::TextureHandle::Invalid);
#endif
        return gfx::TextureHandle::Invalid;
    }
    if (foundExt.empty())
        foundExt = ExtensionLower(std::filesystem::path(path));

    std::vector<u8> rgba;
    i32 w = 0, h = 0;
    if (!DecodeToRGBA8(*data, foundExt, rgba, w, h) || w <= 0 || h <= 0) {
        std::fprintf(stderr, "[splat] ERR: tex decode FAIL '%s' ext='%s' bytes=%zu\n", path.c_str(),
                     foundExt.c_str(), data->size());
#if !defined(__EMSCRIPTEN__)
        // Decode failures are deterministic (the bytes won't suddenly
        // become valid). Pin so we don't redecode every spawn.
        textureCache_.emplace(path, gfx::TextureHandle::Invalid);
#endif
        return gfx::TextureHandle::Invalid;
    }

    gfx::TextureDesc td;
    td.width = w;
    td.height = h;
    td.mipLevels = 1;
    td.format = gfx::Format::R8G8B8A8_UNORM;
    td.usage = gfx::TextureUsage::ShaderResource;
    auto tex = gfx_->CreateTexture(td, rgba.data());
    textureCache_.emplace(path, tex);
    return tex;
}

} // namespace whiteout::flakes::renderer::particle

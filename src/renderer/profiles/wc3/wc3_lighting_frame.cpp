#include "profiles/wc3/wc3_lighting_frame.h"

#include "whiteout/flakes/util/coordinate_system.h"

#include <algorithm>
#include <cmath>

namespace whiteout::flakes::renderer::profiles::wc3 {

namespace {

constexpr f32 kOneStep = 1.0f / 255.0f;

// The radius GxuLight_AttenuationCullRadius returns for a light with no falloff.
constexpr f32 kUnboundedRadius = 100000.0f;

f32 ByteChannel(f32 v) {
    const f32 c = std::clamp(v, 0.0f, 1.0f);
    return static_cast<f32>(static_cast<u8>(static_cast<i32>(c * 255.0f))) * kOneStep;
}

} // namespace

Vector3f EngineLightColor(const Vector3f& rgb) {
    return {ByteChannel(rgb.x), ByteChannel(rgb.y), ByteChannel(rgb.z)};
}

f32 ClusterCullRadius(f32 radiance, f32 q, f32 l, f32 e) {
    if (radiance <= 0.0f || kOneStep / radiance >= 1.0f)
        return 0.0f;
    const f32 target = std::max(0.000001f, kOneStep / radiance);

    if (l <= 0.0f && q <= 0.0f) {
        if (e <= 0.0f)
            return kUnboundedRadius;
        return std::sqrt(-std::log(target) / e);
    }

    // 1 + l d + q d^2 = 1 / target, the rational part alone.
    const f32 rhs = 1.0f / target - 1.0f;
    f32 d;
    if (q <= 0.0f) {
        d = rhs * (1.0f / (l != 0.0f ? l : 0.000001f));
    } else {
        const f32 disc = std::max(0.0f, q * 4.0f * rhs + l * l);
        d = (std::sqrt(disc) - l) / (q + q);
    }
    d = std::max(d, 0.0f);
    if (e <= 0.0f)
        return d;

    // Newton on f(d) = -e d^2 - log(1 + l d + q d^2) - log(target), started at
    // the smaller of the two single-term radii, each step clamped to 1 + d/2.
    const f32 logTarget = std::log(target);
    f32 x = std::min(std::sqrt(-logTarget / e), d);
    for (i32 i = 0; i < 8; ++i) {
        const f32 denom = x * l + 1.0f + x * x * q;
        if (denom <= 0.0f)
            break;
        const f32 f = -e * (x * x) - std::log(denom) - logTarget;
        if (std::abs(f) < 0.0001f)
            break;
        const f32 df = e * -2.0f * x - ((q + q) * x + l) / denom;
        if (std::abs(df) < 1.0e-10f)
            break;
        const f32 limit = x * 0.5f + 1.0f;
        const f32 step = std::clamp(f / df, -limit, limit);
        x = std::max(x - step, 0.0f);
    }
    return x;
}

bls::HdClusterLight PackClusterLight(const LightState& light, const Matrix44f& view) {
    bls::HdClusterLight out{};
    const Vector3f c = EngineLightColor(
        light.dirIntensity > 0.0f
            ? Vector3f{light.diffuse.x / light.dirIntensity, light.diffuse.y / light.dirIntensity,
                       light.diffuse.z / light.dirIntensity}
            : Vector3f{0, 0, 0});
    out.colorR = c.x * light.dirIntensity;
    out.colorG = c.y * light.dirIntensity;
    out.colorB = c.z * light.dirIntensity;
    out.shadowIdx = bls::IntBits(-1);
    const Vector3f p = whiteout::transform_point(light.worldPos, view);
    out.posX = p.x;
    out.posY = p.y;
    out.posZ = p.z;
    out.quadAtten = light.quadraticFalloff;
    out.linAtten = light.linearFalloff;
    out.expAtten = light.damping;
    return out;
}

namespace {

// The clustered records and their cull radii for every light the set can use.
void GatherClusterLights(std::span<const LightState> lights, const Matrix44f& view,
                         std::vector<bls::HdClusterLight>& records, std::vector<f32>& radii,
                         std::span<const Vector3f> shadowSlotPositions = {}) {
    records.clear();
    radii.clear();
    for (const LightState& L : lights) {
        if (!L.enabled || L.kind != model::FrameState::LightKind::Omni)
            continue;
        if (records.size() >= 0xFFFF) // indices are 16-bit
            break;
        bls::HdClusterLight rec = PackClusterLight(L, view);
        if (L.shadowCasting) {
            i32 slot = -2;
            for (usize s = 0; s < shadowSlotPositions.size(); ++s) {
                const Vector3f& p = shadowSlotPositions[s];
                const f32 dx = p.x - L.worldPos.x, dy = p.y - L.worldPos.y, dz = p.z - L.worldPos.z;
                if (dx * dx + dy * dy + dz * dz < 1.0e-4f) {
                    slot = static_cast<i32>(s);
                    break;
                }
            }
            rec.shadowIdx = bls::IntBits(slot);
        }
        const f32 radiance =
            std::sqrt(rec.colorR * rec.colorR + rec.colorG * rec.colorG + rec.colorB * rec.colorB);
        const f32 radius = ClusterCullRadius(radiance, rec.quadAtten, rec.linAtten, rec.expAtten);
        if (radius <= 0.0f)
            continue;
        records.push_back(rec);
        radii.push_back(radius);
    }
}

// Append one tile's list to the half-u32 index stream; returns its slot offset.
u32 AppendTileList(std::span<const u16> list, std::vector<u32>& indices, u32& slotCursor) {
    const u32 offset = slotCursor;
    for (u16 idx : list) {
        const u32 slot = slotCursor++;
        if ((slot >> 1) >= indices.size())
            indices.push_back(0u);
        indices[slot >> 1] |= (slot & 1u) ? (static_cast<u32>(idx) << 16) : idx;
    }
    return offset;
}

// A structured buffer cannot be empty; a zero count keeps the padding record unread.
void PadEmptyBuffers(ClusterSet& out) {
    out.lightCount = static_cast<u32>(out.lights.size());
    if (out.lights.empty())
        out.lights.push_back({});
    if (out.indices.empty())
        out.indices.push_back(0u);
}

} // namespace

void BuildSingleTileClusterSet(std::span<const LightState> lights, const Matrix44f& view,
                               ClusterSet& out) {
    std::vector<f32> radii;
    GatherClusterLights(lights, view, out.lights, radii);
    out.indices.clear();
    out.tiles.clear();
    out.gridWidth = 1;
    out.gridHeight = 1;

    const u32 count = std::min<u32>(static_cast<u32>(out.lights.size()), bls::kClusterMaxCount);
    std::vector<u16> all(count);
    for (u32 i = 0; i < count; ++i)
        all[i] = static_cast<u16>(i);
    u32 cursor = 0;
    const u32 offset = AppendTileList(all, out.indices, cursor);
    out.tiles.push_back((offset << bls::kClusterCountBits) | count);
    PadEmptyBuffers(out);
}

void ClusterGridDims(u32 viewportWidth, u32 viewportHeight, u32& gridWidth, u32& gridHeight) {
    constexpr u32 kTilePixels = 16;
    constexpr u32 kMaxLongSide = 400;
    const u32 w = std::max(viewportWidth, 1u);
    const u32 h = std::max(viewportHeight, 1u);
    const u32 longPx = std::max(w, h);
    const u32 shortPx = std::min(w, h);
    const u32 longTiles = std::min(kMaxLongSide, (longPx + kTilePixels - 1) / kTilePixels);
    const u32 shortTiles = std::max<u32>(
        1u, static_cast<u32>(std::ceil(static_cast<double>(longTiles) * shortPx / longPx)));
    gridWidth = (w >= h) ? longTiles : shortTiles;
    gridHeight = (w >= h) ? shortTiles : longTiles;
}

void BuildBinnedClusterSet(std::span<const LightState> lights, const Matrix44f& view,
                           const Matrix44f& projection, u32 viewportWidth, u32 viewportHeight,
                           ClusterSet& out, std::span<const Vector3f> shadowSlotPositions) {
    std::vector<f32> radii;
    GatherClusterLights(lights, view, out.lights, radii, shadowSlotPositions);
    out.indices.clear();
    out.tiles.clear();
    ClusterGridDims(viewportWidth, viewportHeight, out.gridWidth, out.gridHeight);
    const u32 gw = out.gridWidth;
    const u32 gh = out.gridHeight;

    // D3D LH projection, row vectors: z' = z P[2][2] + P[3][2] with
    // P[2][2] = f / (f - n) and P[3][2] = -n f / (f - n), so the near plane is
    // -P[3][2] / P[2][2].
    const f32 nearZ = (projection.data[2][2] != 0.0f)
                          ? -projection.data[3][2] / projection.data[2][2]
                          : 0.0f;

    // Tile rectangle per light, inclusive, or empty when fully off screen.
    std::vector<std::vector<u16>> tileLists(static_cast<usize>(gw) * gh);
    for (u32 li = 0; li < out.lights.size(); ++li) {
        const auto& rec = out.lights[li];
        const f32 r = radii[li];
        const Vector3f c = {rec.posX, rec.posY, rec.posZ};
        u32 x0 = 0, y0 = 0, x1 = gw - 1, y1 = gh - 1;
        if (c.z + r < nearZ)
            continue; // entirely behind the camera
        if (c.z - r > nearZ && r < 50000.0f) {
            f32 minX = 1e30f, minY = 1e30f, maxX = -1e30f, maxY = -1e30f;
            for (i32 k = 0; k < 8; ++k) {
                const Vector3f p = {c.x + ((k & 1) ? r : -r), c.y + ((k & 2) ? r : -r),
                                    c.z + ((k & 4) ? r : -r)};
                const f32 cx = p.x * projection.data[0][0] + p.y * projection.data[1][0] +
                               p.z * projection.data[2][0] + projection.data[3][0];
                const f32 cy = p.x * projection.data[0][1] + p.y * projection.data[1][1] +
                               p.z * projection.data[2][1] + projection.data[3][1];
                const f32 cw = p.x * projection.data[0][3] + p.y * projection.data[1][3] +
                               p.z * projection.data[2][3] + projection.data[3][3];
                if (cw <= 1e-6f)
                    continue;
                minX = std::min(minX, cx / cw);
                maxX = std::max(maxX, cx / cw);
                minY = std::min(minY, cy / cw);
                maxY = std::max(maxY, cy / cw);
            }
            // One pixel of slack: a pixel whose ray meets the sphere can have
            // its centre, which is what picks the tile, half a pixel outside
            // the projection.
            const f32 padX = 2.0f / static_cast<f32>(std::max(viewportWidth, 1u));
            const f32 padY = 2.0f / static_cast<f32>(std::max(viewportHeight, 1u));
            minX -= padX;
            maxX += padX;
            minY -= padY;
            maxY += padY;
            if (maxX < -1.0f || minX > 1.0f || maxY < -1.0f || minY > 1.0f)
                continue;
            // NDC to tiles; y flips (NDC +1 is the top row).
            auto toTile = [](f32 v01, u32 n) {
                return static_cast<u32>(std::clamp(static_cast<i32>(std::floor(v01 * n)), 0,
                                                   static_cast<i32>(n) - 1));
            };
            x0 = toTile((minX * 0.5f + 0.5f), gw);
            x1 = toTile((maxX * 0.5f + 0.5f), gw);
            y0 = toTile((0.5f - maxY * 0.5f), gh);
            y1 = toTile((0.5f - minY * 0.5f), gh);
        }
        for (u32 y = y0; y <= y1; ++y)
            for (u32 x = x0; x <= x1; ++x)
                tileLists[static_cast<usize>(y) * gw + x].push_back(static_cast<u16>(li));
    }

    out.tiles.assign(static_cast<usize>(gw) * gh, 0u);
    u32 cursor = 0;
    for (u32 y = 0; y < gh; ++y) {
        for (u32 x = 0; x < gw; ++x) {
            const usize t = static_cast<usize>(y) * gw + x;
            auto& list = tileLists[t];
            const u32 count = std::min<u32>(static_cast<u32>(list.size()), bls::kClusterMaxCount);
            if (x > 0 && tileLists[t - 1] == list) {
                out.tiles[t] = out.tiles[t - 1];
                continue;
            }
            if (y > 0 && tileLists[t - gw] == list) {
                out.tiles[t] = out.tiles[t - gw];
                continue;
            }
            const u32 offset =
                AppendTileList(std::span<const u16>(list.data(), count), out.indices, cursor);
            out.tiles[t] = (offset << bls::kClusterCountBits) | count;
        }
    }
    PadEmptyBuffers(out);
}

void FillClusterConstants(const ClusterSet& set, f32 viewportWidth, f32 viewportHeight,
                          bls::HdPsClusteredCb& out) {
    out.lightDebugMode = bls::IntBits(0);
    out.invClusterDimX = 1.0f / static_cast<f32>(std::max(set.gridWidth, 1u));
    out.invClusterDimY = 1.0f / static_cast<f32>(std::max(set.gridHeight, 1u));
    out.lightArrayBase = bls::IntBits(0);
    out.lightIndexBase = bls::IntBits(0);
    out.clusterBase = bls::IntBits(0);
    out.gridStride = bls::IntBits(static_cast<i32>(set.gridWidth));
    out.gridRows = bls::IntBits(static_cast<i32>(set.gridHeight));
    out._p41_w = 0.0f;
    out.viewportRect = {0.0f, 0.0f, viewportWidth, viewportHeight};
}

} // namespace whiteout::flakes::renderer::profiles::wc3

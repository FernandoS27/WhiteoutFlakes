#include <cornflakes/render/ribbon_geometry.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace whiteout::cornflakes {

namespace {

constexpr Float3 kZeroVec3{0.0F, 0.0F, 0.0F};
constexpr Float4 kWhite{1.0F, 1.0F, 1.0F, 1.0F};
constexpr Float4 kOnes4{1.0F, 1.0F, 1.0F, 1.0F};

Float3 cameraPositionFromView(const Mat4& view) noexcept {
    const f32 tx = view.m[12];
    const f32 ty = view.m[13];
    const f32 tz = view.m[14];
    return Float3{
        -(view.m[0] * tx + view.m[1] * ty + view.m[2] * tz),
        -(view.m[4] * tx + view.m[5] * ty + view.m[6] * tz),
        -(view.m[8] * tx + view.m[9] * ty + view.m[10] * tz),
    };
}

Float3 sub(const Float3& a, const Float3& b) noexcept {
    return Float3{a.x - b.x, a.y - b.y, a.z - b.z};
}

Float3 cross3(const Float3& a, const Float3& b) noexcept {
    return Float3{
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x,
    };
}

Float3 normalize3(const Float3& v) noexcept {
    const f32 len2 = v.x * v.x + v.y * v.y + v.z * v.z;
    if (len2 <= 1.0e-12F) {
        return Float3{1.0F, 0.0F, 0.0F};
    }
    const f32 inv = 1.0F / std::sqrt(len2);
    return Float3{v.x * inv, v.y * inv, v.z * inv};
}

Float3 add(const Float3& a, const Float3& b) noexcept {
    return Float3{a.x + b.x, a.y + b.y, a.z + b.z};
}

Float3 scale(const Float3& a, f32 s) noexcept {
    return Float3{a.x * s, a.y * s, a.z * s};
}

enum class RibbonMode : u8 {
    ViewposAligned = 0,
    NormalAxisAligned = 1,
    SideAxisAligned = 2,
};

Float3 buildUpAxisViewPos(const Float3& cameraPos, const Float3& c0, const Float3& c1) noexcept {
    const Float3 viewToC0 = sub(c0, cameraPos);
    const Float3 segDir = sub(c1, c0);
    return normalize3(cross3(segDir, viewToC0));
}

Float3 buildUpAxisNormal(const Float3& axis, const Float3& c0, const Float3& c1) noexcept {
    return normalize3(cross3(axis, sub(c1, c0)));
}

Float3 buildUpAxisSide(const Float3& axis) noexcept {
    return normalize3(axis);
}

struct RibbonShadingBasis {
    Float3 normalPlus;
    Float3 normalMinus;
    Float4 tangent;
};

RibbonShadingBasis buildShadingBasis(const Float3& c0, const Float3& c1, const Float3& up,
                                     f32 bend) noexcept {
    const Float3 along = normalize3(sub(c1, c0));
    const Float3 face = cross3(along, up);
    const f32 inv = 1.0F - bend;
    return RibbonShadingBasis{
        Float3{face.x * inv + up.x * bend, face.y * inv + up.y * bend, face.z * inv + up.z * bend},
        Float3{face.x * inv - up.x * bend, face.y * inv - up.y * bend, face.z * inv - up.z * bend},
        Float4{along.x, along.y, along.z, 1.0F},
    };
}

Float3 readFloat3(std::span<const std::byte> bytes, std::size_t i) noexcept {
    if (bytes.empty())
        return kZeroVec3;
    const auto* f = reinterpret_cast<const f32*>(bytes.data());
    return Float3{f[i * 3 + 0], f[i * 3 + 1], f[i * 3 + 2]};
}

f32 readFloat(std::span<const std::byte> bytes, std::size_t i) noexcept {
    if (bytes.empty())
        return 0.0F;
    const auto* f = reinterpret_cast<const f32*>(bytes.data());
    return f[i];
}

Float4 readFloat4(std::span<const std::byte> bytes, std::size_t i, Float4 fallback) noexcept {
    if (bytes.empty())
        return fallback;
    const auto* f = reinterpret_cast<const f32*>(bytes.data());
    return Float4{f[i * 4 + 0], f[i * 4 + 1], f[i * 4 + 2], f[i * 4 + 3]};
}

u8 readEnabled(std::span<const std::byte> bytes, std::size_t i) noexcept {
    if (bytes.empty())
        return 1U;

    const auto* f = reinterpret_cast<const f32*>(bytes.data());
    return f[i] != 0.0F ? 1U : 0U;
}

u64 readU64(std::span<const std::byte> bytes, std::size_t i) noexcept {
    if (bytes.empty())
        return 0U;
    u64 v = 0U;
    std::memcpy(&v, bytes.data() + i * sizeof(u64), sizeof(u64));
    return v;
}

struct RibbonUVBasis {
    f32 mulU, mulV;
    f32 add0U, add0V;
    f32 add1U, add1V;
};

RibbonUVBasis makeUVBasis(const RibbonUVConfig& cfg) noexcept {
    const bool flipUEff = cfg.rotate != cfg.flipU;
    const f32 uflipa = flipUEff ? -1.0F : 1.0F;
    const f32 uflipb = flipUEff ? 1.0F : 0.0F;
    const f32 texv0 = cfg.flipV ? 1.0F : 0.0F;
    const f32 texv1 = 1.0F - texv0;
    if (cfg.rotate) {
        return {0.0F, uflipa, texv0, uflipb, texv1, uflipb};
    }
    return {uflipa, 0.0F, uflipb, texv0, uflipb, texv1};
}

struct RibbonUVFactors {
    Float4 v0, v1, v2, v3;
};

RibbonUVFactors computeUVFactors(const Float3& p0, const Float3& p1, const Float3& p2,
                                 const Float3& p3) noexcept {
    const auto len = [](const Float3& a, const Float3& b) {
        const Float3 d = sub(a, b);
        return std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    };
    const f32 e0 = len(p3, p1);
    const f32 e1 = len(p3, p2);
    const f32 e2 = len(p2, p0);
    const f32 e3 = len(p1, p0);
    const auto ratio = [](f32 a, f32 b) { return (b > 1.0e-12F) ? (a / b) : 1.0F; };
    const f32 rx = ratio(e0, e2);
    const f32 ry = ratio(e1, e3);
    const f32 rz = ratio(e2, e0);
    const f32 rw = ratio(e3, e1);
    constexpr Float4 kOnes{1.0F, 1.0F, 1.0F, 1.0F};
    return {kOnes, Float4{rx, 1.0F, 1.0F, rw}, Float4{1.0F, ry, rz, 1.0F}, kOnes};
}

}

RibbonGeometryOutput buildRibbonGeometry(const RenderPacket& packet, const ViewParams& view,
                                         const RibbonUVConfig& uvCfg, IArena& arena) {
    if (packet.cls != RendererClass::Ribbon || packet.particleCount == 0U) {
        return {};
    }
    const auto& positionSlot = packet.slots[static_cast<std::size_t>(RenderSlot::Position)];
    const auto& sizeSlot = packet.slots[static_cast<std::size_t>(RenderSlot::Size)];
    const auto& enabledSlot = packet.slots[static_cast<std::size_t>(RenderSlot::Enabled)];
    const auto& colorSlot = packet.slots[static_cast<std::size_t>(RenderSlot::Color)];
    const auto& axisSlot = packet.slots[static_cast<std::size_t>(RenderSlot::Axis0)];
    const auto& selfIdSlot = packet.slots[static_cast<std::size_t>(RenderSlot::SelfID)];
    const auto& parentIdSlot = packet.slots[static_cast<std::size_t>(RenderSlot::ParentID)];

    if (positionSlot.empty() || sizeSlot.empty() || parentIdSlot.empty() || selfIdSlot.empty()) {

        return {};
    }

    const auto& texUSlot = packet.slots[static_cast<std::size_t>(RenderSlot::TextureU)];
    const auto& cursorSlot = packet.slots[static_cast<std::size_t>(RenderSlot::Cursor)];
    const auto& texIdSlot = packet.slots[static_cast<std::size_t>(RenderSlot::TextureID)];
    const RibbonUVBasis basis = makeUVBasis(uvCfg);
    const u32 frameCount = (uvCfg.atlasSubDivX > 0U && uvCfg.atlasSubDivY > 0U)
                               ? static_cast<u32>(uvCfg.atlasSubDivX) * uvCfg.atlasSubDivY
                               : 0U;

    const Float3 cameraPos = cameraPositionFromView(view.view);
    const u32 particleCount = packet.particleCount;

    struct SortKey {
        u32 idx;
        u64 parentId;
        u64 selfId;
    };
    std::vector<SortKey> keys;
    keys.reserve(particleCount);
    for (u32 i = 0; i < particleCount; ++i) {
        if (readEnabled(enabledSlot, i) == 0U)
            continue;
        keys.push_back({i, readU64(parentIdSlot, i), readU64(selfIdSlot, i)});
    }
    if (keys.empty())
        return {};

    std::sort(keys.begin(), keys.end(), [](const SortKey& a, const SortKey& b) {
        if (a.parentId != b.parentId)
            return a.parentId < b.parentId;
        return a.selfId < b.selfId;
    });

    const std::size_t aliveCount = keys.size();
    const std::size_t vertexCount = aliveCount * 4U;
    auto verts = arenaArray<RibbonVertex>(arena, vertexCount);
    std::size_t vw = 0;

    for (std::size_t k = 0; k < aliveCount; ++k) {
        const u32 i = keys[k].idx;
        const u64 myParent = keys[k].parentId;

        const bool hasNext = (k + 1U < aliveCount) && (keys[k + 1U].parentId == myParent);
        if (!hasNext) {

            for (int q = 0; q < 4; ++q) {
                verts[vw + q] = RibbonVertex{kZeroVec3, kWhite, 0.0F,      0.0F,  kOnes4,
                                             kOnes4,    0.0F,   kZeroVec3, Float4{}};
            }
            vw += 4;
            continue;
        }
        const bool hasPrev = (k > 0U) && (keys[k - 1U].parentId == myParent);

        const Float3 c0 = readFloat3(positionSlot, i);
        const Float3 c1 = readFloat3(positionSlot, keys[k + 1U].idx);
        const Float3 cm1 = hasPrev ? readFloat3(positionSlot, keys[k - 1U].idx) : c0;
        const f32 w0 = readFloat(sizeSlot, i);
        const f32 w1 = readFloat(sizeSlot, keys[k + 1U].idx);

        const Float4 col0 = readFloat4(colorSlot, i, kWhite);
        const Float4 col1 = readFloat4(colorSlot, keys[k + 1U].idx, kWhite);

        const auto mode = static_cast<RibbonMode>(packet.billboardingMode);
        Float3 up0{};
        Float3 up1{};
        switch (mode) {
        case RibbonMode::NormalAxisAligned: {
            const Float3 ax0 = readFloat3(axisSlot, i);
            const Float3 ax1 = readFloat3(axisSlot, keys[k + 1U].idx);
            up0 = hasPrev ? buildUpAxisNormal(ax0, cm1, c0) : buildUpAxisNormal(ax0, c0, c1);
            up1 = buildUpAxisNormal(ax1, c0, c1);
            break;
        }
        case RibbonMode::SideAxisAligned:
            up0 = buildUpAxisSide(readFloat3(axisSlot, i));
            up1 = buildUpAxisSide(readFloat3(axisSlot, keys[k + 1U].idx));
            break;
        case RibbonMode::ViewposAligned:
        default:
            up0 = hasPrev ? buildUpAxisViewPos(cameraPos, cm1, c0)
                          : buildUpAxisViewPos(cameraPos, c0, c1);
            up1 = buildUpAxisViewPos(cameraPos, c0, c1);
            break;
        }
        const Float3 off0 = scale(up0, w0);
        const Float3 off1 = scale(up1, w1);

        const f32 pu0 = uvCfg.customTextureU ? readFloat(texUSlot, i) : 0.0F;
        const f32 pu1 =
            uvCfg.customTextureU ? readFloat(texUSlot, keys[k + 1U].idx) : 1.0F;

        f32 rectU0 = 0.0F;
        f32 rectV0 = 0.0F;
        f32 rectSpanU = 1.0F;
        f32 rectSpanV = 1.0F;
        if (frameCount > 0U) {
            const f32 raw = readFloat(texIdSlot, i);
            f32 cursor = std::isfinite(raw) ? std::fabs(raw) : 0.0F;
            const auto maxFrame = static_cast<f32>(frameCount - 1U);
            if (cursor > maxFrame) {
                cursor = maxFrame;
            }
            const auto frame = static_cast<u32>(cursor);
            rectSpanU = 1.0F / static_cast<f32>(uvCfg.atlasSubDivX);
            rectSpanV = 1.0F / static_cast<f32>(uvCfg.atlasSubDivY);
            rectU0 = static_cast<f32>(frame % uvCfg.atlasSubDivX) * rectSpanU;
            rectV0 = static_cast<f32>(frame / uvCfg.atlasSubDivX) * rectSpanV;
        }

        const f32 dpu = pu1 - pu0;
        const f32 aU = dpu * basis.mulU;
        const f32 aV = dpu * basis.mulV;
        const f32 cU = basis.add1U - basis.add0U;
        const f32 cV = basis.add1V - basis.add0V;
        const f32 baseU = pu0 * basis.mulU + basis.add0U;
        const f32 baseV = pu0 * basis.mulV + basis.add0V;
        const f32 sclU = uvCfg.rotate ? cU : aU;
        const f32 sclV = uvCfg.rotate ? aV : cV;
        const Float4 scaleBias{sclU * rectSpanU, sclV * rectSpanV,
                               baseU * rectSpanU + rectU0, baseV * rectSpanV + rectV0};

        const f32 cur0 = readFloat(cursorSlot, i);
        const f32 cur1 = readFloat(cursorSlot, keys[k + 1U].idx);
        const Float3 p0 = add(c0, off0);
        const Float3 p1 = sub(c0, off0);
        const Float3 p2 = add(c1, off1);
        const Float3 p3 = sub(c1, off1);
        const RibbonUVFactors fac = uvCfg.correctDeformation
                                        ? computeUVFactors(p0, p1, p2, p3)
                                        : RibbonUVFactors{kOnes4, kOnes4, kOnes4, kOnes4};
        const Float3 pos[4] = {p0, p1, p2, p3};
        const Float4 col[4] = {col0, col0, col1, col1};
        const Float4 facs[4] = {fac.v0, fac.v1, fac.v2, fac.v3};
        const f32 curs[4] = {cur0, cur0, cur1, cur1};

        Float3 nrm[4]{};
        Float4 tan[4]{};
        if (uvCfg.needsNormals) {
            const RibbonShadingBasis b0 =
                hasPrev ? buildShadingBasis(cm1, c0, up0, uvCfg.normalBendingFactor)
                        : buildShadingBasis(c0, c1, up0, uvCfg.normalBendingFactor);
            const RibbonShadingBasis b1 = buildShadingBasis(c0, c1, up1, uvCfg.normalBendingFactor);
            nrm[0] = b0.normalPlus;
            nrm[1] = b0.normalMinus;
            nrm[2] = b1.normalPlus;
            nrm[3] = b1.normalMinus;
            tan[0] = tan[1] = b0.tangent;
            tan[2] = tan[3] = b1.tangent;
        }

        static constexpr int kSlotToVertex[2][4] = {{0, 1, 2, 3}, {0, 2, 1, 3}};
        const int* slotMap = kSlotToVertex[uvCfg.rotate ? 1 : 0];
        for (int s = 0; s < 4; ++s) {
            const int vi = slotMap[s];
            verts[vw + static_cast<std::size_t>(s)] =
                RibbonVertex{pos[vi],   col[vi],  static_cast<f32>(s >> 1),
                             static_cast<f32>(s & 1), scaleBias, facs[vi],
                             curs[vi], nrm[vi],  tan[vi]};
        }
        vw += 4;
    }

    return RibbonGeometryOutput{
        std::span<const RibbonVertex>{verts.data(), vw},
    };
}

}

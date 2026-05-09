#include <cornflakes/render/ribbon_geometry.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace whiteout::cornflakes {

namespace {

constexpr Float3 kZeroVec3{0.0F, 0.0F, 0.0F};
constexpr Float4 kWhite{1.0F, 1.0F, 1.0F, 1.0F};

Float3 cameraPositionFromView(const Mat4& view) noexcept {
    return Float3{view.m[12], view.m[13], view.m[14]};
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

Float3 buildUpAxis(const Float3& cameraPos, const Float3& c0, const Float3& c1) noexcept {
    const Float3 viewToC0 = sub(c0, cameraPos);
    const Float3 segDir = sub(c1, c0);
    const Float3 axis = cross3(segDir, viewToC0);
    return normalize3(axis);
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

} // namespace

RibbonGeometryOutput buildRibbonGeometry(const RenderPacket& packet, const ViewParams& view,
                                         IArena& arena) {
    if (packet.cls != RendererClass::Ribbon || packet.particleCount == 0U) {
        return {};
    }
    const auto& positionSlot = packet.slots[static_cast<std::size_t>(RenderSlot::Position)];
    const auto& sizeSlot = packet.slots[static_cast<std::size_t>(RenderSlot::Size)];
    const auto& enabledSlot = packet.slots[static_cast<std::size_t>(RenderSlot::Enabled)];
    const auto& colorSlot = packet.slots[static_cast<std::size_t>(RenderSlot::Color)];
    const auto& selfIdSlot = packet.slots[static_cast<std::size_t>(RenderSlot::SelfID)];
    const auto& parentIdSlot = packet.slots[static_cast<std::size_t>(RenderSlot::ParentID)];

    if (positionSlot.empty() || sizeSlot.empty() || parentIdSlot.empty() || selfIdSlot.empty()) {

        return {};
    }

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
                verts[vw + q] = RibbonVertex{kZeroVec3, kWhite, 0.0F, 0.0F};
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

        const Float3 up0 =
            hasPrev ? buildUpAxis(cameraPos, cm1, c0) : buildUpAxis(cameraPos, c0, c1);
        const Float3 up1 = buildUpAxis(cameraPos, c0, c1);
        const Float3 off0 = scale(up0, w0);
        const Float3 off1 = scale(up1, w1);

        verts[vw + 0] = RibbonVertex{add(c0, off0), col0, 0.0F, 1.0F};
        verts[vw + 1] = RibbonVertex{sub(c0, off0), col0, 0.0F, 0.0F};
        verts[vw + 2] = RibbonVertex{add(c1, off1), col1, 1.0F, 1.0F};
        verts[vw + 3] = RibbonVertex{sub(c1, off1), col1, 1.0F, 0.0F};
        vw += 4;
    }

    return RibbonGeometryOutput{
        std::span<const RibbonVertex>{verts.data(), vw},
    };
}

} // namespace whiteout::cornflakes

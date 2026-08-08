
#include <cornflakes/sampler/texture.hpp>
#include <cornflakes/sampler/vector_field.hpp>

#include <cmath>
#include <cstring>

namespace whiteout::cornflakes {

namespace {

constexpr f32 kU8SnScale = 1.0F / 127.0F;
constexpr f32 kU8SnOffset = -128.0F / 127.0F;

Float3 decodeCell(const SamplerVectorField& vf, u32 cell) noexcept {
    const std::size_t scalar = vectorFieldScalarSize(vf.dataType);
    const std::size_t base = static_cast<std::size_t>(cell) * 3U * scalar;
    if (base + (3U * scalar) > vf.data.size()) {
        return Float3{0.0F, 0.0F, 0.0F};
    }
    const auto* p = reinterpret_cast<const u8*>(vf.data.data()) + base;
    f32 c[3] = {0.0F, 0.0F, 0.0F};
    for (std::size_t i = 0; i < 3U; ++i) {
        switch (vf.dataType) {
        case VectorFieldDataType::U8SN:
            c[i] = (static_cast<f32>(p[i]) * kU8SnScale) + kU8SnOffset;
            break;
        case VectorFieldDataType::Fp16: {
            u16 h = 0;
            std::memcpy(&h, p + (i * 2U), sizeof(h));
            c[i] = halfToFloat(h);
            break;
        }
        case VectorFieldDataType::Fp32:
            std::memcpy(&c[i], p + (i * 4U), sizeof(f32));
            break;
        }
    }
    return Float3{c[0], c[1], c[2]};
}

i32 floorToInt(f32 v) noexcept {
    if (!std::isfinite(v)) {
        return 0;
    }
    const f32 f = std::floor(v);
    if (f <= -2.147483e9F || f >= 2.147483e9F) {
        return 0;
    }
    return static_cast<i32>(f);
}

f32 wrapUnit(f32 v) noexcept {
    return std::isfinite(v) ? (v - std::floor(v)) : 0.0F;
}

u32 clampIndex(i32 idx, u32 dim) noexcept {
    const auto hi = static_cast<i32>(dim - 1U);
    if (idx < 0) {
        return 0U;
    }
    return static_cast<u32>(idx > hi ? hi : idx);
}

u32 cellIndexOnAxis(const SamplerVectorField& vf, std::size_t axis, i32 idx) noexcept {
    return clampIndex(idx & vf.addrMask[axis], vf.dimensions[axis]);
}

u32 frameIndex(const SamplerVectorField& vf, f32 animTime) noexcept {
    f32 n = animTime * vf.timeScale;
    if (vf.wrapTime) {
        n -= std::isfinite(n) ? std::floor(n) : n;
    }
    return clampIndex(floorToInt(n * static_cast<f32>(vf.dimensions[3])), vf.dimensions[3]);
}

u32 foldIndex(const SamplerVectorField& vf, u32 x, u32 y, u32 z, u32 frameBase) noexcept {
    return x + (y << vf.strideL2[0]) + (z << vf.strideL2[1]) + frameBase;
}

Float3 lerp3(Float3 a, Float3 b, f32 t) noexcept {
    return Float3{a.x + ((b.x - a.x) * t), a.y + ((b.y - a.y) * t), a.z + ((b.z - a.z) * t)};
}

Float3 triSample(const SamplerVectorField& vf, const u32 lo[3], const u32 hi[3], const f32 frac[3],
                 u32 frameBase) noexcept {
    const Float3 c000 = decodeCell(vf, foldIndex(vf, lo[0], lo[1], lo[2], frameBase));
    const Float3 c100 = decodeCell(vf, foldIndex(vf, hi[0], lo[1], lo[2], frameBase));
    const Float3 c010 = decodeCell(vf, foldIndex(vf, lo[0], hi[1], lo[2], frameBase));
    const Float3 c110 = decodeCell(vf, foldIndex(vf, hi[0], hi[1], lo[2], frameBase));
    const Float3 c001 = decodeCell(vf, foldIndex(vf, lo[0], lo[1], hi[2], frameBase));
    const Float3 c101 = decodeCell(vf, foldIndex(vf, hi[0], lo[1], hi[2], frameBase));
    const Float3 c011 = decodeCell(vf, foldIndex(vf, lo[0], hi[1], hi[2], frameBase));
    const Float3 c111 = decodeCell(vf, foldIndex(vf, hi[0], hi[1], hi[2], frameBase));

    const Float3 x00 = lerp3(c000, c100, frac[0]);
    const Float3 x10 = lerp3(c010, c110, frac[0]);
    const Float3 x01 = lerp3(c001, c101, frac[0]);
    const Float3 x11 = lerp3(c011, c111, frac[0]);
    return lerp3(lerp3(x00, x10, frac[1]), lerp3(x01, x11, frac[1]), frac[2]);
}

}

Float3 sampleVectorFieldVelocity(const SamplerVectorField& vf, Float3 posWorld,
                                 const f32* animTime) noexcept {
    if (!vf.valid || vf.data.empty()) {
        return Float3{0.0F, 0.0F, 0.0F};
    }

    const f32 world[3] = {posWorld.x, posWorld.y, posWorld.z};
    f32 cell[3];
    for (std::size_t i = 0; i < 3U; ++i) {
        cell[i] = (world[i] * vf.gridScale[i]) + vf.gridOffset[i];
    }

    const bool animated = (animTime != nullptr) && vf.dimensions[3] > 1U;
    VectorFieldInterpolation interp = vf.interpolation;
    if (!animated && interp == VectorFieldInterpolation::Quadrilinear) {
        interp = VectorFieldInterpolation::Trilinear;
    }

    Float3 local{};
    if (interp == VectorFieldInterpolation::Quadrilinear) {
        const f32 dim[4] = {
            static_cast<f32>(vf.dimensions[0]),
            static_cast<f32>(vf.dimensions[1]),
            static_cast<f32>(vf.dimensions[2]),
            static_cast<f32>(vf.dimensions[3]),
        };
        const f32 norm[4] = {cell[0] / dim[0], cell[1] / dim[1], cell[2] / dim[2],
                             *animTime * vf.timeScale};
        const bool wrap[4] = {vf.addrMask[0] != -1, vf.addrMask[1] != -1, vf.addrMask[2] != -1,
                              vf.wrapTime};

        u32 lo[4];
        u32 hi[4];
        f32 frac[4];
        for (std::size_t i = 0; i < 4U; ++i) {
            const f32 half = 0.5F / dim[i];
            f32 a = norm[i] - half;
            f32 b = norm[i] + half;
            if (wrap[i]) {
                a = wrapUnit(a);
                b = wrapUnit(b);
            }
            const f32 ac = a * dim[i];
            const i32 ai = floorToInt(ac);
            frac[i] = std::isfinite(ac) ? (ac - static_cast<f32>(ai)) : 0.0F;
            lo[i] = clampIndex(ai, vf.dimensions[i]);
            hi[i] = clampIndex(floorToInt(b * dim[i]), vf.dimensions[i]);
        }

        const u32 spatialLo[3] = {lo[0], lo[1], lo[2]};
        const u32 spatialHi[3] = {hi[0], hi[1], hi[2]};
        const Float3 t0 = triSample(vf, spatialLo, spatialHi, frac, lo[3] << vf.strideL2[2]);
        const Float3 t1 = triSample(vf, spatialLo, spatialHi, frac, hi[3] << vf.strideL2[2]);
        local = lerp3(t0, t1, frac[3]);
    } else {
        const u32 frameBase = animated ? (frameIndex(vf, *animTime) << vf.strideL2[2]) : 0U;

        if (interp == VectorFieldInterpolation::Point) {
            u32 idx[3];
            for (std::size_t i = 0; i < 3U; ++i) {
                idx[i] = cellIndexOnAxis(vf, i, floorToInt(cell[i]));
            }
            local = decodeCell(vf, foldIndex(vf, idx[0], idx[1], idx[2], frameBase));
        } else {
            u32 lo[3];
            u32 hi[3];
            f32 frac[3];
            for (std::size_t i = 0; i < 3U; ++i) {
                const f32 shifted = cell[i] - 0.5F;
                const i32 whole = floorToInt(shifted);
                frac[i] = std::isfinite(shifted) ? (shifted - static_cast<f32>(whole)) : 0.0F;
                lo[i] = cellIndexOnAxis(vf, i, whole);
                hi[i] = cellIndexOnAxis(vf, i, whole + 1);
            }
            local = triSample(vf, lo, hi, frac, frameBase);
        }
    }

    return Float3{local.x * vf.vecScale, local.y * vf.vecScale, local.z * vf.vecScale};
}

}

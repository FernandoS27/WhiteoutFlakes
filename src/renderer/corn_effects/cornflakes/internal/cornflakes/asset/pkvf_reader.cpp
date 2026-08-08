
#include <cornflakes/interface/asset/vector_field_asset.hpp>

#include <cstring>

namespace whiteout::cornflakes {

namespace {

u32 readU32(std::span<const std::byte> b, std::size_t off) noexcept {
    u32 v = 0;
    std::memcpy(&v, b.data() + off, sizeof(v));
    return v;
}

f32 readF32(std::span<const std::byte> b, std::size_t off) noexcept {
    f32 v = 0.0F;
    std::memcpy(&v, b.data() + off, sizeof(v));
    return v;
}

}

u32 vectorFieldScalarSize(VectorFieldDataType type) noexcept {
    switch (type) {
    case VectorFieldDataType::Fp32:
        return 4U;
    case VectorFieldDataType::Fp16:
        return 2U;
    case VectorFieldDataType::U8SN:
        return 1U;
    }
    return 0U;
}

std::optional<VectorFieldAsset> parsePkvf(std::span<const std::byte> bytes) noexcept {
    if (bytes.size() < kPkvfHeaderSize) {
        return std::nullopt;
    }
    const auto* magic = reinterpret_cast<const u8*>(bytes.data());
    if (magic[0] != 'P' || magic[1] != 'K' || magic[2] != 'V' || magic[3] != 'F') {
        return std::nullopt;
    }
    if (magic[4] != kPkvfFileVersion) {
        return std::nullopt;
    }

    VectorFieldAsset vf;
    if (magic[5] >= 3U) {
        return std::nullopt;
    }
    vf.dataType = static_cast<VectorFieldDataType>(magic[5]);
    vf.coordinateFrame = magic[6];
    vf.intensityMultiplier = readF32(bytes, 0x08U);
    for (std::size_t i = 0; i < 4U; ++i) {
        vf.dimensions[i] = readU32(bytes, 0x0CU + (i * 4U));
    }
    for (std::size_t i = 0; i < 4U; ++i) {
        vf.boundsMin[i] = readF32(bytes, 0x1CU + (i * 4U));
        vf.boundsMax[i] = readF32(bytes, 0x2CU + (i * 4U));
    }

    const u64 perFrame = static_cast<u64>(vf.dimensions[0]) * vf.dimensions[1] * vf.dimensions[2];
    const u64 total = perFrame * vf.dimensions[3];
    if (total == 0U || perFrame > 0x8000000U) {
        return std::nullopt;
    }

    const u64 need = total * 3U * vectorFieldScalarSize(vf.dataType);
    if (bytes.size() - kPkvfHeaderSize < need) {
        return std::nullopt;
    }
    vf.data = bytes.subspan(kPkvfHeaderSize, static_cast<std::size_t>(need));
    return vf;
}

}

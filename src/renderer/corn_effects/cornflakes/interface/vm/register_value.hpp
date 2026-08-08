#pragma once

#include <cornflakes/interface/core/types.hpp>

#include <cstring>

namespace whiteout::cornflakes {

inline constexpr u32 kRegVoid = 0xFFFFFFFFU;

namespace bank {
inline constexpr u8 kHandle = 0x00U;
inline constexpr u8 kBool = 0x02U;
inline constexpr u8 kBool3 = 0x04U;
inline constexpr u8 kInt = 0x08U;
inline constexpr u8 kInt2 = 0x09U;
inline constexpr u8 kPtr = 0x1AU;
inline constexpr u8 kInt2Alt = 0x1BU;
inline constexpr u8 kInt3 = 0x1CU;
inline constexpr u8 kInt4 = 0x1DU;
inline constexpr u8 kFloat = 0x20U;
inline constexpr u8 kFloat2 = 0x21U;
inline constexpr u8 kFloat3 = 0x22U;
inline constexpr u8 kFloat4 = 0x23U;
inline constexpr u8 kIntAlt = 0x25U;
inline constexpr u8 kInt2Alt2 = 0x26U;
}

namespace scope {
inline constexpr u8 kConstPool    = 0x00U;
inline constexpr u8 kLocal        = 0x01U;
inline constexpr u8 kInput        = 0x02U;
inline constexpr u8 kStream       = 0x03U;
}

enum class SwizzleCode : u8 {
    LaneX = 0,
    LaneY = 1,
    LaneZ = 2,
    LaneW = 3,
    LiteralZero = 4,
    LiteralOne = 5,
    Count,
};

inline constexpr u8 kSwizzleBitsPerCode = 3U;
inline constexpr u8 kSwizzleCodeMask = 0x7U;
inline constexpr u8 kSwizzleMaxLanes = 4U;

namespace fpbits {

inline constexpr u32 kOneF32 = 0x3F800000U;
inline constexpr u32 kInfF32 = 0x7F800000U;
inline constexpr u32 kBoolTrue = 0xFFFFFFFFU;
inline constexpr u32 kRandMantissaShift = 9U;
}

struct DecodedRegId {
    u8 bank;
    u8 scope;
    u32 idx;
    u16 localIdx;
};

constexpr u8 normaliseScope(u8 scopeByte) noexcept {
    return static_cast<u8>(((scopeByte >= 0x20U) ? (scopeByte >> 5U) : scopeByte) & 0x03U);
}

constexpr DecodedRegId decodeRegId(u32 v) noexcept {
    return {static_cast<u8>((v >> 24) & 0xFFU), normaliseScope(static_cast<u8>((v >> 16) & 0xFFU)),
            v & 0x00FFFFFFU, static_cast<u16>(v & 0xFFFFU)};
}

constexpr u8 componentCountForBank(u8 b) noexcept {
    switch (b) {
    case bank::kFloat2:
    case bank::kInt2:
    case bank::kInt2Alt:
    case bank::kInt2Alt2:
        return 2;
    case bank::kFloat3:
    case bank::kInt3:
    case bank::kBool3:
        return 3;
    case bank::kFloat4:
    case bank::kInt4:
    case bank::kIntAlt:
        return 4;
    default:
        return 1;
    }
}

constexpr u8 floatBankForComponentCount(u8 components) noexcept {
    switch (components) {
    case 2:
        return bank::kFloat2;
    case 3:
        return bank::kFloat3;
    case 4:
        return bank::kFloat4;
    default:
        return bank::kFloat;
    }
}

constexpr u8 intBankForComponentCount(u8 components) noexcept {
    switch (components) {
    case 2:
        return bank::kInt2;
    case 3:
        return bank::kInt3;
    case 4:
        return bank::kInt4;
    default:
        return bank::kInt;
    }
}

constexpr bool bankIsIntegral(u8 b) noexcept {
    return b == bank::kBool || b == bank::kInt || b == bank::kInt2 || b == bank::kInt2Alt ||
           b == bank::kInt2Alt2 || b == bank::kInt3 || b == bank::kInt4 || b == bank::kPtr;
}

struct RegisterValue {
    f32 lanes[4] = {0.0F, 0.0F, 0.0F, 0.0F};
    u8 componentCount = 0;
    u8 typeBank = 0;

    static RegisterValue scalar(f32 v) noexcept {
        RegisterValue r;
        r.lanes[0] = v;
        r.componentCount = 1;
        r.typeBank = bank::kFloat;
        return r;
    }

    static RegisterValue scalarI(i32 v) noexcept {
        RegisterValue r;
        std::memcpy(&r.lanes[0], &v, sizeof(v));
        r.componentCount = 1;
        r.typeBank = bank::kInt;
        return r;
    }
};

inline i32 laneAsI32(const RegisterValue& r, u8 lane) noexcept {
    i32 out;
    std::memcpy(&out, &r.lanes[lane], sizeof(out));
    return out;
}

inline u32 laneAsU32(const RegisterValue& r, u8 lane) noexcept {
    u32 out;
    std::memcpy(&out, &r.lanes[lane], sizeof(out));
    return out;
}

inline void setLaneI32(RegisterValue& r, u8 lane, i32 v) noexcept {
    std::memcpy(&r.lanes[lane], &v, sizeof(v));
}

inline void setLaneU32(RegisterValue& r, u8 lane, u32 v) noexcept {
    std::memcpy(&r.lanes[lane], &v, sizeof(v));
}

}

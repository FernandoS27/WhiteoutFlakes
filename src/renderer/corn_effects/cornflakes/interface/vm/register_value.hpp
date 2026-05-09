#pragma once

/// @file
/// @brief Typed VM register storage and the bank/scope/swizzle tag schema.
///
/// A `RegisterValue` is a 4-lane f32 cell reinterpreted as i32/u32/bool when
/// the bank tag says so. Register IDs are u32s with the bank in the high
/// byte, scope in the next byte, and the local index in the low 16 bits.

#include <cornflakes/interface/core/types.hpp>

#include <cstring>

namespace whiteout::cornflakes {

/// @brief Sentinel for an unbound / no-op destination or source register.
inline constexpr u32 kRegVoid = 0xFFFFFFFFU;

/// @brief Bank tag values for the high byte of a register ID and for `RegisterValue::typeBank`.
namespace bank {
inline constexpr u8 kHandle = 0x00U;
inline constexpr u8 kBool = 0x02U;
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
} // namespace bank

/// @brief Scope tag values (middle byte of a register ID).
namespace scope {
inline constexpr u8 kConstPool = 0x00U; ///< Constant pool entry.
inline constexpr u8 kInput = 0x10U;     ///< Input external (read-only at scope entry).
} // namespace scope

/// @brief Per-lane source for a VectorSwizzler operation.
enum class SwizzleCode : u8 {
    LaneX = 0,       ///< Copy source lane 0.
    LaneY = 1,       ///< Copy source lane 1.
    LaneZ = 2,       ///< Copy source lane 2.
    LaneW = 3,       ///< Copy source lane 3.
    LiteralZero = 4, ///< Emit a typed literal zero.
    LiteralOne = 5,  ///< Emit a typed literal one (1.0F / 1 / all-ones for bool).
    Count,
};

inline constexpr u8 kSwizzleBitsPerCode = 3U;
inline constexpr u8 kSwizzleCodeMask = 0x7U;
inline constexpr u8 kSwizzleMaxLanes = 4U;

/// @brief IEEE 754 bit-pattern constants the VM treats as type-aware literals.
namespace fpbits {

inline constexpr u32 kOneF32 = 0x3F800000U;     ///< 1.0F as raw bits.
inline constexpr u32 kInfF32 = 0x7F800000U;     ///< +inf as raw bits (used by lifeRatio kill cmp).
inline constexpr u32 kBoolTrue = 0xFFFFFFFFU;   ///< SIMD-style "true" mask.
inline constexpr u32 kRandMantissaShift = 9U;  ///< Shift used by the [1.0,2.0)→[0,1) random idiom.
} // namespace fpbits

/// @brief Result of decoding a 32-bit register ID into its tag fields.
struct DecodedRegId {
    u8 bank;       ///< High byte: type bank (see ::bank).
    u8 scope;      ///< Middle byte: scope (see ::scope).
    u32 idx;       ///< Low 24 bits: full register index.
    u16 localIdx;  ///< Low 16 bits: index within the scope.
};

/// @brief Splits a packed register ID into bank/scope/idx/localIdx.
constexpr DecodedRegId decodeRegId(u32 v) noexcept {
    return {static_cast<u8>((v >> 24) & 0xFFU), static_cast<u8>((v >> 16) & 0xFFU), v & 0x00FFFFFFU,
            static_cast<u16>(v & 0xFFFFU)};
}

/// @brief Lane count carried by `bank` (1 for scalars, 2/3/4 for the vector banks).
constexpr u8 componentCountForBank(u8 b) noexcept {
    switch (b) {
    case bank::kFloat2:
    case bank::kInt2:
    case bank::kInt2Alt:
    case bank::kInt2Alt2:
        return 2;
    case bank::kFloat3:
    case bank::kInt3:
        return 3;
    case bank::kFloat4:
    case bank::kInt4:
        return 4;
    default:
        return 1;
    }
}

/// @brief Inverse of componentCountForBank() for the float scalar family. Width
/// outside 1..4 falls back to scalar `bank::kFloat`.
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

/// @brief Inverse of componentCountForBank() for the int scalar family.
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

/// @brief True if `bank` stores integer / bool / pointer lanes (i.e. operands
/// should be reinterpreted via laneAsI32).
constexpr bool bankIsIntegral(u8 b) noexcept {
    return b == bank::kBool || b == bank::kInt || b == bank::kInt2 || b == bank::kInt2Alt ||
           b == bank::kInt2Alt2 || b == bank::kInt3 || b == bank::kInt4 || b == bank::kIntAlt ||
           b == bank::kPtr;
}

/// @brief 4-lane typed register cell. Lanes are stored as f32 and reinterpreted
/// via the `laneAs*` helpers when `typeBank` indicates an integral kind.
struct RegisterValue {
    f32 lanes[4] = {0.0F, 0.0F, 0.0F, 0.0F};
    u8 componentCount = 0; ///< Live lane count (1..4).
    u8 typeBank = 0;       ///< Bank tag (see ::bank); 0 means "uninitialised / scalar f32".

    /// @brief Build a 1-lane f32 cell.
    static RegisterValue scalar(f32 v) noexcept {
        RegisterValue r;
        r.lanes[0] = v;
        r.componentCount = 1;
        r.typeBank = bank::kFloat;
        return r;
    }

    /// @brief Build a 1-lane i32 cell (bit-copied into lane 0).
    static RegisterValue scalarI(i32 v) noexcept {
        RegisterValue r;
        std::memcpy(&r.lanes[0], &v, sizeof(v));
        r.componentCount = 1;
        r.typeBank = bank::kInt;
        return r;
    }
};

/// @brief Bit-cast read of an i32 lane out of a register cell.
inline i32 laneAsI32(const RegisterValue& r, u8 lane) noexcept {
    i32 out;
    std::memcpy(&out, &r.lanes[lane], sizeof(out));
    return out;
}

/// @brief Bit-cast read of a u32 lane out of a register cell.
inline u32 laneAsU32(const RegisterValue& r, u8 lane) noexcept {
    u32 out;
    std::memcpy(&out, &r.lanes[lane], sizeof(out));
    return out;
}

/// @brief Bit-cast write of an i32 into a register lane.
inline void setLaneI32(RegisterValue& r, u8 lane, i32 v) noexcept {
    std::memcpy(&r.lanes[lane], &v, sizeof(v));
}

/// @brief Bit-cast write of a u32 into a register lane.
inline void setLaneU32(RegisterValue& r, u8 lane, u32 v) noexcept {
    std::memcpy(&r.lanes[lane], &v, sizeof(v));
}

} // namespace whiteout::cornflakes

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
// Bank byte = base-type id. Names below carry the engine type (verified against
// CBaseTypeTraits @ preview.exe 0x7ff60aa1b780, 32-byte stride, name string at
// entry+24). Some legacy names are kept for source compatibility with a note on
// the real type; corpus-used banks: 0x00,0x02,0x04,0x1A,0x1B,0x1C,0x20-0x23,0x25.
inline constexpr u8 kHandle = 0x00U;  ///< engine "class"/handle.
inline constexpr u8 kBool = 0x02U;    ///< engine "bool".
inline constexpr u8 kBool3 = 0x04U;   ///< engine "bool3" (3 components).
inline constexpr u8 kInt = 0x08U;     ///< NOTE: engine "ubyte3" — not used in WC3 corpus.
inline constexpr u8 kInt2 = 0x09U;    ///< not used in WC3 corpus.
inline constexpr u8 kPtr = 0x1AU;     ///< NOTE: engine "int" (scalar). Real WC3 int bank.
inline constexpr u8 kInt2Alt = 0x1BU; ///< engine "int2".
inline constexpr u8 kInt3 = 0x1CU;    ///< engine "int3".
inline constexpr u8 kInt4 = 0x1DU;    ///< engine "int4".
inline constexpr u8 kFloat = 0x20U;   ///< engine "float".
inline constexpr u8 kFloat2 = 0x21U;  ///< engine "float2".
inline constexpr u8 kFloat3 = 0x22U;  ///< engine "float3".
inline constexpr u8 kFloat4 = 0x23U;  ///< engine "float4".
inline constexpr u8 kIntAlt = 0x25U;  ///< NOTE: engine "quaternion" (4 components, float-family).
inline constexpr u8 kInt2Alt2 = 0x26U; ///< not used in WC3 corpus (beyond traits array).
} // namespace bank

/// @brief Scope tag values for a register ID.
///
/// The engine's real scope is the 2-bit field `(raw >> 16) & 3` (CRegID::ToString
/// @ preview.exe 0x7ff60a5ab720): 0=const(cr), 1=local(vr), 2=input(ir),
/// 3=stream(sr). Bit 20 is an extra flag seen only on const-scope registers,
/// giving the byte value 0x10. Corpus-verified: real register operands carry
/// scope bytes {0x00, 0x01, 0x02, 0x03, 0x10} only. `decodeRegId` extracts the
/// full middle byte, so these compare against the byte values below.
namespace scope {
inline constexpr u8 kConstPool    = 0x00U; ///< 2-bit scope 0: constant pool (cr).
inline constexpr u8 kLocal        = 0x01U; ///< 2-bit scope 1: local/scratch register (vr).
inline constexpr u8 kInput        = 0x02U; ///< 2-bit scope 2: input register (ir), read-only.
inline constexpr u8 kStream       = 0x03U; ///< 2-bit scope 3: particle stream register (sr).
inline constexpr u8 kConstFlagged = 0x10U; ///< const scope + bit-20 flag; also a const-pool entry.
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
    u8 bank;       ///< High byte (bits 24-31): type bank (see ::bank).
    u8 scope;      ///< Middle byte (bits 16-23): scope; real scope is the low 2 bits (see ::scope).
    u32 idx;       ///< Low 24 bits (legacy; prefer localIdx — the real index is the low 16).
    u16 localIdx;  ///< Low 16 bits: register index (matches engine CRegID `(u16)raw`).
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
    case bank::kBool3: // engine "bool3"
        return 3;
    case bank::kFloat4:
    case bank::kInt4:
    case bank::kIntAlt: // engine "quaternion" — 4 components
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
    // NOTE: kIntAlt (0x25) is the engine "quaternion" (float-family), so it is NOT
    // integral. kPtr (0x1A) is the engine scalar "int" → integral.
    return b == bank::kBool || b == bank::kInt || b == bank::kInt2 || b == bank::kInt2Alt ||
           b == bank::kInt2Alt2 || b == bank::kInt3 || b == bank::kInt4 || b == bank::kPtr;
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

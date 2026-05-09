#pragma once

/// @file
/// @brief Per-category numeric `Issue::code` namespaces (asset, binding, vm, sim, ...).

#include <cornflakes/interface/core/types.hpp>

namespace whiteout::cornflakes::issues {

namespace scheduler {
inline constexpr u32 kFlatDagBreach = 0x01U;
inline constexpr u32 kSignalBeforeWait = 0x02U;
inline constexpr u32 kSemaphoreRegressed = 0x03U;
inline constexpr u32 kSerialWaitUnsignaled = 0x04U;
} // namespace scheduler

namespace service {
inline constexpr u32 kNegativeDt = 0x01U;
inline constexpr u32 kEmitterEffectMissing = 0x10U;
} // namespace service

namespace asset {

inline constexpr u32 kNoReaderMatched = 0x01U;

inline constexpr u32 kPkbTooShort = 0x11U;
inline constexpr u32 kPkbBadMagic = 0x12U;
inline constexpr u32 kPkbStaleVersion = 0x13U;
inline constexpr u32 kPkbGenerator = 0x14U;

inline constexpr u32 kTextEmpty = 0x21U;
inline constexpr u32 kTextUnterminated = 0x22U;
inline constexpr u32 kTextBadGenerator = 0x23U;
inline constexpr u32 kTextBadVersion = 0x24U;

inline constexpr u32 kGenerator = 0x31U;
inline constexpr u32 kNoRootEffect = 0x32U;
inline constexpr u32 kNoRootLayer = 0x33U;
} // namespace asset

namespace binding {
inline constexpr u32 kLowerOutOfRange = 0x41U;
inline constexpr u32 kLowerIrNotImplemented = 0x42U;
} // namespace binding

namespace vm {

inline constexpr u32 kUnknownMathFunc3 = 0x51U;
inline constexpr u32 kUnknownMathFunc1 = 0x52U;

inline constexpr u32 kUnknownOpcode = 0x71U;
inline constexpr u32 kOperandCount = 0x72U;
inline constexpr u32 kRegisterOob = 0x73U;
inline constexpr u32 kDivByZero = 0x74U;
inline constexpr u32 kUnmatchedEpilog = 0x75U;
inline constexpr u32 kMathOpUnimplemented = 0x76U;
inline constexpr u32 kExternalOob = 0x77U;

inline constexpr u32 kConstPoolOob = 0x78U;
inline constexpr u32 kFunctionCallStub = 0x79U;
inline constexpr u32 kUnknownMathOp = 0x7AU;
inline constexpr u32 kUnknownMathFunc2 = 0x7BU;
inline constexpr u32 kSwizzleMaskOob = 0x7CU;
} // namespace vm

namespace sim {
inline constexpr u32 kSpawnOverflow = 0x81U;
inline constexpr u32 kSpawnPageMissing = 0x82U;
inline constexpr u32 kEvolveNoLifeStream = 0x85U;
} // namespace sim

namespace events {
inline constexpr u32 kPayloadOob = 0x91U;
}

namespace diagnostics {
inline constexpr u32 kTrackerUndeclaredRead = 0xB1U;
inline constexpr u32 kTrackerUndeclaredWrite = 0xB2U;
} // namespace diagnostics

} // namespace whiteout::cornflakes::issues

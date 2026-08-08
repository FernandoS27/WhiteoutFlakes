#pragma once

#ifndef CORNFLAKES_DETERMINISM_FLAGS_APPLIED
#error                                                                                             \
    "cornflakes/core/determinism.hpp included in a translation unit that is not linked against cornflakes::determinism"
#endif

static_assert(CORNFLAKES_DETERMINISM_FLAGS_APPLIED == 1,
              "CORNFLAKES_DETERMINISM_FLAGS_APPLIED must be 1");

#if defined(__FMA__) || defined(__AVX2__) || defined(__AVX512F__)
#if !defined(CORNFLAKES_FP_CONTRACT_DISABLED)
#error                                                                                             \
    "FMA-capable target with no proven -ffp-contract=off: float results become TU-dependent. See S-0 in cornflakes/docs/SIMT_MIGRATION_PLAN.md and cmake/Determinism.cmake."
#endif
#endif

namespace whiteout::cornflakes::core {

inline constexpr bool kDeterminismFlagsApplied = true;

#if defined(CORNFLAKES_FP_CONTRACT_DISABLED)
inline constexpr bool kFpContractDisabled = true;
#else
inline constexpr bool kFpContractDisabled = false;
#endif

inline constexpr bool kTargetHasFma =
#if defined(__FMA__) || defined(__AVX2__) || defined(__AVX512F__)
    true;
#else
    false;
#endif

}

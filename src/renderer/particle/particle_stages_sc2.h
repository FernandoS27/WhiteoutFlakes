#pragma once

// ============================================================================
// SC2 particle stage kernels — the pure functions the oracle pins.
//
// The contract the ribbon track established (design R3): every routine
// `tools/sc2_particle_oracle/` recorded from the shipped client is a free
// function with an explicit inputs struct, called by the emitter's stage
// method and by the replay test alike. Nothing here reads `Emitter2`, so a
// divergence from the golden is a red gate rather than a silent drift inside
// a loop that also does ten other things.
//
// Kernels shared with the ribbon dialect live in `sc2/` instead; these are the
// particle-only half (design R6: `particle/` and `ribbon/` never include each
// other).
//
// The float operation ORDER is part of the contract — written in the binary's
// order, not the algebraically tidy one.
// ============================================================================

#include "sc2_kernel_types.h"
#include "sc2_kernels_build.h"
#include "sc2_kernels_emit.h"
#include "sc2_kernels_model.h"
#include "sc2_kernels_move.h"
#include "sc2_kernels_spawn.h"

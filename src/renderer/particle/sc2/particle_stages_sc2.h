#pragma once

// ============================================================================
// SC2 particle stage kernels — the pure functions the oracle pins: free
// functions over explicit inputs structs (design R3), called by the stage and
// the replay alike, in the binary's float operation ORDER. Particle-only;
// shared kernels live in `sc2/` (design R6). See SC2_PARTICLE_DESIGN.md §16.3.
// ============================================================================

#include "renderer/particle/sc2/sc2_kernel_types.h"
#include "renderer/particle/sc2/sc2_kernels_build.h"
#include "renderer/particle/sc2/sc2_kernels_emit.h"
#include "renderer/particle/sc2/sc2_kernels_model.h"
#include "renderer/particle/sc2/sc2_kernels_move.h"
#include "renderer/particle/sc2/sc2_kernels_spawn.h"

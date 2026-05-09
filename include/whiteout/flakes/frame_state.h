#pragma once

// ============================================================================
// WhiteoutFlakes — per-frame animation state.
//
// FrameState is the contract returned by IAnimationSource::Evaluate. It
// carries the per-frame world-space outputs the renderer needs to drive
// skinning, particle/ribbon/corn emitters, attachment slots, lights, layer
// alpha / fresnel / texanim states. The struct itself is defined in
// model_types.h; this header is provided for callers that only need the
// frame-state piece.
// ============================================================================

#include "model_types.h"

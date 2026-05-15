#pragma once

/// @file frame_state.h
/// @brief Forwarding include for the per-frame animation contract.
///
/// `FrameState` (defined in `model_types.h`) is what
/// `IAnimationSource::Evaluate` returns each frame: world-space bone
/// matrices, layer alpha / fresnel / texanim states, particle / ribbon /
/// corn-emitter samples, attachment slot transforms, light overrides.
/// Sourced here so a host that only needs the frame-state piece can
/// include a short header.

#include "model_types.h"

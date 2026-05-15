#pragma once

/// @file model_data.h
/// @brief Convenience aggregator for the public model-data POD set.
///
/// Pulls in `model_types.h` (the actual struct definitions),
/// `display.h` (camera presets / display flags), `enums.h` (filter
/// modes, shading flags), and `types.h` (scalar/vector aliases). Host
/// code building model snapshots can include just this one header.

#include "display.h"
#include "enums.h"
#include "model_types.h"
#include "types.h"

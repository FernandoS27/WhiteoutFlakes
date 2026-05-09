#pragma once

/// @file
/// @brief Cross-TU declarations shared by the effect-binder file split.
///
/// The binding pipeline is broken across four translation units:
///   * effect_binder.cpp   — public `bind()` + driver + bindBakedLayers
///   * program_binder.cpp  — bytecode/program loaders
///   * renderer_binder.cpp — renderer property scan
///   * sampler_binder.cpp  — sampler builders
/// Each `load*` entry point is declared here; one shared utility
/// (`stableCopy`) is also exposed so file-local arena copies stay consistent.

#include <cornflakes/interface/asset/effect_asset_model.hpp>
#include <cornflakes/asset/object_accessors.hpp>
#include <cornflakes/interface/binding/layer_program.hpp>
#include <cornflakes/interface/core/arena.hpp>
#include <cornflakes/interface/core/types.hpp>

#include <string_view>

namespace whiteout::cornflakes {

/// @brief Copies `src` into the arena (returning a stable view) or `{}` if empty.
std::string_view stableCopy(std::string_view src, IArena& arena);

/// @brief Populates `lp.{init,physics,timeFixed,timeVarying,program}` programs.
void loadScopePrograms(const EffectAssetModel& model, const AssetObject& layerCache,
                       LayerProgram& lp, IArena& arena);

/// @brief Populates `lp.renderers` from the layer cache's Renderers field.
void loadRenderers(const EffectAssetModel& model, const AssetObject& layerCache, LayerProgram& lp,
                   IArena& arena);

/// @brief Populates `lp.samplers` (curve / shape / event-stream variants).
void loadSamplers(const EffectAssetModel& model, const AssetObject& layerCache, LayerProgram& lp,
                  IArena& arena);

} // namespace whiteout::cornflakes

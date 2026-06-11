#pragma once

#include <cornflakes/interface/asset/effect_asset_model.hpp>
#include <cornflakes/asset/object_accessors.hpp>
#include <cornflakes/interface/binding/layer_program.hpp>
#include <cornflakes/interface/core/arena.hpp>
#include <cornflakes/interface/core/types.hpp>

#include <string_view>

namespace whiteout::cornflakes {

std::string_view stableCopy(std::string_view src, IArena& arena);

void loadScopePrograms(const EffectAssetModel& model, const AssetObject& layerCache,
                       LayerProgram& lp, IArena& arena);

void loadRenderers(const EffectAssetModel& model, const AssetObject& layerCache, LayerProgram& lp,
                   IArena& arena);

void loadSamplers(const EffectAssetModel& model, const AssetObject& layerCache, LayerProgram& lp,
                  IArena& arena);

}

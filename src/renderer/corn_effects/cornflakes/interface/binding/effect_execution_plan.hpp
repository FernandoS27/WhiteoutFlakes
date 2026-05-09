#pragma once

/// @file
/// @brief Top-level bound plan: layers + effect-wide tables (externals, events, payloads).

#include <cornflakes/interface/asset/effect_asset_model.hpp>
#include <cornflakes/interface/binding/layer_program.hpp>
#include <cornflakes/interface/binding/tables.hpp>
#include <cornflakes/interface/schema/handles.hpp>

#include <span>

namespace whiteout::cornflakes {

/// @brief Effect-wide bound plan; spans into the bind arena owned by the binder caller.
struct EffectExecutionPlan {
    EffectId id;
    AssetVersion version;
    BakerGenerator generator = BakerGenerator::Editor;

    std::span<const LayerProgram> layers;
    ExternalBindingTable externalBindings;
    EventRoutingTable eventRouting;
    PayloadElementViewTable payloadViews;
};

} // namespace whiteout::cornflakes

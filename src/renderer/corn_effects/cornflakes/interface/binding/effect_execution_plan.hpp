#pragma once

#include <cornflakes/interface/asset/effect_asset_model.hpp>
#include <cornflakes/interface/binding/layer_program.hpp>
#include <cornflakes/interface/binding/tables.hpp>
#include <cornflakes/interface/schema/handles.hpp>

#include <span>

namespace whiteout::cornflakes {

struct EffectExecutionPlan {
    EffectId id;
    AssetVersion version;
    BakerGenerator generator = BakerGenerator::Editor;

    std::span<const LayerProgram> layers;
    ExternalBindingTable externalBindings;
    EventRoutingTable eventRouting;
    PayloadElementViewTable payloadViews;
};

}

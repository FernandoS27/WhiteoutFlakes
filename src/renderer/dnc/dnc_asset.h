#pragma once

#include "common_types.h"
#include "io/mdx_animation.h"
#include "renderer/dnc/dnc_service.h"
#include "renderer/types.h"

#include <whiteout/models/mdx/structures.h>

#include <string>

namespace whiteout::flakes::renderer::dnc {

struct DncAsset {

    std::string                 key;
    whiteout::mdx::Model        model;
    io::MdxHierarchy            hierarchy;
    i32                         seqStartMs   = 0;
    i32                         seqEndMs     = 0;
    i32                         lightNodeIdx = -1;
    u32                         refs         = 0;

    i32 AnimLengthMs() const { return seqEndMs - seqStartMs; }
    bool HasLight() const    { return lightNodeIdx >= 0; }
};

DncSample Sample(const DncAsset& asset,
                 f32             todHours,
                 f32             hoursPerDay,
                 f32             ambModifier = 0.0f);

}

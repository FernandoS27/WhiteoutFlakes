#pragma once

// The ShadingModelId → IShadingModel* map SurfacePass indexes. A fixed array
// rather than a container: the id space is a compile-time enum, lookup is on
// the per-draw path, and an unregistered id must be a cheap null rather than a
// miss that allocates or throws.
//
// Products are compile-time optional, so a build with WDX_ENABLE_M3=OFF simply
// never registers the M3 ids — and SurfacePass's null check is what turns "this
// draw names a model this build doesn't have" into a skipped draw instead of
// undefined behaviour.

#include "core/surface_vocabulary.h"
#include "whiteout/flakes/types.h"

#include <array>

namespace whiteout::flakes::renderer::shading {

class IShadingModel;

class ShadingRegistry {
public:
    /// @brief Register @p model under its own Id(). Replaces any previous
    ///        entry. The registry does not own the model — profiles hold them.
    void Register(IShadingModel* model);

    void Unregister(core::ShadingModelId id) {
        const auto i = static_cast<usize>(id);
        if (i < models_.size())
            models_[i] = nullptr;
    }

    void Clear() {
        models_.fill(nullptr);
    }

    /// @brief The model for @p id, or null if this build never registered it.
    IShadingModel* Get(core::ShadingModelId id) const {
        const auto i = static_cast<usize>(id);
        return (i < models_.size()) ? models_[i] : nullptr;
    }

private:
    std::array<IShadingModel*, static_cast<usize>(core::ShadingModelId::Count)> models_{};
};

} // namespace whiteout::flakes::renderer::shading

#include <cornflakes/interface/binding/spatial_layer_resource.hpp>

namespace whiteout::cornflakes {

const SpatialLayerResource* findSpatialLayerByName(std::span<const SpatialLayerResource> layers,
                                                   std::string_view name) noexcept {
    if (name.empty()) {
        return nullptr;
    }
    for (const auto& l : layers) {
        if (l.name == name) {
            return &l;
        }
    }
    return nullptr;
}

} // namespace whiteout::cornflakes

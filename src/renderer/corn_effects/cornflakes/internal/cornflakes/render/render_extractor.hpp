#pragma once

/// @file
/// @brief Builds `RenderPacket`s from a `MediumState` page set and a `LayerProgram`.

#include <cornflakes/interface/binding/layer_program.hpp>
#include <cornflakes/interface/core/arena.hpp>
#include <cornflakes/interface/diagnostics/issue.hpp>
#include <cornflakes/interface/render/render_packet.hpp>
#include <cornflakes/interface/schema/handles.hpp>
#include <cornflakes/sim/medium.hpp>
#include <cornflakes/sim/particle_page.hpp>

#include <span>
#include <vector>

namespace whiteout::cornflakes {

/// @brief Per-emitter packet builder for the page-based simulation path.
class RenderExtractor {
public:
    RenderExtractor() = default;

    /// @brief Walk every page in `medium` and produce one packet per page per renderer.
    std::vector<RenderPacket> extract(const MediumState& medium, const LayerProgram& layer,
                                      IArena& frameArena, IssueBag& issues) const;

    /// @brief Build a single packet for `page` of class `cls`. Spans live in `frameArena`.
    RenderPacket extractPacket(const ParticlePage& page, EmitterId emitter, LayerId layer,
                               RendererClass cls, IArena& frameArena, IssueBag& issues) const;
};

} // namespace whiteout::cornflakes

#pragma once

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

class RenderExtractor {
public:
    RenderExtractor() = default;

    std::vector<RenderPacket> extract(const MediumState& medium, const LayerProgram& layer,
                                      IArena& frameArena, IssueBag& issues) const;

    RenderPacket extractPacket(const ParticlePage& page, EmitterId emitter, LayerId layer,
                               RendererClass cls, IArena& frameArena, IssueBag& issues) const;
};

}

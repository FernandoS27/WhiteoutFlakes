#pragma once

#include <cornflakes/interface/binding/layer_program.hpp>
#include <cornflakes/interface/diagnostics/issue.hpp>
#include <cornflakes/interface/render/render_packet.hpp>
#include <cornflakes/interface/render/render_view.hpp>

#include <span>

namespace whiteout::cornflakes {

class IRenderBackend {
public:
    virtual ~IRenderBackend() = default;

    virtual bool prepare(std::span<const LayerProgram> layers, IssueBag& issues) = 0;

    virtual void submit(std::span<const RenderPacket> packets, const ViewParams& view,
                        IssueBag& issues) = 0;

    virtual void shutdown(IssueBag& issues) = 0;
};

}

#pragma once

/// @file
/// @brief Backend interface — a sink that consumes render packets each frame.

#include <cornflakes/interface/binding/layer_program.hpp>
#include <cornflakes/interface/diagnostics/issue.hpp>
#include <cornflakes/interface/render/render_packet.hpp>
#include <cornflakes/interface/render/render_view.hpp>

#include <span>

namespace whiteout::cornflakes {

/// @brief Render-backend sink interface.
///
/// Lifecycle: `prepare()` once per effect (compile shader perms, allocate
/// resources), `submit()` once per tick, `shutdown()` once at teardown.
class IRenderBackend {
public:
    virtual ~IRenderBackend() = default;

    /// @brief Pre-compile / pre-allocate per the layer set; called once before the first submit.
    virtual bool prepare(std::span<const LayerProgram> layers, IssueBag& issues) = 0;

    /// @brief Consume one frame's packets.
    virtual void submit(std::span<const RenderPacket> packets, const ViewParams& view,
                        IssueBag& issues) = 0;

    virtual void shutdown(IssueBag& issues) = 0;
};

} // namespace whiteout::cornflakes

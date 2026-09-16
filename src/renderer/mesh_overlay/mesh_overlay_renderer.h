#pragma once

// Mesh overlay, GPU half: packs each geoset's element buffer and issues the
// face, edge and vertex draws (DEBUG_VIEW_DESIGN.md §9). A shading model calls
// Emit from inside its own draw, once its per-draw constants and palette are
// bound, naming the overlay vertex stage that skins the way its surface
// program does. Everything is created on first use, so a session that never
// shows a wireframe allocates nothing.

#include "core/debug_view.h"
#include "core/mesh_overlay.h"
#include "core/surface_vocabulary.h"
#include "gfx/gfx.h"
#include "whiteout/flakes/types.h"

#include <map>
#include <set>
#include <utility>

namespace whiteout::flakes::renderer {
class RenderService;
}

namespace whiteout::flakes::renderer::render_detail {
struct RenderableView;
}

namespace whiteout::flakes::renderer::model {
struct RenderModel;
}

namespace whiteout::flakes::renderer::mesh_overlay {

// The attachments of the pass an overlay draw lands in; its PSO must match.
struct OverlayTarget {
    gfx::Format rtv = gfx::Format::Unknown;
    gfx::Format extra[3] = {gfx::Format::Unknown, gfx::Format::Unknown, gfx::Format::Unknown};
    u32 extraCount = 0;
    gfx::Format dsv = gfx::Format::Unknown;
};

struct OverlayDraw {
    const render_detail::RenderableView* view = nullptr;
    i32 geoIdx = -1;
    // The product's overlay entry for this draw's skinning, and the vertex
    // constant-buffer slot it reads MeshOverlayData from.
    gfx::ShaderHandle vs = gfx::ShaderHandle::Invalid;
    u32 cbSlot = 3;
    OverlayTarget target;
};

class MeshOverlayRenderer {
public:
    explicit MeshOverlayRenderer(RenderService& rs) : rs_(rs) {}

    /// @brief Once per viewport, after the frame's debug view is resolved.
    void BeginFrame(const core::DebugFrame& frame, const core::DebugTargetInfo& target, i32 width,
                    i32 height, const Matrix44f& projection, u32 backgroundRgb);

    /// @brief Whether a model drawing into `pass` should emit at all.
    bool Wants(core::PassSlot pass) const;

    /// @brief One geoset's overlay, at most once per frame however many
    ///        surfaces the geoset draws.
    void Emit(gfx::IGFXCommandList* cmd, const OverlayDraw& draw);

    /// @brief Destroy everything this owns while the device is alive.
    void Release();

private:
    struct PsoKey {
        u64 vs = 0;
        u32 pass = 0;
        bool occluded = false;
        bool markedOnly = false;
        gfx::Format rtv = gfx::Format::Unknown;
        gfx::Format extra0 = gfx::Format::Unknown;
        gfx::Format extra1 = gfx::Format::Unknown;
        gfx::Format extra2 = gfx::Format::Unknown;
        u32 extraCount = 0;
        gfx::Format dsv = gfx::Format::Unknown;
        auto operator<=>(const PsoKey&) const = default;
    };

    void Init();
    gfx::PipelineHandle Pso(const PsoKey& key);
    // Packs the geoset's element buffer when its states, its topology or its
    // deform moved. False when the geoset has nothing to draw from.
    bool Prepare(const render_detail::RenderableView& view, i32 geoIdx);

    RenderService& rs_;
    bool initTried_ = false;
    gfx::ShaderHandle ps_ = gfx::ShaderHandle::Invalid;
    gfx::BufferHandle cb_ = gfx::BufferHandle::Invalid;
    std::map<PsoKey, gfx::PipelineHandle> psos_;

    core::DebugFrame frame_;
    bool targetEncodesSrgb_ = false;
    f32 halfViewport_[2] = {0, 0};
    f32 depthPull_ = 0.0f;
    u32 background_ = 0;
    // The (actor, geoset) pairs emitted this frame.
    std::set<std::pair<const model::RenderModel*, i32>> emitted_;
};

} // namespace whiteout::flakes::renderer::mesh_overlay

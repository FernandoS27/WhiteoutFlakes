#pragma once

// SurfacePass — the submission loop, lifted out of the pass classes so it is
// written once and dispatches to whichever model each item names.
//
// Two entry shapes because the renderer has two: the opaque pass hands over a
// whole list, while the unified transparent queue interleaves geosets with
// particles, ribbons and corn by depth and can only offer one item at a time.
// Both share the same open/close bookkeeping, which is the part worth having in
// one place.

#include "core/draw_list.h"
#include "core/render_detail.h"
#include "core/surface_vocabulary.h"
#include "shading/shading_registry.h"
#include "whiteout/flakes/types.h"

#include <span>

namespace whiteout::flakes::renderer::shading {

class IShadingModel;

class SurfacePass {
public:
    explicit SurfacePass(const ShadingRegistry& models) : models_(models) {}

    /// @brief Submit one item. Opens the naming model's pass when the sequence
    ///        crosses a model boundary, closing the previous one first.
    void Submit(const render_detail::DrawItem& item, const core::PassContext& ctx,
                const render_detail::CollectedDrawLists& lists);

    /// @brief Close whatever is still open. Must be called once the caller is
    ///        done submitting, including when it submitted nothing.
    void Finish(const core::PassContext& ctx);

    /// @brief Submit a whole list, then Finish.
    void Run(std::span<const render_detail::DrawItem> items, const core::PassContext& ctx,
             const render_detail::CollectedDrawLists& lists);

private:
    const ShadingRegistry& models_;
    IShadingModel* open_ = nullptr;                              // BeginPass succeeded
    core::ShadingModelId tried_ = core::ShadingModelId::None;    // last id attempted
};

} // namespace whiteout::flakes::renderer::shading

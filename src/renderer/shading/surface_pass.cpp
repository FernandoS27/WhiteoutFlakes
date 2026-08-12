#include "shading/surface_pass.h"

#include "shading/shading_model.h"

namespace whiteout::flakes::renderer::shading {

void ShadingRegistry::Register(IShadingModel* model) {
    if (!model)
        return;
    const auto i = static_cast<usize>(model->Id());
    if (i < models_.size())
        models_[i] = model;
}

void SurfacePass::Submit(const render_detail::DrawItem& item, const core::PassContext& ctx,
                         const render_detail::CollectedDrawLists& lists) {
    // A surface that opted out of this pass is not a draw at all. WC3 surfaces
    // all carry PassMask::Default, so today this never fires; M2 shadow batches
    // and M3 unshaded materials are what it exists for.
    if (!Any(item.key.passes & core::PassMaskBit(ctx.pass)))
        return;

    // Compare against `tried`, not `open`. A model whose BeginPass declines is
    // probed once per run of its items rather than once per draw — otherwise an
    // unavailable model costs a virtual call and a state check on every item it
    // names.
    if (item.key.model != tried_) {
        if (open_)
            open_->EndPass(ctx);
        tried_ = item.key.model;
        IShadingModel* m = models_.Get(tried_);
        // Null when this build never registered the id — a WDX_ENABLE_M3=OFF
        // binary meeting an M3 surface. Skip it rather than dereferencing.
        open_ = (m && m->BeginPass(ctx, lists)) ? m : nullptr;
    }
    if (open_)
        open_->Draw(item, ctx);
}

void SurfacePass::Finish(const core::PassContext& ctx) {
    if (open_)
        open_->EndPass(ctx);
    open_ = nullptr;
    tried_ = core::ShadingModelId::None;
}

void SurfacePass::Run(std::span<const render_detail::DrawItem> items, const core::PassContext& ctx,
                      const render_detail::CollectedDrawLists& lists) {
    for (const auto& item : items)
        Submit(item, ctx, lists);
    Finish(ctx);
}

} // namespace whiteout::flakes::renderer::shading

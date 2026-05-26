#include "io/mdx_model_adapter.h"
#include "renderer/animation/animation.h"
#include "renderer/model/model_template.h"

namespace whiteout::flakes::renderer::model {

ModelTemplate::ModelTemplate() = default;
ModelTemplate::~ModelTemplate() {
    // RAII GPU cleanup. The device pointer is set the first time
    // uploadTemplateGpu runs; if the template was never uploaded
    // (failed parse, never-spawned variant) the pointer stays null
    // and there's nothing to release. The renderer's shutdown path
    // explicitly calls ReleaseAllGPU before tearing down the device,
    // which clears gpuUploaded and zeroes the device pointer, so we
    // don't double-destroy or use a dangling device here.
    if (gpuDevice && gpuUploaded) {
        ReleaseGPU(*gpuDevice);
    }
}

void ModelTemplate::ReleaseGPU(gfx::IGFXDevice& gfx) {
    for (auto& g : sharedGeosets) {
        gfx.Destroy(g.ib);
        gfx.Destroy(g.unskinnedVb);
        gfx.Destroy(g.unskinnedVb1);
        gfx.Destroy(g.tangentVb);
        gfx.Destroy(g.boneVb);
    }
    sharedGeosets.clear();
    gpuUploaded = false;
    // Clear the device pointer so ~ModelTemplate doesn't try to
    // free a second time. The host (RenderPipeline shutdown) calls
    // ReleaseGPU explicitly before tearing the device down — after
    // that, any still-alive template just no-ops on destruction.
    gpuDevice = nullptr;
}

} // namespace whiteout::flakes::renderer::model

#include "renderer/model/model_template.h"
#include "renderer/animation/animation.h"
#include "io/mdx_model_adapter.h"

namespace whiteout::flakes::renderer::model {

ModelTemplate::ModelTemplate()  = default;
ModelTemplate::~ModelTemplate() = default;

void ModelTemplate::ReleaseGPU(gfx::IGFXDevice& gfx) {
    for (auto& g : sharedGeosets) {
        gfx.Destroy(g.ib);
        gfx.Destroy(g.unskinnedVb);
        gfx.Destroy(g.unskinnedVb1);
        gfx.Destroy(g.tangentVb);
        gfx.Destroy(g.boneVb);
    }
    sharedGeosets.clear();

    templateTextures.reset();
    gpuUploaded = false;
}

}

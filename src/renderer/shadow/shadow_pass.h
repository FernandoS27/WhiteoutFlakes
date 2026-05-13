#pragma once

#include "gfx/gfx.h"
#include "renderer/shadow/shadow_service.h"

namespace whiteout::flakes::renderer {
class RenderService;
}

namespace whiteout::flakes::renderer::shadow {

class ShadowPass {
public:
    explicit ShadowPass(RenderService& rs) : rs_(rs) {}

    bool Run(ShadowService& service);

private:
    RenderService& rs_;
};

} // namespace whiteout::flakes::renderer::shadow

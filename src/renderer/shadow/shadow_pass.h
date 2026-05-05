#pragma once

#include "renderer/shadow/shadow_service.h"
#include "gfx/gfx.h"

namespace WhiteoutDex {
class RenderService;
}

namespace WhiteoutDex::shadow {

class ShadowPass {
public:
    explicit ShadowPass(RenderService& rs) : rs_(rs) {}

    bool Run(ShadowService& service);

private:
    RenderService& rs_;
};

}

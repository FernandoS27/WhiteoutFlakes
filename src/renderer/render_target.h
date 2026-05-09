#pragma once

#include "whiteout/flakes/types.h"
#include "gfx/gfx.h"
#include "whiteout/flakes/types.h"
#include "whiteout/flakes/enums.h"
#include "whiteout/flakes/display.h"

namespace whiteout::flakes::renderer {

// Public-API value types are re-imported into the renderer-internal namespace
// so existing code that says `whiteout::flakes::renderer::Rect` /
// `whiteout::flakes::renderer::RenderMode` / etc. keeps compiling. The
// canonical definitions live in include/whiteout/flakes/{types,enums,display}.h.
using ::whiteout::flakes::Rect;
using ::whiteout::flakes::RenderTargetId;
using ::whiteout::flakes::RenderMode;
using ::whiteout::flakes::LightingMode;
using ::whiteout::flakes::IblMode;
using ::whiteout::flakes::DisplayFlags;

struct RenderTarget {
    RenderTargetId        id     = 0;
    gfx::SwapChainHandle  swap   = gfx::SwapChainHandle::Invalid;

    gfx::TextureHandle    color  = gfx::TextureHandle::Invalid;

    gfx::TextureHandle    colorLinear = gfx::TextureHandle::Invalid;

    gfx::TextureHandle    hdrColor = gfx::TextureHandle::Invalid;
    gfx::TextureHandle    depth  = gfx::TextureHandle::Invalid;
    i32                   width  = 0;
    i32                   height = 0;
};

}

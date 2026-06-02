#pragma once

#include "gfx/gfx.h"
#include "whiteout/flakes/display.h"
#include "whiteout/flakes/enums.h"
#include "whiteout/flakes/types.h"

namespace whiteout::flakes::renderer {

// Public-API value types are re-imported into the renderer-internal namespace
// so existing code that says `whiteout::flakes::renderer::Rect` /
// `whiteout::flakes::renderer::RenderMode` / etc. keeps compiling. The
// canonical definitions live in include/whiteout/flakes/{types,enums,display}.h.
using ::whiteout::flakes::DisplayFlags;
using ::whiteout::flakes::IblMode;
using ::whiteout::flakes::LightingMode;
using ::whiteout::flakes::Rect;
using ::whiteout::flakes::RenderMode;
using ::whiteout::flakes::RenderTargetId;

struct RenderTarget {
    RenderTargetId id = 0;
    gfx::SwapChainHandle swap = gfx::SwapChainHandle::Invalid;

    gfx::TextureHandle color = gfx::TextureHandle::Invalid;

    gfx::TextureHandle colorLinear = gfx::TextureHandle::Invalid;

    gfx::TextureHandle hdrColor = gfx::TextureHandle::Invalid;
    gfx::TextureHandle depth = gfx::TextureHandle::Invalid;

    // G-buffer auxiliary color attachments. The HD opaque scene pass binds
    // `hdrColor` (slot 0), `linearDepth` (slot 1, R32F view-space Z), and
    // `normalBuffer` (slot 2, RGBA8 encoded world normal) as a single MRT
    // render pass — mirrors W3 Reforged's `s_worldFBHD` layout (depth +
    // 3 colors). Consumed by later post-process passes (SSAO, fog,
    // distortion). SD mode leaves these `Invalid` — its single-RT pass
    // never references them.
    gfx::TextureHandle linearDepth = gfx::TextureHandle::Invalid;
    gfx::TextureHandle normalBuffer = gfx::TextureHandle::Invalid;

    // GTAO ambient-occlusion buffers. Both R8_UNORM, full-res, HD-only.
    //   aoBufferRaw — noisy GTAO output from the main pass (4×4 horizon
    //                 trace; visible noise without denoise).
    //   aoBuffer    — denoised final factor read by the apply pass that
    //                 modulates hdrColor in place via Zero/SrcColor blend.
    // The spatial denoise pass reads `aoBufferRaw` + `linearDepth` and
    // writes `aoBuffer`. SD mode leaves both Invalid.
    gfx::TextureHandle aoBufferRaw = gfx::TextureHandle::Invalid;
    gfx::TextureHandle aoBuffer = gfx::TextureHandle::Invalid;

    i32 width = 0;
    i32 height = 0;
};

} // namespace whiteout::flakes::renderer

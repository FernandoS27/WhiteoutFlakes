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

    // Previous frame's linearDepth. Written at the end of GTAO's temporal
    // pass (copy of the current frame's linearDepth) and sampled at the
    // top of next frame's temporal pass to detect disocclusion: a large
    // delta between the depth here at prevUv and the depth a static
    // reprojection predicts means the prev-frame surface is no longer
    // visible, so the history sample is rejected.
    gfx::TextureHandle linearDepthHistory = gfx::TextureHandle::Invalid;

    // GTAO ambient-occlusion buffers. Four R8_UNORM scalars + an RGBA8
    // bent normal, all full-res, HD-only.
    //   aoBufferRaw      — noisy GTAO output from the main pass.
    //   aoBufferDenoised — spatial-denoised, pre-temporal.
    //   aoBuffer         — final factor read by the apply pass that
    //                      modulates hdrColor via Zero/SrcColor blend.
    //   aoBufferHistory  — previous frame's final factor; GtaoService
    //                      ping-pongs (`aoBuffer`, `aoBufferHistory`)
    //                      role each frame to avoid a copy.
    //   bentNormalBuffer — view-space bent normal (xyz = n*0.5+0.5,
    //                      w = visibility). MRT slot 1 of the main pass.
    // SD mode leaves all five Invalid.
    gfx::TextureHandle aoBufferRaw = gfx::TextureHandle::Invalid;
    gfx::TextureHandle aoBufferDenoised = gfx::TextureHandle::Invalid;
    gfx::TextureHandle aoBuffer = gfx::TextureHandle::Invalid;
    gfx::TextureHandle aoBufferHistory = gfx::TextureHandle::Invalid;
    gfx::TextureHandle bentNormalBuffer = gfx::TextureHandle::Invalid;

    // Bloom scratch buffers. Full-res HDR scratch the bloom pass ping-pongs
    // between for extract → blur-H → blur-V → combine. After the combine
    // pass the composited HDR scene+bloom lives in `bloomScratchB`; the
    // service blits it back over `hdrColor` so the tonemap downstream is
    // unaware of bloom. Both formats match `hdrColor`'s HDR format.
    gfx::TextureHandle bloomScratchA = gfx::TextureHandle::Invalid;
    gfx::TextureHandle bloomScratchB = gfx::TextureHandle::Invalid;

    i32 width = 0;
    i32 height = 0;
};

} // namespace whiteout::flakes::renderer

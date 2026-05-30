// CreateSwapChain / ResizeSwapChain / DestroySwapChain / Present + the
// per-frame [layer nextDrawable] acquire path. Mirrors
// src/gfx/webgpu/webgpu_swap_chain.cpp.
//
// The swap chain attaches a CAMetalLayer to the GLFW NSWindow's
// contentView (via metal_surface_macos.mm), sets the layer's pixelFormat
// to the requested colorFormat (resolved sRGB ↔ linear), and stores two
// proxy textures (sRGB view + linear partner view) in the texture slot
// map. AcquireSwapChainImageIfNeeded — called from BeginRenderPass —
// pulls [layer nextDrawable] and re-points each proxy's `texture` to
// the drawable's texture (sRGB-typed) and a linear-typed view of the
// same pixels (via newTextureViewWithPixelFormat).

#include "metal_command_list.h"
#include "metal_device.h"
#include "metal_device_state.h"
#include "metal_handles.h"
#include "metal_translate.h"

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <cstdio>

#if defined(__APPLE__)
// Force GLFW to expose the native-handle helpers (glfwGetCocoaWindow).
// Pulled in headers-only — no link impact, GLFW is already linked into
// the standalone target.
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#endif

namespace whiteout::flakes::gfx::metal {

// Defined in metal_surface_macos.mm.
void* CreateMetalLayerForCocoaWindow(void* nsWindow);

namespace {

// Allocate a TextureEntry slot that aliases the swap-chain's drawable.
// We track sRGB and linear views in two separate slots so the renderer
// can pick the right colorspace per draw without re-creating the
// CAMetalLayer.
TextureHandle InsertSwapChainProxy(MetalDeviceState& state, SwapChainHandle owner,
                                   MTLPixelFormat fmt, bool isLinear,
                                   u32 width, u32 height) {
    TextureEntry entry;
    entry.format = fmt;
    entry.width = static_cast<i32>(width);
    entry.height = static_cast<i32>(height);
    entry.swapChainProxy = owner;
    entry.ownsTexture = false;
    entry.isLinearView = isLinear;
    return static_cast<TextureHandle>(state.textures.Insert(std::move(entry)));
}

void DestroyProxy(MetalDeviceState& state, TextureHandle h) {
    if (h == TextureHandle::Invalid)
        return;
    if (auto* p = state.textures.Get(static_cast<u64>(h))) {
        p->texture = nil;
        p->viewLinear = nil;
    }
    state.textures.Remove(static_cast<u64>(h));
}

// Cast helper. CAMetalLayer ownership lives in the NSView's `layer`
// property — we just borrow the pointer.
CAMetalLayer* LayerOf(SwapChainEntry& sc) {
    return sc.layer;
}

} // namespace

// Called at the top of BeginRenderPass before binding the encoder. Pulls
// a fresh drawable if one wasn't already acquired this frame, then
// re-points the two proxy TextureEntry slots at the drawable's texture
// (and a linear-typed view of it).
void AcquireSwapChainImageIfNeeded(MetalDeviceState& state, SwapChainEntry& sc) {
    if (sc.acquiredThisFrame && sc.currentDrawable)
        return;

    CAMetalLayer* layer = LayerOf(sc);
    if (!layer)
        return;

    sc.currentDrawable = [layer nextDrawable];
    if (!sc.currentDrawable) {
        // Drawable acquisition can fail in pathological cases (window
        // minimised, layer not yet hosted). Surfaces as a missed frame
        // rather than a crash — the caller will skip the render.
        sc.acquiredThisFrame = false;
        return;
    }

    id<MTLTexture> bb = sc.currentDrawable.texture;
    if (auto* p = state.textures.Get(static_cast<u64>(sc.proxySrgb))) {
        p->texture = bb;
        p->width = static_cast<i32>(sc.width);
        p->height = static_cast<i32>(sc.height);
    }
    if (sc.proxyLinear != TextureHandle::Invalid && sc.formatLinear != sc.formatSrgb) {
        if (auto* p = state.textures.Get(static_cast<u64>(sc.proxyLinear))) {
            // newTextureViewWithPixelFormat is a thin reinterpret; the
            // drawable's underlying memory is reused. Releases on next
            // re-point (and on Present when we drop currentDrawable).
            p->texture = [bb newTextureViewWithPixelFormat:sc.formatLinear];
            p->width = static_cast<i32>(sc.width);
            p->height = static_cast<i32>(sc.height);
        }
    }
    sc.acquiredThisFrame = true;
}

SwapChainHandle MetalDevice::CreateSwapChain(void* nativeWindowHandle, i32 width, i32 height,
                                             Format colorFormat) {
    @autoreleasepool {
        auto& state = *state_;

        // The host (viewer_app.cpp / equivalent) passes a GLFWwindow* —
        // same convention WebGPU uses on macOS. Pulling glfwGetCocoaWindow
        // here keeps the gfx layer's contract platform-agnostic.
        void* nsWindow = nullptr;
#if defined(__APPLE__)
        GLFWwindow* gw = static_cast<GLFWwindow*>(nativeWindowHandle);
        // glfwGetCocoaWindow returns id (NSWindow*). Under ARC we cross
        // back to a raw void* via __bridge — the contentView keeps the
        // NSWindow alive, so no ownership transfer.
        nsWindow = gw ? (__bridge void*)glfwGetCocoaWindow(gw) : nullptr;
#endif
        if (!nsWindow) {
            std::fprintf(stderr, "[gfx/metal] CreateSwapChain: no NSWindow handle\n");
            return SwapChainHandle::Invalid;
        }

        CAMetalLayer* layer =
            (__bridge CAMetalLayer*)CreateMetalLayerForCocoaWindow(nsWindow);
        if (!layer) {
            std::fprintf(stderr, "[gfx/metal] CreateSwapChain: layer attach failed\n");
            return SwapChainHandle::Invalid;
        }

        MTLPixelFormat sRgbFmt = ToMtlPixelFormat(colorFormat);
        if (sRgbFmt == MTLPixelFormatInvalid)
            sRgbFmt = MTLPixelFormatBGRA8Unorm_sRGB;
        MTLPixelFormat linearFmt = LinearPartnerOf(sRgbFmt);

        layer.device = state.device;
        layer.pixelFormat = sRgbFmt;
        // framebufferOnly=NO lets us create the linear-typed view via
        // newTextureViewWithPixelFormat. Cost: marginal — the drawable
        // is still presentable, just no longer guaranteed to live in
        // tiled memory. Wc3 doesn't push the GPU hard enough for that
        // to matter.
        layer.framebufferOnly = NO;
        layer.drawableSize = CGSizeMake(width, height);
        // CAMetalLayer defaults: displaySyncEnabled=YES and
        // maximumDrawableCount=3, which is exactly what we want.
        // Explicitly setting them sometimes triggers a regression
        // on macOS 14+ where the layer's drawable queue stalls in
        // a way that the GPU later reports as a CB hang. Leaving
        // the defaults alone fixed an observed GPU-hang at
        // submitted=1; revisit if a future macOS changes the
        // defaults.

        SwapChainEntry sc;
        sc.layer = layer;
        sc.formatSrgb = sRgbFmt;
        sc.formatLinear = linearFmt;
        sc.width = static_cast<u32>(width);
        sc.height = static_cast<u32>(height);

        SwapChainHandle handle =
            static_cast<SwapChainHandle>(state.swapchains.Insert(std::move(sc)));

        auto* sc2 = state.swapchains.Get(static_cast<u64>(handle));
        sc2->proxySrgb = InsertSwapChainProxy(state, handle, sRgbFmt, /*isLinear=*/false,
                                              sc2->width, sc2->height);
        if (linearFmt != sRgbFmt) {
            sc2->proxyLinear = InsertSwapChainProxy(state, handle, linearFmt,
                                                    /*isLinear=*/true,
                                                    sc2->width, sc2->height);
        }

        return handle;
    }
}

void MetalDevice::ResizeSwapChain(SwapChainHandle h, i32 width, i32 height) {
    @autoreleasepool {
        auto& state = *state_;
        auto* sc = state.swapchains.Get(static_cast<u64>(h));
        if (!sc || !sc->layer)
            return;
        sc->width = static_cast<u32>(width);
        sc->height = static_cast<u32>(height);
        sc->layer.drawableSize = CGSizeMake(width, height);
        // Drop the cached drawable — the next AcquireSwapChainImageIfNeeded
        // will pull a fresh one matching the new drawableSize.
        sc->currentDrawable = nil;
        sc->acquiredThisFrame = false;
        if (auto* p = state.textures.Get(static_cast<u64>(sc->proxySrgb))) {
            p->width = static_cast<i32>(sc->width);
            p->height = static_cast<i32>(sc->height);
            p->texture = nil;
        }
        if (sc->proxyLinear != TextureHandle::Invalid) {
            if (auto* p = state.textures.Get(static_cast<u64>(sc->proxyLinear))) {
                p->width = static_cast<i32>(sc->width);
                p->height = static_cast<i32>(sc->height);
                p->texture = nil;
            }
        }
    }
}

void MetalDevice::DestroySwapChain(SwapChainHandle h) {
    @autoreleasepool {
        auto& state = *state_;
        auto* sc = state.swapchains.Get(static_cast<u64>(h));
        if (!sc)
            return;
        DestroyProxy(state, sc->proxySrgb);
        DestroyProxy(state, sc->proxyLinear);
        sc->currentDrawable = nil;
        sc->layer = nil;
        state.swapchains.Remove(static_cast<u64>(h));
    }
}

void MetalDevice::Present(SwapChainHandle h) {
    @autoreleasepool {
        auto& state = *state_;
        auto* sc = state.swapchains.Get(static_cast<u64>(h));
        if (!sc)
            return;

        // End any in-flight encoder before submitting. The renderer's
        // FrameTicker calls EndRenderPass before Present, but compute /
        // blit encoders might still be open if a future path doesn't.
        auto& frame = state.frames[state.currentFrame];
        if (frame.renderEncoder) {
            [frame.renderEncoder endEncoding];
            frame.renderEncoder = nil;
        }
        if (frame.computeEncoder) {
            [frame.computeEncoder endEncoding];
            frame.computeEncoder = nil;
        }
        if (frame.blitEncoder) {
            [frame.blitEncoder endEncoding];
            frame.blitEncoder = nil;
        }

        if (!frame.commandBuffer) {
            // Nothing was recorded this frame; skip the commit so we
            // don't churn an empty CB. Drop the drawable so the next
            // frame's Acquire pulls a fresh one.
            sc->currentDrawable = nil;
            sc->acquiredThisFrame = false;
            return;
        }

        if (sc->currentDrawable) {
            [frame.commandBuffer presentDrawable:sc->currentDrawable];
            sc->currentDrawable = nil;
        }
        sc->acquiredThisFrame = false;

        const DeleteEpoch submittedEpoch = ++state.pendingEpoch;
        frame.epoch = submittedEpoch;

        // Completion handler runs on a Metal-internal thread — keep the
        // captured set tiny and lock-free. Updating `completedEpoch`
        // signals the deferred-delete drain that the GPU has retired
        // everything submitted up to `submittedEpoch`. We also surface
        // any command-buffer error (device lost, etc.) here — without
        // this they go to dev/null and the visible symptom is just an
        // all-black window with no log trace.
        MetalDeviceState* sp = &state;
        [frame.commandBuffer addCompletedHandler:^(id<MTLCommandBuffer> cb) {
            // Relaxed is enough — readers (delete-queue drain in the
            // command-list path) re-read with the same ordering.
            DeleteEpoch prev =
                sp->completedEpoch.load(std::memory_order_relaxed);
            while (prev < submittedEpoch &&
                   !sp->completedEpoch.compare_exchange_weak(
                       prev, submittedEpoch, std::memory_order_relaxed)) {
            }
            if (cb.status == MTLCommandBufferStatusError) {
                NSError* e = cb.error;
                std::fprintf(stderr,
                    "[gfx/metal] CB ERROR submitted=%llu: %s\n",
                    (unsigned long long)submittedEpoch,
                    e ? [[e localizedDescription] UTF8String] : "(no error info)");
                NSArray<id<MTLCommandBufferEncoderInfo>>* infos =
                    e ? e.userInfo[MTLCommandBufferEncoderInfoErrorKey] : nil;
                for (id<MTLCommandBufferEncoderInfo> info in infos) {
                    std::fprintf(stderr,
                        "[gfx/metal]   encoder '%s' state=%ld signpost='%s'\n",
                        info.label ? [info.label UTF8String] : "(unnamed)",
                        (long)info.errorState,
                        [[[info.debugSignposts componentsJoinedByString:@" → "]
                            description] UTF8String]);
                }
            }
        }];

        [frame.commandBuffer commit];

        frame.commandBuffer = nil;
        frame.recording = false;

        state.currentFrame = (state.currentFrame + 1) % kFramesInFlight;
    }
}

TextureHandle MetalDevice::GetSwapChainBackBuffer(SwapChainHandle h) {
    auto* sc = state_->swapchains.Get(static_cast<u64>(h));
    return sc ? sc->proxySrgb : TextureHandle::Invalid;
}

TextureHandle MetalDevice::GetSwapChainBackBufferLinear(SwapChainHandle h) {
    auto* sc = state_->swapchains.Get(static_cast<u64>(h));
    if (!sc)
        return TextureHandle::Invalid;
    return sc->proxyLinear != TextureHandle::Invalid ? sc->proxyLinear : sc->proxySrgb;
}

Format MetalDevice::GetSwapChainFormat(SwapChainHandle h) const {
    auto* sc = state_->swapchains.Get(static_cast<u64>(h));
    if (!sc)
        return Format::Unknown;
    return MtlPixelFormatToGfx(sc->formatSrgb);
}

} // namespace whiteout::flakes::gfx::metal

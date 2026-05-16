// wgpu::Surface lifetime, GetCurrentTexture, Present.
//
// Mirrors src/gfx/vulkan/vulkan_swap_chain.cpp's proxy-texture trick:
// the renderer caches one TextureHandle at CreateSwapChain and we
// re-point it at the surface's current texture in
// AcquireSwapChainImageIfNeeded (called from BeginRenderPass).
//
// Platform note: Windows-only for first pass. Linux (xlib / wayland) and
// macOS (CAMetalLayer) follow in Phase 7 — both go through the same
// SurfaceDescriptor + platform `next` chain, just with a different
// concrete SurfaceSource* type.

#include "webgpu_device.h"
#include "webgpu_device_state.h"
#include "webgpu_handles.h"
#include "webgpu_translate.h"

#include <algorithm>
#include <cstdio>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace whiteout::flakes::gfx::webgpu {

namespace {

wgpu::Surface CreateSurfaceForWindow(WebGPUDeviceState& state, void* nativeWindowHandle) {
#if defined(_WIN32)
    wgpu::SurfaceSourceWindowsHWND fromHwnd{};
    fromHwnd.hwnd = nativeWindowHandle;
    fromHwnd.hinstance = ::GetModuleHandleW(nullptr);
    wgpu::SurfaceDescriptor sd{};
    sd.nextInChain = &fromHwnd;
    return state.instance.CreateSurface(&sd);
#else
    // Phase 7: route through GLFW-derived xlib / wayland / metal descriptors.
    (void)state;
    (void)nativeWindowHandle;
    return nullptr;
#endif
}

void ConfigureSurface(WebGPUDeviceState& state, SwapChainEntry& sc, u32 width, u32 height,
                      Format colorFormat) {
    sc.width = width;
    sc.height = height;

    // Pick sRGB / linear pair. We list both in `viewFormats` so each
    // proxy texture can derive its own view at acquire time — same
    // MUTABLE_FORMAT trick as the Vulkan backend.
    sc.formatSrgb = ToWgpuFormat(colorFormat);
    if (sc.formatSrgb == wgpu::TextureFormat::Undefined)
        sc.formatSrgb = wgpu::TextureFormat::BGRA8UnormSrgb;
    sc.formatLinear = LinearPartnerOf(sc.formatSrgb);

    wgpu::SurfaceCapabilities caps{};
    sc.surface.GetCapabilities(state.adapter, &caps);

    // Prefer the requested format; fall back to the first capability if
    // the adapter doesn't expose it.
    bool found = false;
    for (usize i = 0; i < caps.formatCount; ++i) {
        if (caps.formats[i] == sc.formatSrgb) {
            found = true;
            break;
        }
    }
    if (!found && caps.formatCount > 0) {
        sc.formatSrgb = caps.formats[0];
        sc.formatLinear = LinearPartnerOf(sc.formatSrgb);
    }

    wgpu::SurfaceConfiguration cfg{};
    cfg.device = state.device;
    cfg.format = sc.formatSrgb;
    cfg.usage = wgpu::TextureUsage::RenderAttachment;
    cfg.width = width;
    cfg.height = height;
    cfg.presentMode = wgpu::PresentMode::Fifo;
    cfg.alphaMode = wgpu::CompositeAlphaMode::Opaque;
    const wgpu::TextureFormat viewFormats[] = {sc.formatSrgb, sc.formatLinear};
    cfg.viewFormatCount = (sc.formatSrgb == sc.formatLinear) ? 1 : 2;
    cfg.viewFormats = viewFormats;
    sc.surface.Configure(&cfg);
}

TextureHandle InsertSwapChainProxy(WebGPUDeviceState& state, SwapChainHandle scHandle,
                                   wgpu::TextureFormat fmt, bool isLinear, u32 width, u32 height) {
    TextureEntry entry{};
    entry.format = fmt;
    entry.width = static_cast<i32>(width);
    entry.height = static_cast<i32>(height);
    entry.ownsTexture = false;
    entry.swapChainProxy = scHandle;
    entry.isLinearView = isLinear;
    return static_cast<TextureHandle>(state.textures.Insert(std::move(entry)));
}

void RepointProxy(WebGPUDeviceState& state, TextureHandle h, const wgpu::Texture& tex,
                  wgpu::TextureFormat fmt) {
    auto* entry = state.textures.Get(static_cast<u64>(h));
    if (!entry)
        return;
    entry->texture = tex; // shared ref to the surface texture
    wgpu::TextureViewDescriptor vd{};
    vd.format = fmt;
    vd.dimension = wgpu::TextureViewDimension::e2D;
    vd.baseMipLevel = 0;
    vd.mipLevelCount = 1;
    vd.baseArrayLayer = 0;
    vd.arrayLayerCount = 1;
    vd.aspect = wgpu::TextureAspect::All;
    entry->view = tex.CreateView(&vd);
    entry->format = fmt;
}

} // namespace

void AcquireSwapChainImageIfNeeded(WebGPUDeviceState& state, SwapChainEntry& sc) {
    if (sc.acquiredThisFrame)
        return;

    wgpu::SurfaceTexture surfTex{};
    sc.surface.GetCurrentTexture(&surfTex);
    if (surfTex.status != wgpu::SurfaceGetCurrentTextureStatus::SuccessOptimal &&
        surfTex.status != wgpu::SurfaceGetCurrentTextureStatus::SuccessSuboptimal) {
        // Surface lost / out of date — reconfigure and retry once. The
        // renderer's resize callback should have already kicked
        // ResizeSwapChain, so this is mostly a defensive net.
        ConfigureSurface(state, sc, sc.width, sc.height, Format::R8G8B8A8_UNORM_SRGB);
        sc.surface.GetCurrentTexture(&surfTex);
    }
    sc.currentTexture = std::move(surfTex.texture);
    if (!sc.currentTexture)
        return;

    RepointProxy(state, sc.proxySrgb, sc.currentTexture, sc.formatSrgb);
    if (sc.proxyLinear != TextureHandle::Invalid && sc.formatLinear != sc.formatSrgb)
        RepointProxy(state, sc.proxyLinear, sc.currentTexture, sc.formatLinear);
    sc.acquiredThisFrame = true;
}

// ---- IGFXDevice swap-chain methods --------------------------------------

SwapChainHandle WebGPUDevice::CreateSwapChain(void* nativeWindowHandle, i32 width, i32 height,
                                              Format colorFormat) {
    auto& state = *state_;
    if (width <= 0 || height <= 0)
        return SwapChainHandle::Invalid;

    SwapChainEntry entry{};
    entry.surface = CreateSurfaceForWindow(state, nativeWindowHandle);
    if (!entry.surface) {
        std::fprintf(stderr, "[wgpu] CreateSurface failed\n");
        return SwapChainHandle::Invalid;
    }
    ConfigureSurface(state, entry, static_cast<u32>(width), static_cast<u32>(height), colorFormat);

    SwapChainHandle handle =
        static_cast<SwapChainHandle>(state.swapchains.Insert(std::move(entry)));

    // Insert proxy textures and back-fill them into the freshly-stored
    // entry (we needed the handle to record the back-pointer).
    auto* sc = state.swapchains.Get(static_cast<u64>(handle));
    sc->proxySrgb = InsertSwapChainProxy(state, handle, sc->formatSrgb, /*isLinear=*/false,
                                         sc->width, sc->height);
    if (sc->formatLinear != sc->formatSrgb) {
        sc->proxyLinear = InsertSwapChainProxy(state, handle, sc->formatLinear,
                                               /*isLinear=*/true, sc->width, sc->height);
    }
    return handle;
}

void WebGPUDevice::ResizeSwapChain(SwapChainHandle h, i32 width, i32 height) {
    auto& state = *state_;
    auto* sc = state.swapchains.Get(static_cast<u64>(h));
    if (!sc || width <= 0 || height <= 0)
        return;
    ConfigureSurface(state, *sc, static_cast<u32>(width), static_cast<u32>(height),
                     Format::R8G8B8A8_UNORM_SRGB);
    if (auto* p = state.textures.Get(static_cast<u64>(sc->proxySrgb))) {
        p->width = width;
        p->height = height;
    }
    if (sc->proxyLinear != TextureHandle::Invalid) {
        if (auto* p = state.textures.Get(static_cast<u64>(sc->proxyLinear))) {
            p->width = width;
            p->height = height;
        }
    }
    sc->acquiredThisFrame = false;
}

void WebGPUDevice::DestroySwapChain(SwapChainHandle h) {
    auto& state = *state_;
    auto* sc = state.swapchains.Get(static_cast<u64>(h));
    if (!sc)
        return;
    if (sc->proxySrgb != TextureHandle::Invalid)
        state.textures.Remove(static_cast<u64>(sc->proxySrgb));
    if (sc->proxyLinear != TextureHandle::Invalid)
        state.textures.Remove(static_cast<u64>(sc->proxyLinear));
    state.swapchains.Remove(static_cast<u64>(h));
}

void WebGPUDevice::Present(SwapChainHandle h) {
    auto& state = *state_;
    auto* sc = state.swapchains.Get(static_cast<u64>(h));
    if (!sc)
        return;

    // Submit anything the renderer recorded this frame. Bumps the
    // pending-epoch and arms the OnSubmittedWorkDone bookkeeping.
    SubmitFrameAndBumpEpoch(state);

    if (sc->currentTexture)
        sc->surface.Present();

    sc->currentTexture = nullptr;
    sc->acquiredThisFrame = false;

    // Pump Dawn so OnSubmittedWorkDone callbacks land — relevant when the
    // backend's worker thread parks waiting for the renderer to tick.
    state.instance.ProcessEvents();
}

TextureHandle WebGPUDevice::GetSwapChainBackBuffer(SwapChainHandle h) {
    auto& state = *state_;
    auto* sc = state.swapchains.Get(static_cast<u64>(h));
    return sc ? sc->proxySrgb : TextureHandle::Invalid;
}

TextureHandle WebGPUDevice::GetSwapChainBackBufferLinear(SwapChainHandle h) {
    auto& state = *state_;
    auto* sc = state.swapchains.Get(static_cast<u64>(h));
    if (!sc)
        return TextureHandle::Invalid;
    return sc->proxyLinear != TextureHandle::Invalid ? sc->proxyLinear : sc->proxySrgb;
}

} // namespace whiteout::flakes::gfx::webgpu

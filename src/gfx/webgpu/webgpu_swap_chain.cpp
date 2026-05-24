// wgpu::Surface lifetime, GetCurrentTexture, Present.
//
// Mirrors src/gfx/vulkan/vulkan_swap_chain.cpp's proxy-texture trick:
// the renderer caches one TextureHandle at CreateSwapChain and we
// re-point it at the surface's current texture in
// AcquireSwapChainImageIfNeeded (called from BeginRenderPass).
//
// Platform note: `nativeWindowHandle` is an HWND on Windows (matches
// the D3D11/D3D12 backends so the gfx interface stays uniform there)
// and a `GLFWwindow*` on every other platform. On non-Windows we use
// GLFW's native APIs to pull the platform-specific handle pair (Display
// + Window on X11, wl_display + wl_surface on Wayland, NSWindow on
// macOS) — which is more than fits in a single void*. The X11-vs-
// Wayland split on Linux is resolved at runtime via `glfwGetPlatform()`
// so a single binary works on both session types.

#include "webgpu_device.h"
#include "webgpu_device_state.h"
#include "webgpu_handles.h"
#include "webgpu_translate.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <utility>

#if defined(__EMSCRIPTEN__)
// Web target: no GLFW, no native window handles. The surface comes from a
// CSS canvas selector handed in by JS via wf_init(canvasSelector,...).
#include <emscripten/html5.h>
#elif defined(_WIN32)
#include <windows.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#elif defined(__APPLE__)
#define GLFW_EXPOSE_NATIVE_COCOA
#else
#define GLFW_EXPOSE_NATIVE_X11
#define GLFW_EXPOSE_NATIVE_WAYLAND
#endif

#if !defined(__EMSCRIPTEN__)
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#endif

namespace whiteout::flakes::gfx::webgpu {

#if defined(__APPLE__)
// Defined in webgpu_surface_macos.mm. Returns a CAMetalLayer* (as void*)
// attached to the NSWindow's contentView. The layer's lifetime is tied
// to the view, which is owned by GLFW for the window's lifetime.
void* CreateMetalLayerForCocoaWindow(void* nsWindow);
#endif

namespace {

wgpu::Surface CreateSurfaceForWindow(WebGPUDeviceState& state, void* nativeWindowHandle) {
    wgpu::SurfaceDescriptor sd{};
#if defined(__EMSCRIPTEN__)
    // Web: nativeWindowHandle is a NUL-terminated CSS canvas selector
    // ("#wf-canvas") owned by JS for the lifetime of the surface.
    wgpu::EmscriptenSurfaceSourceCanvasHTMLSelector fromCanvas{};
    fromCanvas.selector = static_cast<const char*>(nativeWindowHandle);
    sd.nextInChain = &fromCanvas;
    return state.instance.CreateSurface(&sd);
#elif defined(_WIN32)
    // Windows: nativeWindowHandle is the HWND directly (matches d3d11/d3d12).
    wgpu::SurfaceSourceWindowsHWND fromHwnd{};
    fromHwnd.hwnd = nativeWindowHandle;
    fromHwnd.hinstance = ::GetModuleHandleW(nullptr);
    sd.nextInChain = &fromHwnd;
    return state.instance.CreateSurface(&sd);
#elif defined(__APPLE__)
    // macOS: GLFW gives us NSWindow*; the ObjC++ shim wraps its
    // contentView with a CAMetalLayer and hands the layer back as void*.
    auto* window = static_cast<GLFWwindow*>(nativeWindowHandle);
    void* nsWindow = glfwGetCocoaWindow(window);
    void* metalLayer = CreateMetalLayerForCocoaWindow(nsWindow);
    if (!metalLayer)
        return nullptr;
    wgpu::SurfaceSourceMetalLayer fromMetal{};
    fromMetal.layer = metalLayer;
    sd.nextInChain = &fromMetal;
    return state.instance.CreateSurface(&sd);
#else
    // Linux: pick X11 or Wayland from the GLFW runtime platform. GLFW
    // must have been compiled with both backends; the choice is decided
    // by the session at glfwInit() time and reflected in glfwGetPlatform().
    auto* window = static_cast<GLFWwindow*>(nativeWindowHandle);
    const int platform = glfwGetPlatform();
    if (platform == GLFW_PLATFORM_WAYLAND) {
        wgpu::SurfaceSourceWaylandSurface fromWayland{};
        fromWayland.display = glfwGetWaylandDisplay();
        fromWayland.surface = glfwGetWaylandWindow(window);
        sd.nextInChain = &fromWayland;
        return state.instance.CreateSurface(&sd);
    }
    // X11 is the default fallback (and what GLFW returns for
    // GLFW_PLATFORM_X11). glfwGetX11Window returns an `unsigned long`
    // (XID); SurfaceSourceXlibWindow.window takes a uint64_t which is
    // wider on all targets so the widening conversion is safe.
    wgpu::SurfaceSourceXlibWindow fromXlib{};
    fromXlib.display = glfwGetX11Display();
    fromXlib.window = static_cast<uint64_t>(glfwGetX11Window(window));
    sd.nextInChain = &fromXlib;
    return state.instance.CreateSurface(&sd);
#endif
}

void ConfigureSurface(WebGPUDeviceState& state, SwapChainEntry& sc, u32 width, u32 height,
                      Format colorFormat) {
    sc.width = width;
    sc.height = height;

    // Pick sRGB / linear pair. The renderer-facing format is whatever it
    // requested (e.g. R8G8B8A8_UNORM_SRGB); the *surface* format may be
    // something else (Dawn on Windows D3D12 only ever exposes BGRA8
    // surface formats — RGBA8 isn't a valid surface format there). We
    // configure with whatever the surface allows, then use the
    // viewFormats mechanism to expose an sRGB view on top.
    const wgpu::TextureFormat requestedSrgb = ToWgpuFormat(colorFormat);

    wgpu::SurfaceCapabilities caps{};
    sc.surface.GetCapabilities(state.adapter, &caps);

    // Find the surface format. Prefer the requested sRGB format
    // exactly — that lets the sRGB proxy view fall through with no
    // viewFormats entry, which is the path Dawn's Vulkan backend
    // actually honors. Fall back to the linear partner only when no
    // sRGB form is exposed by the surface (Windows D3D12 surfaces
    // typically only advertise linear formats).
    wgpu::TextureFormat surfaceFmt =
        caps.formatCount > 0 ? caps.formats[0] : wgpu::TextureFormat::BGRA8Unorm;
    bool found = false;
    for (usize i = 0; i < caps.formatCount && !found; ++i) {
        if (caps.formats[i] == requestedSrgb) {
            surfaceFmt = caps.formats[i];
            found = true;
        }
    }
    if (!found) {
        for (usize i = 0; i < caps.formatCount && !found; ++i) {
            if (caps.formats[i] == LinearPartnerOf(requestedSrgb)) {
                surfaceFmt = caps.formats[i];
                found = true;
            }
        }
    }

    // sRGB / linear pair on the surface texture, both exposed via
    // viewFormats so the renderer can pick the encoding it needs.
    // Surface formats are always linear (sRGB-decoded on present) — the
    // sRGB view does the encoding.
    sc.formatSrgb = surfaceFmt;
    sc.formatLinear = surfaceFmt;
    if (surfaceFmt == wgpu::TextureFormat::BGRA8Unorm) {
        sc.formatSrgb = wgpu::TextureFormat::BGRA8UnormSrgb;
    } else if (surfaceFmt == wgpu::TextureFormat::RGBA8Unorm) {
        sc.formatSrgb = wgpu::TextureFormat::RGBA8UnormSrgb;
    } else if (surfaceFmt == wgpu::TextureFormat::BGRA8UnormSrgb) {
        sc.formatLinear = wgpu::TextureFormat::BGRA8Unorm;
    } else if (surfaceFmt == wgpu::TextureFormat::RGBA8UnormSrgb) {
        sc.formatLinear = wgpu::TextureFormat::RGBA8Unorm;
    }

    // List both the sRGB and linear formats — Dawn's Vulkan surface
    // implementation passes viewFormats[] into VkImageFormatListCreateInfo,
    // and the Vulkan validator wants the full set of compatible view
    // formats (including the base surfaceFmt) explicitly enumerated.
    // D3D12 tolerates either form.
    std::array<wgpu::TextureFormat, 2> viewFormats{sc.formatSrgb, sc.formatLinear};
    const u32 viewFormatCount = (sc.formatSrgb == sc.formatLinear) ? 1 : 2;

    wgpu::SurfaceConfiguration cfg{};
    cfg.device = state.device;
    cfg.format = surfaceFmt;
    cfg.usage = wgpu::TextureUsage::RenderAttachment;
    cfg.width = width;
    cfg.height = height;
    cfg.presentMode = wgpu::PresentMode::Fifo;
    cfg.alphaMode = wgpu::CompositeAlphaMode::Opaque;
    cfg.viewFormatCount = viewFormatCount;
    cfg.viewFormats = viewFormats.data();
    sc.surface.Configure(&cfg);
}

// Public-API → gfx::Format mapper for GetSwapChainFormat. We need the
// inverse of ToWgpuFormat for the small subset of formats a surface can
// actually be configured with on this backend.
Format WgpuSurfaceFormatToGfx(wgpu::TextureFormat f) {
    switch (f) {
    case wgpu::TextureFormat::BGRA8Unorm:
        return Format::B8G8R8A8_UNORM;
    case wgpu::TextureFormat::BGRA8UnormSrgb:
        return Format::B8G8R8A8_UNORM_SRGB;
    case wgpu::TextureFormat::RGBA8Unorm:
        return Format::R8G8B8A8_UNORM;
    case wgpu::TextureFormat::RGBA8UnormSrgb:
        return Format::R8G8B8A8_UNORM_SRGB;
    default:
        return Format::Unknown;
    }
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

    // Dawn's Vulkan path sometimes advertises an sRGB cap (e.g. RGBA8UnormSrgb)
    // but configures the actual VkSurface with a different channel order
    // (BGRA8) underneath. The compatible-viewFormats list comes from
    // surface config, so a RepointProxy with the *requested* format would
    // fail CreateView validation. Reconcile by querying the actual surface
    // texture's format and adjusting our cached sRGB / linear pair.
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

Format WebGPUDevice::GetSwapChainFormat(SwapChainHandle h) const {
    auto& state = *state_;
    auto* sc = state.swapchains.Get(static_cast<u64>(h));
    if (!sc)
        return Format::Unknown;
    return WgpuSurfaceFormatToGfx(sc->formatSrgb);
}

} // namespace whiteout::flakes::gfx::webgpu

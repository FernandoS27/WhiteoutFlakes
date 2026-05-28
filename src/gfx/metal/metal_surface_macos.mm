// Wraps a GLFW-created NSWindow's contentView with a CAMetalLayer so
// MetalDevice::CreateSwapChain has a layer to drive. GLFW creates the
// NSWindow with a plain NSView; CAMetalLayer needs a layer-backed view.
//
// Returning a CAMetalLayer* through `void*` keeps the caller (the gfx
// factory glue) free of any Objective-C type knowledge — the swap-chain
// implementation casts back inside its own .mm. Mirrors the WebGPU
// backend's webgpu_surface_macos.mm 1:1 in shape.

#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>

namespace whiteout::flakes::gfx::metal {

// `nsWindow` is the NSWindow* returned by `glfwGetCocoaWindow`. The
// CAMetalLayer is owned by the contentView, which is owned by the
// NSWindow, which is owned by GLFW for the window's lifetime — so we
// don't bump retain ourselves.
void* CreateMetalLayerForCocoaWindow(void* nsWindow) {
    NSWindow* window = (__bridge NSWindow*)nsWindow;
    if (!window)
        return nullptr;

    NSView* view = [window contentView];
    if (!view)
        return nullptr;

    // If a prior call already attached a CAMetalLayer (e.g. CreateSwapChain
    // ran once, was destroyed, and is being re-created on resize), reuse
    // the existing one so we don't strand it.
    if ([[view layer] isKindOfClass:[CAMetalLayer class]])
        return (__bridge void*)[view layer];

    CAMetalLayer* layer = [CAMetalLayer layer];
    [view setWantsLayer:YES];
    [view setLayer:layer];
    return (__bridge void*)layer;
}

} // namespace whiteout::flakes::gfx::metal

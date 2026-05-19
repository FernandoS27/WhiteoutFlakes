// Wraps a GLFW-created NSWindow's contentView with a CAMetalLayer so
// Dawn's `SurfaceSourceMetalLayer` has something to back the swap-chain
// with. GLFW creates the NSWindow with a plain NSView; WebGPU needs a
// layer-backed view whose `layer` is a CAMetalLayer.
//
// Called from webgpu_swap_chain.cpp behind `#if defined(__APPLE__)`.
// Returning `void*` (a CAMetalLayer*) keeps the caller free of any
// Objective-C type knowledge — the layer just travels through the
// SurfaceSourceMetalLayer.layer field as an opaque pointer.

#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>

namespace whiteout::flakes::gfx::webgpu {

// `nsWindow` is the NSWindow* returned by `glfwGetCocoaWindow`. The
// caller retains nothing — the CAMetalLayer is owned by the contentView
// it gets attached to, which is owned by the NSWindow, which is owned
// by GLFW for the window's lifetime.
void* CreateMetalLayerForCocoaWindow(void* nsWindow) {
    NSWindow* window = static_cast<NSWindow*>(nsWindow);
    if (!window)
        return nullptr;

    NSView* view = [window contentView];
    if (!view)
        return nullptr;

    CAMetalLayer* layer = [CAMetalLayer layer];
    [view setWantsLayer:YES];
    [view setLayer:layer];
    return static_cast<void*>(layer);
}

} // namespace whiteout::flakes::gfx::webgpu

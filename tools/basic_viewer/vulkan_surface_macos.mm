// macOS: build the CAMetalLayer that backs the GLFW NSWindow ourselves and
// hand it to vkCreateMetalSurfaceEXT. GLFW's glfwCreateWindowSurface does
// the same dance, but assigns the layer to the contentView *before*
// flipping wantsLayer=YES — the reverse of Apple's documented order for
// custom layers. On macOS 13+ with MoltenVK 1.4 the view's default layer
// can win, leaving the CAMetalLayer un-installed; vkCreateMetalSurfaceEXT
// then returns VK_ERROR_INITIALIZATION_FAILED. Setting wantsLayer first
// (matching the WebGPU surface shim's order in webgpu_surface_macos.mm)
// fixes that.

#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>

#define VK_USE_PLATFORM_METAL_EXT
#include <vulkan/vulkan.h>

namespace whiteout::flakes {

VkResult CreateVulkanSurfaceMacOS(VkInstance instance, void* nsWindow,
                                  VkSurfaceKHR* outSurface) {
    if (!instance || !nsWindow || !outSurface)
        return VK_ERROR_INITIALIZATION_FAILED;

    NSWindow* window = static_cast<NSWindow*>(nsWindow);
    NSView* view = [window contentView];
    if (!view)
        return VK_ERROR_INITIALIZATION_FAILED;

    CAMetalLayer* layer = [CAMetalLayer layer];
    if (!layer)
        return VK_ERROR_INITIALIZATION_FAILED;

    // Match the contentsScale to the view's backing scale so the swap-chain
    // image extent at fullsize matches the framebuffer pixel count, not the
    // points count — without this, Retina displays render at half resolution.
    layer.contentsScale = [window backingScaleFactor];

    // Order matters: wantsLayer first, then assign the custom layer.
    // Otherwise AppKit may instantiate a default CALayer and replace ours.
    [view setWantsLayer:YES];
    [view setLayer:layer];

    auto vkCreateMetalSurfaceEXT = reinterpret_cast<PFN_vkCreateMetalSurfaceEXT>(
        vkGetInstanceProcAddr(instance, "vkCreateMetalSurfaceEXT"));
    if (!vkCreateMetalSurfaceEXT)
        return VK_ERROR_EXTENSION_NOT_PRESENT;

    VkMetalSurfaceCreateInfoEXT ci{};
    ci.sType = VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT;
    ci.pLayer = layer;
    return vkCreateMetalSurfaceEXT(instance, &ci, nullptr, outSurface);
}

} // namespace whiteout::flakes

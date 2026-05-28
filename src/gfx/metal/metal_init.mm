// MetalDevice::Init — pick a device, build the command queue, populate
// the device name. EnumerateAdapterNames walks MTLCopyAllDevices.
//
// Phase A scope: the bare minimum that lets CreateDevice(GfxApi::Metal)
// return a non-null device with a valid GetDeviceName(). Swap-chain,
// resource creation, and command recording live in subsequent files.

#include "metal_device.h"
#include "metal_device_state.h"
#include "metal_handles.h"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <cstdio>
#include <string>
#include <vector>

namespace whiteout::flakes::gfx {
// Defined in gfx_factory.cpp; module-scope preferred-device name set
// by SetPreferredDevice. Empty string = "let MTLCreateSystemDefaultDevice pick".
const std::string& GetPreferredDevice();
} // namespace whiteout::flakes::gfx

namespace whiteout::flakes::gfx::metal {

namespace {

NSArray<id<MTLDevice>>* CopyAllDevices() {
#if TARGET_OS_OSX
    return MTLCopyAllDevices();
#else
    // iOS / tvOS / visionOS expose only a single integrated device.
    id<MTLDevice> dflt = MTLCreateSystemDefaultDevice();
    return dflt ? @[ dflt ] : @[];
#endif
}

id<MTLDevice> PickDevice(const std::string& preferred) {
    NSArray<id<MTLDevice>>* all = CopyAllDevices();
    if (!preferred.empty()) {
        NSString* want = [NSString stringWithUTF8String:preferred.c_str()];
        for (id<MTLDevice> dev in all) {
            if ([[dev name] isEqualToString:want])
                return dev;
        }
    }
    // Default: MTLCreateSystemDefaultDevice mirrors what every Apple sample
    // does — picks the discrete GPU on multi-GPU Macs, the integrated GPU
    // on Apple Silicon, with the eGPU automatic-graphics-switching honored.
    return MTLCreateSystemDefaultDevice();
}

} // namespace

std::vector<std::string> EnumerateAdapterNames() {
    std::vector<std::string> out;
    @autoreleasepool {
        NSArray<id<MTLDevice>>* all = CopyAllDevices();
        out.reserve([all count]);
        for (id<MTLDevice> dev in all)
            out.emplace_back([[dev name] UTF8String]);
    }
    return out;
}

bool MetalDevice::Init(bool enableValidation) {
    @autoreleasepool {
        auto& state = *state_;
        state.validationRequested = enableValidation;

        if (enableValidation) {
            // MTL_DEBUG_LAYER / MTL_SHADER_VALIDATION are env-var gated —
            // setting them after the process has started has no effect.
            // Log a hint so the user knows to re-launch with the env set.
            std::fprintf(stderr,
                "[gfx/metal] enableValidation=true requested. For full validation, "
                "relaunch with MTL_DEBUG_LAYER=1 (and optionally "
                "MTL_SHADER_VALIDATION=1) set in the environment.\n");
        }

        id<MTLDevice> dev = PickDevice(gfx::GetPreferredDevice());
        if (!dev) {
            std::fprintf(stderr,
                "[gfx/metal] MTLCreateSystemDefaultDevice returned nil — "
                "no Metal-capable GPU?\n");
            return false;
        }
        state.device = dev;

        state.commandQueue = [dev newCommandQueue];
        if (!state.commandQueue) {
            std::fprintf(stderr, "[gfx/metal] newCommandQueue failed\n");
            state.device = nil;
            return false;
        }
        if (enableValidation)
            state.commandQueue.label = @"wf.queue";

        deviceName_ = [[dev name] UTF8String];

        std::fprintf(stderr, "[gfx/metal] device='%s' unified=%d low-power=%d\n",
                     deviceName_.c_str(),
                     (int)[dev hasUnifiedMemory],
                     (int)[dev isLowPower]);
        return true;
    }
}

} // namespace whiteout::flakes::gfx::metal

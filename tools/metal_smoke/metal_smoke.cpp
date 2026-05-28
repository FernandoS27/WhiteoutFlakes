// metal_smoke — minimal verification that the Metal backend's swap chain
// + render pass + Present path produces a visible clear color.
//
// Skips the entire renderer / BLS / asset path: just opens a GLFW
// window, asks gfx for a Metal device + swap chain, and runs a small
// loop calling BeginRenderPass with a magenta clear, EndRenderPass,
// Present. Verifies Phase B of the Metal-backend bring-up.
//
// Usage:
//   metal_smoke                — magenta clear, ~3s, auto-close
//   metal_smoke --hold         — runs until you close the window
//
// Build: this target opts out of the renderer; it only needs
// WhiteoutFlakesGfx (which already contains everything Metal-side).

#include "gfx/gfx.h"

#include <GLFW/glfw3.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

namespace gfx = whiteout::flakes::gfx;

int main(int argc, char** argv) {
    bool hold = false;
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--hold") == 0)
            hold = true;

    if (!glfwInit()) {
        std::fprintf(stderr, "glfwInit failed\n");
        return 1;
    }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_COCOA_RETINA_FRAMEBUFFER, GLFW_TRUE);

    GLFWwindow* window =
        glfwCreateWindow(800, 600, "WhiteoutFlakes metal_smoke", nullptr, nullptr);
    if (!window) {
        std::fprintf(stderr, "glfwCreateWindow failed\n");
        glfwTerminate();
        return 1;
    }

    auto device = gfx::CreateDevice(gfx::GfxApi::Metal, /*enableValidation=*/false);
    if (!device) {
        std::fprintf(stderr, "CreateDevice(Metal) returned null\n");
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }
    std::printf("Metal device: %s\n", device->GetDeviceName());

    int fbW = 0, fbH = 0;
    glfwGetFramebufferSize(window, &fbW, &fbH);
    auto swap = device->CreateSwapChain(static_cast<void*>(window), fbW, fbH,
                                        gfx::Format::B8G8R8A8_UNORM_SRGB);
    if (swap == gfx::SwapChainHandle::Invalid) {
        std::fprintf(stderr, "CreateSwapChain returned Invalid\n");
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }
    std::printf("Swap chain created: format=%d %dx%d\n",
                (int)device->GetSwapChainFormat(swap), fbW, fbH);

    auto* cmd = device->GetImmediateContext();
    auto t0 = std::chrono::steady_clock::now();
    int lastW = fbW, lastH = fbH;

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        int w = 0, h = 0;
        glfwGetFramebufferSize(window, &w, &h);
        if (w != lastW || h != lastH) {
            device->ResizeSwapChain(swap, w, h);
            lastW = w;
            lastH = h;
            std::printf("ResizeSwapChain -> %dx%d\n", w, h);
        }

        // Magenta in linear space (sRGB swap chain will gamma-encode at
        // present); easy to spot in a screenshot.
        const float clear[4] = {1.0f, 0.0f, 1.0f, 1.0f};
        auto bb = device->GetSwapChainBackBuffer(swap);
        cmd->BeginRenderPass(bb, gfx::TextureHandle::Invalid, clear, 1.0f, 0);
        cmd->EndRenderPass();
        device->Present(swap);

        if (!hold) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0);
            if (elapsed.count() > 3000)
                glfwSetWindowShouldClose(window, GLFW_TRUE);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    device->DestroySwapChain(swap);
    device.reset();

    glfwDestroyWindow(window);
    glfwTerminate();
    std::printf("metal_smoke: ok\n");
    return 0;
}

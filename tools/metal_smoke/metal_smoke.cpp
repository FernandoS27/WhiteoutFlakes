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
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

static std::vector<std::uint8_t> ReadFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in)
        return {};
    auto sz = in.tellg();
    in.seekg(0);
    std::vector<std::uint8_t> data(static_cast<std::size_t>(sz));
    in.read(reinterpret_cast<char*>(data.data()), sz);
    return data;
}

// Locate triangle.metallib next to the executable. CMake's
// add_custom_command drops it in ${CMAKE_BINARY_DIR}/standalone/ and
// the smoke binary lives in the same dir.
static std::filesystem::path TriangleMetallibPath() {
    std::filesystem::path exe;
#if defined(__APPLE__)
    std::uint32_t bufsize = 0;
    _NSGetExecutablePath(nullptr, &bufsize);
    std::vector<char> buf(bufsize);
    if (_NSGetExecutablePath(buf.data(), &bufsize) == 0)
        exe = std::filesystem::canonical(buf.data());
#endif
    return exe.parent_path() / "triangle.metallib";
}

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

    // ---- Phase C smoke: exercise buffer / texture / sampler creation ----
    {
        // CpuWritable constant buffer (ring-allocated from sharedCb).
        gfx::BufferDesc bd{};
        bd.size = 256;
        bd.usage = gfx::BufferUsage::Constant | gfx::BufferUsage::CpuWritable;
        auto cb = device->CreateBuffer(bd, nullptr);
        if (cb == gfx::BufferHandle::Invalid) {
            std::fprintf(stderr, "CreateBuffer(constant) returned Invalid\n");
            return 1;
        }
        if (void* m = device->MapBuffer(cb)) {
            std::memset(m, 0xAB, 256);
            device->UnmapBuffer(cb);
            std::printf("CB Map/Unmap ok\n");
        }
        device->Destroy(cb);

        // Static dedicated buffer with initial data (private storage path).
        std::uint32_t verts[] = {0u, 1u, 2u, 3u};
        gfx::BufferDesc vbd{};
        vbd.size = sizeof(verts);
        vbd.usage = gfx::BufferUsage::Vertex;
        auto vb = device->CreateBuffer(vbd, verts);
        if (vb == gfx::BufferHandle::Invalid) {
            std::fprintf(stderr, "CreateBuffer(vertex) returned Invalid\n");
            return 1;
        }
        std::printf("Static VB ok\n");
        device->Destroy(vb);

        // Texture with initial pixel data (shared storage path).
        std::uint8_t pix[4 * 4 * 4];
        for (int i = 0; i < 4 * 4; ++i) {
            pix[i * 4 + 0] = (std::uint8_t)(i * 16);
            pix[i * 4 + 1] = 0;
            pix[i * 4 + 2] = 255;
            pix[i * 4 + 3] = 255;
        }
        gfx::TextureDesc td{};
        td.width = 4;
        td.height = 4;
        td.mipLevels = 1;
        td.arraySize = 1;
        td.format = gfx::Format::R8G8B8A8_UNORM;
        td.usage = gfx::TextureUsage::ShaderResource;
        auto tex = device->CreateTexture(td, pix);
        if (tex == gfx::TextureHandle::Invalid) {
            std::fprintf(stderr, "CreateTexture returned Invalid\n");
            return 1;
        }
        std::printf("Texture (with initial pixels) ok\n");
        device->Destroy(tex);

        // Render targets (private storage).
        auto rt = device->CreateColorTarget(256, 256, gfx::Format::R16G16B16A16_FLOAT);
        auto dt = device->CreateDepthTarget(256, 256, device->PreferredDepthStencilFormat());
        if (rt == gfx::TextureHandle::Invalid || dt == gfx::TextureHandle::Invalid) {
            std::fprintf(stderr, "CreateColorTarget/DepthTarget returned Invalid\n");
            return 1;
        }
        std::printf("Color + depth targets ok\n");
        device->Destroy(rt);
        device->Destroy(dt);

        // Sampler.
        gfx::SamplerDesc sd{};
        sd.minFilter = gfx::Filter::Linear;
        sd.magFilter = gfx::Filter::Linear;
        sd.addressU = gfx::AddressMode::Wrap;
        sd.addressV = gfx::AddressMode::Wrap;
        auto smp = device->CreateSampler(sd);
        if (smp == gfx::SamplerHandle::Invalid) {
            std::fprintf(stderr, "CreateSampler returned Invalid\n");
            return 1;
        }
        std::printf("Sampler ok\n");
        device->Destroy(smp);

        std::printf("LiveGpuBytes after destroys: %llu\n",
                    (unsigned long long)device->LiveGpuBytes());
    }

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

    // ---- Phase D smoke: load triangle.metallib, build pipeline, draw ----
    // Lives after CreateSwapChain so the PSO can pick up the swap-chain's
    // resolved color format for its rtvFormat.
    gfx::ShaderHandle vs = gfx::ShaderHandle::Invalid;
    gfx::ShaderHandle ps = gfx::ShaderHandle::Invalid;
    gfx::PipelineHandle triPso = gfx::PipelineHandle::Invalid;
    {
        auto path = TriangleMetallibPath();
        auto bytes = ReadFile(path);
        if (bytes.empty()) {
            std::fprintf(stderr,
                "triangle.metallib not found at %s\n", path.string().c_str());
            return 1;
        }
        vs = device->CreateShader(gfx::ShaderStage::Vertex,
                                  bytes.data(), bytes.size());
        ps = device->CreateShader(gfx::ShaderStage::Pixel,
                                  bytes.data(), bytes.size());
        if (vs == gfx::ShaderHandle::Invalid || ps == gfx::ShaderHandle::Invalid) {
            std::fprintf(stderr, "CreateShader returned Invalid\n");
            return 1;
        }
        std::printf("CreateShader(vs+ps) ok\n");

        gfx::GraphicsPipelineDesc pd{};
        pd.vs = vs;
        pd.ps = ps;
        pd.topology = gfx::PrimitiveTopology::TriangleList;
        pd.blend.enable = false;
        pd.depthStencil.depthTest = false;
        pd.depthStencil.depthWrite = false;
        pd.rasterizer.cull = gfx::CullMode::None;
        pd.rtvFormat = device->GetSwapChainFormat(swap);
        pd.dsvFormat = gfx::Format::Unknown;
        triPso = device->CreateGraphicsPipeline(pd);
        if (triPso == gfx::PipelineHandle::Invalid) {
            std::fprintf(stderr, "CreateGraphicsPipeline returned Invalid\n");
            return 1;
        }
        std::printf("CreateGraphicsPipeline ok\n");
    }

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

        // Dark grey background, RGB-gradient triangle on top.
        const float clear[4] = {0.1f, 0.1f, 0.15f, 1.0f};
        auto bb = device->GetSwapChainBackBuffer(swap);
        cmd->BeginRenderPass(bb, gfx::TextureHandle::Invalid, clear, 1.0f, 0);
        gfx::Viewport vp{};
        vp.x = 0.0f;
        vp.y = 0.0f;
        vp.width = static_cast<float>(w);
        vp.height = static_cast<float>(h);
        vp.minDepth = 0.0f;
        vp.maxDepth = 1.0f;
        cmd->SetViewport(vp);
        cmd->BindPipeline(triPso);
        cmd->Draw(3, 0);
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

    device->WaitIdle();
    device->Destroy(triPso);
    device->Destroy(vs);
    device->Destroy(ps);
    device->DestroySwapChain(swap);
    device.reset();

    glfwDestroyWindow(window);
    glfwTerminate();
    std::printf("metal_smoke: ok\n");
    return 0;
}

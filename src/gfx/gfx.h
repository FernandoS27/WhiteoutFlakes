#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>
#include "gfx/gfx_pipeline_types.h"
#include "whiteout/flakes/gfx_types.h"
#include "whiteout/flakes/types.h"

namespace whiteout::flakes::gfx {

enum class BufferHandle : u64 { Invalid = 0 };
enum class TextureHandle : u64 { Invalid = 0 };
enum class ShaderHandle : u64 { Invalid = 0 };
enum class PipelineHandle : u64 { Invalid = 0 };
enum class SamplerHandle : u64 { Invalid = 0 };
enum class SwapChainHandle : u64 { Invalid = 0 };

struct Viewport {
    f32 x = 0;
    f32 y = 0;
    f32 width = 0;
    f32 height = 0;
    f32 minDepth = 0;
    f32 maxDepth = 1;
};

struct Scissor {
    i32 x = 0;
    i32 y = 0;
    i32 width = 0;
    i32 height = 0;
};

class IGFXCommandList {
public:
    virtual ~IGFXCommandList() = default;

    virtual void BeginRenderPass(TextureHandle color, TextureHandle depth, const f32 clearColor[4],
                                 f32 clearDepth, u8 clearStencil) = 0;

    // Multi-render-target variant. `colors`/`clearColors` are slot-indexed
    // up to `colorCount` (≤ `kMaxColorAttachments`). Backends bind every
    // attachment as an RTV; the bound PSO's `extraRtvCount` + `rtvFormat`
    // must agree with `colorCount` and the slot formats. Single-attachment
    // callers can keep using the existing single-color overload; the
    // default implementation here forwards to it so backends that haven't
    // implemented MRT yet still render slot 0 and silently drop the
    // extras (useful for incremental backend roll-out). MRT-aware
    // backends (D3D12, WebGPU, …) override.
    virtual void BeginRenderPass(const TextureHandle* colors, u32 colorCount,
                                 TextureHandle depth, const f32 (*clearColors)[4],
                                 f32 clearDepth, u8 clearStencil) {
        constexpr f32 kZero[4] = {0, 0, 0, 0};
        const TextureHandle slot0 = (colorCount > 0) ? colors[0] : TextureHandle::Invalid;
        const f32* clear0 = (colorCount > 0 && clearColors) ? clearColors[0] : kZero;
        BeginRenderPass(slot0, depth, clear0, clearDepth, clearStencil);
    }

    // Load-op variant: when `loadColor` is true the color attachment is
    // bound with loadOp=Load (existing contents preserved) and the
    // `clearColor` argument is ignored. Default impl falls back to the
    // single-RT clearing variant for backends that haven't implemented
    // load-op yet — slot 0 still works correctly there because the
    // first frame's clear happens elsewhere; subsequent passes that
    // need preservation must run on a load-op-aware backend.
    virtual void BeginRenderPassLoad(TextureHandle color, TextureHandle depth,
                                     f32 clearDepth, u8 clearStencil) {
        constexpr f32 kZero[4] = {0, 0, 0, 0};
        BeginRenderPass(color, depth, kZero, clearDepth, clearStencil);
    }

    virtual void EndRenderPass() = 0;

    // Tracy-backed GPU profiler zone. Bracket GPU work with a named
    // scope; the Vulkan backend forwards to TracyVkZoneTransient so
    // the zone shows up on Tracy.exe's GPU timeline. D3D11 / D3D12
    // currently no-op (Tracy supports them too, but the backends
    // aren't wired yet). `name` must outlive the current command
    // recording — string literals or stable runtime buffers only.
    // Nesting is supported (Tracy maintains its own depth stack).
    virtual void BeginGpuZone(const char* name) = 0;
    virtual void EndGpuZone() = 0;

    virtual void SetViewport(const Viewport&) = 0;
    virtual void SetScissor(const Scissor&) = 0;

    virtual void BindPipeline(PipelineHandle) = 0;

    virtual void BindVertexBuffer(u32 slot, BufferHandle, u32 stride, u32 offset = 0) = 0;
    virtual void BindIndexBuffer(BufferHandle, Format) = 0;
    virtual void BindConstantBuffer(ShaderStage, u32 slot, BufferHandle) = 0;
    virtual void BindShaderResource(ShaderStage, u32 slot, TextureHandle) = 0;
    virtual void BindShaderResource(ShaderStage, u32 slot, BufferHandle) = 0;
    virtual void BindUnorderedAccess(u32 slot, BufferHandle) = 0;
    virtual void BindSampler(ShaderStage, u32 slot, SamplerHandle) = 0;

    virtual void ClearDepth(TextureHandle depth, f32 clearDepth, u8 clearStencil) = 0;

    virtual void CopyBuffer(BufferHandle dst, BufferHandle src) = 0;

    virtual void Draw(u32 vertexCount, u32 firstVertex = 0) = 0;
    virtual void DrawIndexed(u32 indexCount, u32 firstIndex = 0, i32 baseVertex = 0) = 0;
    virtual void Dispatch(u32 gx, u32 gy, u32 gz) = 0;
};

class IGFXDevice {
public:
    virtual ~IGFXDevice() = default;

    virtual BufferHandle CreateBuffer(const BufferDesc&, const void* initial = nullptr) = 0;
    virtual TextureHandle CreateTexture(const TextureDesc&,
                                        const void* initialPixels = nullptr) = 0;
    virtual ShaderHandle CreateShader(ShaderStage, const void* bytecode, usize size) = 0;
    virtual PipelineHandle CreateGraphicsPipeline(const GraphicsPipelineDesc&) = 0;
    virtual PipelineHandle CreateComputePipeline(const ComputePipelineDesc&) = 0;
    virtual SamplerHandle CreateSampler(const SamplerDesc&) = 0;

    virtual void Destroy(BufferHandle) = 0;
    virtual void Destroy(TextureHandle) = 0;
    virtual void Destroy(ShaderHandle) = 0;
    virtual void Destroy(PipelineHandle) = 0;
    virtual void Destroy(SamplerHandle) = 0;

    virtual void UpdateBuffer(BufferHandle, const void* data, usize size) = 0;
    virtual void* MapBuffer(BufferHandle) = 0;
    virtual void UnmapBuffer(BufferHandle) = 0;

    // `nativeWindowHandle` is interpreted by each backend:
    //   • d3d11 / d3d12: cast to HWND.
    //   • vulkan on Windows: cast to HWND (the backend creates the Win32
    //     VkSurfaceKHR internally).
    //   • vulkan on Linux: cast (via uintptr_t) to a host-supplied
    //     VkSurfaceKHR. The host gets the matching VkInstance from
    //     GetNativeInstance() and creates the surface itself (typically via
    //     glfwCreateWindowSurface), avoiding XCB/Xlib/Wayland branches in gfx.
    //   • webgpu on Windows: cast to HWND.
    //   • webgpu on Linux / macOS: cast to `GLFWwindow*`. The WebGPU backend
    //     pulls the platform-specific handle pair internally via
    //     glfw3native.h (Display+Window on X11, wl_display+wl_surface on
    //     Wayland, NSWindow on Cocoa) — more than one pointer's worth of
    //     state, which is why a single VkSurfaceKHR-style cast isn't enough.
    virtual SwapChainHandle CreateSwapChain(void* nativeWindowHandle, i32 width, i32 height,
                                            Format colorFormat = Format::R8G8B8A8_UNORM_SRGB) = 0;

    // Vulkan backend returns its VkInstance (cast to void*); other backends
    // return nullptr. Used by the host on Linux to build a VkSurfaceKHR via
    // glfwCreateWindowSurface before calling CreateSwapChain.
    virtual void* GetNativeInstance() const {
        return nullptr;
    }
    virtual void ResizeSwapChain(SwapChainHandle, i32 width, i32 height) = 0;
    virtual void DestroySwapChain(SwapChainHandle) = 0;
    virtual void Present(SwapChainHandle) = 0;

    // Read a render target back to the CPU as tightly-packed RGBA8 (row pitch
    // = width*4). Synchronous and slow (submits + WaitIdle internally) — for
    // off-screen / headless capture, not the hot path. The texture must have
    // been created with TextureUsage::CopySrc. Returns false if unsupported on
    // this backend or the copy failed. Default: unsupported.
    virtual bool ReadbackTexture(TextureHandle, i32 width, i32 height,
                                 std::vector<u8>& outRgba) {
        (void)width;
        (void)height;
        (void)outRgba;
        return false;
    }

    // Submit the current frame's recorded commands WITHOUT presenting — the
    // headless equivalent of Present. On backends with a deferred,
    // present-driven submit (Vulkan, WebGPU) the command buffer is only flushed
    // to the queue inside Present; a viewport that renders into an off-screen
    // target never calls Present, so its work would never execute and a
    // readback would see nothing. Hosts call this once per off-screen viewport
    // per frame in place of Present. No-op on immediate-context backends
    // (D3D11/D3D12) and on backends that submit per command.
    virtual void SubmitFrame() {}

    // Block until every GPU command submitted so far has fully completed.
    // A heavy stall — not for the hot path. Used by offline operations
    // (e.g. frame-capture export) that must read back GPU results
    // deterministically.
    virtual void WaitIdle() = 0;
    virtual TextureHandle GetSwapChainBackBuffer(SwapChainHandle) = 0;

    virtual TextureHandle GetSwapChainBackBufferLinear(SwapChainHandle) = 0;

    // Format the back-buffer was actually configured with — may differ
    // from the `colorFormat` requested at CreateSwapChain time (e.g. Dawn
    // on Windows D3D12 only exposes BGRA8 surface formats, so a request
    // for R8G8B8A8_UNORM_SRGB resolves to B8G8R8A8_UNORM_SRGB). Used by
    // the renderer to build PSOs whose rtvFormat matches the
    // back-buffer when rendering directly to the swap chain.
    virtual Format GetSwapChainFormat(SwapChainHandle) const = 0;

    virtual TextureHandle CreateColorTarget(i32 w, i32 h, Format f) = 0;
    virtual TextureHandle CreateDepthTarget(i32 w, i32 h, Format f) = 0;

    virtual IGFXCommandList* GetImmediateContext() = 0;

    virtual GfxApi GetApi() const = 0;
    virtual const char* GetDeviceName() const = 0;

    virtual Format PreferredDepthStencilFormat() const = 0;

    /// @brief Live GPU bytes currently allocated by this device, summed over
    ///        every CreateTexture / CreateBuffer that hasn't yet been
    ///        retired. Diagnostic only; backends that don't track this
    ///        return 0.
    virtual u64 LiveGpuBytes() const {
        return 0;
    }

    /// @brief Whether the device can sample BCn-compressed textures
    ///        directly. Desktop backends (D3D11/12, Vulkan, Metal) report
    ///        true — BC support is part of the feature-level floor we
    ///        target. WebGPU reports the negotiated `texture-compression-bc`
    ///        feature; adapters that lack it (older mobile, some software
    ///        fallbacks, browsers with the feature off) force the asset
    ///        path to decompress BCn → RGBA8 before upload.
    virtual bool SupportsBlockCompression() const {
        return true;
    }
};

// `enableValidation` turns on the API's debug / validation machinery:
//   • d3d11: D3D11_CREATE_DEVICE_DEBUG
//   • d3d12: D3D12 debug layer + DXGI debug + InfoQueue break-on-error
//   • vulkan: VK_LAYER_KHRONOS_validation + VK_EXT_debug_utils messenger
// Off by default — turning it on costs frame time and requires the
// platform's debug/validation runtimes to be installed (Graphics Tools
// optional feature on Windows; Vulkan SDK for the Khronos layer).
std::unique_ptr<IGFXDevice> CreateDevice(GfxApi api, bool enableValidation = false);

// Set the on-disk path the Vulkan backend uses for its
// VkPipelineCache blob. Must be called before CreateDevice (or
// IGFXDevice::Init for backends that ever take that route). Empty
// path = no persistence; the gfx layer still keeps an in-memory
// cache so PSO builds within a single run dedupe. The d3d backends
// ignore it — d3d12 and d3d11 manage their own pipeline state
// caches at the driver level. Path resolution (exe dir, %APPDATA%,
// XDG_CACHE_HOME, etc.) is a host concern; gfx never calls
// GetModuleFileName / readlink / SHGetKnownFolderPath itself.
void SetPipelineCachePath(const char* utf8Path);

// Read back the path previously set via SetPipelineCachePath. Used by
// renderer-level callers that want to put their own per-run state
// (e.g. the BLS PSO-trace file) next to the gfx pipeline cache so
// everything device-related lives in one host-controlled directory.
// Returns an empty path if SetPipelineCachePath was never called.
const std::filesystem::path& GetPipelineCachePath();

// Enumerate the physical devices a given backend can present to the
// host. Used by Settings UIs to populate a "preferred device" picker.
// Cheap: each backend spins up the smallest amount of state it needs
// (DXGI factory for d3d11/d3d12, a throw-away VkInstance for vulkan).
// Returns each device's marketing name in driver-reported order; on
// failure (no driver / no compatible adapter) returns an empty vector.
std::vector<std::string> EnumerateDevices(GfxApi api);

// Set the preferred adapter / physical device by *exact name match*
// against EnumerateDevices(api). Empty (the default) means "highest-
// VRAM discrete or fall back to integrated", matching what each
// backend used to do unconditionally. Must be called before
// CreateDevice; like SetPipelineCachePath, this is module-scope
// state on the gfx layer that the next CreateDevice consumes.
void SetPreferredDevice(const char* utf8Name);

// WebGPU-only: force Dawn's adapter backend (d3d11 / d3d12 / vulkan /
// gl / metal). Empty (default) lets Dawn pick. Useful for triaging
// device-hung issues that only repro on one backend. Ignored by other
// gfx APIs. Must be called before CreateDevice(GfxApi::WebGPU).
void SetWebGPUBackend(const char* utf8Name);
const std::string& GetWebGPUBackend();

} // namespace whiteout::flakes::gfx

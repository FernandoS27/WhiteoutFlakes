#pragma once

#include "common_types.h"
#include "gfx/gfx_types.h"
#include <memory>

namespace whiteout::flakes::gfx {

enum class BufferHandle    : u64 { Invalid = 0 };
enum class TextureHandle   : u64 { Invalid = 0 };
enum class ShaderHandle    : u64 { Invalid = 0 };
enum class PipelineHandle  : u64 { Invalid = 0 };
enum class SamplerHandle   : u64 { Invalid = 0 };
enum class SwapChainHandle : u64 { Invalid = 0 };

struct Viewport {
    f32 x        = 0;
    f32 y        = 0;
    f32 width    = 0;
    f32 height   = 0;
    f32 minDepth = 0;
    f32 maxDepth = 1;
};

struct Scissor {
    i32 x      = 0;
    i32 y      = 0;
    i32 width  = 0;
    i32 height = 0;
};

class IGFXCommandList {
public:
    virtual ~IGFXCommandList() = default;

    virtual void BeginRenderPass(TextureHandle color, TextureHandle depth,
                                 const f32 clearColor[4], f32 clearDepth,
                                 u8 clearStencil) = 0;
    virtual void EndRenderPass() = 0;

    virtual void SetViewport(const Viewport&) = 0;
    virtual void SetScissor (const Scissor&) = 0;

    virtual void BindPipeline(PipelineHandle) = 0;

    virtual void BindVertexBuffer  (u32 slot, BufferHandle, u32 stride, u32 offset = 0) = 0;
    virtual void BindIndexBuffer   (BufferHandle, Format  ) = 0;
    virtual void BindConstantBuffer(ShaderStage, u32 slot, BufferHandle) = 0;
    virtual void BindShaderResource(ShaderStage, u32 slot, TextureHandle) = 0;
    virtual void BindShaderResource(ShaderStage, u32 slot, BufferHandle  ) = 0;
    virtual void BindUnorderedAccess(u32 slot, BufferHandle) = 0;
    virtual void BindSampler       (ShaderStage, u32 slot, SamplerHandle) = 0;

    virtual void ClearDepth(TextureHandle depth, f32 clearDepth, u8 clearStencil) = 0;

    virtual void CopyBuffer(BufferHandle dst, BufferHandle src) = 0;

    virtual void Draw       (u32 vertexCount, u32 firstVertex = 0) = 0;
    virtual void DrawIndexed(u32 indexCount,  u32 firstIndex = 0, i32 baseVertex = 0) = 0;
    virtual void Dispatch   (u32 gx, u32 gy, u32 gz) = 0;
};

class IGFXDevice {
public:
    virtual ~IGFXDevice() = default;

    virtual BufferHandle   CreateBuffer (const BufferDesc&,  const void* initial = nullptr) = 0;
    virtual TextureHandle  CreateTexture(const TextureDesc&, const void* initialPixels = nullptr) = 0;
    virtual ShaderHandle   CreateShader (ShaderStage, const void* bytecode, usize size) = 0;
    virtual PipelineHandle CreateGraphicsPipeline(const GraphicsPipelineDesc&) = 0;
    virtual PipelineHandle CreateComputePipeline (const ComputePipelineDesc&) = 0;
    virtual SamplerHandle  CreateSampler(const SamplerDesc&) = 0;

    virtual void Destroy(BufferHandle)   = 0;
    virtual void Destroy(TextureHandle)  = 0;
    virtual void Destroy(ShaderHandle)   = 0;
    virtual void Destroy(PipelineHandle) = 0;
    virtual void Destroy(SamplerHandle)  = 0;

    virtual void  UpdateBuffer(BufferHandle, const void* data, usize size) = 0;
    virtual void* MapBuffer   (BufferHandle) = 0;
    virtual void  UnmapBuffer (BufferHandle) = 0;

    virtual SwapChainHandle CreateSwapChain(void* nativeWindowHandle,
                                            i32 width, i32 height,
                                            Format colorFormat = Format::R8G8B8A8_UNORM_SRGB) = 0;
    virtual void          ResizeSwapChain (SwapChainHandle, i32 width, i32 height) = 0;
    virtual void          DestroySwapChain(SwapChainHandle) = 0;
    virtual void          Present         (SwapChainHandle) = 0;
    virtual TextureHandle GetSwapChainBackBuffer(SwapChainHandle) = 0;

    virtual TextureHandle GetSwapChainBackBufferLinear(SwapChainHandle) = 0;

    virtual TextureHandle CreateColorTarget(i32 w, i32 h, Format f) = 0;
    virtual TextureHandle CreateDepthTarget(i32 w, i32 h, Format f) = 0;

    virtual IGFXCommandList* GetImmediateContext() = 0;

    virtual GfxApi      GetApi() const = 0;
    virtual const char* GetDeviceName() const = 0;
};

std::unique_ptr<IGFXDevice> CreateDevice(GfxApi api);

}

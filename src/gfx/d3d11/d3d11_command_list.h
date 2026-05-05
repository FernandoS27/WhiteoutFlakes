#pragma once

#include "gfx/gfx.h"

namespace WhiteoutDex::gfx::d3d11 {

class D3D11Device;

class D3D11CommandList final : public IGFXCommandList {
public:
    explicit D3D11CommandList(D3D11Device& device);
    ~D3D11CommandList() override = default;

    void BeginRenderPass(TextureHandle color, TextureHandle depth,
                         const f32 clearColor[4], f32 clearDepth,
                         u8 clearStencil) override;
    void EndRenderPass() override;

    void SetViewport(const Viewport&) override;
    void SetScissor (const Scissor&) override;

    void BindPipeline(PipelineHandle) override;

    void BindVertexBuffer  (u32 slot, BufferHandle, u32 stride, u32 offset) override;
    void BindIndexBuffer   (BufferHandle, Format) override;
    void BindConstantBuffer(ShaderStage, u32 slot, BufferHandle) override;
    void BindShaderResource(ShaderStage, u32 slot, TextureHandle) override;
    void BindShaderResource(ShaderStage, u32 slot, BufferHandle) override;
    void BindUnorderedAccess(u32 slot, BufferHandle) override;
    void BindSampler       (ShaderStage, u32 slot, SamplerHandle) override;

    void ClearDepth(TextureHandle depth, f32 clearDepth, u8 clearStencil) override;

    void CopyBuffer(BufferHandle dst, BufferHandle src) override;

    void Draw       (u32 vertexCount, u32 firstVertex) override;
    void DrawIndexed(u32 indexCount,  u32 firstIndex, i32 baseVertex) override;
    void Dispatch   (u32 gx, u32 gy, u32 gz) override;

private:
    D3D11Device& device_;
    bool         inRenderPass_ = false;
};

}

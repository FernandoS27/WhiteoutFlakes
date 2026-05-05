#pragma once

#include "gfx/gfx.h"
#include "d3d12_resources.h"

#include <array>

namespace WhiteoutDex::gfx::d3d12 {

class D3D12Device;

class D3D12CommandList final : public IGFXCommandList {
public:
    explicit D3D12CommandList(D3D12Device& device);
    ~D3D12CommandList() override = default;

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

    void OnFrameBegin();

    void TransitionBuffer (BufferEntry&, D3D12_RESOURCE_STATES newState);
    void TransitionTexture(TextureEntry&, D3D12_RESOURCE_STATES newState);

private:
    void EnsureDescriptorHeapsBound();
    void ApplyGraphicsBindings();
    void ApplyComputeBindings();
    void PromoteSrv(BufferHandle, ShaderStage);
    void PromoteSrv(TextureHandle, ShaderStage);

    D3D12Device& device_;

    bool inRenderPass_   = false;
    bool lastWasCompute_ = false;
    bool haveAnyPipeline_ = false;

    bool descriptorHeapsBound_ = false;

    TextureHandle currentColorRt_{};
    TextureHandle currentDepthRt_{};

    struct CbvSlot { BufferHandle buffer = BufferHandle::Invalid; };
    std::array<CbvSlot, kRootCbvsPerStage> cbvVs_{};
    std::array<CbvSlot, kRootCbvsPerStage> cbvPs_{};
    std::array<CbvSlot, kRootCbvsPerStage> cbvCs_{};

    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, kSrvsPerStage> srvVs_{};
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, kSrvsPerStage> srvPs_{};
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, kSrvsPerStage> srvCs_{};

    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, kUavsForCompute> uavCs_{};

    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, kSamplersPerStage> samplerPs_{};
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, kSamplersPerStage> samplerCs_{};
};

}

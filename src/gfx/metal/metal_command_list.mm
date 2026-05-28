// MetalCommandList — Phase A stub. Every IGFXCommandList method is a
// no-op pending the swap-chain + pipeline work in phases (b)..(d).
//
// FlushBindings, the per-stage encoder routing, and the dirty-tracked
// (stage, slot) → setVertexBuffer:/setFragmentBuffer:/...:atIndex:
// translation land alongside CreateGraphicsPipeline in metal_pipeline.mm.

#include "metal_command_list.h"
#include "metal_device.h"
#include "metal_device_state.h"

namespace whiteout::flakes::gfx::metal {

MetalCommandList::MetalCommandList(MetalDevice& device) : device_(device) {}
MetalCommandList::~MetalCommandList() = default;

void MetalCommandList::BeginRenderPass(TextureHandle, TextureHandle,
                                       const f32[4], f32, u8) {}
void MetalCommandList::EndRenderPass() {}

void MetalCommandList::SetViewport(const Viewport&) {}
void MetalCommandList::SetScissor(const Scissor&) {}

void MetalCommandList::BindPipeline(PipelineHandle h) {
    lastBoundPipeline_ = h;
}

void MetalCommandList::BindVertexBuffer(u32, BufferHandle, u32, u32) {}
void MetalCommandList::BindIndexBuffer(BufferHandle, Format) {}
void MetalCommandList::BindConstantBuffer(ShaderStage, u32, BufferHandle) {}
void MetalCommandList::BindShaderResource(ShaderStage, u32, TextureHandle) {}
void MetalCommandList::BindShaderResource(ShaderStage, u32, BufferHandle) {}
void MetalCommandList::BindUnorderedAccess(u32, BufferHandle) {}
void MetalCommandList::BindSampler(ShaderStage, u32, SamplerHandle) {}

void MetalCommandList::ClearDepth(TextureHandle, f32, u8) {}
void MetalCommandList::CopyBuffer(BufferHandle, BufferHandle) {}

void MetalCommandList::Draw(u32, u32) {}
void MetalCommandList::DrawIndexed(u32, u32, i32) {}
void MetalCommandList::Dispatch(u32, u32, u32) {}

void MetalCommandList::FlushBindings() {}

} // namespace whiteout::flakes::gfx::metal

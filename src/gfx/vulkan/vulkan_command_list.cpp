// Vulkan IGFXCommandList — dynamic-rendering pass plus the bind / draw
// scaffolding. vulkan.hpp method-style API; the per-frame command
// buffer is a vk::raii::CommandBuffer rotated by VulkanDeviceState.
//
// On the first BeginRenderPass each frame we wait on the previous
// fence, reset the buffer, and call begin(); subsequent passes within
// the same frame keep recording into the same buffer. Present (in
// vulkan_swap_chain.cpp) ends the buffer and submits.

#include "vulkan_device.h"
#include "vulkan_resources.h"
#include "vulkan_translate.h"

#include <cstdio>
#include <cstring>

namespace whiteout::flakes::gfx::vulkan {

namespace {

void TransitionImageLayout(vk::raii::CommandBuffer& cb, VkImage image,
                           vk::ImageAspectFlags aspect,
                           vk::ImageLayout oldLayout, vk::ImageLayout newLayout,
                           vk::PipelineStageFlags2 srcStage,
                           vk::AccessFlags2        srcAccess,
                           vk::PipelineStageFlags2 dstStage,
                           vk::AccessFlags2        dstAccess)
{
    vk::ImageMemoryBarrier2 barrier{
        .srcStageMask  = srcStage,
        .srcAccessMask = srcAccess,
        .dstStageMask  = dstStage,
        .dstAccessMask = dstAccess,
        .oldLayout     = oldLayout,
        .newLayout     = newLayout,
        .image         = vk::Image(image),
        .subresourceRange = { aspect, 0, 1, 0, 1 },
    };
    vk::DependencyInfo dep{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers    = &barrier,
    };
    cb.pipelineBarrier2(dep);
}

void EnsureRecording(VulkanDeviceState& s, FrameContext& frame) {
    if (frame.recording) return;
    (void)s.device.waitForFences(*frame.inFlightFence, vk::True, UINT64_MAX);
    s.device.resetFences(*frame.inFlightFence);
    frame.commandBuffer.reset({});
    (void)frame.commandBuffer.begin({
        .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
    });
    frame.recording = true;
}

}  // namespace

VulkanCommandList::VulkanCommandList(VulkanDevice& device) : device_(device) {}
VulkanCommandList::~VulkanCommandList() = default;

void VulkanCommandList::BeginRenderPass(TextureHandle color, TextureHandle depth,
                                         const f32 clearColor[4],
                                         f32 clearDepth, u8 clearStencil)
{
    auto& s     = device_.State();
    auto& frame = s.frames[s.frameIndex];

    EnsureRecording(s, frame);

    auto* colorTex = s.textures.Get(static_cast<u64>(color));
    auto* depthTex = s.textures.Get(static_cast<u64>(depth));

    // Acquire-on-first-bind: if the color attachment is a swap-chain
    // proxy, AcquireSwapChainImageIfNeeded fetches the next image and
    // re-points the proxy's image / view to it.
    if (colorTex && colorTex->swapChainProxy != SwapChainHandle::Invalid) {
        if (auto* sc = s.swapchains.Get(static_cast<u64>(colorTex->swapChainProxy))) {
            AcquireSwapChainImageIfNeeded(s, *sc, frame);
        }
    }

    vk::RenderingAttachmentInfo colorAttach{};
    if (colorTex) {
        TransitionImageLayout(frame.commandBuffer, colorTex->image,
            vk::ImageAspectFlagBits::eColor,
            colorTex->currentLayout,
            vk::ImageLayout::eColorAttachmentOptimal,
            vk::PipelineStageFlagBits2::eTopOfPipe, {},
            vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            vk::AccessFlagBits2::eColorAttachmentWrite);
        colorTex->currentLayout = vk::ImageLayout::eColorAttachmentOptimal;

        vk::ClearColorValue cc{};
        std::memcpy(cc.float32.data(), clearColor, sizeof(f32) * 4);

        colorAttach = vk::RenderingAttachmentInfo{
            .imageView   = vk::ImageView(colorTex->view),
            .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
            .loadOp      = vk::AttachmentLoadOp::eClear,
            .storeOp     = vk::AttachmentStoreOp::eStore,
            .clearValue  = vk::ClearValue{ .color = cc },
        };
    }

    vk::RenderingAttachmentInfo depthAttach{};
    if (depthTex) {
        TransitionImageLayout(frame.commandBuffer, depthTex->image,
            depthTex->aspect,
            depthTex->currentLayout,
            vk::ImageLayout::eDepthStencilAttachmentOptimal,
            vk::PipelineStageFlagBits2::eTopOfPipe, {},
            vk::PipelineStageFlagBits2::eEarlyFragmentTests
              | vk::PipelineStageFlagBits2::eLateFragmentTests,
            vk::AccessFlagBits2::eDepthStencilAttachmentRead
              | vk::AccessFlagBits2::eDepthStencilAttachmentWrite);
        depthTex->currentLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal;

        depthAttach = vk::RenderingAttachmentInfo{
            .imageView   = vk::ImageView(depthTex->view),
            .imageLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal,
            .loadOp      = vk::AttachmentLoadOp::eClear,
            .storeOp     = vk::AttachmentStoreOp::eStore,
            .clearValue  = vk::ClearValue{
                .depthStencil = { clearDepth, clearStencil },
            },
        };
    }

    const i32 width  = colorTex ? colorTex->width  : (depthTex ? depthTex->width  : 0);
    const i32 height = colorTex ? colorTex->height : (depthTex ? depthTex->height : 0);

    vk::RenderingInfo info{
        .renderArea          = { {0, 0}, { static_cast<u32>(width), static_cast<u32>(height) } },
        .layerCount          = 1,
        .colorAttachmentCount = colorTex ? 1u : 0u,
        .pColorAttachments    = colorTex ? &colorAttach : nullptr,
        .pDepthAttachment     = depthTex ? &depthAttach : nullptr,
    };
    frame.commandBuffer.beginRendering(info);

    // Default viewport / scissor (renderer typically overrides).
    vk::Viewport vp{ 0.0f, 0.0f,
                     static_cast<f32>(width), static_cast<f32>(height),
                     0.0f, 1.0f };
    frame.commandBuffer.setViewport(0, vp);
    vk::Rect2D scissor{ {0, 0}, { static_cast<u32>(width), static_cast<u32>(height) } };
    frame.commandBuffer.setScissor(0, scissor);
}

void VulkanCommandList::EndRenderPass() {
    auto& s     = device_.State();
    auto& frame = s.frames[s.frameIndex];
    if (frame.recording) {
        frame.commandBuffer.endRendering();
    }
}

void VulkanCommandList::SetViewport(const Viewport& v) {
    auto& s     = device_.State();
    auto& frame = s.frames[s.frameIndex];
    if (!frame.recording) return;
    vk::Viewport vp{ v.x, v.y, v.width, v.height, v.minDepth, v.maxDepth };
    frame.commandBuffer.setViewport(0, vp);
}

void VulkanCommandList::SetScissor(const Scissor& sc) {
    auto& s     = device_.State();
    auto& frame = s.frames[s.frameIndex];
    if (!frame.recording) return;
    vk::Rect2D r{ { sc.x, sc.y },
                  { static_cast<u32>(sc.width), static_cast<u32>(sc.height) } };
    frame.commandBuffer.setScissor(0, r);
}

// ---- Pipeline + vertex/index/constant binding -----------------------------

void VulkanCommandList::BindPipeline(PipelineHandle h) {
    auto& s     = device_.State();
    auto& frame = s.frames[s.frameIndex];
    if (!frame.recording) return;
    auto* p = s.pipelines.Get(static_cast<u64>(h));
    if (!p) return;
    frame.commandBuffer.bindPipeline(
        p->isCompute ? vk::PipelineBindPoint::eCompute
                     : vk::PipelineBindPoint::eGraphics,
        *p->pipeline);
}

void VulkanCommandList::BindVertexBuffer(u32 slot, BufferHandle h,
                                          u32 /*stride*/, u32 offset)
{
    auto& s     = device_.State();
    auto& frame = s.frames[s.frameIndex];
    if (!frame.recording) return;
    auto* b = s.buffers.Get(static_cast<u64>(h));
    if (!b) return;
    const vk::Buffer     buf = vk::Buffer(b->buffer);
    const vk::DeviceSize off = offset;
    frame.commandBuffer.bindVertexBuffers(slot, buf, off);
}

void VulkanCommandList::BindIndexBuffer(BufferHandle h, Format f) {
    auto& s     = device_.State();
    auto& frame = s.frames[s.frameIndex];
    if (!frame.recording) return;
    auto* b = s.buffers.Get(static_cast<u64>(h));
    if (!b) return;
    const vk::IndexType type = (f == Format::R16_UINT)
                                    ? vk::IndexType::eUint16
                                    : vk::IndexType::eUint32;
    frame.commandBuffer.bindIndexBuffer(vk::Buffer(b->buffer), 0, type);
}

void VulkanCommandList::BindConstantBuffer(ShaderStage /*stage*/, u32 slot,
                                            BufferHandle buffer)
{
    if (slot >= pendingCBs_.size()) return;
    pendingCBs_[slot] = { buffer, true };
    anyDescriptorDirty_ = true;
}

// SRV/UAV/Sampler push-descriptor bindings: scaffold only — Phase 1
// grid/viewcube paths don't need them, so we leave them as no-ops. The
// shape of the layout (kCbBindingBase / kCbBindingCount in
// vulkan_resources.h) is what the next milestone extends.
void VulkanCommandList::BindShaderResource(ShaderStage, u32, TextureHandle) {}
void VulkanCommandList::BindShaderResource(ShaderStage, u32, BufferHandle)  {}
void VulkanCommandList::BindUnorderedAccess(u32, BufferHandle)              {}
void VulkanCommandList::BindSampler       (ShaderStage, u32, SamplerHandle) {}

void VulkanCommandList::FlushDescriptors() {
    if (!anyDescriptorDirty_) return;

    auto& s     = device_.State();
    auto& frame = s.frames[s.frameIndex];

    std::array<vk::DescriptorBufferInfo, kCbBindingCount> infos{};
    std::array<vk::WriteDescriptorSet,   kCbBindingCount> writes{};
    u32 writeCount = 0;
    for (u32 i = 0; i < pendingCBs_.size(); ++i) {
        if (!pendingCBs_[i].dirty) continue;
        auto* b = s.buffers.Get(static_cast<u64>(pendingCBs_[i].buffer));
        if (!b) continue;
        infos[writeCount] = vk::DescriptorBufferInfo{
            .buffer = vk::Buffer(b->buffer),
            .offset = 0,
            .range  = VK_WHOLE_SIZE,
        };
        writes[writeCount] = vk::WriteDescriptorSet{
            .dstBinding      = kCbBindingBase + i,
            .descriptorCount = 1,
            .descriptorType  = vk::DescriptorType::eUniformBuffer,
            .pBufferInfo     = &infos[writeCount],
        };
        ++writeCount;
        pendingCBs_[i].dirty = false;
    }

    if (writeCount > 0) {
        frame.commandBuffer.pushDescriptorSetKHR(
            vk::PipelineBindPoint::eGraphics,
            *s.pipelineLayout,
            /*set*/ 0,
            vk::ArrayProxy<const vk::WriteDescriptorSet>{
                writeCount, writes.data() });
    }
    anyDescriptorDirty_ = false;
}

void VulkanCommandList::ClearDepth(TextureHandle depth, f32 clearDepth, u8 clearStencil) {
    auto& s     = device_.State();
    auto& frame = s.frames[s.frameIndex];
    auto* t = s.textures.Get(static_cast<u64>(depth));
    if (!t || !frame.recording) return;
    TransitionImageLayout(frame.commandBuffer, t->image, t->aspect,
        t->currentLayout, vk::ImageLayout::eTransferDstOptimal,
        vk::PipelineStageFlagBits2::eTopOfPipe, {},
        vk::PipelineStageFlagBits2::eClear, vk::AccessFlagBits2::eTransferWrite);
    vk::ClearDepthStencilValue v{ clearDepth, clearStencil };
    vk::ImageSubresourceRange range{ t->aspect, 0, 1, 0, 1 };
    frame.commandBuffer.clearDepthStencilImage(
        vk::Image(t->image), vk::ImageLayout::eTransferDstOptimal, v, range);
    t->currentLayout = vk::ImageLayout::eTransferDstOptimal;
}
void VulkanCommandList::CopyBuffer(BufferHandle dst, BufferHandle src) {
    auto& s     = device_.State();
    auto& frame = s.frames[s.frameIndex];
    if (!frame.recording) return;
    auto* d = s.buffers.Get(static_cast<u64>(dst));
    auto* sr = s.buffers.Get(static_cast<u64>(src));
    if (!d || !sr) return;
    vk::BufferCopy region{ .size = std::min(d->desc.size, sr->desc.size) };
    frame.commandBuffer.copyBuffer(vk::Buffer(sr->buffer), vk::Buffer(d->buffer), region);
}

void VulkanCommandList::Draw(u32 vertexCount, u32 firstVertex) {
    auto& s     = device_.State();
    auto& frame = s.frames[s.frameIndex];
    if (!frame.recording) return;
    FlushDescriptors();
    frame.commandBuffer.draw(vertexCount, /*instances*/ 1, firstVertex, /*firstInstance*/ 0);
}

void VulkanCommandList::DrawIndexed(u32 indexCount, u32 firstIndex, i32 baseVertex) {
    auto& s     = device_.State();
    auto& frame = s.frames[s.frameIndex];
    if (!frame.recording) return;
    FlushDescriptors();
    frame.commandBuffer.drawIndexed(indexCount, /*instances*/ 1,
                                     firstIndex, baseVertex, /*firstInstance*/ 0);
}

void VulkanCommandList::Dispatch(u32 gx, u32 gy, u32 gz) {
    auto& s     = device_.State();
    auto& frame = s.frames[s.frameIndex];
    if (!frame.recording) return;
    FlushDescriptors();
    frame.commandBuffer.dispatch(gx, gy, gz);
}

}  // namespace whiteout::flakes::gfx::vulkan

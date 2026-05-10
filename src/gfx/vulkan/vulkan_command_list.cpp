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
    // Frame fence reached → run any deferred destroys whose timeline
    // value the GPU has caught up to. Cheap: just queries one counter
    // and walks the list.
    DrainPendingDeletes(s);
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

    // Default viewport / scissor — flip Y to match D3D conventions
    // (renderer hands us projection matrices that assume top-left
    // origin going down). Vulkan 1.1+ permits negative viewport
    // height, equivalent to a Y-flip without extra matrix work.
    vk::Viewport vp{ 0.0f, static_cast<f32>(height),
                     static_cast<f32>(width), -static_cast<f32>(height),
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
    // Flip Y: see comment in BeginRenderPass.
    vk::Viewport vp{ v.x, v.y + v.height, v.width, -v.height,
                     v.minDepth, v.maxDepth };
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

void VulkanCommandList::BindShaderResource(ShaderStage /*stage*/, u32 slot,
                                            TextureHandle texture) {
    if (slot >= pendingSRVs_.size()) return;
    pendingSRVs_[slot] = { texture, true };
    anyDescriptorDirty_ = true;
}

// Buffer-shaped SRVs and UAVs aren't wired in Phase 1; the BLS path
// uses them but we don't compile BLS for Vulkan yet.
void VulkanCommandList::BindShaderResource(ShaderStage, u32, BufferHandle) {}
void VulkanCommandList::BindUnorderedAccess(u32, BufferHandle)             {}

void VulkanCommandList::BindSampler(ShaderStage /*stage*/, u32 slot,
                                     SamplerHandle sampler) {
    if (slot >= pendingSamplers_.size()) return;
    pendingSamplers_[slot] = { sampler, true };
    anyDescriptorDirty_ = true;
}

void VulkanCommandList::FlushDescriptors() {
    if (!anyDescriptorDirty_) return;

    auto& s     = device_.State();
    auto& frame = s.frames[s.frameIndex];

    // CBs, SRVs, and samplers all push at once. Use small fixed-size
    // arrays sized to the per-kind binding budget so we don't allocate.
    constexpr u32 kMaxWrites = 12;
    std::array<vk::DescriptorBufferInfo, kMaxWrites> bufInfos{};
    std::array<vk::DescriptorImageInfo,  kMaxWrites> imgInfos{};
    std::array<vk::WriteDescriptorSet,   kMaxWrites> writes{};
    u32 writeCount = 0;

    for (u32 i = 0; i < pendingCBs_.size() && i < kCbBindingCount; ++i) {
        if (!pendingCBs_[i].dirty) continue;
        auto* b = s.buffers.Get(static_cast<u64>(pendingCBs_[i].buffer));
        if (!b) continue;
        bufInfos[writeCount] = vk::DescriptorBufferInfo{
            .buffer = vk::Buffer(b->buffer),
            .offset = 0,
            .range  = VK_WHOLE_SIZE,
        };
        writes[writeCount] = vk::WriteDescriptorSet{
            .dstBinding      = kCbBindingBase + i,
            .descriptorCount = 1,
            .descriptorType  = vk::DescriptorType::eUniformBuffer,
            .pBufferInfo     = &bufInfos[writeCount],
        };
        ++writeCount;
        pendingCBs_[i].dirty = false;
    }
    for (u32 i = 0; i < pendingSRVs_.size() && i < kSrvBindingCount; ++i) {
        if (!pendingSRVs_[i].dirty) continue;
        auto* t = s.textures.Get(static_cast<u64>(pendingSRVs_[i].texture));
        if (!t) continue;
        imgInfos[writeCount] = vk::DescriptorImageInfo{
            .imageView   = vk::ImageView(t->view),
            .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
        };
        writes[writeCount] = vk::WriteDescriptorSet{
            .dstBinding      = kSrvBindingBase + i,
            .descriptorCount = 1,
            .descriptorType  = vk::DescriptorType::eSampledImage,
            .pImageInfo      = &imgInfos[writeCount],
        };
        ++writeCount;
        pendingSRVs_[i].dirty = false;
    }
    for (u32 i = 0; i < pendingSamplers_.size() && i < kSamplerBindingCount; ++i) {
        if (!pendingSamplers_[i].dirty) continue;
        auto* sm = s.samplers.Get(static_cast<u64>(pendingSamplers_[i].sampler));
        if (!sm) continue;
        imgInfos[writeCount] = vk::DescriptorImageInfo{
            .sampler = *sm->sampler,
        };
        writes[writeCount] = vk::WriteDescriptorSet{
            .dstBinding      = kSamplerBindingBase + i,
            .descriptorCount = 1,
            .descriptorType  = vk::DescriptorType::eSampler,
            .pImageInfo      = &imgInfos[writeCount],
        };
        ++writeCount;
        pendingSamplers_[i].dirty = false;
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

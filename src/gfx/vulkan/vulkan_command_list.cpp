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
        // VK_REMAINING_{MIP_LEVELS,ARRAY_LAYERS} cover every subresource
        // — render-target attachments have only mip 0 / layer 0, but
        // sampled textures may be mip-chained or cube arrays. Keeping
        // the range whole keeps the texture's tracked currentLayout in
        // sync regardless of which view the renderer binds.
        .subresourceRange = {
            aspect,
            0, VK_REMAINING_MIP_LEVELS,
            0, VK_REMAINING_ARRAY_LAYERS,
        },
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
    // Same drain on the transfer-timeline list — staging buffers and
    // one-shot transfer command buffers from completed uploads get
    // reaped here. Cheap when empty; only polls the timeline counter
    // when there's something to consider.
    DrainPendingTransferDeletes(s);
    // Reset the SRV/sampler descriptor pool — the CB set is push-
    // descriptor (no allocation, lives in the command buffer) so the
    // pool only carries the two non-push sets. vkResetDescriptorPool
    // frees every set allocated in the previous frame in one shot.
    frame.descriptorPool.reset(vk::DescriptorPoolResetFlags{});
    frame.commandBuffer.reset({});
    (void)frame.commandBuffer.begin({
        .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
    });
    frame.recording = true;

    // Drain SRV transitions queued by CreateTexture. Batch every
    // pending texture into a single pipelineBarrier2 so a frame that
    // creates many textures doesn't pay one barrier each. Must happen
    // BEFORE the first BeginRenderPass — image-layout transitions
    // aren't allowed inside a dynamic-rendering scope.
    if (!s.pendingSrvTransitions.empty()) {
        std::vector<vk::ImageMemoryBarrier2> barriers;
        barriers.reserve(s.pendingSrvTransitions.size());
        for (u64 slot : s.pendingSrvTransitions) {
            auto* t = s.textures.Get(slot);
            if (!t || !t->image) continue;
            if (t->currentLayout == vk::ImageLayout::eShaderReadOnlyOptimal) continue;
            barriers.push_back(vk::ImageMemoryBarrier2{
                .srcStageMask  = vk::PipelineStageFlagBits2::eAllCommands,
                .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
                .dstStageMask  = vk::PipelineStageFlagBits2::eAllCommands,
                .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
                .oldLayout     = t->currentLayout,
                .newLayout     = vk::ImageLayout::eShaderReadOnlyOptimal,
                .image         = vk::Image(t->image),
                .subresourceRange = {
                    t->aspect,
                    0, VK_REMAINING_MIP_LEVELS,
                    0, VK_REMAINING_ARRAY_LAYERS,
                },
            });
            t->currentLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
        }
        if (!barriers.empty()) {
            frame.commandBuffer.pipelineBarrier2(vk::DependencyInfo{
                .imageMemoryBarrierCount = static_cast<u32>(barriers.size()),
                .pImageMemoryBarriers    = barriers.data(),
            });
        }
        s.pendingSrvTransitions.clear();
    }
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

    // Detect the frame-start transition: pool reset (Sets 1/2) + cmd-
    // buffer reset (Set 0 push state) both happen inside EnsureRecording,
    // which means whatever Bind* we did during the previous frame is
    // gone from the GPU side. Force every set-dirty so the first
    // FlushDescriptors this frame rebuilds them; after that the
    // equality short-circuit in Bind* keeps unchanged sets cheap.
    const bool startingNewFrame = !frame.recording;
    EnsureRecording(s, frame);
    if (startingNewFrame) {
        cbSetDirty_      = true;
        srvSetDirty_     = true;
        samplerSetDirty_ = true;
    }

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

    activeColorAttachment_ = colorTex ? color : TextureHandle::Invalid;
    activeColorFormat_     = colorTex
        ? static_cast<u32>(colorTex->format)
        : static_cast<u32>(vk::Format::eUndefined);

    // Batch the color + depth attachment transitions into a single
    // pipelineBarrier2. The driver wants every {image, oldLayout,
    // newLayout, stage masks} tuple it has — calling
    // pipelineBarrier2 twice with one barrier each costs two API
    // round-trips and two driver-side scheduling decisions for what
    // is conceptually one frame-boundary handoff.
    std::array<vk::ImageMemoryBarrier2, 2> attachBarriers{};
    u32 barrierCount = 0;

    vk::RenderingAttachmentInfo colorAttach{};
    if (colorTex) {
        attachBarriers[barrierCount++] = vk::ImageMemoryBarrier2{
            .srcStageMask  = vk::PipelineStageFlagBits2::eTopOfPipe,
            .srcAccessMask = {},
            .dstStageMask  = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
            .oldLayout     = colorTex->currentLayout,
            .newLayout     = vk::ImageLayout::eColorAttachmentOptimal,
            .image         = vk::Image(colorTex->image),
            .subresourceRange = {
                vk::ImageAspectFlagBits::eColor,
                0, VK_REMAINING_MIP_LEVELS,
                0, VK_REMAINING_ARRAY_LAYERS,
            },
        };
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
        attachBarriers[barrierCount++] = vk::ImageMemoryBarrier2{
            .srcStageMask  = vk::PipelineStageFlagBits2::eTopOfPipe,
            .srcAccessMask = {},
            .dstStageMask  = vk::PipelineStageFlagBits2::eEarlyFragmentTests
                           | vk::PipelineStageFlagBits2::eLateFragmentTests,
            .dstAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentRead
                           | vk::AccessFlagBits2::eDepthStencilAttachmentWrite,
            .oldLayout     = depthTex->currentLayout,
            .newLayout     = vk::ImageLayout::eDepthStencilAttachmentOptimal,
            .image         = vk::Image(depthTex->image),
            .subresourceRange = {
                depthTex->aspect,
                0, VK_REMAINING_MIP_LEVELS,
                0, VK_REMAINING_ARRAY_LAYERS,
            },
        };
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

    if (barrierCount > 0) {
        frame.commandBuffer.pipelineBarrier2({
            .imageMemoryBarrierCount = barrierCount,
            .pImageMemoryBarriers    = attachBarriers.data(),
        });
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
    if (!frame.recording) return;
    frame.commandBuffer.endRendering();

    // Eagerly transition the color attachment to eShaderReadOnlyOptimal
    // so the next pass can sample it (HDR → tonemap is the canonical
    // case). The transition is illegal inside an active rendering scope
    // — endRendering above closes it — and the renderer never exposes
    // explicit barriers through the gfx API, so doing it here is the
    // only place we know the attachment write is finished. Swap-chain
    // proxies aren't sampled (they're consumed by Present), so we skip
    // them; their layout is driven by the Present path instead.
    if (activeColorAttachment_ != TextureHandle::Invalid) {
        auto* t = s.textures.Get(static_cast<u64>(activeColorAttachment_));
        if (t && t->image && t->swapChainProxy == SwapChainHandle::Invalid &&
            t->currentLayout != vk::ImageLayout::eShaderReadOnlyOptimal)
        {
            TransitionImageLayout(frame.commandBuffer, t->image, t->aspect,
                t->currentLayout, vk::ImageLayout::eShaderReadOnlyOptimal,
                vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                vk::AccessFlagBits2::eColorAttachmentWrite,
                vk::PipelineStageFlagBits2::eFragmentShader,
                vk::AccessFlagBits2::eShaderRead);
            t->currentLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
        }
    }
    activeColorAttachment_ = TextureHandle::Invalid;
    activeColorFormat_     = static_cast<u32>(vk::Format::eUndefined);
    lastBoundPipeline_     = PipelineHandle::Invalid;
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
    // Catch VUID-vkCmdDraw-dynamicRenderingUnusedAttachments-08910
    // pre-Draw so we can attribute the mismatch to a C++ call site
    // (BindPipeline) rather than digging through the validation layer's
    // command-buffer dump. Skipped for compute pipelines + depth-only
    // passes (pipeline.colorFormat == eUndefined).
    const vk::Format activeFmt = static_cast<vk::Format>(activeColorFormat_);
    if (!p->isCompute &&
        p->colorFormat != vk::Format::eUndefined &&
        activeFmt != vk::Format::eUndefined &&
        p->colorFormat != activeFmt)
    {
        std::fprintf(stderr,
            "[vk] BindPipeline format mismatch: pipeline=%s renderpass=%s "
            "(handle=%llu)\n",
            vk::to_string(p->colorFormat).c_str(),
            vk::to_string(activeFmt).c_str(),
            static_cast<unsigned long long>(h));
    }
    lastBoundPipeline_ = h;
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
    // CpuWritable vertex buffers (ribbons, particles) ride the same
    // per-Map ring as CBs — add the buffer's current ring offset so
    // the GPU reads the slot the CPU just wrote, not slot 0.
    const vk::DeviceSize off = offset + b->currentOffset();
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
    // Same ring-offset rule for dynamic index buffers.
    frame.commandBuffer.bindIndexBuffer(vk::Buffer(b->buffer),
                                         b->currentOffset(), type);
}

// Translate a renderer-facing (stage, slot) pair into a global
// descriptor binding number. The slang sources annotate every PS-side
// resource at `register_index + PsBindingOffset`, so we mirror the
// same shift here. Future stages claim their own slice
// (GsBindingOffset = 32, …) when added.
static u32 BindingForStage(ShaderStage stage, u32 slot) {
    return slot + (stage == ShaderStage::Pixel ? kStageBindingShift : 0);
}

void VulkanCommandList::BindConstantBuffer(ShaderStage stage, u32 slot,
                                            BufferHandle buffer)
{
    const u32 binding = BindingForStage(stage, slot);
    if (binding >= pendingCBs_.size()) return;
    // Capture the buffer's CURRENT ring offset so subsequent MapBuffer
    // rotations don't move the data this draw is supposed to read.
    // The range used in the descriptor write is computed from the
    // buffer's slot stride at flush time.
    auto& s = device_.State();
    auto* b = s.buffers.Get(static_cast<u64>(buffer));
    const u64 off = b ? b->currentOffset() : 0;
    auto& cur = pendingCBs_[binding];
    if (cur.buffer == buffer && cur.offset == off) return;  // identical → no-op
    cur = { buffer, off };
    cbSetDirty_ = true;
}

void VulkanCommandList::BindShaderResource(ShaderStage stage, u32 slot,
                                            TextureHandle texture) {
    const u32 binding = BindingForStage(stage, slot);
    if (binding >= pendingSRVs_.size()) return;
    auto& cur = pendingSRVs_[binding];
    if (cur.texture == texture) return;  // identical → no-op
    cur = { texture };
    srvSetDirty_ = true;
}

// Buffer-shaped SRVs and UAVs aren't wired in Phase 1; the BLS path
// uses them but we don't compile BLS for Vulkan yet.
void VulkanCommandList::BindShaderResource(ShaderStage, u32, BufferHandle) {}
void VulkanCommandList::BindUnorderedAccess(u32, BufferHandle)             {}

void VulkanCommandList::BindSampler(ShaderStage stage, u32 slot,
                                     SamplerHandle sampler) {
    const u32 binding = BindingForStage(stage, slot);
    if (binding >= pendingSamplers_.size()) return;
    auto& cur = pendingSamplers_[binding];
    if (cur.sampler == sampler) return;  // identical → no-op
    cur = { sampler };
    samplerSetDirty_ = true;
}

void VulkanCommandList::FlushDescriptors() {
    if (!cbSetDirty_ && !srvSetDirty_ && !samplerSetDirty_) return;

    auto& s     = device_.State();
    auto& frame = s.frames[s.frameIndex];

    // Mixed-mode descriptors:
    //   • set 0 (CBs)      — push descriptors. The CB set rewrites on
    //                         every Bind because of the per-frame ring
    //                         offset, so pushing inline beats alloc+
    //                         update+bind from a pool.
    //   • set 1 (SRVs)     — pool-allocated per Flush. Vulkan caps
    //   • set 2 (Samplers) — pool-allocated per Flush. push sets to
    //                         one per pipeline layout, so the lower-
    //                         churn sets stay on the pool path.
    //
    // Per-set dirty flags let unchanged sets skip work entirely.
    // Within a frame, an already-bound SRV/sampler set stays bound on
    // the pipeline-layout slot until something else replaces it, so
    // skipping its re-alloc + re-bind is safe across consecutive
    // draws that re-use it (common in the geoset loop).

    // ---- Set 0: CBs (push descriptors, no allocation) ----
    if (cbSetDirty_) {
        std::array<vk::DescriptorBufferInfo, kCbBindingCount> infos{};
        std::array<vk::WriteDescriptorSet,   kCbBindingCount> writes{};
        u32 count = 0;
        for (u32 i = 0; i < pendingCBs_.size() && i < kCbBindingCount; ++i) {
            auto* b = s.buffers.Get(static_cast<u64>(pendingCBs_[i].buffer));
            if (!b) continue;
            infos[count] = vk::DescriptorBufferInfo{
                .buffer = vk::Buffer(b->buffer),
                .offset = pendingCBs_[i].offset,
                .range  = b->slotCount > 1 ? b->desc.size : VK_WHOLE_SIZE,
            };
            writes[count] = vk::WriteDescriptorSet{
                // dstSet is ignored by vkCmdPushDescriptorSetKHR.
                .dstBinding      = i,
                .descriptorCount = 1,
                .descriptorType  = vk::DescriptorType::eUniformBuffer,
                .pBufferInfo     = &infos[count],
            };
            ++count;
        }
        if (count > 0) {
            frame.commandBuffer.pushDescriptorSetKHR(
                vk::PipelineBindPoint::eGraphics,
                *s.pipelineLayout,
                kCbSetIndex,
                vk::ArrayProxy<const vk::WriteDescriptorSet>{ count, writes.data() });
        }
        cbSetDirty_ = false;
    }

    // ---- Sets 1 & 2: pool-allocated path ----------------------------
    // Treat SRV + sampler as a unit: if either is dirty we re-alloc
    // and re-bind both, since bindDescriptorSets takes a contiguous
    // range starting at firstSet. The cost is one extra pool alloc /
    // descriptor write per unchanged set in the pair — small.
    // Bypass the raii allocateDescriptorSets() — its raii::DescriptorSet
    // destructor calls vkFreeDescriptorSets, which is illegal on pools
    // created without FREE_DESCRIPTOR_SET_BIT (we whole-pool reset at
    // frame start instead).
    if (srvSetDirty_ || samplerSetDirty_) {
        auto allocSet = [&](VkDescriptorSetLayout layout) -> VkDescriptorSet {
            VkDescriptorSetAllocateInfo ai{
                .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                .descriptorPool     = *frame.descriptorPool,
                .descriptorSetCount = 1,
                .pSetLayouts        = &layout,
            };
            VkDescriptorSet set = VK_NULL_HANDLE;
            if (vkAllocateDescriptorSets(*s.device, &ai, &set) != VK_SUCCESS) {
                return VK_NULL_HANDLE;
            }
            return set;
        };

        std::array<VkDescriptorSet, 2> setsToBind{ VK_NULL_HANDLE, VK_NULL_HANDLE };

        // ---- Set 1: SRVs ----
        {
            std::array<vk::DescriptorImageInfo, kSrvBindingCount> infos{};
            std::array<vk::WriteDescriptorSet,  kSrvBindingCount> writes{};
            u32 count = 0;
            VkDescriptorSet set = allocSet(*s.srvSetLayout);
            if (set != VK_NULL_HANDLE) {
                for (u32 i = 0; i < pendingSRVs_.size() && i < kSrvBindingCount; ++i) {
                    auto* t = s.textures.Get(static_cast<u64>(pendingSRVs_[i].texture));
                    if (!t) continue;
                    infos[count] = vk::DescriptorImageInfo{
                        .imageView   = vk::ImageView(t->view),
                        .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
                    };
                    writes[count] = vk::WriteDescriptorSet{
                        .dstSet          = vk::DescriptorSet(set),
                        .dstBinding      = i,
                        .descriptorCount = 1,
                        .descriptorType  = vk::DescriptorType::eSampledImage,
                        .pImageInfo      = &infos[count],
                    };
                    ++count;
                }
                if (count > 0) {
                    s.device.updateDescriptorSets(
                        vk::ArrayProxy<const vk::WriteDescriptorSet>{ count, writes.data() },
                        nullptr);
                }
                setsToBind[0] = set;
            }
        }

        // ---- Set 2: Samplers ----
        {
            std::array<vk::DescriptorImageInfo, kSamplerBindingCount> infos{};
            std::array<vk::WriteDescriptorSet,  kSamplerBindingCount> writes{};
            u32 count = 0;
            VkDescriptorSet set = allocSet(*s.samplerSetLayout);
            if (set != VK_NULL_HANDLE) {
                for (u32 i = 0; i < pendingSamplers_.size() && i < kSamplerBindingCount; ++i) {
                    auto* sm = s.samplers.Get(static_cast<u64>(pendingSamplers_[i].sampler));
                    if (!sm) continue;
                    infos[count] = vk::DescriptorImageInfo{
                        .sampler = *sm->sampler,
                    };
                    writes[count] = vk::WriteDescriptorSet{
                        .dstSet          = vk::DescriptorSet(set),
                        .dstBinding      = i,
                        .descriptorCount = 1,
                        .descriptorType  = vk::DescriptorType::eSampler,
                        .pImageInfo      = &infos[count],
                    };
                    ++count;
                }
                if (count > 0) {
                    s.device.updateDescriptorSets(
                        vk::ArrayProxy<const vk::WriteDescriptorSet>{ count, writes.data() },
                        nullptr);
                }
                setsToBind[1] = set;
            }
        }

        // bindDescriptorSets binds [firstSet, firstSet+N) so we issue the
        // SRV + sampler pair starting at set 1. The CB set (0) was already
        // recorded via pushDescriptorSetKHR above. Sets allocated above
        // default to fully-valid even when no writes were issued
        // (descriptors are uninitialised but unused by the bound stages).
        if (setsToBind[0] || setsToBind[1]) {
            frame.commandBuffer.bindDescriptorSets(
                vk::PipelineBindPoint::eGraphics,
                *s.pipelineLayout,
                /*firstSet*/ kSrvSetIndex,
                vk::ArrayProxy<const vk::DescriptorSet>{
                    static_cast<u32>(setsToBind.size()),
                    reinterpret_cast<const vk::DescriptorSet*>(setsToBind.data()) },
                /*dynamicOffsets*/ nullptr);
        }

        srvSetDirty_     = false;
        samplerSetDirty_ = false;
    }
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
    // baseOffset is non-zero for shared CB-ring sub-allocs (otherwise 0),
    // so this works uniformly for both own-allocation and shared
    // buffers.
    vk::BufferCopy region{
        .srcOffset = sr->baseOffset,
        .dstOffset = d->baseOffset,
        .size      = std::min(d->desc.size, sr->desc.size),
    };
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

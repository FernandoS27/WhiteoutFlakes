// IGFXCommandList for Vulkan. First BeginRenderPass each frame waits
// on the prev-frame fence and resets the command buffer; subsequent
// passes keep recording into the same buffer until Present submits.

#include "vulkan_command_list.h"
#include "vulkan_device.h"
#include "vulkan_device_state.h"
#include "vulkan_handles.h"
#include "vulkan_translate.h"

#include <cstdio>
#include <cstring>

#if defined(TRACY_ENABLE)
#include <tracy/Tracy.hpp>
#include <tracy/TracyVulkan.hpp>
#endif

namespace whiteout::flakes::gfx::vulkan {

namespace {

void TransitionImageLayout(vk::raii::CommandBuffer& cb, VkImage image, vk::ImageAspectFlags aspect,
                           vk::ImageLayout oldLayout, vk::ImageLayout newLayout,
                           vk::PipelineStageFlags2 srcStage, vk::AccessFlags2 srcAccess,
                           vk::PipelineStageFlags2 dstStage, vk::AccessFlags2 dstAccess) {
    vk::ImageMemoryBarrier2 barrier{
        .srcStageMask = srcStage,
        .srcAccessMask = srcAccess,
        .dstStageMask = dstStage,
        .dstAccessMask = dstAccess,
        .oldLayout = oldLayout,
        .newLayout = newLayout,
        .image = vk::Image(image),
        // Whole-resource range keeps currentLayout valid for any view
        // (mip chains, cube arrays) the renderer might bind later.
        .subresourceRange =
            {
                aspect,
                0,
                VK_REMAINING_MIP_LEVELS,
                0,
                VK_REMAINING_ARRAY_LAYERS,
            },
    };
    vk::DependencyInfo dep{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &barrier,
    };
    cb.pipelineBarrier2(dep);
}

void EnsureRecording(VulkanDeviceState& state, FrameContext& frame) {
    if (frame.recording)
        return;
    {
#if defined(TRACY_ENABLE)
        ZoneScopedN("vkWaitForFences");
#endif
        (void)state.device.waitForFences(*frame.inFlightFence, vk::True, UINT64_MAX);
    }
    state.device.resetFences(*frame.inFlightFence);
    DrainPendingDeletes(state);
    DrainPendingTransferDeletes(state);
    frame.descriptorPool.reset(vk::DescriptorPoolResetFlags{});
    frame.commandBuffer.reset({});
    (void)frame.commandBuffer.begin({
        .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
    });
    frame.recording = true;

    // Batch every CreateTexture-queued transition into one barrier.
    // Must run before any BeginRenderPass — layout transitions are
    // illegal inside dynamic rendering.
    if (!state.pendingSrvTransitions.empty()) {
        std::vector<vk::ImageMemoryBarrier2> barriers;
        barriers.reserve(state.pendingSrvTransitions.size());
        for (u64 slot : state.pendingSrvTransitions) {
            auto* texture = state.textures.Get(slot);
            if (!texture || !texture->image)
                continue;
            if (texture->currentLayout == vk::ImageLayout::eShaderReadOnlyOptimal)
                continue;
            barriers.push_back(vk::ImageMemoryBarrier2{
                .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
                .oldLayout = texture->currentLayout,
                .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
                .image = vk::Image(texture->image),
                .subresourceRange =
                    {
                        texture->aspect,
                        0,
                        VK_REMAINING_MIP_LEVELS,
                        0,
                        VK_REMAINING_ARRAY_LAYERS,
                    },
            });
            texture->currentLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
        }
        if (!barriers.empty()) {
            frame.commandBuffer.pipelineBarrier2(vk::DependencyInfo{
                .imageMemoryBarrierCount = static_cast<u32>(barriers.size()),
                .pImageMemoryBarriers = barriers.data(),
            });
        }
        state.pendingSrvTransitions.clear();
    }
}

} // namespace

VulkanCommandList::VulkanCommandList(VulkanDevice& device) : device_(device) {}
VulkanCommandList::~VulkanCommandList() = default;

void VulkanCommandList::BeginRenderPass(TextureHandle color, TextureHandle depth,
                                        const f32 clearColor[4], f32 clearDepth, u8 clearStencil) {
    auto& state = device_.State();
    auto& frame = state.frames[state.frameIndex];

    // EnsureRecording resets the command buffer and descriptor pool,
    // so every set we bound last frame is gone from the GPU side.
    // Force-dirty so the first FlushDescriptors this frame rebuilds.
    const bool startingNewFrame = !frame.recording;
    EnsureRecording(state, frame);
    if (startingNewFrame) {
        cbSetDirty_ = true;
        srvSetDirty_ = true;
        samplerSetDirty_ = true;
    }

    auto* colorTex = state.textures.Get(static_cast<u64>(color));
    auto* depthTex = state.textures.Get(static_cast<u64>(depth));

    // Acquire-on-first-bind for swap-chain proxies.
    if (colorTex && colorTex->swapChainProxy != SwapChainHandle::Invalid) {
        if (auto* sc = state.swapchains.Get(static_cast<u64>(colorTex->swapChainProxy))) {
            AcquireSwapChainImageIfNeeded(state, *sc, frame);
        }
    }

    activeColorAttachment_ = colorTex ? color : TextureHandle::Invalid;
    activeColorFormat_ =
        colorTex ? static_cast<u32>(colorTex->format) : static_cast<u32>(vk::Format::eUndefined);

    // Batch color + depth attachment transitions into one barrier2.
    std::array<vk::ImageMemoryBarrier2, 2> attachBarriers{};
    u32 barrierCount = 0;

    vk::RenderingAttachmentInfo colorAttach{};
    if (colorTex) {
        attachBarriers[barrierCount++] = vk::ImageMemoryBarrier2{
            .srcStageMask = vk::PipelineStageFlagBits2::eTopOfPipe,
            .srcAccessMask = {},
            .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
            .oldLayout = colorTex->currentLayout,
            .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
            .image = vk::Image(colorTex->image),
            .subresourceRange =
                {
                    vk::ImageAspectFlagBits::eColor,
                    0,
                    VK_REMAINING_MIP_LEVELS,
                    0,
                    VK_REMAINING_ARRAY_LAYERS,
                },
        };
        colorTex->currentLayout = vk::ImageLayout::eColorAttachmentOptimal;

        vk::ClearColorValue cc{};
        std::memcpy(cc.float32.data(), clearColor, sizeof(f32) * 4);

        colorAttach = vk::RenderingAttachmentInfo{
            .imageView = vk::ImageView(colorTex->view),
            .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
            .loadOp = vk::AttachmentLoadOp::eClear,
            .storeOp = vk::AttachmentStoreOp::eStore,
            .clearValue = vk::ClearValue{.color = cc},
        };
    }

    vk::RenderingAttachmentInfo depthAttach{};
    if (depthTex) {
        attachBarriers[barrierCount++] = vk::ImageMemoryBarrier2{
            .srcStageMask = vk::PipelineStageFlagBits2::eTopOfPipe,
            .srcAccessMask = {},
            .dstStageMask = vk::PipelineStageFlagBits2::eEarlyFragmentTests |
                            vk::PipelineStageFlagBits2::eLateFragmentTests,
            .dstAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentRead |
                             vk::AccessFlagBits2::eDepthStencilAttachmentWrite,
            .oldLayout = depthTex->currentLayout,
            .newLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal,
            .image = vk::Image(depthTex->image),
            .subresourceRange =
                {
                    depthTex->aspect,
                    0,
                    VK_REMAINING_MIP_LEVELS,
                    0,
                    VK_REMAINING_ARRAY_LAYERS,
                },
        };
        depthTex->currentLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal;

        depthAttach = vk::RenderingAttachmentInfo{
            .imageView = vk::ImageView(depthTex->view),
            .imageLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal,
            .loadOp = vk::AttachmentLoadOp::eClear,
            .storeOp = vk::AttachmentStoreOp::eStore,
            .clearValue =
                vk::ClearValue{
                    .depthStencil = {clearDepth, clearStencil},
                },
        };
    }

    if (barrierCount > 0) {
        frame.commandBuffer.pipelineBarrier2({
            .imageMemoryBarrierCount = barrierCount,
            .pImageMemoryBarriers = attachBarriers.data(),
        });
    }

    const i32 width = colorTex ? colorTex->width : (depthTex ? depthTex->width : 0);
    const i32 height = colorTex ? colorTex->height : (depthTex ? depthTex->height : 0);

    vk::RenderingInfo info{
        .renderArea = {{0, 0}, {static_cast<u32>(width), static_cast<u32>(height)}},
        .layerCount = 1,
        .colorAttachmentCount = colorTex ? 1u : 0u,
        .pColorAttachments = colorTex ? &colorAttach : nullptr,
        .pDepthAttachment = depthTex ? &depthAttach : nullptr,
    };
    frame.commandBuffer.beginRendering(info);

    // Negative viewport height = Y-flip to match D3D conventions
    // (renderer projection matrices assume top-left origin).
    vk::Viewport vp{
        0.0f, static_cast<f32>(height), static_cast<f32>(width), -static_cast<f32>(height), 0.0f,
        1.0f};
    frame.commandBuffer.setViewport(0, vp);
    vk::Rect2D scissor{{0, 0}, {static_cast<u32>(width), static_cast<u32>(height)}};
    frame.commandBuffer.setScissor(0, scissor);

    activeDepthAttachment_ = depthTex ? depth : TextureHandle::Invalid;
    currentVpX_ = 0.0f;
    currentVpY_ = 0.0f;
    currentVpW_ = static_cast<f32>(width);
    currentVpH_ = static_cast<f32>(height);
}

void VulkanCommandList::EndRenderPass() {
    auto& state = device_.State();
    auto& frame = state.frames[state.frameIndex];
    if (!frame.recording)
        return;
    frame.commandBuffer.endRendering();

    // Eagerly transition the color attachment to ShaderReadOnly so a
    // following pass can sample it (HDR → tonemap). Swap-chain proxies
    // are consumed by Present, so we skip them.
    if (activeColorAttachment_ != TextureHandle::Invalid) {
        auto* texture = state.textures.Get(static_cast<u64>(activeColorAttachment_));
        if (texture && texture->image && texture->swapChainProxy == SwapChainHandle::Invalid &&
            texture->currentLayout != vk::ImageLayout::eShaderReadOnlyOptimal) {
            TransitionImageLayout(frame.commandBuffer, texture->image, texture->aspect,
                                  texture->currentLayout, vk::ImageLayout::eShaderReadOnlyOptimal,
                                  vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                  vk::AccessFlagBits2::eColorAttachmentWrite,
                                  vk::PipelineStageFlagBits2::eFragmentShader,
                                  vk::AccessFlagBits2::eShaderRead);
            texture->currentLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
        }
    }
    activeColorAttachment_ = TextureHandle::Invalid;
    activeDepthAttachment_ = TextureHandle::Invalid;
    activeColorFormat_ = static_cast<u32>(vk::Format::eUndefined);
    lastBoundPipeline_ = PipelineHandle::Invalid;
}

void VulkanCommandList::BeginGpuZone(const char* name) {
#if defined(TRACY_ENABLE)
    auto& state = device_.State();
    if (!state.tracyCtx || !name)
        return;
    auto& frame = state.frames[state.frameIndex];
    EnsureRecording(state, frame);
    const size_t nameLen = std::strlen(name);
    static const char kFile[] = __FILE__;
    static const char kFunc[] = "GpuZone";
    gpuZoneStack_.push_back(std::make_unique<tracy::VkCtxScope>(
        state.tracyCtx, static_cast<uint32_t>(__LINE__), kFile, sizeof(kFile) - 1, kFunc,
        sizeof(kFunc) - 1, name, nameLen, static_cast<VkCommandBuffer>(*frame.commandBuffer),
        /*is_active=*/true));
#else
    (void)name;
#endif
}

void VulkanCommandList::EndGpuZone() {
#if defined(TRACY_ENABLE)
    if (gpuZoneStack_.empty())
        return;
    gpuZoneStack_.pop_back(); // ~VkCtxScope emits the end timestamp
#endif
}

void VulkanCommandList::SetViewport(const Viewport& v) {
    auto& state = device_.State();
    auto& frame = state.frames[state.frameIndex];
    if (!frame.recording)
        return;
    // Flip Y: see comment in BeginRenderPass.
    vk::Viewport vp{v.x, v.y + v.height, v.width, -v.height, v.minDepth, v.maxDepth};
    frame.commandBuffer.setViewport(0, vp);
    currentVpX_ = v.x;
    currentVpY_ = v.y;
    currentVpW_ = v.width;
    currentVpH_ = v.height;
}

void VulkanCommandList::SetScissor(const Scissor& sc) {
    auto& state = device_.State();
    auto& frame = state.frames[state.frameIndex];
    if (!frame.recording)
        return;
    vk::Rect2D r{{sc.x, sc.y}, {static_cast<u32>(sc.width), static_cast<u32>(sc.height)}};
    frame.commandBuffer.setScissor(0, r);
}

// ---- Pipeline + vertex/index/constant binding -----------------------------

void VulkanCommandList::BindPipeline(PipelineHandle h) {
    auto& state = device_.State();
    auto& frame = state.frames[state.frameIndex];
    if (!frame.recording)
        return;
    auto* pipeline = state.pipelines.Get(static_cast<u64>(h));
    if (!pipeline)
        return;
    // Pre-Draw catch for VUID-vkCmdDraw-08910 so the format mismatch
    // is attributed to BindPipeline, not a validation-layer cmd dump.
    const vk::Format activeFmt = static_cast<vk::Format>(activeColorFormat_);
    if (!pipeline->isCompute && pipeline->colorFormat != vk::Format::eUndefined &&
        activeFmt != vk::Format::eUndefined && pipeline->colorFormat != activeFmt) {
        std::fprintf(stderr,
                     "[vk] BindPipeline format mismatch: pipeline=%s renderpass=%s "
                     "(handle=%llu)\n",
                     vk::to_string(pipeline->colorFormat).c_str(), vk::to_string(activeFmt).c_str(),
                     static_cast<unsigned long long>(h));
    }
    lastBoundPipeline_ = h;
    frame.commandBuffer.bindPipeline(pipeline->isCompute ? vk::PipelineBindPoint::eCompute
                                                         : vk::PipelineBindPoint::eGraphics,
                                     *pipeline->pipeline);
}

void VulkanCommandList::BindVertexBuffer(u32 slot, BufferHandle h, u32 /*stride*/, u32 offset) {
    auto& state = device_.State();
    auto& frame = state.frames[state.frameIndex];
    if (!frame.recording)
        return;
    auto* buffer = state.buffers.Get(static_cast<u64>(h));
    if (!buffer)
        return;
    const vk::Buffer buf = vk::Buffer(buffer->buffer);
    // Ring offset matters for CpuWritable buffers (ribbons, particles).
    const vk::DeviceSize off = offset + buffer->currentOffset();
    frame.commandBuffer.bindVertexBuffers(slot, buf, off);
}

void VulkanCommandList::BindIndexBuffer(BufferHandle h, Format format) {
    auto& state = device_.State();
    auto& frame = state.frames[state.frameIndex];
    if (!frame.recording)
        return;
    auto* buffer = state.buffers.Get(static_cast<u64>(h));
    if (!buffer)
        return;
    const vk::IndexType type =
        (format == Format::R16_UINT) ? vk::IndexType::eUint16 : vk::IndexType::eUint32;
    frame.commandBuffer.bindIndexBuffer(vk::Buffer(buffer->buffer), buffer->currentOffset(), type);
}

// PS slots shift by kStageBindingShift to match the slang convention.
static u32 BindingForStage(ShaderStage stage, u32 slot) {
    return slot + (stage == ShaderStage::Pixel ? kStageBindingShift : 0);
}

void VulkanCommandList::BindConstantBuffer(ShaderStage stage, u32 slot, BufferHandle handle) {
    const u32 binding = BindingForStage(stage, slot);
    if (binding >= pendingCBs_.size())
        return;
    // Capture the ring offset now; later MapBuffer rotations would
    // otherwise leave this draw reading the wrong slot.
    auto& state = device_.State();
    auto* entry = state.buffers.Get(static_cast<u64>(handle));
    const u64 off = entry ? entry->currentOffset() : 0;
    auto& cur = pendingCBs_[binding];
    if (cur.buffer == handle && cur.offset == off)
        return; // identical → no-op
    cur = {handle, off};
    cbSetDirty_ = true;
}

void VulkanCommandList::BindShaderResource(ShaderStage stage, u32 slot, TextureHandle texture) {
    const u32 binding = BindingForStage(stage, slot);
    if (binding >= pendingSRVs_.size())
        return;
    auto& cur = pendingSRVs_[binding];
    if (cur.texture == texture)
        return; // identical → no-op
    cur = {texture};
    srvSetDirty_ = true;
}

// Buffer SRVs not yet wired up.
void VulkanCommandList::BindShaderResource(ShaderStage, u32, BufferHandle) {}

// UAV slot 0 feeds the compute Dispatch's storage-buffer binding.
void VulkanCommandList::BindUnorderedAccess(u32 slot, BufferHandle h) {
    if (slot == 0)
        pendingComputeUav_ = h;
}

void VulkanCommandList::BindSampler(ShaderStage stage, u32 slot, SamplerHandle sampler) {
    const u32 binding = BindingForStage(stage, slot);
    if (binding >= pendingSamplers_.size())
        return;
    auto& cur = pendingSamplers_[binding];
    if (cur.sampler == sampler)
        return; // identical → no-op
    cur = {sampler};
    samplerSetDirty_ = true;
}

void VulkanCommandList::FlushDescriptors() {
    if (!cbSetDirty_ && !srvSetDirty_ && !samplerSetDirty_)
        return;

    auto& state = device_.State();
    auto& frame = state.frames[state.frameIndex];

    // CBs use push descriptors (rewritten every Bind by ring rotation).
    // SRVs + samplers are pool-allocated (Vulkan caps push sets to one
    // per pipeline layout, so the lower-churn sets get the pool path).

    // ---- Set 0: CBs (push descriptors, no allocation) ----
    if (cbSetDirty_) {
        std::array<vk::DescriptorBufferInfo, kCbBindingCount> infos{};
        std::array<vk::WriteDescriptorSet, kCbBindingCount> writes{};
        u32 count = 0;
        for (u32 i = 0; i < pendingCBs_.size() && i < kCbBindingCount; ++i) {
            auto* buffer = state.buffers.Get(static_cast<u64>(pendingCBs_[i].buffer));
            if (!buffer)
                continue;
            infos[count] = vk::DescriptorBufferInfo{
                .buffer = vk::Buffer(buffer->buffer),
                .offset = pendingCBs_[i].offset,
                .range = buffer->slotCount > 1 ? buffer->desc.size : VK_WHOLE_SIZE,
            };
            writes[count] = vk::WriteDescriptorSet{
                // dstSet is ignored by vkCmdPushDescriptorSetKHR.
                .dstBinding = i,
                .descriptorCount = 1,
                .descriptorType = vk::DescriptorType::eUniformBuffer,
                .pBufferInfo = &infos[count],
            };
            ++count;
        }
        if (count > 0) {
            frame.commandBuffer.pushDescriptorSetKHR(
                vk::PipelineBindPoint::eGraphics, *state.pipelineLayout, kCbSetIndex,
                vk::ArrayProxy<const vk::WriteDescriptorSet>{count, writes.data()});
        }
        cbSetDirty_ = false;
    }

    // SRV + sampler are bound as a contiguous pair. Raw
    // vkAllocateDescriptorSets bypasses raii's free-on-destroy, which
    // is illegal on our reset-only pool.
    if (srvSetDirty_ || samplerSetDirty_) {
        auto allocSet = [&](VkDescriptorSetLayout layout) -> VkDescriptorSet {
            VkDescriptorSetAllocateInfo ai{
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                .descriptorPool = *frame.descriptorPool,
                .descriptorSetCount = 1,
                .pSetLayouts = &layout,
            };
            VkDescriptorSet set = VK_NULL_HANDLE;
            if (vkAllocateDescriptorSets(*state.device, &ai, &set) != VK_SUCCESS) {
                return VK_NULL_HANDLE;
            }
            return set;
        };

        std::array<VkDescriptorSet, 2> setsToBind{VK_NULL_HANDLE, VK_NULL_HANDLE};

        // ---- Set 1: SRVs ----
        {
            std::array<vk::DescriptorImageInfo, kSrvBindingCount> infos{};
            std::array<vk::WriteDescriptorSet, kSrvBindingCount> writes{};
            u32 count = 0;
            VkDescriptorSet set = allocSet(*state.srvSetLayout);
            if (set != VK_NULL_HANDLE) {
                for (u32 i = 0; i < pendingSRVs_.size() && i < kSrvBindingCount; ++i) {
                    auto* texture = state.textures.Get(static_cast<u64>(pendingSRVs_[i].texture));
                    if (!texture)
                        continue;
                    infos[count] = vk::DescriptorImageInfo{
                        .imageView = vk::ImageView(texture->view),
                        .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
                    };
                    writes[count] = vk::WriteDescriptorSet{
                        .dstSet = vk::DescriptorSet(set),
                        .dstBinding = i,
                        .descriptorCount = 1,
                        .descriptorType = vk::DescriptorType::eSampledImage,
                        .pImageInfo = &infos[count],
                    };
                    ++count;
                }
                if (count > 0) {
                    state.device.updateDescriptorSets(
                        vk::ArrayProxy<const vk::WriteDescriptorSet>{count, writes.data()},
                        nullptr);
                }
                setsToBind[0] = set;
            }
        }

        // ---- Set 2: Samplers ----
        {
            std::array<vk::DescriptorImageInfo, kSamplerBindingCount> infos{};
            std::array<vk::WriteDescriptorSet, kSamplerBindingCount> writes{};
            u32 count = 0;
            VkDescriptorSet set = allocSet(*state.samplerSetLayout);
            if (set != VK_NULL_HANDLE) {
                for (u32 i = 0; i < pendingSamplers_.size() && i < kSamplerBindingCount; ++i) {
                    auto* sm = state.samplers.Get(static_cast<u64>(pendingSamplers_[i].sampler));
                    if (!sm)
                        continue;
                    infos[count] = vk::DescriptorImageInfo{
                        .sampler = *sm->sampler,
                    };
                    writes[count] = vk::WriteDescriptorSet{
                        .dstSet = vk::DescriptorSet(set),
                        .dstBinding = i,
                        .descriptorCount = 1,
                        .descriptorType = vk::DescriptorType::eSampler,
                        .pImageInfo = &infos[count],
                    };
                    ++count;
                }
                if (count > 0) {
                    state.device.updateDescriptorSets(
                        vk::ArrayProxy<const vk::WriteDescriptorSet>{count, writes.data()},
                        nullptr);
                }
                setsToBind[1] = set;
            }
        }

        if (setsToBind[0] || setsToBind[1]) {
            frame.commandBuffer.bindDescriptorSets(
                vk::PipelineBindPoint::eGraphics, *state.pipelineLayout,
                /*firstSet*/ kSrvSetIndex,
                vk::ArrayProxy<const vk::DescriptorSet>{
                    static_cast<u32>(setsToBind.size()),
                    reinterpret_cast<const vk::DescriptorSet*>(setsToBind.data())},
                /*dynamicOffsets*/ nullptr);
        }

        srvSetDirty_ = false;
        samplerSetDirty_ = false;
    }
}

void VulkanCommandList::ClearDepth(TextureHandle depth, f32 clearDepth, u8 clearStencil) {
    auto& state = device_.State();
    auto& frame = state.frames[state.frameIndex];
    auto* texture = state.textures.Get(static_cast<u64>(depth));
    if (!texture || !frame.recording)
        return;

    // Inside a render pass, vkCmdClearDepthStencilImage is illegal AND
    // would force the depth image into eTransferDstOptimal — wrong for
    // the subsequent attachment writes. Use vkCmdClearAttachments with
    // the current viewport as the clear rect (matches the d3d11/d3d12
    // ClearDepthStencilView semantics RenderViewCube relies on, which
    // scope the clear to the viewport-restricted region).
    const bool insideRenderPass = (activeDepthAttachment_ == depth);
    if (insideRenderPass) {
        // BeginRenderPass binds only pDepthAttachment (no pStencilAttachment),
        // so the clear aspect must be eDepth alone — passing eStencil when
        // there's no stencil attachment bound is a validation error and the
        // driver rejects the whole clear.
        vk::ClearAttachment attach{
            .aspectMask = vk::ImageAspectFlagBits::eDepth,
            .clearValue = vk::ClearValue{.depthStencil = {clearDepth, clearStencil}},
        };
        const i32 rx = static_cast<i32>(currentVpX_);
        const i32 ry = static_cast<i32>(currentVpY_);
        const u32 rw = static_cast<u32>(std::max(0.0f, currentVpW_));
        const u32 rh = static_cast<u32>(std::max(0.0f, currentVpH_));
        vk::ClearRect rect{
            .rect = {{rx, ry}, {rw, rh}},
            .baseArrayLayer = 0,
            .layerCount = 1,
        };
        frame.commandBuffer.clearAttachments(attach, rect);
        return;
    }

    // Outside a render pass — fall back to the image clear. Currently
    // unused, kept so a future caller that wants a global pre-pass clear
    // still has a working path.
    TransitionImageLayout(frame.commandBuffer, texture->image, texture->aspect,
                          texture->currentLayout, vk::ImageLayout::eTransferDstOptimal,
                          vk::PipelineStageFlagBits2::eTopOfPipe, {},
                          vk::PipelineStageFlagBits2::eClear, vk::AccessFlagBits2::eTransferWrite);
    vk::ClearDepthStencilValue v{clearDepth, clearStencil};
    vk::ImageSubresourceRange range{texture->aspect, 0, 1, 0, 1};
    frame.commandBuffer.clearDepthStencilImage(vk::Image(texture->image),
                                               vk::ImageLayout::eTransferDstOptimal, v, range);
    texture->currentLayout = vk::ImageLayout::eTransferDstOptimal;
}
void VulkanCommandList::CopyBuffer(BufferHandle dst, BufferHandle src) {
    auto& state = device_.State();
    auto& frame = state.frames[state.frameIndex];
    if (!frame.recording)
        return;
    auto* dstBuffer = state.buffers.Get(static_cast<u64>(dst));
    auto* srcBuffer = state.buffers.Get(static_cast<u64>(src));
    if (!dstBuffer || !srcBuffer)
        return;
    vk::BufferCopy region{
        .srcOffset = srcBuffer->baseOffset,
        .dstOffset = dstBuffer->baseOffset,
        .size = std::min(dstBuffer->desc.size, srcBuffer->desc.size),
    };
    frame.commandBuffer.copyBuffer(vk::Buffer(srcBuffer->buffer), vk::Buffer(dstBuffer->buffer),
                                   region);
}

void VulkanCommandList::Draw(u32 vertexCount, u32 firstVertex) {
    auto& state = device_.State();
    auto& frame = state.frames[state.frameIndex];
    if (!frame.recording)
        return;
    FlushDescriptors();
    frame.commandBuffer.draw(vertexCount, /*instances*/ 1, firstVertex, /*firstInstance*/ 0);
}

void VulkanCommandList::DrawIndexed(u32 indexCount, u32 firstIndex, i32 baseVertex) {
    auto& state = device_.State();
    auto& frame = state.frames[state.frameIndex];
    if (!frame.recording)
        return;
    FlushDescriptors();
    frame.commandBuffer.drawIndexed(indexCount, /*instances*/ 1, firstIndex, baseVertex,
                                    /*firstInstance*/ 0);
}

void VulkanCommandList::Dispatch(u32 gx, u32 gy, u32 gz) {
    auto& state = device_.State();
    auto& frame = state.frames[state.frameIndex];
    if (!frame.recording)
        return;

    // Compute uses a dedicated single-set layout (uniform buffer / sampled
    // image / storage buffer) — see CreatePipelineLayout. Bindings come from
    // pendingCBs_[0], pendingSRVs_[0] and pendingComputeUav_.
    auto* cbBuf = state.buffers.Get(static_cast<u64>(pendingCBs_[0].buffer));
    auto* srvTex = state.textures.Get(static_cast<u64>(pendingSRVs_[0].texture));
    auto* uavBuf = state.buffers.Get(static_cast<u64>(pendingComputeUav_));
    if (!cbBuf || !srvTex || !uavBuf || !srvTex->image) {
        std::fprintf(stderr, "[vk] Dispatch: compute bindings incomplete — skipped\n");
        return;
    }

    // The sampled source must be in shader-read layout for the compute read.
    if (srvTex->currentLayout != vk::ImageLayout::eShaderReadOnlyOptimal) {
        TransitionImageLayout(frame.commandBuffer, srvTex->image, srvTex->aspect,
                              srvTex->currentLayout, vk::ImageLayout::eShaderReadOnlyOptimal,
                              vk::PipelineStageFlagBits2::eAllCommands,
                              vk::AccessFlagBits2::eMemoryWrite,
                              vk::PipelineStageFlagBits2::eComputeShader,
                              vk::AccessFlagBits2::eShaderRead);
        srvTex->currentLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
    }

    VkDescriptorSetLayout rawLayout = *state.computeSetLayout;
    VkDescriptorSetAllocateInfo ai{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = *frame.descriptorPool,
        .descriptorSetCount = 1,
        .pSetLayouts = &rawLayout,
    };
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(*state.device, &ai, &set) != VK_SUCCESS) {
        std::fprintf(stderr, "[vk] Dispatch: compute descriptor alloc failed\n");
        return;
    }

    vk::DescriptorBufferInfo cbInfo{
        .buffer = vk::Buffer(cbBuf->buffer),
        .offset = pendingCBs_[0].offset,
        .range = cbBuf->slotCount > 1 ? cbBuf->desc.size : VK_WHOLE_SIZE,
    };
    vk::DescriptorImageInfo srvInfo{
        .imageView = vk::ImageView(srvTex->view),
        .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
    };
    vk::DescriptorBufferInfo uavInfo{
        .buffer = vk::Buffer(uavBuf->buffer),
        .offset = uavBuf->currentOffset(),
        .range = uavBuf->desc.size,
    };
    const std::array<vk::WriteDescriptorSet, 3> writes = {
        vk::WriteDescriptorSet{.dstSet = vk::DescriptorSet(set),
                               .dstBinding = 0,
                               .descriptorCount = 1,
                               .descriptorType = vk::DescriptorType::eUniformBuffer,
                               .pBufferInfo = &cbInfo},
        vk::WriteDescriptorSet{.dstSet = vk::DescriptorSet(set),
                               .dstBinding = 1,
                               .descriptorCount = 1,
                               .descriptorType = vk::DescriptorType::eSampledImage,
                               .pImageInfo = &srvInfo},
        vk::WriteDescriptorSet{.dstSet = vk::DescriptorSet(set),
                               .dstBinding = 2,
                               .descriptorCount = 1,
                               .descriptorType = vk::DescriptorType::eStorageBuffer,
                               .pBufferInfo = &uavInfo},
    };
    state.device.updateDescriptorSets(writes, nullptr);

    frame.commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *state.computeLayout,
                                           /*firstSet*/ 0, vk::DescriptorSet(set),
                                           /*dynamicOffsets*/ nullptr);
    frame.commandBuffer.dispatch(gx, gy, gz);

    // Make the storage-buffer writes visible to the CopyBuffer that follows.
    vk::MemoryBarrier2 barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
    };
    frame.commandBuffer.pipelineBarrier2(vk::DependencyInfo{
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &barrier,
    });

    // The graphics descriptor sets were not touched; force a rebuild before
    // the next draw so it doesn't read a compute-era set.
    cbSetDirty_ = srvSetDirty_ = samplerSetDirty_ = true;
}

} // namespace whiteout::flakes::gfx::vulkan

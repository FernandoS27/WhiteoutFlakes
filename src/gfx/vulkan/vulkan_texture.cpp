// Texture create/destroy + render targets + initial-data upload.

#include "vulkan_device.h"
#include "vulkan_device_state.h"
#include "vulkan_handles.h"
#include "vulkan_transfer.h"
#include "vulkan_translate.h"

#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>

namespace whiteout::flakes::gfx::vulkan {

namespace {

// Mip footprint matching D3D12::GetCopyableFootprints + DXTex slice
// ordering. Tightly packed — no row padding.
struct SubresLayout {
    u32 width;
    u32 height;
    u64 rowsInBlocks;
    u64 rowSizeBytes;
    u64 sliceSizeBytes;
};

SubresLayout ComputeSubresLayout(Format fmt, u32 width, u32 height) {
    SubresLayout L{};
    L.width = width;
    L.height = height;
    const u32 bytesPerBlock = FormatBytesPerBlock(fmt);
    if (IsBlockCompressed(fmt)) {
        const u32 blocksW = std::max(1u, (width + 3) / 4);
        const u32 blocksH = std::max(1u, (height + 3) / 4);
        L.rowSizeBytes = static_cast<u64>(blocksW) * bytesPerBlock;
        L.rowsInBlocks = blocksH;
    } else {
        L.rowSizeBytes = static_cast<u64>(width) * bytesPerBlock;
        L.rowsInBlocks = height;
    }
    L.sliceSizeBytes = L.rowSizeBytes * L.rowsInBlocks;
    return L;
}

// Stage initialPixels through the transfer queue: Undefined →
// TransferDst → copy → ShaderReadOnly. Graphics waits on the
// transfer timeline at submit time.
bool UploadTexturePixels(VulkanDeviceState& state, VkImage image, const TextureDesc& desc,
                         vk::ImageAspectFlags aspect, const void* initialPixels) {
    const u32 mipLevels = std::max(1u, static_cast<u32>(desc.mipLevels));
    const u32 layers = std::max(1u, static_cast<u32>(desc.arraySize));

    struct Subres {
        u64 stagingOffset;
        u32 width;
        u32 height;
        u64 sizeBytes;
    };
    std::vector<Subres> subs;
    subs.reserve(static_cast<usize>(mipLevels) * layers);
    u64 totalBytes = 0;
    for (u32 layer = 0; layer < layers; ++layer) {
        u32 w = desc.width;
        u32 h = desc.height;
        for (u32 mip = 0; mip < mipLevels; ++mip) {
            const auto L = ComputeSubresLayout(desc.format, w, h);
            subs.push_back({totalBytes, w, h, L.sliceSizeBytes});
            totalBytes += L.sliceSizeBytes;
            w = std::max(1u, w / 2);
            h = std::max(1u, h / 2);
        }
    }
    if (totalBytes == 0)
        return true;

    // ---- Staging buffer (host-visible + mapped) ----
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = totalBytes;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    aci.flags =
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VkBuffer stagingBuf = VK_NULL_HANDLE;
    VmaAllocation stagingAlloc = VK_NULL_HANDLE;
    VmaAllocationInfo stagingInfo{};
    if (vmaCreateBuffer(state.allocator, &bci, &aci, &stagingBuf, &stagingAlloc, &stagingInfo) !=
        VK_SUCCESS) {
        return false;
    }
    std::memcpy(stagingInfo.pMappedData, initialPixels, totalBytes);
    vmaFlushAllocation(state.allocator, stagingAlloc, 0, totalBytes);

    return SubmitTransferAndDeferStaging(
        state, stagingBuf, stagingAlloc, [&](vk::raii::CommandBuffer& cmdBuf) {
            vk::ImageMemoryBarrier2 toCopy{
                .srcStageMask = vk::PipelineStageFlagBits2::eTopOfPipe,
                .srcAccessMask = vk::AccessFlagBits2::eNone,
                .dstStageMask = vk::PipelineStageFlagBits2::eCopy,
                .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
                .oldLayout = vk::ImageLayout::eUndefined,
                .newLayout = vk::ImageLayout::eTransferDstOptimal,
                .image = vk::Image(image),
                .subresourceRange = {aspect, 0, mipLevels, 0, layers},
            };
            cmdBuf.pipelineBarrier2(
                {.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &toCopy});

            std::vector<vk::BufferImageCopy> regions;
            regions.reserve(subs.size());
            for (u32 layer = 0; layer < layers; ++layer) {
                for (u32 mip = 0; mip < mipLevels; ++mip) {
                    const auto& sub = subs[layer * mipLevels + mip];
                    regions.push_back(vk::BufferImageCopy{
                        .bufferOffset = sub.stagingOffset,
                        .imageSubresource = {aspect, mip, layer, 1},
                        .imageOffset = {0, 0, 0},
                        .imageExtent = {sub.width, sub.height, 1},
                    });
                }
            }
            cmdBuf.copyBufferToImage(vk::Buffer(stagingBuf), vk::Image(image),
                                     vk::ImageLayout::eTransferDstOptimal, regions);

            // dstStage=BottomOfPipe is the only legal "end of work"
            // mask on a transfer queue; shader visibility comes from
            // the timeline-semaphore wait on the graphics submit.
            vk::ImageMemoryBarrier2 toRead{
                .srcStageMask = vk::PipelineStageFlagBits2::eCopy,
                .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eBottomOfPipe,
                .dstAccessMask = {},
                .oldLayout = vk::ImageLayout::eTransferDstOptimal,
                .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
                .image = vk::Image(image),
                .subresourceRange = {aspect, 0, mipLevels, 0, layers},
            };
            cmdBuf.pipelineBarrier2(
                {.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &toRead});
        });
}

} // namespace

TextureHandle VulkanDevice::CreateTexture(const TextureDesc& desc, const void* initialPixels) {
    auto& state = *state_;
    const vk::Format fmt = ToVkFormat(desc.format);
    if (fmt == vk::Format::eUndefined)
        return TextureHandle::Invalid;

    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = static_cast<VkFormat>(fmt);
    ici.extent = {static_cast<u32>(desc.width), static_cast<u32>(desc.height), 1};
    ici.mipLevels = static_cast<u32>(desc.mipLevels);
    ici.arrayLayers = static_cast<u32>(desc.arraySize);
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = 0;
    if (hasFlag(desc.usage, TextureUsage::ShaderResource))
        ici.usage |= VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (hasFlag(desc.usage, TextureUsage::RenderTarget))
        ici.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (hasFlag(desc.usage, TextureUsage::DepthStencil))
        ici.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    // Sampled images that get uploaded on the transfer queue need
    // CONCURRENT sharing (QFOT barriers buy back nothing on modern
    // drivers). RTs stay EXCLUSIVE — they don't cross families.
    const bool sampledImg = hasFlag(desc.usage, TextureUsage::ShaderResource);
    const u32 sharedFamilies[2] = {state.queueFamily, state.transferQueueFamily};
    if (sampledImg && state.hasAsyncTransfer) {
        ici.sharingMode = VK_SHARING_MODE_CONCURRENT;
        ici.queueFamilyIndexCount = 2;
        ici.pQueueFamilyIndices = sharedFamilies;
    } else {
        ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }
    if (desc.isCube)
        ici.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;

    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;

    TextureEntry texture{};
    if (vmaCreateImage(state.allocator, &ici, &aci, &texture.image, &texture.allocation, nullptr) !=
        VK_SUCCESS) {
        return TextureHandle::Invalid;
    }

    const bool isDepth = hasFlag(desc.usage, TextureUsage::DepthStencil);
    const bool hasStencilAspect =
        (fmt == vk::Format::eD24UnormS8Uint || fmt == vk::Format::eD32SfloatS8Uint);
    texture.aspect =
        isDepth ? (vk::ImageAspectFlagBits::eDepth |
                   (hasStencilAspect ? vk::ImageAspectFlagBits::eStencil : vk::ImageAspectFlags{}))
                : vk::ImageAspectFlags(vk::ImageAspectFlagBits::eColor);

    // eCubeArray requires arraySize multiple of 6 (IBL probes are 12).
    vk::ImageViewType viewType = vk::ImageViewType::e2D;
    if (desc.isCube) {
        viewType = (desc.arraySize > 6) ? vk::ImageViewType::eCubeArray : vk::ImageViewType::eCube;
    }
    auto viewR = state.device.createImageView({
        .image = vk::Image(texture.image),
        .viewType = viewType,
        .format = fmt,
        .subresourceRange =
            {
                .aspectMask = texture.aspect,
                .baseMipLevel = 0,
                .levelCount = static_cast<u32>(desc.mipLevels),
                .baseArrayLayer = 0,
                .layerCount = static_cast<u32>(desc.arraySize),
            },
    });
    if (viewR.result != vk::Result::eSuccess) {
        vmaDestroyImage(state.allocator, texture.image, texture.allocation);
        return TextureHandle::Invalid;
    }
    texture.ownedView = std::move(viewR.value);
    texture.view = *texture.ownedView;

    texture.format = fmt;
    texture.currentLayout = vk::ImageLayout::eUndefined;
    texture.width = desc.width;
    texture.height = desc.height;
    texture.ownsImage = true;

    const bool sampled = hasFlag(desc.usage, TextureUsage::ShaderResource);
    const bool hasInitData = sampled && initialPixels != nullptr;
    const VkImage rawImage = texture.image;

    // Upload before SlotMap insert so a failure orphans the local entry.
    if (hasInitData) {
        if (!UploadTexturePixels(state, rawImage, desc, texture.aspect, initialPixels)) {
            vmaDestroyImage(state.allocator, texture.image, texture.allocation);
            return TextureHandle::Invalid;
        }
        texture.currentLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
    }

    const u64 slot = state.textures.Insert(std::move(texture));
    // Sampled textures without initial data get transitioned by
    // EnsureRecording at the next frame start.
    if (sampled && !hasInitData) {
        state.pendingSrvTransitions.push_back(slot);
    }
    return static_cast<TextureHandle>(slot);
}

namespace {

TextureHandle CreateRenderTargetImage(VulkanDeviceState& state, i32 w, i32 h, vk::Format fmt,
                                      VkImageUsageFlags usage, vk::ImageAspectFlags aspect) {
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = static_cast<VkFormat>(fmt);
    ici.extent = {static_cast<u32>(w), static_cast<u32>(h), 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = usage;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;

    TextureEntry texture{};
    if (vmaCreateImage(state.allocator, &ici, &aci, &texture.image, &texture.allocation, nullptr) !=
        VK_SUCCESS) {
        return TextureHandle::Invalid;
    }
    texture.aspect = aspect;

    auto viewR = state.device.createImageView({
        .image = vk::Image(texture.image),
        .viewType = vk::ImageViewType::e2D,
        .format = fmt,
        .subresourceRange = {aspect, 0, 1, 0, 1},
    });
    if (viewR.result != vk::Result::eSuccess) {
        vmaDestroyImage(state.allocator, texture.image, texture.allocation);
        return TextureHandle::Invalid;
    }
    texture.ownedView = std::move(viewR.value);
    texture.view = *texture.ownedView;
    texture.format = fmt;
    texture.currentLayout = vk::ImageLayout::eUndefined;
    texture.width = w;
    texture.height = h;
    texture.ownsImage = true;
    return static_cast<TextureHandle>(state.textures.Insert(std::move(texture)));
}

} // namespace

TextureHandle VulkanDevice::CreateColorTarget(i32 w, i32 h, Format f) {
    const vk::Format fmt = ToVkFormat(f);
    if (fmt == vk::Format::eUndefined)
        return TextureHandle::Invalid;
    return CreateRenderTargetImage(*state_, w, h, fmt,
                                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                       VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                   vk::ImageAspectFlagBits::eColor);
}

TextureHandle VulkanDevice::CreateDepthTarget(i32 w, i32 h, Format f) {
    const vk::Format fmt = ToVkFormat(f);
    if (fmt == vk::Format::eUndefined)
        return TextureHandle::Invalid;
    const bool hasStencil =
        (fmt == vk::Format::eD24UnormS8Uint || fmt == vk::Format::eD32SfloatS8Uint);
    const auto aspect = vk::ImageAspectFlagBits::eDepth |
                        (hasStencil ? vk::ImageAspectFlags(vk::ImageAspectFlagBits::eStencil)
                                    : vk::ImageAspectFlags{});
    return CreateRenderTargetImage(
        *state_, w, h, fmt,
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, aspect);
}

void VulkanDevice::Destroy(TextureHandle h) {
    auto& state = *state_;
    auto* texture = state.textures.Get(static_cast<u64>(h));
    if (!texture)
        return;
    TextureEntry moved = std::move(*texture);
    state.textures.Remove(static_cast<u64>(h));
    state.pendingDeletes.push_back(MakePendingDelete(
        state.nextSubmitValue, [owned = std::move(moved)](VulkanDeviceState& st) mutable {
            // ownedView auto-destroys. Proxies (!ownsImage) borrow
            // swap-chain images and skip vmaDestroyImage.
            if (owned.ownsImage && owned.image && owned.allocation)
                vmaDestroyImage(st.allocator, owned.image, owned.allocation);
        }));
}

} // namespace whiteout::flakes::gfx::vulkan

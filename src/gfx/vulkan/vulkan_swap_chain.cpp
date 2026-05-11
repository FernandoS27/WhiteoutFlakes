// Vulkan swap chain — surface + VkSwapchainKHR + sRGB/linear view pair.
// Uses vk::raii::* for everything we own; swap-chain images themselves
// are owned by the swapchain (no destroy).

#include "vulkan_device.h"
#include "vulkan_resources.h"
#include "vulkan_translate.h"

#include <algorithm>
#include <cstdio>
#include <vector>

namespace whiteout::flakes::gfx::vulkan {

namespace {

vk::raii::ImageView MakeBackbufferView(const vk::raii::Device& device,
                                        VkImage image, vk::Format fmt) {
    auto r = device.createImageView({
        .image    = vk::Image(image),
        .viewType = vk::ImageViewType::e2D,
        .format   = fmt,
        .subresourceRange = {
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .levelCount = 1,
            .layerCount = 1,
        },
    });
    if (r.result != vk::Result::eSuccess) return nullptr;
    return std::move(r.value);
}

bool CreateSwapchainObjects(VulkanDeviceState& s, SwapChainEntry& sc,
                            i32 width, i32 height, Format colorFormat) {
    auto capsR = s.physicalDevice.getSurfaceCapabilitiesKHR(*sc.surface);
    if (capsR.result != vk::Result::eSuccess) return false;
    const auto& caps = capsR.value;

    sc.extent = caps.currentExtent;
    if (sc.extent.width == 0xFFFFFFFFu) {
        sc.extent.width  = std::clamp<u32>(static_cast<u32>(width),
                                           caps.minImageExtent.width,
                                           caps.maxImageExtent.width);
        sc.extent.height = std::clamp<u32>(static_cast<u32>(height),
                                           caps.minImageExtent.height,
                                           caps.maxImageExtent.height);
    }

    auto fmtsR = s.physicalDevice.getSurfaceFormatsKHR(*sc.surface);
    if (fmtsR.result != vk::Result::eSuccess || fmtsR.value.empty()) return false;
    const vk::Format preferredSrgb = ToVkFormat(colorFormat);
    vk::SurfaceFormatKHR chosen = fmtsR.value[0];
    for (const auto& f : fmtsR.value) {
        if (f.format == preferredSrgb &&
            f.colorSpace == vk::ColorSpaceKHR::eSrgbNonlinear) {
            chosen = f;
            break;
        }
    }
    sc.formatSrgb   = chosen.format;
    sc.formatLinear = LinearPartnerOf(chosen.format);

    // MUTABLE_FORMAT_BIT + image-format-list lets us create both sRGB and
    // linear views on the same VkImage.
    const std::array<vk::Format, 2> formatList = { sc.formatSrgb, sc.formatLinear };
    vk::ImageFormatListCreateInfo formatListInfo{
        .viewFormatCount = static_cast<u32>(formatList.size()),
        .pViewFormats    = formatList.data(),
    };

    u32 minImageCount = std::max<u32>(caps.minImageCount, kFramesInFlight);
    if (caps.maxImageCount > 0)
        minImageCount = std::min(minImageCount, caps.maxImageCount);

    vk::SwapchainCreateInfoKHR ci{
        .pNext            = &formatListInfo,
        .flags            = vk::SwapchainCreateFlagBitsKHR::eMutableFormat,
        .surface          = *sc.surface,
        .minImageCount    = minImageCount,
        .imageFormat      = sc.formatSrgb,
        .imageColorSpace  = chosen.colorSpace,
        .imageExtent      = sc.extent,
        .imageArrayLayers = 1,
        .imageUsage       = vk::ImageUsageFlagBits::eColorAttachment
                          | vk::ImageUsageFlagBits::eTransferDst,
        .imageSharingMode = vk::SharingMode::eExclusive,
        .preTransform     = caps.currentTransform,
        .compositeAlpha   = vk::CompositeAlphaFlagBitsKHR::eOpaque,
        .presentMode      = vk::PresentModeKHR::eFifo,
        .clipped          = vk::True,
    };

    auto scR = s.device.createSwapchainKHR(ci);
    if (scR.result != vk::Result::eSuccess) {
        std::fprintf(stderr, "[vk] createSwapchainKHR failed (%s)\n",
                     vk::to_string(scR.result).c_str());
        return false;
    }
    sc.swapchain = std::move(scR.value);

    auto imagesR = sc.swapchain.getImages();
    if (imagesR.result != vk::Result::eSuccess) return false;
    sc.images = std::move(imagesR.value);

    sc.viewsSrgb.clear();
    sc.viewsLinear.clear();
    sc.viewsSrgb.reserve(sc.images.size());
    sc.viewsLinear.reserve(sc.images.size());
    for (const auto& image : sc.images) {
        sc.viewsSrgb.push_back(MakeBackbufferView(s.device, image, sc.formatSrgb));
        sc.viewsLinear.push_back(MakeBackbufferView(s.device, image, sc.formatLinear));
    }

    // Allocate one acquire semaphore per image plus one spare. The
    // acquire dance in AcquireSwapChainImageIfNeeded swaps the spare
    // with the per-image slot on every call, so a semaphore is never
    // re-used while the prior present using it is still in flight.
    // The render-done semaphores follow the same per-image pinning —
    // signaled by submit, waited on by present, freed implicitly by
    // the next acquire of the same image.
    sc.imageAcquireSems.clear();
    sc.imageRenderDoneSems.clear();
    sc.imageAcquireSems.reserve(sc.images.size());
    sc.imageRenderDoneSems.reserve(sc.images.size());
    for (usize i = 0; i < sc.images.size(); ++i) {
        auto r1 = s.device.createSemaphore({});
        auto r2 = s.device.createSemaphore({});
        if (r1.result != vk::Result::eSuccess ||
            r2.result != vk::Result::eSuccess) return false;
        sc.imageAcquireSems.push_back(std::move(r1.value));
        sc.imageRenderDoneSems.push_back(std::move(r2.value));
    }
    {
        auto r = s.device.createSemaphore({});
        if (r.result != vk::Result::eSuccess) return false;
        sc.spareAcquireSem = std::move(r.value);
    }
    return true;
}

}  // namespace

// ---- IGFXDevice swap-chain methods --------------------------------------

SwapChainHandle VulkanDevice::CreateSwapChain(void* nativeWindowHandle,
                                              i32 width, i32 height,
                                              Format colorFormat) {
    auto& s = *state_;

    SwapChainEntry sc{};
    auto surfR = s.instance.createWin32SurfaceKHR({
        .hinstance = GetModuleHandleW(nullptr),
        .hwnd      = reinterpret_cast<HWND>(nativeWindowHandle),
    });
    if (surfR.result != vk::Result::eSuccess) {
        std::fprintf(stderr, "[vk] createWin32SurfaceKHR failed (%s)\n",
                     vk::to_string(surfR.result).c_str());
        return SwapChainHandle::Invalid;
    }
    sc.surface = std::move(surfR.value);

    auto presentR = s.physicalDevice.getSurfaceSupportKHR(s.queueFamily, *sc.surface);
    if (presentR.result != vk::Result::eSuccess || !presentR.value) {
        std::fprintf(stderr,
                     "[vk] queue family %u doesn't support presenting on this surface\n",
                     s.queueFamily);
        return SwapChainHandle::Invalid;
    }

    if (!CreateSwapchainObjects(s, sc, width, height, colorFormat)) {
        return SwapChainHandle::Invalid;
    }

    // Allocate the two swap-chain proxy textures. Their image / view
    // fields get repointed every frame in BeginRenderPass before the
    // attachments are bound.
    TextureEntry srgbProxy{};
    srgbProxy.format        = sc.formatSrgb;
    srgbProxy.aspect        = vk::ImageAspectFlagBits::eColor;
    srgbProxy.width         = static_cast<i32>(sc.extent.width);
    srgbProxy.height        = static_cast<i32>(sc.extent.height);
    srgbProxy.ownsImage     = false;
    srgbProxy.isLinearView  = false;
    srgbProxy.image         = sc.images[0];
    srgbProxy.view          = *sc.viewsSrgb[0];

    TextureEntry linearProxy{};
    linearProxy.format        = sc.formatLinear;
    linearProxy.aspect        = vk::ImageAspectFlagBits::eColor;
    linearProxy.width         = srgbProxy.width;
    linearProxy.height        = srgbProxy.height;
    linearProxy.ownsImage     = false;
    linearProxy.isLinearView  = true;
    linearProxy.image         = sc.images[0];
    linearProxy.view          = *sc.viewsLinear[0];

    const u64 proxySrgbRaw   = s.textures.Insert(std::move(srgbProxy));
    const u64 proxyLinearRaw = s.textures.Insert(std::move(linearProxy));
    sc.proxySrgb   = static_cast<TextureHandle>(proxySrgbRaw);
    sc.proxyLinear = static_cast<TextureHandle>(proxyLinearRaw);

    const u64 raw = s.swapchains.Insert(std::move(sc));
    const SwapChainHandle handle = static_cast<SwapChainHandle>(raw);

    // Patch back-pointers on the proxies now that the SwapChainHandle
    // is allocated.
    if (auto* p = s.textures.Get(proxySrgbRaw))   p->swapChainProxy = handle;
    if (auto* p = s.textures.Get(proxyLinearRaw)) p->swapChainProxy = handle;
    return handle;
}

void VulkanDevice::ResizeSwapChain(SwapChainHandle handle, i32 width, i32 height) {
    auto& s = *state_;
    auto* sc = s.swapchains.Get(static_cast<u64>(handle));
    if (!sc) return;
    s.device.waitIdle();
    sc->viewsSrgb.clear();
    sc->viewsLinear.clear();
    sc->images.clear();
    sc->swapchain = nullptr;  // raii destroy
    if (!CreateSwapchainObjects(s, *sc, width, height,
                                 sc->formatSrgb == vk::Format::eR8G8B8A8Srgb
                                     ? Format::R8G8B8A8_UNORM_SRGB
                                     : Format::B8G8R8A8_UNORM)) {
        return;
    }
    if (auto* p = s.textures.Get(static_cast<u64>(sc->proxySrgb))) {
        p->image  = sc->images[0];
        p->view   = *sc->viewsSrgb[0];
        p->width  = static_cast<i32>(sc->extent.width);
        p->height = static_cast<i32>(sc->extent.height);
    }
    if (auto* p = s.textures.Get(static_cast<u64>(sc->proxyLinear))) {
        p->image  = sc->images[0];
        p->view   = *sc->viewsLinear[0];
        p->width  = static_cast<i32>(sc->extent.width);
        p->height = static_cast<i32>(sc->extent.height);
    }
}

void VulkanDevice::DestroySwapChain(SwapChainHandle handle) {
    auto& s = *state_;
    auto* sc = s.swapchains.Get(static_cast<u64>(handle));
    if (!sc) return;
    s.device.waitIdle();
    s.textures.Remove(static_cast<u64>(sc->proxySrgb));
    s.textures.Remove(static_cast<u64>(sc->proxyLinear));
    s.swapchains.Remove(static_cast<u64>(handle));  // raii teardown
}

TextureHandle VulkanDevice::GetSwapChainBackBuffer(SwapChainHandle handle) {
    auto* sc = state_->swapchains.Get(static_cast<u64>(handle));
    return sc ? sc->proxySrgb : TextureHandle::Invalid;
}

TextureHandle VulkanDevice::GetSwapChainBackBufferLinear(SwapChainHandle handle) {
    auto* sc = state_->swapchains.Get(static_cast<u64>(handle));
    return sc ? sc->proxyLinear : TextureHandle::Invalid;
}

u32 AcquireSwapChainImageIfNeeded(VulkanDeviceState& s, SwapChainEntry& sc,
                                   FrameContext& frame) {
    if (sc.acquiredThisFrame) return sc.imageIndex;

    // Per-image acquire-semaphore swap dance:
    //  1. Use the spare to acquire — it's known free (either freshly
    //     created, or just finished its prior use one cycle ago).
    //  2. After acquire, swap the spare with the per-image slot so the
    //     newly-signaled semaphore is owned by that image. The slot's
    //     previous semaphore (released N acquires ago for that image)
    //     becomes the new spare.
    // The frame's submit then waits on imageAcquireSems[idx], which is
    // guaranteed to be the freshly-signaled one.
    auto r = sc.swapchain.acquireNextImage(UINT64_MAX,
                                           *sc.spareAcquireSem,
                                           VK_NULL_HANDLE);
    if (r.result != vk::Result::eSuccess && r.result != vk::Result::eSuboptimalKHR) {
        std::fprintf(stderr, "[vk] acquireNextImage failed (%s)\n",
                     vk::to_string(r.result).c_str());
        return UINT32_MAX;
    }
    const u32 idx = r.value;
    std::swap(sc.spareAcquireSem, sc.imageAcquireSems[idx]);

    sc.imageIndex        = idx;
    sc.acquiredThisFrame = true;
    frame.acquireWaitSem = *sc.imageAcquireSems[idx];
    frame.renderDoneSem  = *sc.imageRenderDoneSems[idx];

    if (auto* p = s.textures.Get(static_cast<u64>(sc.proxySrgb))) {
        p->image         = sc.images[idx];
        p->view          = *sc.viewsSrgb[idx];
        p->currentLayout = vk::ImageLayout::eUndefined;
    }
    if (auto* p = s.textures.Get(static_cast<u64>(sc.proxyLinear))) {
        p->image         = sc.images[idx];
        p->view          = *sc.viewsLinear[idx];
        p->currentLayout = vk::ImageLayout::eUndefined;
    }
    return idx;
}

void VulkanDevice::Present(SwapChainHandle handle) {
    auto& s = *state_;
    auto* sc = s.swapchains.Get(static_cast<u64>(handle));
    if (!sc || !sc->acquiredThisFrame) return;

    auto& frame = s.frames[s.frameIndex];

    if (frame.recording) {
        // Transition the just-rendered image to PRESENT_SRC.
        vk::ImageMemoryBarrier2 barrier{
            .srcStageMask  = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
            .dstStageMask  = vk::PipelineStageFlagBits2::eBottomOfPipe,
            .dstAccessMask = {},
            .oldLayout     = vk::ImageLayout::eColorAttachmentOptimal,
            .newLayout     = vk::ImageLayout::ePresentSrcKHR,
            .image         = sc->images[sc->imageIndex],
            .subresourceRange = {
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .levelCount = 1, .layerCount = 1,
            },
        };
        vk::DependencyInfo dep{ .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier };
        frame.commandBuffer.pipelineBarrier2(dep);
        (void)frame.commandBuffer.end();
        frame.recording = false;

        // Build the wait list. Always wait on the swap-chain acquire
        // semaphore. When `hasAsyncTransfer` is true and at least one
        // upload has happened, also wait on the transfer timeline at
        // its latest signaled value so any draw in this submit sees
        // the uploaded data. The wait is essentially free if the
        // timeline has already reached that value (cheap timeline-wait
        // semantics).
        std::array<vk::SemaphoreSubmitInfo, 2> waitInfos{};
        waitInfos[0] = vk::SemaphoreSubmitInfo{
            .semaphore = vk::Semaphore(frame.acquireWaitSem),
            .stageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        };
        u32 waitCount = 1;
        if (s.hasAsyncTransfer && s.transferLastSignaled > 0) {
            waitInfos[1] = vk::SemaphoreSubmitInfo{
                .semaphore = *s.transferTimelineSem,
                .value     = s.transferLastSignaled,
                // Block any draw or copy that consumes uploaded data.
                // Vertex/index/uniform fetches happen pre-rasterization;
                // sampled images can be read at any shader stage.
                .stageMask = vk::PipelineStageFlagBits2::eVertexInput
                           | vk::PipelineStageFlagBits2::eAllGraphics,
            };
            waitCount = 2;
        }
        // Two signals: the per-image binary render-done semaphore for
        // present, and the timeline semaphore at this submit's value
        // for deferred-delete tracking.
        std::array<vk::SemaphoreSubmitInfo, 2> signalInfos{
            vk::SemaphoreSubmitInfo{
                .semaphore = vk::Semaphore(frame.renderDoneSem),
                .stageMask = vk::PipelineStageFlagBits2::eAllCommands,
            },
            vk::SemaphoreSubmitInfo{
                .semaphore = *s.timelineSem,
                .value     = s.nextSubmitValue,
                .stageMask = vk::PipelineStageFlagBits2::eAllCommands,
            },
        };
        vk::CommandBufferSubmitInfo cbInfo{ .commandBuffer = *frame.commandBuffer };
        vk::SubmitInfo2 submit{
            .waitSemaphoreInfoCount   = waitCount, .pWaitSemaphoreInfos      = waitInfos.data(),
            .commandBufferInfoCount   = 1,         .pCommandBufferInfos      = &cbInfo,
            .signalSemaphoreInfoCount = static_cast<u32>(signalInfos.size()),
            .pSignalSemaphoreInfos    = signalInfos.data(),
        };
        (void)s.queue.submit2(submit, *frame.inFlightFence);
        s.nextSubmitValue++;
    }

    const VkSemaphore waitSem = frame.renderDoneSem;
    const VkSwapchainKHR swap = *sc->swapchain;
    const u32 idx = sc->imageIndex;
    vk::PresentInfoKHR pi{
        .waitSemaphoreCount = 1,
        .pWaitSemaphores    = reinterpret_cast<const vk::Semaphore*>(&waitSem),
        .swapchainCount     = 1,
        .pSwapchains        = reinterpret_cast<const vk::SwapchainKHR*>(&swap),
        .pImageIndices      = &idx,
    };
    (void)s.queue.presentKHR(pi);

    sc->acquiredThisFrame = false;
    s.frameIndex          = (s.frameIndex + 1) % kFramesInFlight;
}

}  // namespace whiteout::flakes::gfx::vulkan

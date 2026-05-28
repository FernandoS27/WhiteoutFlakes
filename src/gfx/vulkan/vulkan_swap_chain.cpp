// Surface + VkSwapchainKHR + paired sRGB/linear views.

#include "vulkan_device.h"
#include "vulkan_device_state.h"
#include "vulkan_handles.h"
#include "vulkan_translate.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

#if defined(TRACY_ENABLE)
#include <tracy/Tracy.hpp>       // FrameMark
#include <tracy/TracyVulkan.hpp> // TracyVkCollect
#endif

namespace whiteout::flakes::gfx::vulkan {

namespace {

vk::raii::ImageView MakeBackbufferView(const vk::raii::Device& device, VkImage image,
                                       vk::Format fmt) {
    auto r = device.createImageView({
        .image = vk::Image(image),
        .viewType = vk::ImageViewType::e2D,
        .format = fmt,
        .subresourceRange =
            {
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .levelCount = 1,
                .layerCount = 1,
            },
    });
    if (r.result != vk::Result::eSuccess)
        return nullptr;
    return std::move(r.value);
}

bool CreateSwapchainObjects(VulkanDeviceState& state, SwapChainEntry& sc, i32 width, i32 height,
                            Format colorFormat) {
    auto capsR = state.physicalDevice.getSurfaceCapabilitiesKHR(*sc.surface);
    if (capsR.result != vk::Result::eSuccess)
        return false;
    const auto& caps = capsR.value;

    sc.extent = caps.currentExtent;
    if (sc.extent.width == 0xFFFFFFFFu) {
        sc.extent.width = std::clamp<u32>(static_cast<u32>(width), caps.minImageExtent.width,
                                          caps.maxImageExtent.width);
        sc.extent.height = std::clamp<u32>(static_cast<u32>(height), caps.minImageExtent.height,
                                           caps.maxImageExtent.height);
    }

    auto fmtsR = state.physicalDevice.getSurfaceFormatsKHR(*sc.surface);
    if (fmtsR.result != vk::Result::eSuccess || fmtsR.value.empty())
        return false;
    // Prefer the requested sRGB format. If the surface doesn't expose it
    // (Metal-backed Vulkan on macOS only advertises BGRA8 / RGB10A2 family),
    // fall back to *any* sRGB-nonlinear variant before accepting whatever
    // the driver hands back at index 0. Picking the linear BGRA8 that
    // MoltenVK lists first would skip the hardware sRGB encode on store,
    // and the SD pipeline (which writes linear-light values directly to
    // the swap chain backbuffer) would then display ~2.2x too dark — the
    // tonemap-using HD path is less visibly affected because its output
    // is already in display-encoded space.
    const vk::Format preferredSrgb = ToVkFormat(colorFormat);
    vk::SurfaceFormatKHR chosen{};
    bool found = false;
    auto pick = [&](vk::Format want) {
        if (found)
            return;
        for (const auto& f : fmtsR.value) {
            if (f.format == want && f.colorSpace == vk::ColorSpaceKHR::eSrgbNonlinear) {
                chosen = f;
                found = true;
                return;
            }
        }
    };
    pick(preferredSrgb);
    pick(vk::Format::eB8G8R8A8Srgb);
    pick(vk::Format::eR8G8B8A8Srgb);
    if (!found)
        chosen = fmtsR.value[0];
    sc.formatSrgb = chosen.format;
    sc.formatLinear = LinearPartnerOf(chosen.format);

    // MUTABLE_FORMAT + format-list lets us create both sRGB and linear
    // views on the same VkImage.
    const std::array<vk::Format, 2> formatList = {sc.formatSrgb, sc.formatLinear};
    vk::ImageFormatListCreateInfo formatListInfo{
        .viewFormatCount = static_cast<u32>(formatList.size()),
        .pViewFormats = formatList.data(),
    };

    u32 minImageCount = std::max<u32>(caps.minImageCount, kFramesInFlight);
    if (caps.maxImageCount > 0)
        minImageCount = std::min(minImageCount, caps.maxImageCount);

    vk::SwapchainCreateInfoKHR ci{
        .pNext = &formatListInfo,
        .flags = vk::SwapchainCreateFlagBitsKHR::eMutableFormat,
        .surface = *sc.surface,
        .minImageCount = minImageCount,
        .imageFormat = sc.formatSrgb,
        .imageColorSpace = chosen.colorSpace,
        .imageExtent = sc.extent,
        .imageArrayLayers = 1,
        .imageUsage =
            vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferDst,
        .imageSharingMode = vk::SharingMode::eExclusive,
        .preTransform = caps.currentTransform,
        .compositeAlpha = vk::CompositeAlphaFlagBitsKHR::eOpaque,
        .presentMode = vk::PresentModeKHR::eFifo,
        .clipped = vk::True,
    };

    auto scR = state.device.createSwapchainKHR(ci);
    if (scR.result != vk::Result::eSuccess) {
        std::fprintf(stderr, "[vk] createSwapchainKHR failed (%s)\n",
                     vk::to_string(scR.result).c_str());
        return false;
    }
    sc.swapchain = std::move(scR.value);

    auto imagesR = sc.swapchain.getImages();
    if (imagesR.result != vk::Result::eSuccess)
        return false;
    sc.images = std::move(imagesR.value);

    sc.viewsSrgb.clear();
    sc.viewsLinear.clear();
    sc.viewsSrgb.reserve(sc.images.size());
    sc.viewsLinear.reserve(sc.images.size());
    for (const auto& image : sc.images) {
        sc.viewsSrgb.push_back(MakeBackbufferView(state.device, image, sc.formatSrgb));
        sc.viewsLinear.push_back(MakeBackbufferView(state.device, image, sc.formatLinear));
    }

    // One acquire + render-done semaphore per image, plus one spare
    // that the swap-with-spare dance in AcquireSwapChainImageIfNeeded
    // uses to avoid re-using a semaphore mid-present.
    sc.imageAcquireSems.clear();
    sc.imageRenderDoneSems.clear();
    sc.imageAcquireSems.reserve(sc.images.size());
    sc.imageRenderDoneSems.reserve(sc.images.size());
    for (usize i = 0; i < sc.images.size(); ++i) {
        auto r1 = state.device.createSemaphore({});
        auto r2 = state.device.createSemaphore({});
        if (r1.result != vk::Result::eSuccess || r2.result != vk::Result::eSuccess)
            return false;
        sc.imageAcquireSems.push_back(std::move(r1.value));
        sc.imageRenderDoneSems.push_back(std::move(r2.value));
    }
    {
        auto r = state.device.createSemaphore({});
        if (r.result != vk::Result::eSuccess)
            return false;
        sc.spareAcquireSem = std::move(r.value);
    }
    return true;
}

} // namespace

// ---- IGFXDevice swap-chain methods --------------------------------------

SwapChainHandle VulkanDevice::CreateSwapChain(void* nativeWindowHandle, i32 width, i32 height,
                                              Format colorFormat) {
    auto& state = *state_;

    SwapChainEntry sc{};
#if defined(VK_USE_PLATFORM_WIN32_KHR)
    // Windows path: nativeWindowHandle is an HWND; we create the Win32
    // surface internally so the host doesn't have to link against vulkan.
    auto surfR = state.instance.createWin32SurfaceKHR({
        .hinstance = GetModuleHandleW(nullptr),
        .hwnd = reinterpret_cast<HWND>(nativeWindowHandle),
    });
    if (surfR.result != vk::Result::eSuccess) {
        std::fprintf(stderr, "[vk] createWin32SurfaceKHR failed (%s)\n",
                     vk::to_string(surfR.result).c_str());
        return SwapChainHandle::Invalid;
    }
    sc.surface = std::move(surfR.value);
#else
    // Cross-platform path: the host pre-built a VkSurfaceKHR (typically via
    // glfwCreateWindowSurface) and handed it to us as the void*. Wrap in
    // vk::raii::SurfaceKHR so it's destroyed alongside the swap chain.
    if (!nativeWindowHandle) {
        std::fprintf(stderr, "[vk] CreateSwapChain: null surface handle\n");
        return SwapChainHandle::Invalid;
    }
    // VkSurfaceKHR is a 64-bit non-dispatchable handle on x86_64; void* and
    // uintptr_t are the same width, so the round-trip through uintptr_t is
    // bit-preserving.
    VkSurfaceKHR rawSurface = VK_NULL_HANDLE;
    static_assert(sizeof(VkSurfaceKHR) == sizeof(uintptr_t),
                  "VkSurfaceKHR must be the same width as a pointer");
    std::memcpy(&rawSurface, &nativeWindowHandle, sizeof(VkSurfaceKHR));
    sc.surface = vk::raii::SurfaceKHR(state.instance, rawSurface);
#endif

    auto presentR = state.physicalDevice.getSurfaceSupportKHR(state.queueFamily, *sc.surface);
    if (presentR.result != vk::Result::eSuccess || !presentR.value) {
        std::fprintf(stderr, "[vk] queue family %u doesn't support presenting on this surface\n",
                     state.queueFamily);
        return SwapChainHandle::Invalid;
    }

    if (!CreateSwapchainObjects(state, sc, width, height, colorFormat)) {
        return SwapChainHandle::Invalid;
    }

    // Proxy textures — image/view get repointed every Acquire.
    TextureEntry srgbProxy{};
    srgbProxy.format = sc.formatSrgb;
    srgbProxy.aspect = vk::ImageAspectFlagBits::eColor;
    srgbProxy.width = static_cast<i32>(sc.extent.width);
    srgbProxy.height = static_cast<i32>(sc.extent.height);
    srgbProxy.ownsImage = false;
    srgbProxy.isLinearView = false;
    srgbProxy.image = sc.images[0];
    srgbProxy.view = *sc.viewsSrgb[0];

    TextureEntry linearProxy{};
    linearProxy.format = sc.formatLinear;
    linearProxy.aspect = vk::ImageAspectFlagBits::eColor;
    linearProxy.width = srgbProxy.width;
    linearProxy.height = srgbProxy.height;
    linearProxy.ownsImage = false;
    linearProxy.isLinearView = true;
    linearProxy.image = sc.images[0];
    linearProxy.view = *sc.viewsLinear[0];

    const u64 proxySrgbRaw = state.textures.Insert(std::move(srgbProxy));
    const u64 proxyLinearRaw = state.textures.Insert(std::move(linearProxy));
    sc.proxySrgb = static_cast<TextureHandle>(proxySrgbRaw);
    sc.proxyLinear = static_cast<TextureHandle>(proxyLinearRaw);

    const u64 raw = state.swapchains.Insert(std::move(sc));
    const SwapChainHandle handle = static_cast<SwapChainHandle>(raw);

    // Back-pointers now that the SwapChainHandle is allocated.
    if (auto* proxy = state.textures.Get(proxySrgbRaw))
        proxy->swapChainProxy = handle;
    if (auto* proxy = state.textures.Get(proxyLinearRaw))
        proxy->swapChainProxy = handle;
    return handle;
}

void VulkanDevice::ResizeSwapChain(SwapChainHandle handle, i32 width, i32 height) {
    auto& state = *state_;
    auto* sc = state.swapchains.Get(static_cast<u64>(handle));
    if (!sc)
        return;
    state.device.waitIdle();
    sc->viewsSrgb.clear();
    sc->viewsLinear.clear();
    sc->images.clear();
    sc->swapchain = nullptr; // raii destroy
    if (!CreateSwapchainObjects(state, *sc, width, height,
                                sc->formatSrgb == vk::Format::eR8G8B8A8Srgb
                                    ? Format::R8G8B8A8_UNORM_SRGB
                                    : Format::B8G8R8A8_UNORM)) {
        return;
    }
    if (auto* proxy = state.textures.Get(static_cast<u64>(sc->proxySrgb))) {
        proxy->image = sc->images[0];
        proxy->view = *sc->viewsSrgb[0];
        proxy->width = static_cast<i32>(sc->extent.width);
        proxy->height = static_cast<i32>(sc->extent.height);
    }
    if (auto* proxy = state.textures.Get(static_cast<u64>(sc->proxyLinear))) {
        proxy->image = sc->images[0];
        proxy->view = *sc->viewsLinear[0];
        proxy->width = static_cast<i32>(sc->extent.width);
        proxy->height = static_cast<i32>(sc->extent.height);
    }
}

void VulkanDevice::DestroySwapChain(SwapChainHandle handle) {
    auto& state = *state_;
    auto* sc = state.swapchains.Get(static_cast<u64>(handle));
    if (!sc)
        return;
    state.device.waitIdle();
    state.textures.Remove(static_cast<u64>(sc->proxySrgb));
    state.textures.Remove(static_cast<u64>(sc->proxyLinear));
    state.swapchains.Remove(static_cast<u64>(handle)); // raii teardown
}

TextureHandle VulkanDevice::GetSwapChainBackBuffer(SwapChainHandle handle) {
    auto* sc = state_->swapchains.Get(static_cast<u64>(handle));
    return sc ? sc->proxySrgb : TextureHandle::Invalid;
}

TextureHandle VulkanDevice::GetSwapChainBackBufferLinear(SwapChainHandle handle) {
    auto* sc = state_->swapchains.Get(static_cast<u64>(handle));
    return sc ? sc->proxyLinear : TextureHandle::Invalid;
}

Format VulkanDevice::GetSwapChainFormat(SwapChainHandle handle) const {
    auto* sc = state_->swapchains.Get(static_cast<u64>(handle));
    if (!sc)
        return Format::Unknown;
    switch (sc->formatSrgb) {
    case vk::Format::eR8G8B8A8Unorm:
        return Format::R8G8B8A8_UNORM;
    case vk::Format::eR8G8B8A8Srgb:
        return Format::R8G8B8A8_UNORM_SRGB;
    case vk::Format::eB8G8R8A8Unorm:
        return Format::B8G8R8A8_UNORM;
    case vk::Format::eB8G8R8A8Srgb:
        return Format::B8G8R8A8_UNORM_SRGB;
    default:
        return Format::Unknown;
    }
}

u32 AcquireSwapChainImageIfNeeded(VulkanDeviceState& state, SwapChainEntry& sc,
                                  FrameContext& frame) {
    if (sc.acquiredThisFrame)
        return sc.imageIndex;

    // Swap-with-spare so an acquire never reuses a semaphore that the
    // previous present is still waiting on:
    //  1. Acquire with the spare (always free).
    //  2. Swap it into the per-image slot; the old slot becomes the
    //     new spare. Releases one acquire cycle later, when the same
    //     previous semaphore (released N acquires ago for that image)
    //     image is next acquired.
#if defined(TRACY_ENABLE)
    vk::ResultValue<u32> r{vk::Result::eSuccess, 0u};
    {
        ZoneScopedN("vkAcquireNextImage");
        r = sc.swapchain.acquireNextImage(UINT64_MAX, *sc.spareAcquireSem, VK_NULL_HANDLE);
    }
#else
    auto r = sc.swapchain.acquireNextImage(UINT64_MAX, *sc.spareAcquireSem, VK_NULL_HANDLE);
#endif
    if (r.result != vk::Result::eSuccess && r.result != vk::Result::eSuboptimalKHR) {
        std::fprintf(stderr, "[vk] acquireNextImage failed (%s)\n",
                     vk::to_string(r.result).c_str());
        return UINT32_MAX;
    }
    const u32 idx = r.value;
    std::swap(sc.spareAcquireSem, sc.imageAcquireSems[idx]);

    sc.imageIndex = idx;
    sc.acquiredThisFrame = true;
    frame.acquireWaitSem = *sc.imageAcquireSems[idx];
    frame.renderDoneSem = *sc.imageRenderDoneSems[idx];

    if (auto* proxy = state.textures.Get(static_cast<u64>(sc.proxySrgb))) {
        proxy->image = sc.images[idx];
        proxy->view = *sc.viewsSrgb[idx];
        proxy->currentLayout = vk::ImageLayout::eUndefined;
    }
    if (auto* proxy = state.textures.Get(static_cast<u64>(sc.proxyLinear))) {
        proxy->image = sc.images[idx];
        proxy->view = *sc.viewsLinear[idx];
        proxy->currentLayout = vk::ImageLayout::eUndefined;
    }
    return idx;
}

void VulkanDevice::Present(SwapChainHandle handle) {
    auto& state = *state_;
    auto* sc = state.swapchains.Get(static_cast<u64>(handle));
    if (!sc || !sc->acquiredThisFrame)
        return;

    auto& frame = state.frames[state.frameIndex];

    if (frame.recording) {
        // Transition the rendered image to PRESENT_SRC.
        vk::ImageMemoryBarrier2 barrier{
            .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eBottomOfPipe,
            .dstAccessMask = {},
            .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
            .newLayout = vk::ImageLayout::ePresentSrcKHR,
            .image = sc->images[sc->imageIndex],
            .subresourceRange =
                {
                    .aspectMask = vk::ImageAspectFlagBits::eColor,
                    .levelCount = 1,
                    .layerCount = 1,
                },
        };
        vk::DependencyInfo dep{.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier};
        frame.commandBuffer.pipelineBarrier2(dep);
#if defined(TRACY_ENABLE)
        // Must be in an open, non-render-pass CB — barrier above keeps us there.
        if (state.tracyCtx) {
            TracyVkCollect(state.tracyCtx, static_cast<VkCommandBuffer>(*frame.commandBuffer));
        }
#endif
        (void)frame.commandBuffer.end();
        frame.recording = false;

        // Wait on the acquire semaphore + (when applicable) the latest
        // transfer-timeline signal so draws see uploaded data.
        std::array<vk::SemaphoreSubmitInfo, 2> waitInfos{};
        waitInfos[0] = vk::SemaphoreSubmitInfo{
            .semaphore = vk::Semaphore(frame.acquireWaitSem),
            .stageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        };
        u32 waitCount = 1;
        if (state.hasAsyncTransfer && state.transferLastSignaled > 0) {
            waitInfos[1] = vk::SemaphoreSubmitInfo{
                .semaphore = *state.transferTimelineSem,
                .value = state.transferLastSignaled,
                .stageMask = vk::PipelineStageFlagBits2::eVertexInput |
                             vk::PipelineStageFlagBits2::eAllGraphics,
            };
            waitCount = 2;
        }
        // renderDoneSem for present + timelineSem for deferred-delete.
        std::array<vk::SemaphoreSubmitInfo, 2> signalInfos{
            vk::SemaphoreSubmitInfo{
                .semaphore = vk::Semaphore(frame.renderDoneSem),
                .stageMask = vk::PipelineStageFlagBits2::eAllCommands,
            },
            vk::SemaphoreSubmitInfo{
                .semaphore = *state.timelineSem,
                .value = state.nextSubmitValue,
                .stageMask = vk::PipelineStageFlagBits2::eAllCommands,
            },
        };
        vk::CommandBufferSubmitInfo cbInfo{.commandBuffer = *frame.commandBuffer};
        vk::SubmitInfo2 submit{
            .waitSemaphoreInfoCount = waitCount,
            .pWaitSemaphoreInfos = waitInfos.data(),
            .commandBufferInfoCount = 1,
            .pCommandBufferInfos = &cbInfo,
            .signalSemaphoreInfoCount = static_cast<u32>(signalInfos.size()),
            .pSignalSemaphoreInfos = signalInfos.data(),
        };
        {
#if defined(TRACY_ENABLE)
            ZoneScopedN("vkQueueSubmit2");
#endif
            (void)state.queue.submit2(submit, *frame.inFlightFence);
        }
        state.nextSubmitValue++;
    }

    const VkSemaphore waitSem = frame.renderDoneSem;
    const VkSwapchainKHR swap = *sc->swapchain;
    const u32 idx = sc->imageIndex;
    vk::PresentInfoKHR pi{
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = reinterpret_cast<const vk::Semaphore*>(&waitSem),
        .swapchainCount = 1,
        .pSwapchains = reinterpret_cast<const vk::SwapchainKHR*>(&swap),
        .pImageIndices = &idx,
    };
    {
#if defined(TRACY_ENABLE)
        ZoneScopedN("vkQueuePresent");
#endif
        (void)state.queue.presentKHR(pi);
    }

    sc->acquiredThisFrame = false;
    state.frameIndex = (state.frameIndex + 1) % kFramesInFlight;

#if defined(TRACY_ENABLE)
    // Mark the end of the frame for Tracy's CPU timeline. Place this
    // after Present so the frame's duration on Tracy matches what the
    // user actually perceives (wall-clock end of frame, not end of
    // submit). FrameMark is a no-op when TRACY_ENABLE is off.
    FrameMark;
#endif
}

} // namespace whiteout::flakes::gfx::vulkan

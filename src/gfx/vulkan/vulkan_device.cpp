// Vulkan IGFXDevice — instance + physical device + logical device + VMA
// init. Uses vulkan.hpp (vk::raii::*) for all non-VMA Vulkan objects so
// teardown is automatic on scope exit. VMA's allocator handle and the
// VMA-owned VkBuffer/VkImage are still raw — torn down explicitly in
// the destructor after the slot maps clear.

#include "vulkan_device.h"
#include "vulkan_resources.h"
#include "vulkan_translate.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <vector>

namespace whiteout::flakes::gfx::vulkan {

// VulkanDeviceState lives in vulkan_resources.h.

namespace {

VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT       severity,
    VkDebugUtilsMessageTypeFlagsEXT              /*types*/,
    const VkDebugUtilsMessengerCallbackDataEXT*  cb,
    void*                                        /*userData*/)
{
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        std::fprintf(stderr, "[vk] ERR: %s\n", cb->pMessage);
    } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        std::fprintf(stderr, "[vk] WARN: %s\n", cb->pMessage);
    }
    return VK_FALSE;
}

bool HasInstanceLayer(const vk::raii::Context& ctx, const char* name) {
    auto [r, layers] = ctx.enumerateInstanceLayerProperties();
    if (r != vk::Result::eSuccess) return false;
    for (const auto& l : layers) {
        if (std::strcmp(l.layerName, name) == 0) return true;
    }
    return false;
}

bool HasInstanceExtension(const vk::raii::Context& ctx, const char* name) {
    auto [r, exts] = ctx.enumerateInstanceExtensionProperties();
    if (r != vk::Result::eSuccess) return false;
    for (const auto& e : exts) {
        if (std::strcmp(e.extensionName, name) == 0) return true;
    }
    return false;
}

i32 ScoreDevice(const vk::raii::PhysicalDevice& pd) {
    const auto props = pd.getProperties();
    if (VK_API_VERSION_MAJOR(props.apiVersion) < 1 ||
        (VK_API_VERSION_MAJOR(props.apiVersion) == 1 &&
         VK_API_VERSION_MINOR(props.apiVersion) < 3)) {
        return -1;
    }
    const auto mem = pd.getMemoryProperties();
    u64 vram = 0;
    for (u32 i = 0; i < mem.memoryHeapCount; ++i) {
        if (mem.memoryHeaps[i].flags & vk::MemoryHeapFlagBits::eDeviceLocal) {
            vram = std::max(vram, static_cast<u64>(mem.memoryHeaps[i].size));
        }
    }
    const i32 vramScore = static_cast<i32>(vram / (256ull * 1024 * 1024));
    const i32 typeBonus = (props.deviceType == vk::PhysicalDeviceType::eDiscreteGpu)
                              ? 1000 : 0;
    return typeBonus + vramScore;
}

bool DeviceSupportsExtensions(const vk::raii::PhysicalDevice& pd,
                              const std::vector<const char*>& required) {
    auto [r, avail] = pd.enumerateDeviceExtensionProperties();
    if (r != vk::Result::eSuccess) return false;
    auto has = [&](const char* name) {
        for (const auto& e : avail) {
            if (std::strcmp(e.extensionName, name) == 0) return true;
        }
        return false;
    };
    for (const char* ext : required) {
        if (!has(ext)) return false;
    }
    return true;
}

i32 PickGraphicsQueueFamily(const vk::raii::PhysicalDevice& pd) {
    const auto fams = pd.getQueueFamilyProperties();
    for (u32 i = 0; i < fams.size(); ++i) {
        if (fams[i].queueFlags & vk::QueueFlagBits::eGraphics) {
            return static_cast<i32>(i);
        }
    }
    return -1;
}

VkBufferUsageFlags ToVkBufferUsage(BufferUsage u) {
    VkBufferUsageFlags out = 0;
    if (hasFlag(u, BufferUsage::Vertex))          out |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    if (hasFlag(u, BufferUsage::Index))           out |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    if (hasFlag(u, BufferUsage::Constant))        out |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    if (hasFlag(u, BufferUsage::ShaderResource))  out |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (hasFlag(u, BufferUsage::UnorderedAccess)) out |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    out |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    return out;
}

}  // namespace

VulkanDevice::VulkanDevice()
    : state_(std::make_unique<VulkanDeviceState>())
    , immediate_(std::make_unique<VulkanCommandList>(*this)) {}

VulkanDevice::~VulkanDevice() {
    auto& s = *state_;
    if (*s.device) {
        s.device.waitIdle();
    }

    // Tear down VMA-owned resources (everything in the slot maps that's
    // raw VkXxx + VmaAllocation) before the allocator goes away.
    s.buffers.ForEach([&](BufferEntry& e) {
        if (e.buffer && e.allocation)
            vmaDestroyBuffer(s.allocator, e.buffer, e.allocation);
        e.buffer = VK_NULL_HANDLE;
        e.allocation = VK_NULL_HANDLE;
    });
    s.textures.ForEach([&](TextureEntry& e) {
        if (e.ownsImage && e.image && e.allocation)
            vmaDestroyImage(s.allocator, e.image, e.allocation);
        e.image = VK_NULL_HANDLE;
        e.allocation = VK_NULL_HANDLE;
    });
    s.buffers.Clear();
    s.textures.Clear();

    if (s.allocator) {
        vmaDestroyAllocator(s.allocator);
        s.allocator = VK_NULL_HANDLE;
    }
    // raii teardown for the rest happens automatically in member dtor order.
}

VulkanDeviceState&       VulkanDevice::State()       { return *state_; }
const VulkanDeviceState& VulkanDevice::State() const { return *state_; }

bool VulkanDevice::Init() {
    auto& s = *state_;

    // ---- 1. Instance ------------------------------------------------------
    vk::ApplicationInfo appInfo{
        .pApplicationName   = "WhiteoutFlakes",
        .applicationVersion = VK_MAKE_VERSION(0, 1, 0),
        .pEngineName        = "WhiteoutFlakes",
        .engineVersion      = VK_MAKE_VERSION(0, 1, 0),
        .apiVersion         = VK_API_VERSION_1_3,
    };

    std::vector<const char*> instExts = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
    };
    std::vector<const char*> instLayers;

#ifndef NDEBUG
    const bool wantValidation =
        HasInstanceLayer(s.ctx, "VK_LAYER_KHRONOS_validation") &&
        HasInstanceExtension(s.ctx, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    if (wantValidation) {
        instLayers.push_back("VK_LAYER_KHRONOS_validation");
        instExts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }
#else
    constexpr bool wantValidation = false;
#endif

    vk::InstanceCreateInfo ici{
        .pApplicationInfo        = &appInfo,
        .enabledLayerCount       = static_cast<u32>(instLayers.size()),
        .ppEnabledLayerNames     = instLayers.data(),
        .enabledExtensionCount   = static_cast<u32>(instExts.size()),
        .ppEnabledExtensionNames = instExts.data(),
    };

    auto instanceResult = s.ctx.createInstance(ici);
    if (instanceResult.result != vk::Result::eSuccess) {
        std::fprintf(stderr, "[vk] createInstance failed (%s)\n",
                     vk::to_string(instanceResult.result).c_str());
        return false;
    }
    s.instance = std::move(instanceResult.value);

    if (wantValidation) {
        vk::DebugUtilsMessengerCreateInfoEXT dbg{
            .messageSeverity =
                vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning |
                vk::DebugUtilsMessageSeverityFlagBitsEXT::eError,
            .messageType =
                vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral |
                vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation |
                vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance,
            .pfnUserCallback = reinterpret_cast<vk::PFN_DebugUtilsMessengerCallbackEXT>(
                                    DebugCallback),
        };
        auto dbgResult = s.instance.createDebugUtilsMessengerEXT(dbg);
        if (dbgResult.result == vk::Result::eSuccess) {
            s.debugMessenger = std::move(dbgResult.value);
        }
    }

    // ---- 2. Physical device ----------------------------------------------
    auto [pdResult, pds] = s.instance.enumeratePhysicalDevices();
    if (pdResult != vk::Result::eSuccess || pds.empty()) {
        std::fprintf(stderr, "[vk] no Vulkan physical devices\n");
        return false;
    }

    std::vector<const char*> deviceExts = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME,
    };

    i32 bestScore = -1;
    for (auto& pd : pds) {
        if (!DeviceSupportsExtensions(pd, deviceExts)) continue;
        if (PickGraphicsQueueFamily(pd) < 0) continue;
        const i32 score = ScoreDevice(pd);
        if (score > bestScore) {
            bestScore = score;
            s.physicalDevice = std::move(pd);
        }
    }
    if (!*s.physicalDevice) {
        std::fprintf(stderr,
                     "[vk] no physical device meets requirements "
                     "(Vulkan 1.3 + swapchain + push_descriptor)\n");
        return false;
    }
    const auto props = s.physicalDevice.getProperties();
    deviceName_ = props.deviceName.data();
    s.queueFamily = static_cast<u32>(PickGraphicsQueueFamily(s.physicalDevice));

    // ---- 3. Logical device + dynamic-rendering / sync2 features -----------
    const f32 queuePriority = 1.0f;
    vk::DeviceQueueCreateInfo qci{
        .queueFamilyIndex = s.queueFamily,
        .queueCount       = 1,
        .pQueuePriorities = &queuePriority,
    };

    vk::PhysicalDeviceVulkan13Features vk13{
        .synchronization2 = vk::True,
        .dynamicRendering = vk::True,
    };

    vk::DeviceCreateInfo dci{
        .pNext                   = &vk13,
        .queueCreateInfoCount    = 1,
        .pQueueCreateInfos       = &qci,
        .enabledExtensionCount   = static_cast<u32>(deviceExts.size()),
        .ppEnabledExtensionNames = deviceExts.data(),
    };

    auto deviceResult = s.physicalDevice.createDevice(dci);
    if (deviceResult.result != vk::Result::eSuccess) {
        std::fprintf(stderr, "[vk] createDevice failed (%s)\n",
                     vk::to_string(deviceResult.result).c_str());
        return false;
    }
    s.device = std::move(deviceResult.value);
    s.queue  = s.device.getQueue(s.queueFamily, 0);

    // ---- 4. VMA -----------------------------------------------------------
    VmaAllocatorCreateInfo aci{};
    aci.physicalDevice   = *s.physicalDevice;
    aci.device           = *s.device;
    aci.instance         = *s.instance;
    aci.vulkanApiVersion = VK_API_VERSION_1_3;
    if (vmaCreateAllocator(&aci, &s.allocator) != VK_SUCCESS) {
        std::fprintf(stderr, "[vk] vmaCreateAllocator failed\n");
        return false;
    }

    // ---- 5. Shared pipeline layout + push-descriptor set layout ----------
    // One layout for every PSO so Bind* calls can target the same
    // descriptor set regardless of which pipeline is bound. Push
    // descriptors avoid per-frame pool/set allocation.
    {
        std::array<vk::DescriptorSetLayoutBinding, kCbBindingCount> cbBindings{};
        for (u32 i = 0; i < kCbBindingCount; ++i) {
            cbBindings[i] = vk::DescriptorSetLayoutBinding{
                .binding         = kCbBindingBase + i,
                .descriptorType  = vk::DescriptorType::eUniformBuffer,
                .descriptorCount = 1,
                .stageFlags      = vk::ShaderStageFlagBits::eVertex
                                 | vk::ShaderStageFlagBits::eFragment,
            };
        }
        auto dslR = s.device.createDescriptorSetLayout({
            .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
            .bindingCount = static_cast<u32>(cbBindings.size()),
            .pBindings    = cbBindings.data(),
        });
        if (dslR.result != vk::Result::eSuccess) {
            std::fprintf(stderr, "[vk] createDescriptorSetLayout failed (%s)\n",
                         vk::to_string(dslR.result).c_str());
            return false;
        }
        s.pushDescSetLayout = std::move(dslR.value);

        const VkDescriptorSetLayout rawDsl = *s.pushDescSetLayout;
        auto plR = s.device.createPipelineLayout({
            .setLayoutCount = 1,
            .pSetLayouts    = reinterpret_cast<const vk::DescriptorSetLayout*>(&rawDsl),
        });
        if (plR.result != vk::Result::eSuccess) {
            std::fprintf(stderr, "[vk] createPipelineLayout failed (%s)\n",
                         vk::to_string(plR.result).c_str());
            return false;
        }
        s.pipelineLayout = std::move(plR.value);
    }

    // ---- 6. Per-frame command pool + sync ---------------------------------
    for (u32 i = 0; i < kFramesInFlight; ++i) {
        auto& f = s.frames[i];

        auto poolR = s.device.createCommandPool({
            .flags = vk::CommandPoolCreateFlagBits::eTransient
                   | vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
            .queueFamilyIndex = s.queueFamily,
        });
        if (poolR.result != vk::Result::eSuccess) {
            std::fprintf(stderr, "[vk] createCommandPool failed (frame %u)\n", i);
            return false;
        }
        f.commandPool = std::move(poolR.value);

        auto cbsR = s.device.allocateCommandBuffers({
            .commandPool        = *f.commandPool,
            .level              = vk::CommandBufferLevel::ePrimary,
            .commandBufferCount = 1,
        });
        if (cbsR.result != vk::Result::eSuccess) {
            std::fprintf(stderr, "[vk] allocateCommandBuffers failed (frame %u)\n", i);
            return false;
        }
        f.commandBuffer = std::move(cbsR.value[0]);

        auto sem1 = s.device.createSemaphore({});
        auto sem2 = s.device.createSemaphore({});
        if (sem1.result != vk::Result::eSuccess ||
            sem2.result != vk::Result::eSuccess) {
            std::fprintf(stderr, "[vk] createSemaphore failed (frame %u)\n", i);
            return false;
        }
        f.acquireSem    = std::move(sem1.value);
        f.renderDoneSem = std::move(sem2.value);

        auto fenceR = s.device.createFence({
            .flags = vk::FenceCreateFlagBits::eSignaled,
        });
        if (fenceR.result != vk::Result::eSuccess) {
            std::fprintf(stderr, "[vk] createFence failed (frame %u)\n", i);
            return false;
        }
        f.inFlightFence = std::move(fenceR.value);
    }

    std::printf("[vk] device='%s' api=%u.%u.%u queueFamily=%u%s\n",
                deviceName_.c_str(),
                VK_API_VERSION_MAJOR(props.apiVersion),
                VK_API_VERSION_MINOR(props.apiVersion),
                VK_API_VERSION_PATCH(props.apiVersion),
                s.queueFamily,
                wantValidation ? " (validation)" : "");
    return true;
}

const char* VulkanDevice::GetDeviceName() const { return deviceName_.c_str(); }

IGFXCommandList* VulkanDevice::GetImmediateContext() { return immediate_.get(); }

// ---- Buffer / texture / shader / pipeline / sampler ----------------------

BufferHandle VulkanDevice::CreateBuffer(const BufferDesc& desc, const void* initial) {
    auto& s = *state_;

    VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size  = desc.size;
    bci.usage = ToVkBufferUsage(desc.usage);
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo aci{};
    if (hasFlag(desc.usage, BufferUsage::CpuWritable)) {
        aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
        aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
                  | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    } else {
        aci.usage = VMA_MEMORY_USAGE_AUTO;
    }

    BufferEntry e{};
    e.desc = desc;
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(s.allocator, &bci, &aci, &e.buffer, &e.allocation, &info)
            != VK_SUCCESS) {
        return BufferHandle::Invalid;
    }
    e.mapped = info.pMappedData;

    if (initial && desc.size > 0) {
        if (e.mapped) {
            std::memcpy(e.mapped, initial, desc.size);
            vmaFlushAllocation(s.allocator, e.allocation, 0, desc.size);
        } else {
            // Stage via temporary CPU-visible buffer.
            VkBufferCreateInfo sbci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            sbci.size  = desc.size;
            sbci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
            sbci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            VmaAllocationCreateInfo saci{};
            saci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
            saci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
                       | VMA_ALLOCATION_CREATE_MAPPED_BIT;
            VkBuffer      stagingBuf{};
            VmaAllocation stagingAlloc{};
            VmaAllocationInfo si{};
            vmaCreateBuffer(s.allocator, &sbci, &saci, &stagingBuf, &stagingAlloc, &si);
            std::memcpy(si.pMappedData, initial, desc.size);
            vmaFlushAllocation(s.allocator, stagingAlloc, 0, desc.size);

            auto& frame = s.frames[s.frameIndex];
            auto cbsR = s.device.allocateCommandBuffers({
                .commandPool        = *frame.commandPool,
                .level              = vk::CommandBufferLevel::ePrimary,
                .commandBufferCount = 1,
            });
            if (cbsR.result == vk::Result::eSuccess) {
                vk::raii::CommandBuffer xfer = std::move(cbsR.value[0]);
                (void)xfer.begin({ .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit });
                vk::BufferCopy region{ .size = desc.size };
                xfer.copyBuffer(vk::Buffer(stagingBuf), vk::Buffer(e.buffer), region);
                (void)xfer.end();

                vk::CommandBufferSubmitInfo cbInfo{ .commandBuffer = *xfer };
                vk::SubmitInfo2 submit{
                    .commandBufferInfoCount = 1,
                    .pCommandBufferInfos    = &cbInfo,
                };
                (void)s.queue.submit2(submit);
                (void)s.queue.waitIdle();
            }
            vmaDestroyBuffer(s.allocator, stagingBuf, stagingAlloc);
        }
    }

    return static_cast<BufferHandle>(s.buffers.Insert(std::move(e)));
}

TextureHandle VulkanDevice::CreateTexture(const TextureDesc& desc,
                                          const void* /*initialPixels*/)
{
    auto& s = *state_;
    const vk::Format fmt = ToVkFormat(desc.format);
    if (fmt == vk::Format::eUndefined) return TextureHandle::Invalid;

    VkImageCreateInfo ici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ici.imageType   = VK_IMAGE_TYPE_2D;
    ici.format      = static_cast<VkFormat>(fmt);
    ici.extent      = { static_cast<u32>(desc.width),
                        static_cast<u32>(desc.height), 1 };
    ici.mipLevels   = static_cast<u32>(desc.mipLevels);
    ici.arrayLayers = static_cast<u32>(desc.arraySize);
    ici.samples     = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling      = VK_IMAGE_TILING_OPTIMAL;
    ici.usage       = 0;
    if (hasFlag(desc.usage, TextureUsage::ShaderResource))
        ici.usage |= VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (hasFlag(desc.usage, TextureUsage::RenderTarget))
        ici.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (hasFlag(desc.usage, TextureUsage::DepthStencil))
        ici.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (desc.isCube) ici.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;

    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;

    TextureEntry t{};
    if (vmaCreateImage(s.allocator, &ici, &aci, &t.image, &t.allocation, nullptr)
            != VK_SUCCESS) {
        return TextureHandle::Invalid;
    }

    const bool isDepth = hasFlag(desc.usage, TextureUsage::DepthStencil);
    t.aspect = isDepth ? (vk::ImageAspectFlagBits::eDepth |
                          (fmt == vk::Format::eD24UnormS8Uint
                               ? vk::ImageAspectFlagBits::eStencil
                               : vk::ImageAspectFlags{}))
                       : vk::ImageAspectFlags(vk::ImageAspectFlagBits::eColor);

    auto viewR = s.device.createImageView({
        .image    = vk::Image(t.image),
        .viewType = desc.isCube ? vk::ImageViewType::eCube : vk::ImageViewType::e2D,
        .format   = fmt,
        .subresourceRange = {
            .aspectMask     = t.aspect,
            .baseMipLevel   = 0,
            .levelCount     = static_cast<u32>(desc.mipLevels),
            .baseArrayLayer = 0,
            .layerCount     = static_cast<u32>(desc.arraySize),
        },
    });
    if (viewR.result != vk::Result::eSuccess) {
        vmaDestroyImage(s.allocator, t.image, t.allocation);
        return TextureHandle::Invalid;
    }
    t.ownedView = std::move(viewR.value);
    t.view      = *t.ownedView;

    t.format        = fmt;
    t.currentLayout = vk::ImageLayout::eUndefined;
    t.width         = desc.width;
    t.height        = desc.height;
    t.ownsImage     = true;
    return static_cast<TextureHandle>(s.textures.Insert(std::move(t)));
}

TextureHandle VulkanDevice::CreateColorTarget(i32, i32, Format) {
    return TextureHandle::Invalid;  // Phase 2
}

TextureHandle VulkanDevice::CreateDepthTarget(i32 w, i32 h, Format f) {
    auto& s = *state_;
    const vk::Format fmt = ToVkFormat(f);
    if (fmt == vk::Format::eUndefined) return TextureHandle::Invalid;

    VkImageCreateInfo ici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ici.imageType     = VK_IMAGE_TYPE_2D;
    ici.format        = static_cast<VkFormat>(fmt);
    ici.extent        = { static_cast<u32>(w), static_cast<u32>(h), 1 };
    ici.mipLevels     = 1;
    ici.arrayLayers   = 1;
    ici.samples       = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling        = VK_IMAGE_TILING_OPTIMAL;
    ici.usage         = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                      | VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;

    TextureEntry t{};
    if (vmaCreateImage(s.allocator, &ici, &aci, &t.image, &t.allocation, nullptr)
            != VK_SUCCESS) {
        return TextureHandle::Invalid;
    }

    const bool hasStencil = (fmt == vk::Format::eD24UnormS8Uint);
    t.aspect = vk::ImageAspectFlagBits::eDepth
             | (hasStencil ? vk::ImageAspectFlags(vk::ImageAspectFlagBits::eStencil)
                           : vk::ImageAspectFlags{});

    auto viewR = s.device.createImageView({
        .image    = vk::Image(t.image),
        .viewType = vk::ImageViewType::e2D,
        .format   = fmt,
        .subresourceRange = {
            .aspectMask     = t.aspect,
            .baseMipLevel   = 0,
            .levelCount     = 1,
            .baseArrayLayer = 0,
            .layerCount     = 1,
        },
    });
    if (viewR.result != vk::Result::eSuccess) {
        vmaDestroyImage(s.allocator, t.image, t.allocation);
        return TextureHandle::Invalid;
    }
    t.ownedView     = std::move(viewR.value);
    t.view          = *t.ownedView;
    t.format        = fmt;
    t.currentLayout = vk::ImageLayout::eUndefined;
    t.width         = w;
    t.height        = h;
    t.ownsImage     = true;
    return static_cast<TextureHandle>(s.textures.Insert(std::move(t)));
}

ShaderHandle VulkanDevice::CreateShader(ShaderStage stage,
                                        const void* bytecode, usize size)
{
    auto& s = *state_;
    auto modR = s.device.createShaderModule({
        .codeSize = size,
        .pCode    = static_cast<const u32*>(bytecode),
    });
    if (modR.result != vk::Result::eSuccess) {
        return ShaderHandle::Invalid;
    }
    ShaderEntry e{};
    e.module = std::move(modR.value);
    e.stage  = stage;
    return static_cast<ShaderHandle>(s.shaders.Insert(std::move(e)));
}

PipelineHandle VulkanDevice::CreateGraphicsPipeline(const GraphicsPipelineDesc& desc) {
    auto& s = *state_;

    auto* vs = s.shaders.Get(static_cast<u64>(desc.vs));
    auto* ps = s.shaders.Get(static_cast<u64>(desc.ps));
    if (!vs) return PipelineHandle::Invalid;

    // ---- Shader stages ----
    std::array<vk::PipelineShaderStageCreateInfo, 2> stages{};
    u32 stageCount = 0;
    stages[stageCount++] = vk::PipelineShaderStageCreateInfo{
        .stage  = vk::ShaderStageFlagBits::eVertex,
        .module = *vs->module,
        .pName  = "main",
    };
    if (ps) {
        stages[stageCount++] = vk::PipelineShaderStageCreateInfo{
            .stage  = vk::ShaderStageFlagBits::eFragment,
            .module = *ps->module,
            .pName  = "main",
        };
    }

    // ---- Vertex input — one VkVertexInputBinding per slot used ----
    // The desc.inputLayout enumerates per-attribute (semantic, format,
    // offset, slot). Bindings are derived by collecting unique slot
    // indices; per-binding stride is the max (offset + element-size)
    // we see for that slot.
    std::array<vk::VertexInputBindingDescription, 8>   bindings{};
    std::array<u32, 8>                                 slotStride{};
    std::array<bool, 8>                                slotUsed{};
    std::vector<vk::VertexInputAttributeDescription>   attrs;
    attrs.reserve(desc.inputLayout.size());

    for (u32 i = 0; i < desc.inputLayout.size(); ++i) {
        const auto& el = desc.inputLayout[i];
        if (el.inputSlot >= slotUsed.size()) continue;
        slotUsed[el.inputSlot] = true;
        attrs.push_back(vk::VertexInputAttributeDescription{
            .location = i,
            .binding  = el.inputSlot,
            .format   = ToVkFormat(el.format),
            .offset   = el.offset,
        });
        // Element size derived from format — 16/12/8/4 for the formats
        // the renderer actually uses for vertex input. Fall back on 4 so
        // an unrecognised format doesn't truncate the stride past 0.
        u32 elemSize = 4;
        switch (el.format) {
            case Format::R32G32B32A32_FLOAT: elemSize = 16; break;
            case Format::R32G32B32_FLOAT:    elemSize = 12; break;
            case Format::R32G32_FLOAT:       elemSize = 8;  break;
            case Format::R8G8B8A8_UNORM:
            case Format::R8G8B8A8_UNORM_SRGB:
            case Format::R8G8B8A8_UINT:
            case Format::B8G8R8A8_UNORM:
            case Format::R32_FLOAT:
            case Format::R32_UINT:
            case Format::R11G11B10_FLOAT:
            case Format::R16G16_UNORM:        elemSize = 4; break;
            default: break;
        }
        slotStride[el.inputSlot] = std::max(slotStride[el.inputSlot],
                                            el.offset + elemSize);
    }

    u32 bindingCount = 0;
    for (u32 slot = 0; slot < slotUsed.size(); ++slot) {
        if (!slotUsed[slot]) continue;
        bindings[bindingCount++] = vk::VertexInputBindingDescription{
            .binding   = slot,
            .stride    = slotStride[slot],
            .inputRate = vk::VertexInputRate::eVertex,
        };
    }

    vk::PipelineVertexInputStateCreateInfo vi{
        .vertexBindingDescriptionCount   = bindingCount,
        .pVertexBindingDescriptions      = bindings.data(),
        .vertexAttributeDescriptionCount = static_cast<u32>(attrs.size()),
        .pVertexAttributeDescriptions    = attrs.data(),
    };

    // ---- Input assembly / rasterizer / depth-stencil / blend ----
    vk::PipelineInputAssemblyStateCreateInfo ia{
        .topology = ToVkTopology(desc.topology),
    };

    vk::PipelineViewportStateCreateInfo vp{
        .viewportCount = 1,
        .scissorCount  = 1,
    };

    vk::PipelineRasterizationStateCreateInfo rs{
        .polygonMode = ToVkPolygonMode(desc.rasterizer.fill),
        .cullMode    = ToVkCull(desc.rasterizer.cull),
        .frontFace   = desc.rasterizer.frontCCW
                           ? vk::FrontFace::eCounterClockwise
                           : vk::FrontFace::eClockwise,
        .depthBiasEnable         = (desc.rasterizer.depthBias != 0
                                    || desc.rasterizer.slopeScaledDepthBias != 0.0f)
                                       ? vk::True : vk::False,
        .depthBiasConstantFactor = static_cast<f32>(desc.rasterizer.depthBias),
        .depthBiasClamp          = desc.rasterizer.depthBiasClamp,
        .depthBiasSlopeFactor    = desc.rasterizer.slopeScaledDepthBias,
        .lineWidth               = 1.0f,
    };

    vk::PipelineMultisampleStateCreateInfo ms{
        .rasterizationSamples = vk::SampleCountFlagBits::e1,
    };

    vk::PipelineDepthStencilStateCreateInfo ds{
        .depthTestEnable  = desc.depthStencil.depthTest  ? vk::True : vk::False,
        .depthWriteEnable = desc.depthStencil.depthWrite ? vk::True : vk::False,
        .depthCompareOp   = ToVkCompareOp(desc.depthStencil.depthCompare),
    };

    vk::PipelineColorBlendAttachmentState blendAttach{
        .blendEnable         = desc.blend.enable ? vk::True : vk::False,
        .srcColorBlendFactor = ToVkBlendFactor(desc.blend.srcColor),
        .dstColorBlendFactor = ToVkBlendFactor(desc.blend.dstColor),
        .colorBlendOp        = ToVkBlendOp(desc.blend.opColor),
        .srcAlphaBlendFactor = ToVkBlendFactor(desc.blend.srcAlpha),
        .dstAlphaBlendFactor = ToVkBlendFactor(desc.blend.dstAlpha),
        .alphaBlendOp        = ToVkBlendOp(desc.blend.opAlpha),
        .colorWriteMask      = desc.blend.colorWrite
                                   ? (vk::ColorComponentFlagBits::eR
                                      | vk::ColorComponentFlagBits::eG
                                      | vk::ColorComponentFlagBits::eB
                                      | vk::ColorComponentFlagBits::eA)
                                   : vk::ColorComponentFlags{},
    };
    vk::PipelineColorBlendStateCreateInfo cb{
        .attachmentCount = (desc.rtvFormat == Format::Unknown) ? 0u : 1u,
        .pAttachments    = (desc.rtvFormat == Format::Unknown) ? nullptr : &blendAttach,
    };

    const std::array<vk::DynamicState, 2> dynStates{
        vk::DynamicState::eViewport,
        vk::DynamicState::eScissor,
    };
    vk::PipelineDynamicStateCreateInfo dynState{
        .dynamicStateCount = static_cast<u32>(dynStates.size()),
        .pDynamicStates    = dynStates.data(),
    };

    // ---- Dynamic-rendering attachment formats ----
    const vk::Format rtvFmt = ToVkFormat(desc.rtvFormat);
    const vk::Format dsvFmt = ToVkFormat(desc.dsvFormat);
    vk::PipelineRenderingCreateInfo renderingInfo{
        .colorAttachmentCount    = (rtvFmt == vk::Format::eUndefined) ? 0u : 1u,
        .pColorAttachmentFormats = (rtvFmt == vk::Format::eUndefined) ? nullptr : &rtvFmt,
        .depthAttachmentFormat   = dsvFmt,
    };

    vk::GraphicsPipelineCreateInfo gpci{
        .pNext               = &renderingInfo,
        .stageCount          = stageCount,
        .pStages             = stages.data(),
        .pVertexInputState   = &vi,
        .pInputAssemblyState = &ia,
        .pViewportState      = &vp,
        .pRasterizationState = &rs,
        .pMultisampleState   = &ms,
        .pDepthStencilState  = &ds,
        .pColorBlendState    = &cb,
        .pDynamicState       = &dynState,
        .layout              = *s.pipelineLayout,
    };

    auto pR = s.device.createGraphicsPipeline(nullptr, gpci);
    if (pR.result != vk::Result::eSuccess) {
        std::fprintf(stderr, "[vk] createGraphicsPipeline failed (%s)\n",
                     vk::to_string(pR.result).c_str());
        return PipelineHandle::Invalid;
    }
    PipelineEntry e{};
    e.pipeline       = std::move(pR.value);
    e.isCompute      = false;
    return static_cast<PipelineHandle>(s.pipelines.Insert(std::move(e)));
}

PipelineHandle VulkanDevice::CreateComputePipeline(const ComputePipelineDesc&) {
    return PipelineHandle::Invalid;  // Phase 2
}

SamplerHandle VulkanDevice::CreateSampler(const SamplerDesc& desc) {
    auto& s = *state_;
    auto sR = s.device.createSampler({
        .magFilter        = ToVkFilter(desc.magFilter),
        .minFilter        = ToVkFilter(desc.minFilter),
        .mipmapMode       = (desc.minFilter == Filter::Linear)
                                ? vk::SamplerMipmapMode::eLinear
                                : vk::SamplerMipmapMode::eNearest,
        .addressModeU     = ToVkAddressMode(desc.addressU),
        .addressModeV     = ToVkAddressMode(desc.addressV),
        .addressModeW     = ToVkAddressMode(desc.addressW),
        .compareEnable    = desc.comparison ? vk::True : vk::False,
        .compareOp        = ToVkCompareOp(desc.comparisonFunc),
        .maxLod           = VK_LOD_CLAMP_NONE,
    });
    if (sR.result != vk::Result::eSuccess) return SamplerHandle::Invalid;
    SamplerEntry e{};
    e.sampler = std::move(sR.value);
    return static_cast<SamplerHandle>(s.samplers.Insert(std::move(e)));
}

// ---- Destruction --------------------------------------------------------

void VulkanDevice::Destroy(BufferHandle h) {
    auto& s = *state_;
    auto* b = s.buffers.Get(static_cast<u64>(h));
    if (!b) return;
    if (b->buffer && b->allocation)
        vmaDestroyBuffer(s.allocator, b->buffer, b->allocation);
    s.buffers.Remove(static_cast<u64>(h));
}

void VulkanDevice::Destroy(TextureHandle h) {
    auto& s = *state_;
    auto* t = s.textures.Get(static_cast<u64>(h));
    if (!t) return;
    if (t->ownsImage && t->image && t->allocation) {
        vmaDestroyImage(s.allocator, t->image, t->allocation);
    }
    s.textures.Remove(static_cast<u64>(h));
}

void VulkanDevice::Destroy(ShaderHandle h)   { state_->shaders.Remove(static_cast<u64>(h));   }
void VulkanDevice::Destroy(PipelineHandle h) { state_->pipelines.Remove(static_cast<u64>(h)); }
void VulkanDevice::Destroy(SamplerHandle h)  { state_->samplers.Remove(static_cast<u64>(h));  }

// ---- Buffer map / update ------------------------------------------------

void VulkanDevice::UpdateBuffer(BufferHandle h, const void* data, usize size) {
    auto& s = *state_;
    auto* b = s.buffers.Get(static_cast<u64>(h));
    if (!b) return;
    if (b->mapped) {
        std::memcpy(b->mapped, data, size);
        vmaFlushAllocation(s.allocator, b->allocation, 0, size);
    } else {
        // Slow path: map for the duration of the copy. Used only by
        // non-mappable buffers — the renderer's hot path uses CpuWritable
        // (persistently mapped) for everything that updates per-frame.
        void* dst = nullptr;
        if (vmaMapMemory(s.allocator, b->allocation, &dst) == VK_SUCCESS && dst) {
            std::memcpy(dst, data, size);
            vmaUnmapMemory(s.allocator, b->allocation);
            vmaFlushAllocation(s.allocator, b->allocation, 0, size);
        }
    }
}

void* VulkanDevice::MapBuffer(BufferHandle h) {
    auto& s = *state_;
    auto* b = s.buffers.Get(static_cast<u64>(h));
    if (!b) return nullptr;
    if (b->mapped) return b->mapped;          // persistently mapped
    void* p = nullptr;
    vmaMapMemory(s.allocator, b->allocation, &p);
    return p;
}

void VulkanDevice::UnmapBuffer(BufferHandle h) {
    auto& s = *state_;
    auto* b = s.buffers.Get(static_cast<u64>(h));
    if (!b) return;
    if (b->mapped) {
        // Persistently mapped — flush + leave mapped.
        vmaFlushAllocation(s.allocator, b->allocation, 0, b->desc.size);
    } else {
        vmaUnmapMemory(s.allocator, b->allocation);
    }
}

}  // namespace whiteout::flakes::gfx::vulkan

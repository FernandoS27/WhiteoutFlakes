// Instance / physical device / logical device / VMA setup, pipeline-
// cache I/O, adapter enumeration, depth format probe.

#include "vulkan_device.h"
#include "vulkan_device_state.h"
#include "vulkan_handles.h"

#if defined(TRACY_ENABLE)
#include <tracy/TracyVulkan.hpp>
#endif

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <utility>
#include <vector>

// GetPipelineCachePath is declared in gfx.h; GetPreferredDevice is
// gfx-factory-internal but the backends need it.
namespace whiteout::flakes::gfx {
const std::string& GetPreferredDevice();
} // namespace whiteout::flakes::gfx

namespace whiteout::flakes::gfx::vulkan {

std::vector<std::string> EnumerateAdapterNames() {
    std::vector<std::string> names;
    vk::raii::Context ctx;
    vk::ApplicationInfo appInfo{
        .pApplicationName = "WhiteoutFlakes",
        .apiVersion = VK_API_VERSION_1_3,
    };
    vk::InstanceCreateInfo ici{.pApplicationInfo = &appInfo};
    auto instResult = ctx.createInstance(ici);
    if (instResult.result != vk::Result::eSuccess)
        return names;
    vk::raii::Instance instance = std::move(instResult.value);
    auto [r, pds] = instance.enumeratePhysicalDevices();
    if (r != vk::Result::eSuccess)
        return names;
    for (const auto& pending : pds) {
        // Match what CreateDevice can actually open (1.3+).
        const auto props = pending.getProperties();
        if (VK_API_VERSION_MAJOR(props.apiVersion) < 1 ||
            (VK_API_VERSION_MAJOR(props.apiVersion) == 1 &&
             VK_API_VERSION_MINOR(props.apiVersion) < 3))
            continue;
        names.emplace_back(props.deviceName.data());
    }
    return names;
}

void LoadPipelineCache(VulkanDeviceState& state) {
    std::vector<u8> blob;
    if (!state.pipelineCachePath.empty()) {
        std::ifstream f(state.pipelineCachePath, std::ios::binary | std::ios::ate);
        if (f) {
            const auto bytes = static_cast<usize>(f.tellg());
            f.seekg(0);
            blob.resize(bytes);
            f.read(reinterpret_cast<char*>(blob.data()), static_cast<std::streamsize>(bytes));
        }
    }
    vk::PipelineCacheCreateInfo ci{
        .initialDataSize = blob.size(),
        .pInitialData = blob.empty() ? nullptr : blob.data(),
    };
    auto r = state.device.createPipelineCache(ci);
    if (r.result == vk::Result::eSuccess) {
        state.pipelineCache = std::move(r.value);
    }
}

void SavePipelineCache(VulkanDeviceState& state) {
    if (!*state.pipelineCache || state.pipelineCachePath.empty())
        return;
    auto [r, blob] = state.pipelineCache.getData();
    if (r != vk::Result::eSuccess || blob.empty())
        return;
    std::ofstream f(state.pipelineCachePath, std::ios::binary | std::ios::trunc);
    if (!f)
        return;
    f.write(reinterpret_cast<const char*>(blob.data()), static_cast<std::streamsize>(blob.size()));
}

namespace {

VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT /*types*/,
    const VkDebugUtilsMessengerCallbackDataEXT* callbackData, void* /*userData*/) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        std::fprintf(stderr, "[vk] ERR: %s\n", callbackData->pMessage);
    } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        std::fprintf(stderr, "[vk] WARN: %s\n", callbackData->pMessage);
    }
    return VK_FALSE;
}

bool HasInstanceLayer(const vk::raii::Context& ctx, const char* name) {
    auto [r, layers] = ctx.enumerateInstanceLayerProperties();
    if (r != vk::Result::eSuccess)
        return false;
    for (const auto& l : layers) {
        if (std::strcmp(l.layerName, name) == 0)
            return true;
    }
    return false;
}

bool HasInstanceExtension(const vk::raii::Context& ctx, const char* name) {
    auto [r, exts] = ctx.enumerateInstanceExtensionProperties();
    if (r != vk::Result::eSuccess)
        return false;
    for (const auto& e : exts) {
        if (std::strcmp(e.extensionName, name) == 0)
            return true;
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
    const i32 typeBonus = (props.deviceType == vk::PhysicalDeviceType::eDiscreteGpu) ? 1000 : 0;
    return typeBonus + vramScore;
}

bool DeviceSupportsExtensions(const vk::raii::PhysicalDevice& pd,
                              const std::vector<const char*>& required) {
    auto [r, avail] = pd.enumerateDeviceExtensionProperties();
    if (r != vk::Result::eSuccess)
        return false;
    auto has = [&](const char* name) {
        for (const auto& e : avail) {
            if (std::strcmp(e.extensionName, name) == 0)
                return true;
        }
        return false;
    };
    for (const char* ext : required) {
        if (!has(ext))
            return false;
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

// Prefers DMA-only (no graphics, no compute); falls back to transfer-
// only with compute. -1 if no transfer-only family exists.
i32 PickDedicatedTransferQueueFamily(const vk::raii::PhysicalDevice& pd) {
    const auto fams = pd.getQueueFamilyProperties();
    i32 fallback = -1;
    for (u32 i = 0; i < fams.size(); ++i) {
        const auto flags = fams[i].queueFlags;
        const bool hasTransfer = bool(flags & vk::QueueFlagBits::eTransfer);
        const bool hasGraphics = bool(flags & vk::QueueFlagBits::eGraphics);
        const bool hasCompute = bool(flags & vk::QueueFlagBits::eCompute);
        if (!hasTransfer || hasGraphics)
            continue;
        if (!hasCompute)
            return static_cast<i32>(i);
        if (fallback < 0)
            fallback = static_cast<i32>(i);
    }
    return fallback;
}

bool CreateTimelineSemaphore(const vk::raii::Device& device, vk::raii::Semaphore& out,
                             const char* what) {
    vk::SemaphoreTypeCreateInfo typeInfo{
        .semaphoreType = vk::SemaphoreType::eTimeline,
        .initialValue = 0,
    };
    auto r = device.createSemaphore({.pNext = &typeInfo});
    if (r.result != vk::Result::eSuccess) {
        std::fprintf(stderr, "[vk] createSemaphore (%s) failed (%s)\n", what,
                     vk::to_string(r.result).c_str());
        return false;
    }
    out = std::move(r.value);
    return true;
}

// ---- Init phases --------------------------------------------------------

bool CreateInstance(VulkanDeviceState& state, bool enableValidation) {
    vk::ApplicationInfo appInfo{
        .pApplicationName = "WhiteoutFlakes",
        .applicationVersion = VK_MAKE_VERSION(0, 1, 0),
        .pEngineName = "WhiteoutFlakes",
        .engineVersion = VK_MAKE_VERSION(0, 1, 0),
        .apiVersion = VK_API_VERSION_1_3,
    };

    std::vector<const char*> instExts = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
    };
    std::vector<const char*> instLayers;

    // Validation needs both the Khronos layer (Vulkan SDK) and the
    // debug-utils ext; silently no-op if either is missing.
    const bool wantValidation = enableValidation &&
                                HasInstanceLayer(state.ctx, "VK_LAYER_KHRONOS_validation") &&
                                HasInstanceExtension(state.ctx, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    if (wantValidation) {
        instLayers.push_back("VK_LAYER_KHRONOS_validation");
        instExts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }

    auto r = state.ctx.createInstance({
        .pApplicationInfo = &appInfo,
        .enabledLayerCount = static_cast<u32>(instLayers.size()),
        .ppEnabledLayerNames = instLayers.data(),
        .enabledExtensionCount = static_cast<u32>(instExts.size()),
        .ppEnabledExtensionNames = instExts.data(),
    });
    if (r.result != vk::Result::eSuccess) {
        std::fprintf(stderr, "[vk] createInstance failed (%s)\n", vk::to_string(r.result).c_str());
        return false;
    }
    state.instance = std::move(r.value);

    if (wantValidation) {
        auto dbgR = state.instance.createDebugUtilsMessengerEXT({
            .messageSeverity = vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning |
                               vk::DebugUtilsMessageSeverityFlagBitsEXT::eError,
            .messageType = vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral |
                           vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation |
                           vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance,
            .pfnUserCallback =
                reinterpret_cast<vk::PFN_DebugUtilsMessengerCallbackEXT>(DebugCallback),
        });
        if (dbgR.result == vk::Result::eSuccess) {
            state.debugMessenger = std::move(dbgR.value);
        }
    }
    return true;
}

bool PickPhysicalDevice(VulkanDeviceState& state, std::string& deviceNameOut) {
    auto [r, pds] = state.instance.enumeratePhysicalDevices();
    if (r != vk::Result::eSuccess || pds.empty()) {
        std::fprintf(stderr, "[vk] no Vulkan physical devices\n");
        return false;
    }

    const std::vector<const char*> requiredExts = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        VK_KHR_SWAPCHAIN_MUTABLE_FORMAT_EXTENSION_NAME,
        VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME,
    };

    // Preferred-name match overrides scoring (discrete > integrated, ties by VRAM).
    const std::string& preferred = gfx::GetPreferredDevice();
    i32 bestScore = -1;
    for (auto& pending : pds) {
        if (!DeviceSupportsExtensions(pending, requiredExts))
            continue;
        if (PickGraphicsQueueFamily(pending) < 0)
            continue;
        if (!preferred.empty() &&
            preferred == std::string(pending.getProperties().deviceName.data())) {
            state.physicalDevice = std::move(pending);
            break;
        }
        const i32 score = ScoreDevice(pending);
        if (score > bestScore) {
            bestScore = score;
            state.physicalDevice = std::move(pending);
        }
    }
    if (!*state.physicalDevice) {
        std::fprintf(stderr, "[vk] no physical device meets requirements "
                             "(Vulkan 1.3 + swapchain + swapchain_mutable_format)\n");
        return false;
    }

    const auto props = state.physicalDevice.getProperties();
    deviceNameOut = props.deviceName.data();
    state.queueFamily = static_cast<u32>(PickGraphicsQueueFamily(state.physicalDevice));
    state.minUniformBufferAlign = props.limits.minUniformBufferOffsetAlignment;

    const i32 tf = PickDedicatedTransferQueueFamily(state.physicalDevice);
    state.hasAsyncTransfer = (tf >= 0 && static_cast<u32>(tf) != state.queueFamily);
    state.transferQueueFamily = state.hasAsyncTransfer ? static_cast<u32>(tf) : state.queueFamily;
    return true;
}

bool CreateLogicalDevice(VulkanDeviceState& state) {
    const f32 queuePriority = 1.0f;
    std::array<vk::DeviceQueueCreateInfo, 2> qcis{};
    qcis[0] = vk::DeviceQueueCreateInfo{
        .queueFamilyIndex = state.queueFamily,
        .queueCount = 1,
        .pQueuePriorities = &queuePriority,
    };
    u32 qciCount = 1;
    if (state.hasAsyncTransfer) {
        qcis[1] = vk::DeviceQueueCreateInfo{
            .queueFamilyIndex = state.transferQueueFamily,
            .queueCount = 1,
            .pQueuePriorities = &queuePriority,
        };
        qciCount = 2;
    }

    // shaderDrawParameters: capability slangc emits for SV_*ID.
    // timelineSemaphore: powers the deferred-delete queue.
    vk::PhysicalDeviceVulkan11Features vk11{.shaderDrawParameters = vk::True};
    vk::PhysicalDeviceVulkan12Features vk12{.pNext = &vk11, .timelineSemaphore = vk::True};
    vk::PhysicalDeviceVulkan13Features vk13{
        .pNext = &vk12,
        .synchronization2 = vk::True,
        .dynamicRendering = vk::True,
    };
    vk::PhysicalDeviceFeatures coreFeatures{
        .imageCubeArray = vk::True, // for IBL probes
    };

    const std::array<const char*, 3> deviceExts = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        VK_KHR_SWAPCHAIN_MUTABLE_FORMAT_EXTENSION_NAME,
        VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME,
    };

    auto r = state.physicalDevice.createDevice({
        .pNext = &vk13,
        .queueCreateInfoCount = qciCount,
        .pQueueCreateInfos = qcis.data(),
        .enabledExtensionCount = static_cast<u32>(deviceExts.size()),
        .ppEnabledExtensionNames = deviceExts.data(),
        .pEnabledFeatures = &coreFeatures,
    });
    if (r.result != vk::Result::eSuccess) {
        std::fprintf(stderr, "[vk] createDevice failed (%s)\n", vk::to_string(r.result).c_str());
        return false;
    }
    state.device = std::move(r.value);
    state.queue = state.device.getQueue(state.queueFamily, 0);
    state.transferQueue = state.hasAsyncTransfer
                              ? state.device.getQueue(state.transferQueueFamily, 0)
                              : state.device.getQueue(state.queueFamily, 0);
    return true;
}

bool CreateAllocator(VulkanDeviceState& state) {
    VmaAllocatorCreateInfo aci{};
    aci.physicalDevice = *state.physicalDevice;
    aci.device = *state.device;
    aci.instance = *state.instance;
    aci.vulkanApiVersion = VK_API_VERSION_1_3;
    if (vmaCreateAllocator(&aci, &state.allocator) != VK_SUCCESS) {
        std::fprintf(stderr, "[vk] vmaCreateAllocator failed\n");
        return false;
    }
    return true;
}

// Non-fatal: CreateBuffer falls back to per-CB VMA allocations.
void CreateSharedCbRing(VulkanDeviceState& state) {
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = VulkanDeviceState::kSharedCbCapacity;
    bci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo cbaci{};
    cbaci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
    cbaci.flags =
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(state.allocator, &bci, &cbaci, &state.sharedCbBuffer,
                        &state.sharedCbAllocation, &info) == VK_SUCCESS) {
        state.sharedCbMapped = info.pMappedData;
        state.sharedCbCursor = 0;
    } else {
        std::fprintf(stderr, "[vk] shared CB ring allocation failed; "
                             "falling back to per-CB buffers\n");
    }
}

bool CreatePipelineLayout(VulkanDeviceState& state) {
    const auto bothStages = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment;

    auto buildSet = [&](vk::DescriptorType type, u32 count, bool pushDescriptor,
                        vk::raii::DescriptorSetLayout& out, const char* what) -> bool {
        std::vector<vk::DescriptorSetLayoutBinding> bindings;
        bindings.reserve(count);
        for (u32 i = 0; i < count; ++i) {
            bindings.push_back({
                .binding = i,
                .descriptorType = type,
                .descriptorCount = 1,
                .stageFlags = bothStages,
            });
        }
        const auto flags = pushDescriptor
                               ? vk::DescriptorSetLayoutCreateFlags(
                                     vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR)
                               : vk::DescriptorSetLayoutCreateFlags{};
        auto r = state.device.createDescriptorSetLayout({
            .flags = flags,
            .bindingCount = static_cast<u32>(bindings.size()),
            .pBindings = bindings.data(),
        });
        if (r.result != vk::Result::eSuccess) {
            std::fprintf(stderr, "[vk] createDescriptorSetLayout (%s) failed (%s)\n", what,
                         vk::to_string(r.result).c_str());
            return false;
        }
        out = std::move(r.value);
        return true;
    };

    if (!buildSet(vk::DescriptorType::eUniformBuffer, kCbBindingCount,
                  /*pushDescriptor=*/true, state.cbSetLayout, "CB"))
        return false;
    if (!buildSet(vk::DescriptorType::eSampledImage, kSrvBindingCount, false, state.srvSetLayout,
                  "SRV"))
        return false;
    if (!buildSet(vk::DescriptorType::eSampler, kSamplerBindingCount, false, state.samplerSetLayout,
                  "Sampler"))
        return false;

    const std::array<VkDescriptorSetLayout, 3> rawSets = {
        *state.cbSetLayout,
        *state.srvSetLayout,
        *state.samplerSetLayout,
    };
    auto plR = state.device.createPipelineLayout({
        .setLayoutCount = static_cast<u32>(rawSets.size()),
        .pSetLayouts = reinterpret_cast<const vk::DescriptorSetLayout*>(rawSets.data()),
    });
    if (plR.result != vk::Result::eSuccess) {
        std::fprintf(stderr, "[vk] createPipelineLayout failed (%s)\n",
                     vk::to_string(plR.result).c_str());
        return false;
    }
    state.pipelineLayout = std::move(plR.value);
    return true;
}

bool CreateTransferQueueObjects(VulkanDeviceState& state) {
    auto poolR = state.device.createCommandPool({
        .flags = vk::CommandPoolCreateFlagBits::eTransient,
        .queueFamilyIndex = state.transferQueueFamily,
    });
    if (poolR.result != vk::Result::eSuccess) {
        std::fprintf(stderr, "[vk] createCommandPool (transfer) failed (%s)\n",
                     vk::to_string(poolR.result).c_str());
        return false;
    }
    state.transferCommandPool = std::move(poolR.value);
    return CreateTimelineSemaphore(state.device, state.transferTimelineSem, "transfer timeline");
}

bool CreatePerFrameContexts(VulkanDeviceState& state) {
    // SRV + sampler pool sized for worst-case BLS frames; reset per frame.
    constexpr u32 kSetsPerDraw = 2;
    constexpr u32 kMaxDrawsPerFrame = 4096;
    constexpr u32 kMaxSetsPerFrame = kMaxDrawsPerFrame * kSetsPerDraw;
    constexpr u32 kMaxSrvsPerFrame = kMaxDrawsPerFrame * kSrvBindingCount;
    constexpr u32 kMaxSamplersPerFrame = kMaxDrawsPerFrame * kSamplerBindingCount;
    const std::array<vk::DescriptorPoolSize, 2> poolSizes = {
        vk::DescriptorPoolSize{vk::DescriptorType::eSampledImage, kMaxSrvsPerFrame},
        vk::DescriptorPoolSize{vk::DescriptorType::eSampler, kMaxSamplersPerFrame},
    };

    for (u32 i = 0; i < kFramesInFlight; ++i) {
        auto& frame = state.frames[i];

        auto poolR = state.device.createCommandPool({
            .flags = vk::CommandPoolCreateFlagBits::eTransient |
                     vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
            .queueFamilyIndex = state.queueFamily,
        });
        if (poolR.result != vk::Result::eSuccess) {
            std::fprintf(stderr, "[vk] createCommandPool failed (frame %u)\n", i);
            return false;
        }
        frame.commandPool = std::move(poolR.value);

        auto cbsR = state.device.allocateCommandBuffers({
            .commandPool = *frame.commandPool,
            .level = vk::CommandBufferLevel::ePrimary,
            .commandBufferCount = 1,
        });
        if (cbsR.result != vk::Result::eSuccess) {
            std::fprintf(stderr, "[vk] allocateCommandBuffers failed (frame %u)\n", i);
            return false;
        }
        frame.commandBuffer = std::move(cbsR.value[0]);

        auto fenceR = state.device.createFence({.flags = vk::FenceCreateFlagBits::eSignaled});
        if (fenceR.result != vk::Result::eSuccess) {
            std::fprintf(stderr, "[vk] createFence failed (frame %u)\n", i);
            return false;
        }
        frame.inFlightFence = std::move(fenceR.value);

        auto poolR2 = state.device.createDescriptorPool({
            .maxSets = kMaxSetsPerFrame,
            .poolSizeCount = static_cast<u32>(poolSizes.size()),
            .pPoolSizes = poolSizes.data(),
        });
        if (poolR2.result != vk::Result::eSuccess) {
            std::fprintf(stderr, "[vk] createDescriptorPool failed (frame %u): %s\n", i,
                         vk::to_string(poolR2.result).c_str());
            return false;
        }
        frame.descriptorPool = std::move(poolR2.value);
    }
    return true;
}

#if defined(TRACY_ENABLE)
void InitTracyContext(VulkanDeviceState& state) {
    auto& frame0 = state.frames[0];
    auto cbR = state.device.allocateCommandBuffers({
        .commandPool = *frame0.commandPool,
        .level = vk::CommandBufferLevel::ePrimary,
        .commandBufferCount = 1,
    });
    if (cbR.result != vk::Result::eSuccess || cbR.value.empty())
        return;
    auto& calibCb = cbR.value.front();
    (void)calibCb.begin({.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
    state.tracyCtx = TracyVkContext(
        static_cast<VkPhysicalDevice>(*state.physicalDevice), static_cast<VkDevice>(*state.device),
        static_cast<VkQueue>(*state.queue), static_cast<VkCommandBuffer>(*calibCb));
    (void)calibCb.end();
    vk::CommandBufferSubmitInfo csi{.commandBuffer = *calibCb};
    vk::SubmitInfo2 si{.commandBufferInfoCount = 1, .pCommandBufferInfos = &csi};
    (void)state.queue.submit2(si);
    state.queue.waitIdle();
}
#endif

} // namespace

bool VulkanDevice::Init(bool enableValidation) {
    auto& state = *state_;

    if (!CreateInstance(state, enableValidation))
        return false;
    if (!PickPhysicalDevice(state, deviceName_))
        return false;
    if (!CreateLogicalDevice(state))
        return false;
    if (!CreateAllocator(state))
        return false;
    CreateSharedCbRing(state); // non-fatal
    if (!CreatePipelineLayout(state))
        return false;

    state.pipelineCachePath = gfx::GetPipelineCachePath();
    LoadPipelineCache(state);

    if (!CreateTimelineSemaphore(state.device, state.timelineSem, "timeline"))
        return false;
    state.nextSubmitValue = 1;

    if (!CreateTransferQueueObjects(state))
        return false;
    if (!CreatePerFrameContexts(state))
        return false;

    const auto props = state.physicalDevice.getProperties();
    std::printf("[vk] device='%s' api=%u.%u.%u queueFamily=%u\n", deviceName_.c_str(),
                VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion),
                VK_API_VERSION_PATCH(props.apiVersion), state.queueFamily);

#if defined(TRACY_ENABLE)
    InitTracyContext(state);
#endif
    return true;
}

Format VulkanDevice::PreferredDepthStencilFormat() const {
    auto& state = *state_;
    auto supported = [&](vk::Format f) {
        const auto p = state.physicalDevice.getFormatProperties(f);
        return bool(p.optimalTilingFeatures & vk::FormatFeatureFlagBits::eDepthStencilAttachment);
    };
    // D24_S8 is unsupported on AMD; D32_S8 is the portable fallback.
    if (supported(vk::Format::eD24UnormS8Uint))
        return Format::D24_UNORM_S8_UINT;
    if (supported(vk::Format::eD32SfloatS8Uint))
        return Format::D32_FLOAT_S8_UINT;
    return Format::D32_FLOAT_S8_UINT;
}

} // namespace whiteout::flakes::gfx::vulkan

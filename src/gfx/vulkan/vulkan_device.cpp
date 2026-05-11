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
#include <filesystem>
#include <fstream>
#include <vector>

// Forward-declared in the parent gfx namespace — defined in
// gfx_factory.cpp. Lets the Vulkan device pick up the host-supplied
// pipeline-cache path without anyone here calling platform APIs.
namespace whiteout::flakes::gfx {
const std::filesystem::path& GetPipelineCachePath();
}

namespace whiteout::flakes::gfx::vulkan {

// VulkanDeviceState lives in vulkan_resources.h.

void LoadPipelineCache(VulkanDeviceState& s) {
    // The host hands us a filesystem path through `s.pipelineCachePath`
    // (typically set by gfx::SetPipelineCachePath before CreateDevice).
    // Empty path → create an empty in-memory cache; we just don't
    // persist between runs. The gfx layer stays platform-agnostic and
    // never touches GetModuleFileName / readlink / etc.
    std::vector<u8> blob;
    if (!s.pipelineCachePath.empty()) {
        std::error_code ec;
        if (std::filesystem::exists(s.pipelineCachePath, ec) && !ec) {
            std::ifstream f(s.pipelineCachePath, std::ios::binary);
            if (f) {
                f.seekg(0, std::ios::end);
                const auto sz = static_cast<std::streamsize>(f.tellg());
                if (sz > 0) {
                    blob.resize(static_cast<usize>(sz));
                    f.seekg(0, std::ios::beg);
                    f.read(reinterpret_cast<char*>(blob.data()), sz);
                    if (!f) blob.clear();  // partial read — start empty
                }
            }
        }
    }

    vk::PipelineCacheCreateInfo ci{
        .initialDataSize = blob.size(),
        .pInitialData    = blob.empty() ? nullptr : blob.data(),
    };
    auto r = s.device.createPipelineCache(ci);
    if (r.result != vk::Result::eSuccess) {
        // Driver rejected the rehydrated blob (UUID mismatch — e.g.
        // driver update). Retry empty so we at least warm the cache
        // during this run.
        ci.initialDataSize = 0;
        ci.pInitialData    = nullptr;
        r = s.device.createPipelineCache(ci);
    }
    if (r.result == vk::Result::eSuccess) {
        s.pipelineCache = std::move(r.value);
    }
}

void SavePipelineCache(VulkanDeviceState& s) {
    if (!*s.pipelineCache || s.pipelineCachePath.empty()) return;
    auto [r, blob] = s.pipelineCache.getData();
    if (r != vk::Result::eSuccess || blob.empty()) return;
    std::ofstream f(s.pipelineCachePath, std::ios::binary | std::ios::trunc);
    if (!f) return;
    f.write(reinterpret_cast<const char*>(blob.data()),
            static_cast<std::streamsize>(blob.size()));
}

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

// Look for a queue family that supports transfer but NOT graphics —
// that's the dedicated DMA / SDMA queue on discrete NVIDIA (Maxwell 2+)
// / AMD (GCN+) GPUs, and on most AMD APUs. Intel integrated, mobile,
// and MoltenVK typically expose only one universal family with all
// bits set; in that case the caller falls back to `graphicsFamily`
// and submits transfers on the graphics queue. Either way the upload
// code path stays the same; only the queue + sync changes.
//
// Preference order:
//   1. TRANSFER && !GRAPHICS && !COMPUTE  — purest DMA queue
//   2. TRANSFER && !GRAPHICS              — transfer-only (may also
//                                            do compute)
// Returns -1 if no transfer-only family exists.
i32 PickDedicatedTransferQueueFamily(const vk::raii::PhysicalDevice& pd) {
    const auto fams = pd.getQueueFamilyProperties();
    i32 fallback = -1;
    for (u32 i = 0; i < fams.size(); ++i) {
        const auto flags = fams[i].queueFlags;
        const bool hasTransfer = bool(flags & vk::QueueFlagBits::eTransfer);
        const bool hasGraphics = bool(flags & vk::QueueFlagBits::eGraphics);
        const bool hasCompute  = bool(flags & vk::QueueFlagBits::eCompute);
        if (!hasTransfer || hasGraphics) continue;
        if (!hasCompute) return static_cast<i32>(i);  // ideal: DMA-only
        if (fallback < 0) fallback = static_cast<i32>(i);
    }
    return fallback;
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
        // Persist the warmed-up pipeline cache before we tear the
        // device down. waitIdle above ensures no in-flight pipeline
        // build is still touching the cache.
        SavePipelineCache(s);
    }

    // Run every queued deleter — after waitIdle, all timeline values
    // are safe and the GPU has stopped touching anything. Manually
    // bump `pendingDeletes` lambdas through; we can't go through the
    // normal Drain because it gates on getSemaphoreCounterValue and
    // we want unconditional teardown here.
    for (auto& pd : s.pendingDeletes) pd.deleter->Run(s);
    s.pendingDeletes.clear();
    // Same unconditional drain for transfer-side deletes — waitIdle
    // above guarantees the transfer queue is idle too.
    for (auto& pd : s.pendingTransferDeletes) pd.deleter->Run(s);
    s.pendingTransferDeletes.clear();

    // Tear down whatever's still live in the slot maps (resources the
    // renderer never explicitly destroyed). Slot-map entries hold raw
    // VkXxx + VmaAllocation; raii fields auto-destroy on Clear().
    // Shared CB ring sub-allocs have allocation==VK_NULL_HANDLE — the
    // guard below skips them; the ring itself is freed once after the
    // ForEach completes (after the buffers slot-map is no longer
    // holding aliases of sharedCbBuffer).
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

    if (s.sharedCbBuffer && s.sharedCbAllocation) {
        vmaDestroyBuffer(s.allocator, s.sharedCbBuffer, s.sharedCbAllocation);
        s.sharedCbBuffer     = VK_NULL_HANDLE;
        s.sharedCbAllocation = VK_NULL_HANDLE;
        s.sharedCbMapped     = nullptr;
    }

    if (s.allocator) {
        vmaDestroyAllocator(s.allocator);
        s.allocator = VK_NULL_HANDLE;
    }
    // raii teardown for the rest happens automatically in member dtor order.
}

VulkanDeviceState&       VulkanDevice::State()       { return *state_; }
const VulkanDeviceState& VulkanDevice::State() const { return *state_; }

bool VulkanDevice::Init(bool enableValidation) {
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

    // GraphicsDebug setting (host-side, persisted to .ini) gates the
    // Khronos validation layer + debug-utils extension. We still
    // require both to be available — the layer ships with the Vulkan
    // SDK, not the runtime, so on a clean end-user box the request
    // silently no-ops rather than failing instance creation.
    const bool wantValidation = enableValidation &&
        HasInstanceLayer(s.ctx, "VK_LAYER_KHRONOS_validation") &&
        HasInstanceExtension(s.ctx, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    if (wantValidation) {
        instLayers.push_back("VK_LAYER_KHRONOS_validation");
        instExts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }

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
        // Required to set VkSwapchainCreateInfo::flags to MUTABLE_FORMAT
        // — pairs with VkImageFormatListCreateInfo so a single swap-
        // chain image can have both sRGB and linear views.
        VK_KHR_SWAPCHAIN_MUTABLE_FORMAT_EXTENSION_NAME,
        // Push descriptors: lets FlushDescriptors record bindings into
        // the command buffer directly via vkCmdPushDescriptorSetKHR
        // instead of allocating + updating + binding a fresh
        // VkDescriptorSet from a per-frame pool on every draw. The
        // extension shipped in 2016 and is in core 1.4; every driver
        // we target (NVIDIA Maxwell+, AMD GCN+, Intel Skylake+, MoltenVK)
        // exposes it, so we make it a hard requirement rather than a
        // runtime opt-in.
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
                     "(Vulkan 1.3 + swapchain + swapchain_mutable_format)\n");
        return false;
    }
    const auto props = s.physicalDevice.getProperties();
    deviceName_ = props.deviceName.data();
    s.queueFamily = static_cast<u32>(PickGraphicsQueueFamily(s.physicalDevice));
    // Per-CB ring slot stride must be aligned to the device's
    // minUniformBufferOffsetAlignment (commonly 64 on NVIDIA, 256 on
    // AMD/Intel; Vulkan spec guarantees ≤ 256). Cached here once.
    s.minUniformBufferAlign = props.limits.minUniformBufferOffsetAlignment;

    // Async transfer queue probe. -1 from PickDedicatedTransferQueueFamily
    // means "no transfer-only family found" — Intel iGPUs, mobile GPUs,
    // MoltenVK all hit this and we fall through to the graphics family
    // (uploads still happen, just on the same queue as the renderer).
    const i32 tf = PickDedicatedTransferQueueFamily(s.physicalDevice);
    s.hasAsyncTransfer    = (tf >= 0 && static_cast<u32>(tf) != s.queueFamily);
    s.transferQueueFamily = s.hasAsyncTransfer ? static_cast<u32>(tf)
                                                : s.queueFamily;

    // ---- 3. Logical device + dynamic-rendering / sync2 features -----------
    const f32 queuePriority = 1.0f;
    std::array<vk::DeviceQueueCreateInfo, 2> qcis{};
    qcis[0] = vk::DeviceQueueCreateInfo{
        .queueFamilyIndex = s.queueFamily,
        .queueCount       = 1,
        .pQueuePriorities = &queuePriority,
    };
    u32 qciCount = 1;
    if (s.hasAsyncTransfer) {
        qcis[1] = vk::DeviceQueueCreateInfo{
            .queueFamilyIndex = s.transferQueueFamily,
            .queueCount       = 1,
            .pQueuePriorities = &queuePriority,
        };
        qciCount = 2;
    }

    // Vulkan 1.1 feature struct — `shaderDrawParameters` satisfies the
    // SPIR-V DrawParameters capability that slangc emits for any shader
    // using SV_VertexID / SV_InstanceID-derived built-ins (which is most
    // of the BLS catalog).
    vk::PhysicalDeviceVulkan11Features vk11{
        .shaderDrawParameters = vk::True,
    };

    // Vulkan 1.2 feature struct — needed for `timelineSemaphore`, which
    // gates the deferred-delete queue (see PendingDelete). The 1.3 struct
    // chains onto this one's pNext.
    vk::PhysicalDeviceVulkan12Features vk12{
        .pNext             = &vk11,
        .timelineSemaphore = vk::True,
    };

    vk::PhysicalDeviceVulkan13Features vk13{
        .pNext            = &vk12,
        .synchronization2 = vk::True,
        .dynamicRendering = vk::True,
    };

    vk::PhysicalDeviceFeatures coreFeatures{
        // Required for VK_IMAGE_VIEW_TYPE_CUBE_ARRAY (IBL probes etc).
        .imageCubeArray = vk::True,
    };

    vk::DeviceCreateInfo dci{
        .pNext                   = &vk13,
        .queueCreateInfoCount    = qciCount,
        .pQueueCreateInfos       = qcis.data(),
        .enabledExtensionCount   = static_cast<u32>(deviceExts.size()),
        .ppEnabledExtensionNames = deviceExts.data(),
        .pEnabledFeatures        = &coreFeatures,
    };

    auto deviceResult = s.physicalDevice.createDevice(dci);
    if (deviceResult.result != vk::Result::eSuccess) {
        std::fprintf(stderr, "[vk] createDevice failed (%s)\n",
                     vk::to_string(deviceResult.result).c_str());
        return false;
    }
    s.device = std::move(deviceResult.value);
    s.queue  = s.device.getQueue(s.queueFamily, 0);
    s.transferQueue = s.hasAsyncTransfer
        ? s.device.getQueue(s.transferQueueFamily, 0)
        : s.device.getQueue(s.queueFamily, 0);  // alias the graphics queue

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

    // ---- 4b. Shared CpuWritable CB ring ---------------------------------
    // One persistently-mapped host-visible VkBuffer backs every
    // CpuWritable CreateBuffer (per-CB ring slots are sub-allocs into
    // this). Usage flags are a superset of anything ToVkBufferUsage
    // can produce so any CpuWritable shape — UBO, VB, IB, SSBO — can
    // sub-allocate from here. Failing the create is non-fatal —
    // CreateBuffer falls back to per-CB VMA allocations.
    {
        VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bci.size  = VulkanDeviceState::kSharedCbCapacity;
        bci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT
                  | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
                  | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT
                  | VK_BUFFER_USAGE_INDEX_BUFFER_BIT
                  | VK_BUFFER_USAGE_TRANSFER_SRC_BIT
                  | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VmaAllocationCreateInfo cbaci{};
        cbaci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
        cbaci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
                    | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo info{};
        if (vmaCreateBuffer(s.allocator, &bci, &cbaci,
                            &s.sharedCbBuffer, &s.sharedCbAllocation, &info)
                == VK_SUCCESS) {
            s.sharedCbMapped = info.pMappedData;
            s.sharedCbCursor = 0;
        } else {
            std::fprintf(stderr, "[vk] shared CB ring allocation failed; "
                                 "falling back to per-CB buffers\n");
        }
    }

    // ---- 5. Shared pipeline layout + per-type descriptor set layouts -----
    // Three set layouts, one per resource class — Slang's SPIR-V emit
    // (with [[vk::binding(N, S)]] annotations on every declaration) puts
    // CBs in set 0, SRVs in set 1, and samplers in set 2.
    //
    // Push descriptors aren't usable here: VUID-VkPipelineLayoutCreateInfo-
    // pSetLayouts-00293 forbids more than one push-descriptor set per
    // pipeline layout, and we have three. Instead we allocate transient
    // sets out of a per-frame descriptor pool and free everything at
    // frame start by resetting the pool.
    {
        const auto bothStages = vk::ShaderStageFlagBits::eVertex
                              | vk::ShaderStageFlagBits::eFragment;

        auto buildSet = [&](vk::DescriptorType type, u32 count,
                            vk::raii::DescriptorSetLayout& outLayout,
                            const char* what) -> bool {
            std::vector<vk::DescriptorSetLayoutBinding> bindings;
            bindings.reserve(count);
            for (u32 i = 0; i < count; ++i) {
                bindings.push_back({
                    .binding         = i,
                    .descriptorType  = type,
                    .descriptorCount = 1,
                    .stageFlags      = bothStages,
                });
            }
            // PUSH_DESCRIPTOR_BIT_KHR makes the set eligible for
            // vkCmdPushDescriptorSetKHR (no allocation, no
            // vkUpdateDescriptorSets, just inline writes into the
            // command buffer). Vulkan caps it to ONE push set per
            // pipeline layout (VUID-VkPipelineLayoutCreateInfo-
            // pSetLayouts-00293), and our CB set is the hot one — it
            // gets rewritten on every Bind because of the per-frame
            // ring-buffer offset, while SRVs and samplers only flip
            // when the renderer actually changes the bound texture/
            // sampler. So set 0 is push; sets 1/2 stay on the pool.
            const auto flags = (&outLayout == &s.cbSetLayout)
                ? vk::DescriptorSetLayoutCreateFlags(
                      vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR)
                : vk::DescriptorSetLayoutCreateFlags{};
            auto r = s.device.createDescriptorSetLayout({
                .flags        = flags,
                .bindingCount = static_cast<u32>(bindings.size()),
                .pBindings    = bindings.data(),
            });
            if (r.result != vk::Result::eSuccess) {
                std::fprintf(stderr,
                             "[vk] createDescriptorSetLayout (%s) failed (%s)\n",
                             what, vk::to_string(r.result).c_str());
                return false;
            }
            outLayout = std::move(r.value);
            return true;
        };

        if (!buildSet(vk::DescriptorType::eUniformBuffer, kCbBindingCount,
                      s.cbSetLayout, "CB")) return false;
        if (!buildSet(vk::DescriptorType::eSampledImage, kSrvBindingCount,
                      s.srvSetLayout, "SRV")) return false;
        if (!buildSet(vk::DescriptorType::eSampler, kSamplerBindingCount,
                      s.samplerSetLayout, "Sampler")) return false;

        const std::array<VkDescriptorSetLayout, 3> rawSets = {
            *s.cbSetLayout, *s.srvSetLayout, *s.samplerSetLayout,
        };
        auto plR = s.device.createPipelineLayout({
            .setLayoutCount = static_cast<u32>(rawSets.size()),
            .pSetLayouts    = reinterpret_cast<const vk::DescriptorSetLayout*>(rawSets.data()),
        });
        if (plR.result != vk::Result::eSuccess) {
            std::fprintf(stderr, "[vk] createPipelineLayout failed (%s)\n",
                         vk::to_string(plR.result).c_str());
            return false;
        }
        s.pipelineLayout = std::move(plR.value);
    }

    // ---- 5b. Pipeline cache (rehydrated from disk) -----------------------
    // Persist the driver pipeline cache across runs. The blob carries a
    // driver/device UUID prefix; if the user upgrades their driver or
    // moves the .ini between machines the driver rejects the stale blob
    // and we start with an empty cache, so we don't need any validation
    // here. Path resolution is a host concern (see
    // gfx::SetPipelineCachePath in gfx_factory.cpp).
    s.pipelineCachePath = gfx::GetPipelineCachePath();
    LoadPipelineCache(s);

    // ---- 6. Timeline semaphore (deferred-delete tracking) ----------------
    // Submit signals `timelineSem` at `nextSubmitValue` (a monotonic
    // counter). Destroy() tags resources with the pending value so they
    // free automatically once the GPU reaches that point — replaces a
    // device-wide vkDeviceWaitIdle on every Destroy.
    {
        vk::SemaphoreTypeCreateInfo typeInfo{
            .semaphoreType = vk::SemaphoreType::eTimeline,
            .initialValue  = 0,
        };
        auto r = s.device.createSemaphore({ .pNext = &typeInfo });
        if (r.result != vk::Result::eSuccess) {
            std::fprintf(stderr, "[vk] createSemaphore (timeline) failed (%s)\n",
                         vk::to_string(r.result).c_str());
            return false;
        }
        s.timelineSem = std::move(r.value);
        s.nextSubmitValue = 1;
    }

    // ---- 6b. Transfer command pool + timeline semaphore -----------------
    // Pool lives on `transferQueueFamily`, the dedicated transfer family
    // on discrete NVIDIA/AMD or `queueFamily` itself on Intel iGPU /
    // mobile / MoltenVK. The timeline semaphore is what makes uploads
    // truly async: each upload submit signals it at an incrementing
    // value, the render submit waits on the latest signaled value, and
    // pendingTransferDeletes (staging buffers + one-shot CBs) drains by
    // polling the timeline counter at frame start.
    {
        auto poolR = s.device.createCommandPool({
            .flags = vk::CommandPoolCreateFlagBits::eTransient,
            .queueFamilyIndex = s.transferQueueFamily,
        });
        if (poolR.result != vk::Result::eSuccess) {
            std::fprintf(stderr,
                "[vk] createCommandPool (transfer) failed (%s)\n",
                vk::to_string(poolR.result).c_str());
            return false;
        }
        s.transferCommandPool = std::move(poolR.value);

        vk::SemaphoreTypeCreateInfo typeInfo{
            .semaphoreType = vk::SemaphoreType::eTimeline,
            .initialValue  = 0,
        };
        auto semR = s.device.createSemaphore({ .pNext = &typeInfo });
        if (semR.result != vk::Result::eSuccess) {
            std::fprintf(stderr,
                "[vk] createSemaphore (transfer timeline) failed (%s)\n",
                vk::to_string(semR.result).c_str());
            return false;
        }
        s.transferTimelineSem  = std::move(semR.value);
        s.transferNextValue    = 0;
        s.transferLastSignaled = 0;
    }

    // ---- 7. Per-frame command pool + sync ---------------------------------
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

        auto fenceR = s.device.createFence({
            .flags = vk::FenceCreateFlagBits::eSignaled,
        });
        if (fenceR.result != vk::Result::eSuccess) {
            std::fprintf(stderr, "[vk] createFence failed (frame %u)\n", i);
            return false;
        }
        f.inFlightFence = std::move(fenceR.value);

        // Transient descriptor pool for the SRV + sampler sets (the CB
        // set is on push descriptors, see the layout-flags logic in
        // section 5). Sized for the worst case our BLS catalog throws
        // at us — a busy frame with thousands of geosets, two sets per
        // draw (SRV + Sampler). Pool reset at frame start frees every
        // set in one shot.
        constexpr u32 kSetsPerDraw          = 2;  // SRV + Sampler
        constexpr u32 kMaxDrawsPerFrame     = 4096;
        constexpr u32 kMaxSetsPerFrame      = kMaxDrawsPerFrame * kSetsPerDraw;
        constexpr u32 kMaxSrvsPerFrame      = kMaxDrawsPerFrame * kSrvBindingCount;
        constexpr u32 kMaxSamplersPerFrame  = kMaxDrawsPerFrame * kSamplerBindingCount;
        const std::array<vk::DescriptorPoolSize, 2> poolSizes = {
            vk::DescriptorPoolSize{
                .type = vk::DescriptorType::eSampledImage,
                .descriptorCount = kMaxSrvsPerFrame,
            },
            vk::DescriptorPoolSize{
                .type = vk::DescriptorType::eSampler,
                .descriptorCount = kMaxSamplersPerFrame,
            },
        };
        auto poolR2 = s.device.createDescriptorPool({
            .maxSets       = kMaxSetsPerFrame,
            .poolSizeCount = static_cast<u32>(poolSizes.size()),
            .pPoolSizes    = poolSizes.data(),
        });
        if (poolR2.result != vk::Result::eSuccess) {
            std::fprintf(stderr, "[vk] createDescriptorPool failed (frame %u): %s\n",
                         i, vk::to_string(poolR2.result).c_str());
            return false;
        }
        f.descriptorPool = std::move(poolR2.value);
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

    BufferEntry e{};
    e.desc       = desc;
    e.slotStride = desc.size;
    e.slotCount  = 1;
    e.currentSlot = 0;

    // CpuWritable buffers (typically dynamic CBs) back onto a ring of
    // VulkanDeviceState::kCbRingSlots slots, each `slotStride` bytes.
    // Each MapBuffer rotates to the next slot so a renderer that
    // updates the same logical buffer once per draw still hands each
    // GPU read its own data — without this, all draws in a frame see
    // the final CB state (the GPU reads buffer contents at execution,
    // not at command-recording time).
    const bool ring = hasFlag(desc.usage, BufferUsage::CpuWritable) && desc.size > 0;
    if (ring) {
        // Round per-slot stride up to the device's minUniformBufferAlign
        // so VkDescriptorBufferInfo::offset stays legal.
        const u64 align = std::max<u64>(1, s.minUniformBufferAlign);
        e.slotStride = (desc.size + align - 1) / align * align;
        e.slotCount  = VulkanDeviceState::kCbRingSlots;
    }
    const u64 totalSize = e.slotStride * e.slotCount;

    // CpuWritable buffers try to sub-allocate from the shared CB ring
    // first — one VkBuffer + one VMA allocation backs every dynamic CB
    // in the program. Falls through to the per-CB allocation below
    // when the ring is exhausted or wasn't created.
    if (ring && s.sharedCbBuffer != VK_NULL_HANDLE) {
        // Each sub-alloc starts on minUniformBufferAlign so descriptor
        // offsets remain valid (we add baseOffset + slot*slotStride).
        const u64 align = std::max<u64>(1, s.minUniformBufferAlign);
        const u64 base  = (s.sharedCbCursor + align - 1) / align * align;
        if (base + totalSize <= VulkanDeviceState::kSharedCbCapacity) {
            e.buffer     = s.sharedCbBuffer;
            e.allocation = VK_NULL_HANDLE;  // shared — Destroy skips vmaDestroyBuffer
            // `mapped` is the base of the underlying VkBuffer (the shared
            // ring's persistent map). `currentOffset()` already adds
            // baseOffset + slot*slotStride on top, so callers that compute
            // `mapped + currentOffset()` land at the right slot byte
            // without double-counting.
            e.mapped     = s.sharedCbMapped;
            e.baseOffset = base;
            s.sharedCbCursor = base + totalSize;
            if (initial && desc.size > 0) {
                std::memcpy(static_cast<u8*>(e.mapped) + base, initial, desc.size);
                vmaFlushAllocation(s.allocator, s.sharedCbAllocation,
                                   base, desc.size);
            }
            return static_cast<BufferHandle>(s.buffers.Insert(std::move(e)));
        }
        // Fell through: ring exhausted. Proceed with a per-CB
        // allocation so the CreateBuffer call still succeeds.
    }

    VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size  = totalSize;
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

            // Submit on the transfer queue, signaling the transfer
            // timeline. No waitIdle: the graphics queue's next submit
            // waits on `transferLastSignaled` so any draw it carries
            // sees the upload. Staging buffer + one-shot CB are
            // queued via pendingTransferDeletes so they free once the
            // timeline counter passes the signal value.
            auto cbsR = s.device.allocateCommandBuffers({
                .commandPool        = *s.transferCommandPool,
                .level              = vk::CommandBufferLevel::ePrimary,
                .commandBufferCount = 1,
            });
            if (cbsR.result == vk::Result::eSuccess) {
                vk::raii::CommandBuffer xfer = std::move(cbsR.value[0]);
                (void)xfer.begin({ .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit });
                vk::BufferCopy region{ .size = desc.size };
                xfer.copyBuffer(vk::Buffer(stagingBuf), vk::Buffer(e.buffer), region);
                (void)xfer.end();

                const u64 signalValue = ++s.transferNextValue;
                vk::CommandBufferSubmitInfo cbInfo{ .commandBuffer = *xfer };
                vk::SemaphoreSubmitInfo signalInfo{
                    .semaphore = *s.transferTimelineSem,
                    .value     = signalValue,
                    .stageMask = vk::PipelineStageFlagBits2::eAllCommands,
                };
                vk::SubmitInfo2 submit{
                    .commandBufferInfoCount   = 1,
                    .pCommandBufferInfos      = &cbInfo,
                    .signalSemaphoreInfoCount = 1,
                    .pSignalSemaphoreInfos    = &signalInfo,
                };
                (void)s.transferQueue.submit2(submit);
                s.transferLastSignaled = signalValue;

                s.pendingTransferDeletes.push_back(MakePendingDelete(
                    signalValue,
                    [stagingBuf, stagingAlloc, cb = std::move(xfer)]
                    (VulkanDeviceState& st) mutable {
                        vmaDestroyBuffer(st.allocator, stagingBuf, stagingAlloc);
                        // `cb` raii destructs at lambda destruction (after
                        // the deleter runs), freeing the command buffer
                        // back to transferCommandPool.
                        (void)cb;
                    }));
            } else {
                vmaDestroyBuffer(s.allocator, stagingBuf, stagingAlloc);
            }
        }
    }

    return static_cast<BufferHandle>(s.buffers.Insert(std::move(e)));
}

namespace {

// Per-subresource byte size for a mip level, in linear order
// (matching D3D12::GetCopyableFootprints + DXTex slice ordering).
// Compressed formats are sized in 4x4 blocks; the renderer ships
// initial-pixel data tightly packed (no row padding).
struct SubresLayout {
    u32 width;
    u32 height;
    u64 rowsInBlocks;
    u64 rowSizeBytes;
    u64 sliceSizeBytes;
};

SubresLayout ComputeSubresLayout(Format fmt, u32 width, u32 height) {
    SubresLayout L{};
    L.width  = width;
    L.height = height;
    const u32 bytesPerBlock = FormatBytesPerBlock(fmt);
    if (IsBlockCompressed(fmt)) {
        const u32 blocksW = std::max(1u, (width  + 3) / 4);
        const u32 blocksH = std::max(1u, (height + 3) / 4);
        L.rowSizeBytes   = static_cast<u64>(blocksW) * bytesPerBlock;
        L.rowsInBlocks   = blocksH;
    } else {
        L.rowSizeBytes   = static_cast<u64>(width)  * bytesPerBlock;
        L.rowsInBlocks   = height;
    }
    L.sliceSizeBytes = L.rowSizeBytes * L.rowsInBlocks;
    return L;
}

// Submit a one-time command buffer that uploads `initialPixels` into
// `image`. Allocates a host-visible staging buffer via VMA, copies the
// renderer-packed mip chain into it, then runs a [transition →
// copyBufferToImage → transition] sequence on a dedicated transient
// command buffer and waits for the queue to drain. Slow but simple —
// texture loads only happen at MDX load time so the latency is
// acceptable until M5 introduces a deferred uploader.
bool UploadTexturePixels(VulkanDeviceState& s,
                          VkImage image, const TextureDesc& desc,
                          vk::ImageAspectFlags aspect,
                          const void* initialPixels)
{
    const u32 mipLevels = std::max(1u, static_cast<u32>(desc.mipLevels));
    const u32 layers    = std::max(1u, static_cast<u32>(desc.arraySize));

    // Walk the mip chain once to size the staging buffer and capture
    // per-subresource offsets for the copy regions.
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
            subs.push_back({ totalBytes, w, h, L.sliceSizeBytes });
            totalBytes += L.sliceSizeBytes;
            w = std::max(1u, w / 2);
            h = std::max(1u, h / 2);
        }
    }
    if (totalBytes == 0) return true;

    // ---- Staging buffer (host-visible + mapped) ----
    VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size  = totalBytes;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
              | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VkBuffer       stagingBuf  = VK_NULL_HANDLE;
    VmaAllocation  stagingAlloc = VK_NULL_HANDLE;
    VmaAllocationInfo stagingInfo{};
    if (vmaCreateBuffer(s.allocator, &bci, &aci, &stagingBuf, &stagingAlloc, &stagingInfo)
            != VK_SUCCESS) {
        return false;
    }
    std::memcpy(stagingInfo.pMappedData, initialPixels, totalBytes);
    // VMA *prefers* HOST_COHERENT for SEQUENTIAL_WRITE allocations but
    // can fall back to non-coherent host memory; an explicit flush is
    // a no-op on coherent memory and required on non-coherent so the
    // memcpy is visible to the GPU before submit2 below.
    vmaFlushAllocation(s.allocator, stagingAlloc, 0, totalBytes);

    // ---- One-shot command buffer on the transfer queue ----
    // Allocated from the shared `transferCommandPool` (transient). The
    // raii CB is moved into the pendingTransferDeletes lambda below so
    // it lives until the timeline reaches its signal value — by then
    // the GPU is done with it and the raii destructor returns it to
    // the pool.
    auto cbsR = s.device.allocateCommandBuffers({
        .commandPool        = *s.transferCommandPool,
        .level              = vk::CommandBufferLevel::ePrimary,
        .commandBufferCount = 1,
    });
    if (cbsR.result != vk::Result::eSuccess) {
        vmaDestroyBuffer(s.allocator, stagingBuf, stagingAlloc);
        return false;
    }
    vk::raii::CommandBuffer cb = std::move(cbsR.value[0]);
    (void)cb.begin({ .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit });

    // Transition: Undefined → TransferDstOptimal (whole resource).
    vk::ImageMemoryBarrier2 toCopy{
        .srcStageMask  = vk::PipelineStageFlagBits2::eTopOfPipe,
        .srcAccessMask = vk::AccessFlagBits2::eNone,
        .dstStageMask  = vk::PipelineStageFlagBits2::eCopy,
        .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .oldLayout     = vk::ImageLayout::eUndefined,
        .newLayout     = vk::ImageLayout::eTransferDstOptimal,
        .image         = vk::Image(image),
        .subresourceRange = { aspect, 0, mipLevels, 0, layers },
    };
    cb.pipelineBarrier2({ .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &toCopy });

    // One buffer-image copy per subresource.
    std::vector<vk::BufferImageCopy> regions;
    regions.reserve(subs.size());
    for (u32 layer = 0; layer < layers; ++layer) {
        for (u32 mip = 0; mip < mipLevels; ++mip) {
            const auto& sub = subs[layer * mipLevels + mip];
            regions.push_back(vk::BufferImageCopy{
                .bufferOffset = sub.stagingOffset,
                .imageSubresource = {
                    aspect, mip, layer, 1,
                },
                .imageOffset = { 0, 0, 0 },
                .imageExtent = { sub.width, sub.height, 1 },
            });
        }
    }
    cb.copyBufferToImage(vk::Buffer(stagingBuf), vk::Image(image),
                          vk::ImageLayout::eTransferDstOptimal,
                          regions);

    // Final transition to eShaderReadOnlyOptimal on the same command
    // buffer. Safe across queue families because sampled images are
    // created VK_SHARING_MODE_CONCURRENT when `hasAsyncTransfer` is
    // true (see CreateTexture). The shader-side visibility is
    // provided by the timeline-semaphore wait on the graphics submit
    // (see vulkan_swap_chain.cpp Present) — NOT by this barrier's
    // dstStage/dstAccess, which is illegal here because the transfer
    // queue doesn't support shader stages. We just need the layout
    // transition itself to complete; eBottomOfPipe + no access is the
    // canonical "release ownership / end of work" mask on a transfer
    // queue.
    vk::ImageMemoryBarrier2 toRead{
        .srcStageMask  = vk::PipelineStageFlagBits2::eCopy,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask  = vk::PipelineStageFlagBits2::eBottomOfPipe,
        .dstAccessMask = {},
        .oldLayout     = vk::ImageLayout::eTransferDstOptimal,
        .newLayout     = vk::ImageLayout::eShaderReadOnlyOptimal,
        .image         = vk::Image(image),
        .subresourceRange = { aspect, 0, mipLevels, 0, layers },
    };
    cb.pipelineBarrier2({ .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &toRead });
    (void)cb.end();

    // Submit on the transfer queue, signal the transfer timeline. No
    // waitIdle — the graphics queue's next submit waits on
    // `transferLastSignaled` so the first draw that samples this
    // image sees both the copy and the final layout transition.
    // Staging buffer + one-shot CB ride pendingTransferDeletes and
    // free once the timeline counter passes the signal value.
    const u64 signalValue = ++s.transferNextValue;
    vk::CommandBufferSubmitInfo cbInfo{ .commandBuffer = *cb };
    vk::SemaphoreSubmitInfo signalInfo{
        .semaphore = *s.transferTimelineSem,
        .value     = signalValue,
        .stageMask = vk::PipelineStageFlagBits2::eAllCommands,
    };
    vk::SubmitInfo2 si{
        .commandBufferInfoCount   = 1,
        .pCommandBufferInfos      = &cbInfo,
        .signalSemaphoreInfoCount = 1,
        .pSignalSemaphoreInfos    = &signalInfo,
    };
    (void)s.transferQueue.submit2(si);
    s.transferLastSignaled = signalValue;

    s.pendingTransferDeletes.push_back(MakePendingDelete(
        signalValue,
        [stagingBuf, stagingAlloc, ownedCb = std::move(cb)]
        (VulkanDeviceState& st) mutable {
            vmaDestroyBuffer(st.allocator, stagingBuf, stagingAlloc);
            (void)ownedCb;
        }));
    return true;
}

}  // namespace

TextureHandle VulkanDevice::CreateTexture(const TextureDesc& desc,
                                          const void* initialPixels)
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
    // When the transfer queue is a separate family from the graphics
    // queue, a sampled image whose contents we upload on the transfer
    // queue must either round-trip through queue-family ownership
    // transfer barriers (write a release barrier on the transfer
    // queue then a matching acquire on the graphics queue) or be
    // created with VK_SHARING_MODE_CONCURRENT listing both families.
    // We take the concurrent path — the QFOT bookkeeping would buy
    // back the perf modern drivers no longer get from EXCLUSIVE.
    // Render-target / depth images stay EXCLUSIVE because they never
    // cross queue families.
    const bool sampledImg = hasFlag(desc.usage, TextureUsage::ShaderResource);
    const u32 sharedFamilies[2] = { s.queueFamily, s.transferQueueFamily };
    if (sampledImg && s.hasAsyncTransfer) {
        ici.sharingMode           = VK_SHARING_MODE_CONCURRENT;
        ici.queueFamilyIndexCount = 2;
        ici.pQueueFamilyIndices   = sharedFamilies;
    } else {
        ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }
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

    // For cube images, eCube requires layerCount==6 and eCubeArray
    // requires layerCount that's a multiple of 6 — the renderer
    // creates IBL probes as cube arrays with arraySize == 12 (two
    // cubes), which has to use eCubeArray.
    vk::ImageViewType viewType = vk::ImageViewType::e2D;
    if (desc.isCube) {
        viewType = (desc.arraySize > 6)
                       ? vk::ImageViewType::eCubeArray
                       : vk::ImageViewType::eCube;
    }
    auto viewR = s.device.createImageView({
        .image    = vk::Image(t.image),
        .viewType = viewType,
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

    const bool sampled       = hasFlag(desc.usage, TextureUsage::ShaderResource);
    const bool hasInitData   = sampled && initialPixels != nullptr;
    const VkImage rawImage   = t.image;

    // Upload `initialPixels` synchronously while the texture entry is
    // still local — keeps every UpdateTexture-shaped state out of the
    // SlotMap until we know the data landed. Failure here orphans the
    // VMA image; the entry-level destroy below picks that up.
    if (hasInitData) {
        if (!UploadTexturePixels(s, rawImage, desc, t.aspect, initialPixels)) {
            vmaDestroyImage(s.allocator, t.image, t.allocation);
            return TextureHandle::Invalid;
        }
        // UploadTexturePixels does the full Undefined → TransferDst
        // → ShaderReadOnly chain on the transfer queue and waits for
        // it. Image is sample-ready by the time we return — no
        // deferred transition needed. Cross-queue layout visibility
        // is provided by the CONCURRENT-sharing image (see above)
        // plus the transferQueue.waitIdle() inside UploadTexturePixels.
        t.currentLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
    }

    const u64 slot = s.textures.Insert(std::move(t));
    // Sampled textures with NO initial data were freshly created in
    // eUndefined; the graphics queue transitions them to
    // eShaderReadOnlyOptimal at the next frame's EnsureRecording
    // before any draw samples them. Textures created with initial
    // data already landed in eShaderReadOnlyOptimal above.
    if (sampled && !hasInitData) {
        s.pendingSrvTransitions.push_back(slot);
    }
    return static_cast<TextureHandle>(slot);
}

TextureHandle VulkanDevice::CreateColorTarget(i32 w, i32 h, Format f) {
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
    ici.usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                      | VK_IMAGE_USAGE_SAMPLED_BIT
                      | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ici.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;

    TextureEntry t{};
    if (vmaCreateImage(s.allocator, &ici, &aci, &t.image, &t.allocation, nullptr)
            != VK_SUCCESS) {
        return TextureHandle::Invalid;
    }
    t.aspect = vk::ImageAspectFlagBits::eColor;

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
    // Null perms in a BLS bundle hand the cache an empty span — slang
    // strips perms whose `Conditional<>` features collapse to nothing,
    // and the cache pushes every slot through CreateShader regardless.
    // Vulkan rejects codeSize == 0 (VUID-VkShaderModuleCreateInfo-
    // codeSize-01085) so just return Invalid for those slots; the
    // renderer never picks a null perm at draw time.
    if (size == 0 || bytecode == nullptr) return ShaderHandle::Invalid;
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
        // Vulkan binds attributes by location number. The renderer mixes
        // two semantic conventions:
        //   1. BLS layouts (kMeshSD, kCornFx, …) use "ATTR" + a numeric
        //      semantic index. Slang's SPIR-V emit maps each `: ATTR<N>`
        //      VS input to `Location = N`, so the index *is* the location.
        //   2. Debug-shader layouts (grid lines, tonemap blit, view-cube)
        //      use HLSL-style names like POSITION/COLOR/TEXCOORD with
        //      `semanticIndex = 0` everywhere. Slang's SPIR-V emit just
        //      hands them sequential locations in declaration order.
        // Detect the convention from the semantic prefix.
        const bool useSemanticIndex =
            el.semantic && el.semantic[0] == 'A' && el.semantic[1] == 'T'
                        && el.semantic[2] == 'T' && el.semantic[3] == 'R'
                        && el.semantic[4] == '\0';
        attrs.push_back(vk::VertexInputAttributeDescription{
            .location = useSemanticIndex ? el.semanticIndex : i,
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
        // Renderer convention matches Vulkan 1:1 — `frontCCW=true`
        // means CCW-wound triangles are front. The negative-height
        // viewport applied in BeginRenderPass flips Y for D3D-style
        // top-left origin without changing which winding the
        // rasterizer treats as front (Vulkan re-evaluates winding in
        // clip space, before the viewport transform).
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

    // vk::raii API takes the raii pipeline cache via vk::Optional. The
    // raii::PipelineCache implicitly converts when non-null; the
    // condition guards the case where load+create both failed at Init.
    auto pR = *s.pipelineCache
        ? s.device.createGraphicsPipeline(s.pipelineCache, gpci)
        : s.device.createGraphicsPipeline(nullptr,         gpci);
    if (pR.result != vk::Result::eSuccess) {
        std::fprintf(stderr, "[vk] createGraphicsPipeline failed (%s)\n",
                     vk::to_string(pR.result).c_str());
        return PipelineHandle::Invalid;
    }
    PipelineEntry e{};
    e.pipeline       = std::move(pR.value);
    e.isCompute      = false;
    e.colorFormat    = rtvFmt;
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

// Drain queued deletes whose timeline value the GPU has reached.
void DrainPendingDeletes(VulkanDeviceState& s) {
    if (s.pendingDeletes.empty()) return;
    auto vR = s.timelineSem.getCounterValue();
    if (vR.result != vk::Result::eSuccess) return;
    const u64 completed = vR.value;
    auto it = s.pendingDeletes.begin();
    while (it != s.pendingDeletes.end()) {
        if (it->timelineValue <= completed) {
            it->deleter->Run(s);
            it = s.pendingDeletes.erase(it);
        } else {
            ++it;
        }
    }
}

void DrainPendingTransferDeletes(VulkanDeviceState& s) {
    if (s.pendingTransferDeletes.empty()) return;
    if (!*s.transferTimelineSem) return;
    auto vR = s.transferTimelineSem.getCounterValue();
    if (vR.result != vk::Result::eSuccess) return;
    const u64 completed = vR.value;
    auto it = s.pendingTransferDeletes.begin();
    while (it != s.pendingTransferDeletes.end()) {
        if (it->timelineValue <= completed) {
            it->deleter->Run(s);
            it = s.pendingTransferDeletes.erase(it);
        } else {
            ++it;
        }
    }
}

// Each Destroy(handle) moves the resource state out of the slot map
// (so the renderer's handle is invalidated immediately) and queues a
// deleter tagged with the next-submit timeline value. Once the GPU
// reaches that value the per-frame drain runs the lambda — no
// vkDeviceWaitIdle needed.
void VulkanDevice::Destroy(BufferHandle h) {
    auto& s = *state_;
    auto* b = s.buffers.Get(static_cast<u64>(h));
    if (!b) return;
    BufferEntry moved = std::move(*b);
    s.buffers.Remove(static_cast<u64>(h));
    s.pendingDeletes.push_back(MakePendingDelete(
        s.nextSubmitValue,
        [m = std::move(moved)](VulkanDeviceState& st) mutable {
            if (m.buffer && m.allocation)
                vmaDestroyBuffer(st.allocator, m.buffer, m.allocation);
        }));
}

void VulkanDevice::Destroy(TextureHandle h) {
    auto& s = *state_;
    auto* t = s.textures.Get(static_cast<u64>(h));
    if (!t) return;
    TextureEntry moved = std::move(*t);
    s.textures.Remove(static_cast<u64>(h));
    s.pendingDeletes.push_back(MakePendingDelete(
        s.nextSubmitValue,
        [m = std::move(moved)](VulkanDeviceState& st) mutable {
            // vk::raii::ImageView (m.ownedView) auto-destroys when m
            // goes out of scope. The VMA-owned image only frees when
            // we created it — proxy textures (m.ownsImage==false)
            // borrow swap-chain-owned images and skip vmaDestroyImage.
            if (m.ownsImage && m.image && m.allocation)
                vmaDestroyImage(st.allocator, m.image, m.allocation);
        }));
}

void VulkanDevice::Destroy(ShaderHandle h) {
    auto& s = *state_;
    auto* e = s.shaders.Get(static_cast<u64>(h));
    if (!e) return;
    ShaderEntry moved = std::move(*e);
    s.shaders.Remove(static_cast<u64>(h));
    s.pendingDeletes.push_back(MakePendingDelete(
        s.nextSubmitValue,
        [m = std::move(moved)](VulkanDeviceState&) mutable {
            // vk::raii::ShaderModule destroys on scope exit.
        }));
}

void VulkanDevice::Destroy(PipelineHandle h) {
    auto& s = *state_;
    auto* e = s.pipelines.Get(static_cast<u64>(h));
    if (!e) return;
    PipelineEntry moved = std::move(*e);
    s.pipelines.Remove(static_cast<u64>(h));
    s.pendingDeletes.push_back(MakePendingDelete(
        s.nextSubmitValue,
        [m = std::move(moved)](VulkanDeviceState&) mutable {
            // vk::raii::Pipeline destroys on scope exit.
        }));
}

void VulkanDevice::Destroy(SamplerHandle h) {
    auto& s = *state_;
    auto* e = s.samplers.Get(static_cast<u64>(h));
    if (!e) return;
    SamplerEntry moved = std::move(*e);
    s.samplers.Remove(static_cast<u64>(h));
    s.pendingDeletes.push_back(MakePendingDelete(
        s.nextSubmitValue,
        [m = std::move(moved)](VulkanDeviceState&) mutable {
            // vk::raii::Sampler destroys on scope exit.
        }));
}

// ---- Buffer map / update ------------------------------------------------

void VulkanDevice::UpdateBuffer(BufferHandle h, const void* data, usize size) {
    auto& s = *state_;
    auto* b = s.buffers.Get(static_cast<u64>(h));
    if (!b) return;
    if (b->mapped) {
        // Treat UpdateBuffer like Map+memcpy+Unmap on the ring so the
        // next Bind* captures the freshly-written slot. Non-ring buffers
        // (slotCount==1) keep writing into slot 0 in place.
        if (b->slotCount > 1) {
            b->currentSlot = (b->currentSlot + 1) % b->slotCount;
        }
        const u64 off = b->currentOffset();
        std::memcpy(static_cast<u8*>(b->mapped) + off, data, size);
        // Shared-ring sub-allocs flush against the shared VMA allocation;
        // own-allocation buffers flush against their own.
        VmaAllocation alloc = b->allocation
                                  ? b->allocation
                                  : s.sharedCbAllocation;
        vmaFlushAllocation(s.allocator, alloc, off, size);
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
    if (b->mapped) {
        // Rotate to the next ring slot so this Map's data is visible
        // independently of the previous slot's content. UnmapBuffer
        // flushes just the new slot.
        if (b->slotCount > 1) {
            b->currentSlot = (b->currentSlot + 1) % b->slotCount;
        }
        return static_cast<u8*>(b->mapped) + b->currentOffset();
    }
    void* p = nullptr;
    vmaMapMemory(s.allocator, b->allocation, &p);
    return p;
}

void VulkanDevice::UnmapBuffer(BufferHandle h) {
    auto& s = *state_;
    auto* b = s.buffers.Get(static_cast<u64>(h));
    if (!b) return;
    if (b->mapped) {
        // Persistently mapped — flush just the current ring slot (or
        // the whole buffer for slotCount == 1) and leave mapped.
        // Shared-ring sub-allocs flush against sharedCbAllocation.
        VmaAllocation alloc = b->allocation
                                  ? b->allocation
                                  : s.sharedCbAllocation;
        vmaFlushAllocation(s.allocator, alloc,
                           b->currentOffset(), b->desc.size);
    } else {
        vmaUnmapMemory(s.allocator, b->allocation);
    }
}

}  // namespace whiteout::flakes::gfx::vulkan

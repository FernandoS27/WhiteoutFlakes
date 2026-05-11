#include "gfx/gfx.h"
#include "gfx/d3d11/d3d11_device.h"
#include "gfx/d3d12/d3d12_device.h"

#if WDX_HAS_VULKAN
#include "gfx/vulkan/vulkan_device.h"
#endif

#include <filesystem>
#include <stdexcept>
#include <string>

namespace whiteout::flakes::gfx {

namespace {
// Host-provided path the Vulkan backend uses for its VkPipelineCache.
// Stored module-scope rather than passed through CreateDevice so the
// d3d11 / d3d12 paths (which don't use it) stay unchanged. The Vulkan
// device reads it in Init via GetPipelineCachePath().
std::filesystem::path g_pipelineCachePath;

// Host-selected preferred device name (matched verbatim against the
// strings returned by EnumerateDevices). Empty = "best by VRAM / type"
// — the default each backend used before this knob existed.
std::string g_preferredDevice;
}  // namespace

void SetPipelineCachePath(const char* utf8Path) {
    if (!utf8Path || !*utf8Path) {
        g_pipelineCachePath.clear();
        return;
    }
    g_pipelineCachePath =
        std::filesystem::path(reinterpret_cast<const char8_t*>(utf8Path));
}

const std::filesystem::path& GetPipelineCachePath() {
    return g_pipelineCachePath;
}

void SetPreferredDevice(const char* utf8Name) {
    g_preferredDevice = (utf8Name && *utf8Name) ? utf8Name : std::string{};
}

const std::string& GetPreferredDevice() {
    return g_preferredDevice;
}

std::unique_ptr<IGFXDevice> CreateDevice(GfxApi api, bool enableValidation) {
    switch (api) {
        case GfxApi::D3D11: {
            auto device = std::make_unique<d3d11::D3D11Device>();
            if (!device->Init(enableValidation))
                return nullptr;
            return device;
        }
        case GfxApi::D3D12: {
            auto device = std::make_unique<d3d12::D3D12Device>();
            if (!device->Init(enableValidation))
                return nullptr;
            return device;
        }
        case GfxApi::Vulkan: {
#if WDX_HAS_VULKAN
            auto device = std::make_unique<vulkan::VulkanDevice>();
            if (!device->Init(enableValidation))
                return nullptr;
            return device;
#else
            (void)enableValidation;
            return nullptr;
#endif
        }
    }
    return nullptr;
}

std::vector<std::string> EnumerateDevices(GfxApi api) {
    switch (api) {
        case GfxApi::D3D11:  return d3d11::EnumerateAdapterNames();
        case GfxApi::D3D12:  return d3d12::EnumerateAdapterNames();
        case GfxApi::Vulkan:
#if WDX_HAS_VULKAN
            return vulkan::EnumerateAdapterNames();
#else
            return {};
#endif
    }
    return {};
}

}

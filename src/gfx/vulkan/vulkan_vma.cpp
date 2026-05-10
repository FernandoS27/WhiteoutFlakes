// Single TU that defines VMA_IMPLEMENTATION before including the
// AMD Vulkan Memory Allocator header. Everywhere else in the Vulkan
// backend just includes <vk_mem_alloc.h> for the declarations. The
// header lives under <Vulkan SDK>/Include/vma/, surfaced through
// WhiteoutFlakesGfx's PRIVATE include dirs in CMakeLists.txt.
//
// VMA defaults are fine for our usage (suballocates from VkDeviceMemory
// blocks, tracks per-allocation sizes, supports persistently mapped
// memory for CpuWritable buffers). We don't tune block sizes here — if
// fragmentation becomes a problem we can expose VmaAllocatorCreateInfo
// from the device init path later.

#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

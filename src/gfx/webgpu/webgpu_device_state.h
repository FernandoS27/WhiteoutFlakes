#pragma once

// Aggregate device state + free-function declarations for the lifetime
// utilities (deferred-delete drain, swap-chain acquire, shared CB ring).
//
// Mirrors src/gfx/vulkan/vulkan_device_state.h.

#include "gfx/common/slot_map.h"
#include "webgpu_handles.h"

#include <webgpu/webgpu_cpp.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <deque>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace whiteout::flakes::gfx::webgpu {

// LRU-bounded cache mapping a hash of the materialized bind-group entry
// tuple to its wgpu::BindGroup. WebGPU bind groups are expensive to create
// (JS↔WASM marshaling, validation) and each holds strong refs to its
// resources — without caching, every dirty-flag flip allocated a fresh
// bind group every draw, hundreds per frame, pinning live GPU memory
// until Dawn GCed them. With caching the steady-state of a frame reuses
// the same handful of groups across draws.
//
// Cap is per-group (CB / SRV / sampler). On Destroy of any backing
// resource the entire cache is cleared (see WebGPUDevice::Destroy) so
// the cache can't keep zombie GPU resources alive past their handle.
struct BindGroupCache {
    static constexpr std::size_t kCap = 256;
    struct Entry {
        wgpu::BindGroup bg;
        std::list<u64>::iterator lruIt;
    };
    std::unordered_map<u64, Entry> map;
    std::list<u64> lru;
    wgpu::BindGroup Get(u64 key) {
        auto it = map.find(key);
        if (it == map.end()) return nullptr;
        lru.splice(lru.end(), lru, it->second.lruIt);
        return it->second.bg;
    }
    void Put(u64 key, wgpu::BindGroup bg) {
        while (map.size() >= kCap && !lru.empty()) {
            map.erase(lru.front());
            lru.pop_front();
        }
        lru.push_back(key);
        map.emplace(key, Entry{std::move(bg), std::prev(lru.end())});
    }
    void Clear() {
        map.clear();
        lru.clear();
    }
};

struct WebGPUDeviceState {
    // Public WebGPU handles only — no Dawn-private types. The prebuilt
    // WebGPU-distribution doesn't expose dawn::native::*.
    wgpu::Instance instance;
    wgpu::Adapter adapter;
    wgpu::Device device;
    wgpu::Queue queue;

    // Single shared CB upload buffer — sub-allocated by CreateBuffer for
    // CpuWritable+Constant buffers. CPU writes hit `sharedCbShadow`;
    // every Map/Unmap/UpdateBuffer for a sub-alloc enqueues a matching
    // Queue::WriteBuffer to push the bytes to `sharedCbBuffer`. We don't
    // use mappedAtCreation persistent mapping because WebGPU forbids
    // MapWrite buffers from carrying Uniform / Vertex / Index usages
    // (only CopySrc + MapWrite are compatible), and a buffer that's
    // mapped at submit time fails validation.
    wgpu::Buffer sharedCbBuffer;
    std::vector<u8> sharedCbShadow;
    u64 sharedCbCursor = 0;

    // 256 zero bytes used as the source for phantom vertex slots — when
    // a VS declares an @location(N) the renderer doesn't actually fill,
    // CreateGraphicsPipeline adds a one-attribute VertexBufferLayout and
    // BindPipeline binds this buffer to that slot. Reads as all-zeros on
    // every channel / format, which is what most shaders expect for an
    // optional vertex attribute.
    wgpu::Buffer zeroVertexBuffer;

    // Uniform-buffer minimum offset alignment, queried from the adapter.
    // WebGPU spec floor is 256 bytes (matches D3D12 / Vulkan AMD).
    u64 minUniformBufferAlign = 256;

    // Set at Init time from `adapter.HasFeature(TextureCompressionBC)`.
    // Read by SupportsBlockCompression() and by the asset path to gate
    // BCn → RGBA8 decompression on adapters that can't sample BC directly.
    bool hasBlockCompression = false;

    // Float32Filterable enabled: R32F / RG32F / RGBA32F satisfy Float
    // (filterable) texture entries, not only UnfilterableFloat.
    bool float32Filterable = false;

    // Bind-group layouts built from WGSL declarations, deduplicated by
    // content. PipelineEntry::groupLayouts indexes `bindLayouts`; the index
    // also keys the bind-group caches, so a group built for one layout is
    // never set against another. Entries are never freed.
    struct BindLayout {
        wgpu::BindGroupLayout layout;
        std::vector<wgpu::BindGroupLayoutEntry> entries;
        u8 kindMask = 0; // kBindKind* families the entries draw from
    };
    std::vector<BindLayout> bindLayouts;
    std::unordered_map<std::string, u32> bindLayoutIds;
    std::unordered_map<u64, wgpu::PipelineLayout> pipelineLayouts;

    // Defaults for entries the renderer left unbound (see CreateDefaultResources).
    // The textures are 1x1 with 6 layers; FlushBindings takes views of the
    // dimension each entry declares from `defaultViews`.
    wgpu::Sampler defaultSampler;
    wgpu::Sampler defaultComparisonSampler;
    wgpu::Texture defaultTexture;
    wgpu::Texture defaultDepthTexture;
    wgpu::Texture defaultUintTexture;
    wgpu::Texture defaultSintTexture;
    wgpu::Texture defaultTexture3D;
    wgpu::Buffer defaultStorageBuffer;
    std::unordered_map<u32, wgpu::TextureView> defaultViews;

    // Per-frame transient depth target — auto-attached at
    // BeginRenderPass time whenever the renderer passes depth=Invalid
    // but the bound pipelines declare a dsvFormat (tonemap / ImGui
    // passes are the typical case; they keep dsvFormat set for
    // cross-backend consistency and rely on D3D/Vulkan's permissive
    // attachment-state matching). WebGPU rejects the SetPipeline
    // unless the pass attaches a depth target of the exact same
    // format. We discard contents on store — the data isn't read.
    wgpu::Texture transientDepthTexture;
    wgpu::TextureView transientDepthView;
    u32 transientDepthW = 0;
    u32 transientDepthH = 0;

    SlotMap<BufferEntry> buffers;
    SlotMap<TextureEntry> textures;
    SlotMap<ShaderEntry> shaders;
    SlotMap<PipelineEntry> pipelines;
    SlotMap<SamplerEntry> samplers;
    SlotMap<SwapChainEntry> swapchains;

    // Per-frame command encoders. Unlike Vulkan we don't reset pools —
    // each frame opens a fresh encoder via device.CreateCommandEncoder
    // and submits it via queue.Submit(encoder.Finish()) at Present().
    std::array<FrameContext, kFramesInFlight> frames{};
    u32 frameIndex = 0;

    // Deferred-delete state. Destroy() tags entries with pendingEpoch;
    // EnsureRecording drains those with epoch <= completedEpoch on the
    // next frame's first BeginRenderPass.
    //
    // `completedEpoch` is written from Dawn's worker thread inside the
    // OnSubmittedWorkDone callback; reading it on the renderer thread is
    // safe because std::atomic gives us the acquire/release pair we need.
    DeleteEpoch pendingEpoch = 0;
    std::atomic<DeleteEpoch> completedEpoch{0};
    std::deque<PendingDelete> pendingDeletes;
    std::mutex deleteMutex; // pendingDeletes is touched from Destroy() (main) only

    std::array<BindGroupCache, kMaxBindGroups> bgCaches;

    // Live GPU byte accounting. CreateTexture / CreateBuffer bump
    // `gpuBytesAlloc`; the deferred-delete lambda bumps `gpuBytesFreed`
    // when the actual wgpu handle drops. Difference is live bytes —
    // diagnostic only (JS pulls via wf_gpu_bytes).
    std::atomic<u64> gpuBytesAlloc{0};
    std::atomic<u64> gpuBytesFreed{0};

    bool enableValidation = false;

    // Set by DeviceLostCallback. Read by Draw/DrawIndexed and friends to
    // suppress further work (and per-draw diagnostics) once the GPU has
    // gone away — keeps the device-lost message visible at the bottom
    // of stderr instead of buried under a flood of post-death draws.
    std::atomic<bool> deviceLost{false};
};

// Drain entries whose epoch the GPU has already acked. Called at the top
// of each frame from EnsureEncoderOpen (webgpu_command_list.cpp).
void DrainPendingDeletes(WebGPUDeviceState& state);

// Acquire the surface's current texture and re-point the proxy entries.
// No-op if already acquired this frame.
void AcquireSwapChainImageIfNeeded(WebGPUDeviceState& state, SwapChainEntry& sc);

// Submit the current frame's encoder (if any), bump `pendingEpoch`, and
// post an OnSubmittedWorkDone callback that bumps `completedEpoch`. Used
// by both Present() (with `present=true`) and the implicit end-of-frame
// flush when the renderer never touches the swap chain.
void SubmitFrameAndBumpEpoch(WebGPUDeviceState& state);

// Drop every cached bind group. Called from Destroy(BufferHandle /
// TextureHandle / SamplerHandle) so destroyed resources can't be kept
// alive by stale cache entries referencing them.
inline void InvalidateBindGroupCaches(WebGPUDeviceState& state) {
    for (auto& cache : state.bgCaches)
        cache.Clear();
}

} // namespace whiteout::flakes::gfx::webgpu

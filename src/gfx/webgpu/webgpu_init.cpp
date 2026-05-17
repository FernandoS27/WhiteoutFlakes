// Instance / Adapter / Device setup, adapter enumeration, shared
// bind-group layouts + default resources + shared CB ring.
//
// Uses public WebGPU API only (no dawn::native) so the same code works
// against eliemichel/WebGPU-distribution's prebuilt Dawn — see the
// CMake block in CMakeLists.txt and the design note in WebGPU.md §6/§7.

#include "webgpu_device.h"
#include "webgpu_device_state.h"
#include "webgpu_handles.h"

#include <webgpu/webgpu_cpp.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

// The gfx-factory module exposes GetPreferredDevice() to backends.
namespace whiteout::flakes::gfx {
const std::string& GetPreferredDevice();
} // namespace whiteout::flakes::gfx

namespace whiteout::flakes::gfx::webgpu {

namespace {

void DeviceLostCallback(const wgpu::Device&, wgpu::DeviceLostReason reason,
                        wgpu::StringView message) {
    if (reason == wgpu::DeviceLostReason::Destroyed)
        return; // expected at shutdown
    std::fprintf(stderr, "[wgpu] device lost: %.*s\n", static_cast<int>(message.length),
                 message.data);
}

void UncapturedErrorCallback(const wgpu::Device&, wgpu::ErrorType type, wgpu::StringView message) {
    const char* kind = "?";
    switch (type) {
    case wgpu::ErrorType::Validation:
        kind = "validation";
        break;
    case wgpu::ErrorType::OutOfMemory:
        kind = "oom";
        break;
    case wgpu::ErrorType::Internal:
        kind = "internal";
        break;
    case wgpu::ErrorType::Unknown:
        kind = "unknown";
        break;
    case wgpu::ErrorType::NoError:
        return;
    default:
        break;
    }
    std::fprintf(stderr, "[wgpu] ERR (%s): %.*s\n", kind, static_cast<int>(message.length),
                 message.data);
}

std::string AdapterName(const wgpu::Adapter& adapter) {
    wgpu::AdapterInfo info{};
    adapter.GetInfo(&info);
    if (info.device.length > 0)
        return std::string(info.device.data, info.device.length);
    if (info.description.length > 0)
        return std::string(info.description.data, info.description.length);
    return "<unnamed>";
}

i32 ScoreAdapter(const wgpu::Adapter& adapter) {
    wgpu::AdapterInfo info{};
    adapter.GetInfo(&info);
    i32 score = 0;
    switch (info.adapterType) {
    case wgpu::AdapterType::DiscreteGPU:
        score += 1000;
        break;
    case wgpu::AdapterType::IntegratedGPU:
        score += 100;
        break;
    case wgpu::AdapterType::CPU:
        score -= 1000;
        break;
    default:
        break;
    }
    return score;
}

// Sync wrapper around wgpu::Instance::RequestAdapter. Returns null on
// failure. Pass `powerPreference = Undefined` for "no preference".
wgpu::Adapter RequestAdapterSync(wgpu::Instance instance,
                                 wgpu::PowerPreference powerPreference) {
    wgpu::RequestAdapterOptions opts{};
    opts.powerPreference = powerPreference;

    wgpu::Adapter result;
    wgpu::Future future = instance.RequestAdapter(
        &opts, wgpu::CallbackMode::WaitAnyOnly,
        [&result](wgpu::RequestAdapterStatus status, wgpu::Adapter a, wgpu::StringView message) {
            if (status == wgpu::RequestAdapterStatus::Success) {
                result = std::move(a);
            } else if (message.length > 0) {
                std::fprintf(stderr, "[wgpu] RequestAdapter: %.*s\n",
                             static_cast<int>(message.length), message.data);
            }
        });
    wgpu::FutureWaitInfo wait{future};
    instance.WaitAny(1, &wait, UINT64_MAX);
    return result;
}

wgpu::Instance CreateInstanceWithTimedWait() {
    // Dawn's InstanceDescriptor nests the timedWaitAny knob inside an
    // `InstanceCapabilities` substruct (`capabilities` field).
    wgpu::InstanceDescriptor desc{};
    desc.capabilities.timedWaitAnyEnable = true;
    return wgpu::CreateInstance(&desc);
}

bool CreateInstanceAndAdapter(WebGPUDeviceState& state, std::string& deviceNameOut) {
    state.instance = CreateInstanceWithTimedWait();
    if (!state.instance) {
        std::fprintf(stderr, "[wgpu] wgpu::CreateInstance failed\n");
        return false;
    }

    // We can't enumerate adapters with the public API (Dawn extends it
    // but the prebuilt distribution doesn't expose those headers). Best
    // effort: request HighPerformance and LowPower candidates, keep the
    // highest-scoring one, honor the preferred-device match.
    const std::string& preferred = gfx::GetPreferredDevice();
    wgpu::PowerPreference probes[] = {
        wgpu::PowerPreference::HighPerformance,
        wgpu::PowerPreference::LowPower,
        wgpu::PowerPreference::Undefined,
    };
    wgpu::Adapter best;
    i32 bestScore = -10000;
    for (auto pp : probes) {
        wgpu::Adapter adapter = RequestAdapterSync(state.instance, pp);
        if (!adapter)
            continue;
        const std::string name = AdapterName(adapter);
        std::fprintf(stderr, "[wgpu]   probe %d adapter: '%s'\n", static_cast<int>(pp),
                     name.c_str());
        if (!preferred.empty() && preferred == name) {
            best = std::move(adapter);
            deviceNameOut = name;
            break;
        }
        const i32 score = ScoreAdapter(adapter);
        if (score > bestScore) {
            bestScore = score;
            best = adapter;
            deviceNameOut = name;
        }
    }
    if (!best) {
        std::fprintf(stderr, "[wgpu] no adapter chosen\n");
        return false;
    }
    state.adapter = std::move(best);
    return true;
}

bool RequestDeviceSync(WebGPUDeviceState& state) {
    // Pull adapter-supported limits so we don't request more than the
    // adapter can give us — over-requesting fails RequestDevice outright.
    wgpu::Limits supported{};
    state.adapter.GetLimits(&supported);

    // After the VS/PS visibility split, each stage sees kStageBindingShift
    // (=16) bindings of each kind. We bump uniform-buffer-per-stage to 16
    // (default is 12) and sampled-textures / samplers to 16 (default is
    // 16 already, but explicit is clearer). The dynamic-uniform cap is
    // spec-bounded to ~8-11 on every implementation, so we DON'T use
    // hasDynamicOffset — see CreateSharedBindLayouts. Each per-draw
    // FlushBindings embeds the ring-slot offset into the BindGroupEntry
    // directly.
    auto cap = [](u32 desired, u32 adapterMax) -> u32 {
        return std::min(desired, adapterMax);
    };
    wgpu::Limits required{};
    required.maxSampledTexturesPerShaderStage =
        cap(kStageBindingShift, supported.maxSampledTexturesPerShaderStage);
    required.maxSamplersPerShaderStage =
        cap(kStageBindingShift, supported.maxSamplersPerShaderStage);
    required.maxUniformBuffersPerShaderStage =
        cap(kStageBindingShift, supported.maxUniformBuffersPerShaderStage);
    required.maxBindingsPerBindGroup =
        cap(kCbBindingCount, supported.maxBindingsPerBindGroup);
    required.maxBindGroups = cap(4, supported.maxBindGroups);

    // Features: enable BC texture compression when the adapter exposes
    // it — WC3 ships BC1/BC3/BC7 textures throughout. Same for the
    // float-blendable RTs we use for HDR.
    std::vector<wgpu::FeatureName> features;
    auto maybeEnable = [&](wgpu::FeatureName f, const char* name) {
        if (state.adapter.HasFeature(f)) {
            features.push_back(f);
        } else {
            std::fprintf(stderr, "[wgpu] adapter lacks feature %s — degraded path expected\n",
                         name);
        }
    };
    maybeEnable(wgpu::FeatureName::TextureCompressionBC, "TextureCompressionBC");
    maybeEnable(wgpu::FeatureName::Float32Filterable, "Float32Filterable");
    // The HDR scene target uses R11G11B10_FLOAT (RG11B10Ufloat). WebGPU
    // marks this format sampleable by default but only renderable when
    // the RG11B10UfloatRenderable feature is enabled.
    maybeEnable(wgpu::FeatureName::RG11B10UfloatRenderable, "RG11B10UfloatRenderable");

    wgpu::DeviceDescriptor dd{};
    dd.label = "WhiteoutFlakes";
    dd.requiredLimits = &required;
    dd.requiredFeatureCount = features.size();
    dd.requiredFeatures = features.empty() ? nullptr : features.data();
    dd.SetDeviceLostCallback(wgpu::CallbackMode::AllowSpontaneous, &DeviceLostCallback);
    dd.SetUncapturedErrorCallback(&UncapturedErrorCallback);

    wgpu::Device dev;
    wgpu::Future future = state.adapter.RequestDevice(
        &dd, wgpu::CallbackMode::WaitAnyOnly,
        [&dev](wgpu::RequestDeviceStatus status, wgpu::Device d, wgpu::StringView message) {
            if (status == wgpu::RequestDeviceStatus::Success) {
                dev = std::move(d);
            } else {
                std::fprintf(stderr, "[wgpu] RequestDevice failed: %.*s\n",
                             static_cast<int>(message.length), message.data);
            }
        });
    wgpu::FutureWaitInfo wait{future};
    state.instance.WaitAny(1, &wait, UINT64_MAX);
    if (!dev)
        return false;
    state.device = std::move(dev);
    state.queue = state.device.GetQueue();

    wgpu::Limits limits{};
    if (state.device.GetLimits(&limits) == wgpu::Status::Success) {
        state.minUniformBufferAlign =
            std::max<u64>(limits.minUniformBufferOffsetAlignment, 256ull);
    }
    return true;
}

// Build the three shared bind-group layouts the renderer assumes:
//   group 0: kCbBindingCount uniform-buffer slots, all dynamic-offset
//            (VS in [0, kStageBindingShift), PS in upper half)
//   group 1: kSrvBindingCount sampled-texture slots, both stages
//   group 2: kSamplerBindingCount sampler slots, both stages
// Every PSO uses the same PipelineLayout — matches the Vulkan backend's
// kCbSetIndex / kSrvSetIndex / kSamplerSetIndex split.
bool CreateSharedBindLayouts(WebGPUDeviceState& state) {
    // slangc emits unified bindings — both VS and PS can reference the
    // same @binding(N) @group(0) for a shared uniform buffer. CBs must
    // therefore have visibility = Vertex | Fragment so neither stage
    // gets rejected. With kStageBindingShift==12 and the per-stage cap
    // also 12, each stage still counts exactly 12 uniforms in the layout
    // — within the cap.
    //
    // For SRV / Sampler the slangc-produced WGSL DOES split by stage
    // (textures show up at @binding(0..) for VS, @binding(kStageBindingShift..)
    // for PS), so the per-binding-index visibility split still applies.
    const auto bothStages = wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment;

    // ---- CB layout (uniform buffers, both-stages-visible) ----
    {
        std::vector<wgpu::BindGroupLayoutEntry> entries;
        entries.reserve(kCbBindingCount);
        for (u32 i = 0; i < kCbBindingCount; ++i) {
            wgpu::BindGroupLayoutEntry e{};
            e.binding = i;
            e.visibility = bothStages;
            e.buffer.type = wgpu::BufferBindingType::Uniform;
            e.buffer.hasDynamicOffset = false;
            e.buffer.minBindingSize = 0;
            entries.push_back(e);
        }
        wgpu::BindGroupLayoutDescriptor d{};
        d.label = "wf.cb";
        d.entryCount = static_cast<u32>(entries.size());
        d.entries = entries.data();
        state.cbBgLayout = state.device.CreateBindGroupLayout(&d);
        if (!state.cbBgLayout) {
            std::fprintf(stderr, "[wgpu] CB BindGroupLayout creation failed\n");
            return false;
        }
    }

    // ---- SRV layout (sampled textures, per-binding type) ----
    // Slot map mirrors slang's binding hardcoded for the WGSL target:
    //   [0..12)   VS texture slots (Float / e2D, Filtering sampler)
    //   [12..22)  PS color textures (Float / e2D, Filtering sampler)
    //   [22..25)  PS shadow maps   (Depth / e2D, Comparison sampler)
    //   [25..27)  PS IBL cubemaps  (Float / CubeArray, Filtering sampler)
    //   [27]      PS BRDF LUT      (Float / e2D, Filtering sampler)
    // Slang's `Texture2D<float>` shadow declarations + the WGSL
    // depth-texture post-processor (compile_all_slang.py) emit
    // `texture_depth_2d`, which Dawn requires sampleType=Depth in the
    // layout for. The IBL cubemaps are declared as `TextureCubeArray<float4>`
    // → WGSL `texture_cube_array<f32>`, viewDimension=CubeArray.
    {
        std::vector<wgpu::BindGroupLayoutEntry> entries;
        entries.reserve(kSrvBindingCount);
        for (u32 i = 0; i < kSrvBindingCount; ++i) {
            wgpu::BindGroupLayoutEntry e{};
            e.binding = i;
            e.visibility = (i < kStageBindingShift) ? wgpu::ShaderStage::Vertex
                                                    : wgpu::ShaderStage::Fragment;
            e.texture.multisampled = false;
            if (i >= kPsShadowStartBinding && i < kPsShadowEndBinding) {
                e.texture.sampleType = wgpu::TextureSampleType::Depth;
                e.texture.viewDimension = wgpu::TextureViewDimension::e2D;
            } else if (i == kPsIblCubeFromBinding || i == kPsIblCubeToBinding) {
                e.texture.sampleType = wgpu::TextureSampleType::Float;
                e.texture.viewDimension = wgpu::TextureViewDimension::CubeArray;
            } else {
                e.texture.sampleType = wgpu::TextureSampleType::Float;
                e.texture.viewDimension = wgpu::TextureViewDimension::e2D;
            }
            entries.push_back(e);
        }
        wgpu::BindGroupLayoutDescriptor d{};
        d.label = "wf.srv";
        d.entryCount = static_cast<u32>(entries.size());
        d.entries = entries.data();
        state.srvBgLayout = state.device.CreateBindGroupLayout(&d);
        if (!state.srvBgLayout) {
            std::fprintf(stderr, "[wgpu] SRV BindGroupLayout creation failed\n");
            return false;
        }
    }

    // ---- Sampler layout (per-binding type — Comparison for shadow PCF) ----
    {
        std::vector<wgpu::BindGroupLayoutEntry> entries;
        entries.reserve(kSamplerBindingCount);
        for (u32 i = 0; i < kSamplerBindingCount; ++i) {
            wgpu::BindGroupLayoutEntry e{};
            e.binding = i;
            e.visibility = (i < kStageBindingShift) ? wgpu::ShaderStage::Vertex
                                                    : wgpu::ShaderStage::Fragment;
            if (i >= kPsShadowStartBinding && i < kPsShadowEndBinding)
                e.sampler.type = wgpu::SamplerBindingType::Comparison;
            else
                e.sampler.type = wgpu::SamplerBindingType::Filtering;
            entries.push_back(e);
        }
        wgpu::BindGroupLayoutDescriptor d{};
        d.label = "wf.sampler";
        d.entryCount = static_cast<u32>(entries.size());
        d.entries = entries.data();
        state.samplerBgLayout = state.device.CreateBindGroupLayout(&d);
        if (!state.samplerBgLayout) {
            std::fprintf(stderr, "[wgpu] Sampler BindGroupLayout creation failed\n");
            return false;
        }
    }

    const wgpu::BindGroupLayout layouts[] = {state.cbBgLayout, state.srvBgLayout,
                                             state.samplerBgLayout};
    wgpu::PipelineLayoutDescriptor pld{};
    pld.label = "wf.pipelineLayout";
    pld.bindGroupLayoutCount = 3;
    pld.bindGroupLayouts = layouts;
    state.pipelineLayout = state.device.CreatePipelineLayout(&pld);
    if (!state.pipelineLayout) {
        std::fprintf(stderr, "[wgpu] CreatePipelineLayout failed\n");
        return false;
    }
    return true;
}

// Default textures + samplers used as fallback fills for bind-group
// slots the renderer didn't populate. WebGPU rejects bind groups with
// holes, AND each layout slot has an exact sampleType / viewDimension
// the bound resource must match — so a 2D-color default can't fill a
// depth slot or a cube-array slot. We allocate one of each here.
bool CreateDefaultResources(WebGPUDeviceState& state) {
    auto makeSampler = [&](const char* label, wgpu::CompareFunction cmp,
                           wgpu::Sampler& out) {
        wgpu::SamplerDescriptor sd{};
        sd.label = label;
        sd.addressModeU = wgpu::AddressMode::Repeat;
        sd.addressModeV = wgpu::AddressMode::Repeat;
        sd.addressModeW = wgpu::AddressMode::Repeat;
        sd.magFilter = wgpu::FilterMode::Linear;
        sd.minFilter = wgpu::FilterMode::Linear;
        sd.mipmapFilter = wgpu::MipmapFilterMode::Linear;
        if (cmp != wgpu::CompareFunction::Undefined)
            sd.compare = cmp;
        out = state.device.CreateSampler(&sd);
    };
    makeSampler("wf.defaultSampler", wgpu::CompareFunction::Undefined,
                state.defaultSampler);
    // Always-pass comparison so the shadow-PCF path returns "fully lit"
    // when the renderer hasn't bound a real shadow map yet.
    makeSampler("wf.defaultCmpSampler", wgpu::CompareFunction::Always,
                state.defaultComparisonSampler);

    // 1x1 RGBA8 default color texture.
    {
        wgpu::TextureDescriptor td{};
        td.label = "wf.defaultTexture";
        td.size = {1, 1, 1};
        td.mipLevelCount = 1;
        td.sampleCount = 1;
        td.format = wgpu::TextureFormat::RGBA8Unorm;
        td.dimension = wgpu::TextureDimension::e2D;
        td.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
        state.defaultTexture = state.device.CreateTexture(&td);

        const u8 pixel[4] = {0, 0, 0, 255};
        wgpu::TexelCopyTextureInfo dst{};
        dst.texture = state.defaultTexture;
        wgpu::TexelCopyBufferLayout layout{};
        layout.bytesPerRow = 4;
        layout.rowsPerImage = 1;
        wgpu::Extent3D ext{1, 1, 1};
        state.queue.WriteTexture(&dst, pixel, sizeof(pixel), &layout, &ext);

        wgpu::TextureViewDescriptor vd{};
        state.defaultTextureView = state.defaultTexture.CreateView(&vd);
    }

    // 1x1 Depth32Float default for shadow-map fallback. RenderAttachment
    // usage is required to legally create depth textures; we never
    // render to it but Dawn enforces the usage check.
    {
        wgpu::TextureDescriptor td{};
        td.label = "wf.defaultDepth";
        td.size = {1, 1, 1};
        td.mipLevelCount = 1;
        td.sampleCount = 1;
        td.format = wgpu::TextureFormat::Depth32Float;
        td.dimension = wgpu::TextureDimension::e2D;
        td.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::RenderAttachment;
        state.defaultDepthTexture = state.device.CreateTexture(&td);

        wgpu::TextureViewDescriptor vd{};
        vd.aspect = wgpu::TextureAspect::DepthOnly;
        state.defaultDepthTextureView = state.defaultDepthTexture.CreateView(&vd);
    }

    // 1x1 cube-array (6 layers, depthOrArrayLayers=6 cube) for IBL
    // probe fallback. depthOrArrayLayers must be a multiple of 6 for
    // CubeArray, so 6 = one cube.
    {
        wgpu::TextureDescriptor td{};
        td.label = "wf.defaultCubeArr";
        td.size = {1, 1, 6};
        td.mipLevelCount = 1;
        td.sampleCount = 1;
        td.format = wgpu::TextureFormat::RGBA8Unorm;
        td.dimension = wgpu::TextureDimension::e2D;
        td.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
        state.defaultCubeArrayTexture = state.device.CreateTexture(&td);

        const u8 pixel[4] = {0, 0, 0, 255};
        wgpu::TexelCopyTextureInfo dst{};
        dst.texture = state.defaultCubeArrayTexture;
        wgpu::TexelCopyBufferLayout layout{};
        layout.bytesPerRow = 4;
        layout.rowsPerImage = 1;
        // Single face — the other five stay zero-initialised, which is
        // fine since the cube is just a "no-binding-yet" placeholder.
        wgpu::Extent3D ext{1, 1, 1};
        state.queue.WriteTexture(&dst, pixel, sizeof(pixel), &layout, &ext);

        wgpu::TextureViewDescriptor vd{};
        vd.dimension = wgpu::TextureViewDimension::CubeArray;
        vd.arrayLayerCount = 6;
        state.defaultCubeArrayTextureView = state.defaultCubeArrayTexture.CreateView(&vd);
    }

    if (!state.defaultSampler || !state.defaultComparisonSampler) {
        std::fprintf(stderr, "[wgpu] default sampler creation failed\n");
        return false;
    }
    if (!state.defaultTextureView || !state.defaultDepthTextureView ||
        !state.defaultCubeArrayTextureView) {
        std::fprintf(stderr, "[wgpu] default texture/view creation failed\n");
        return false;
    }
    return true;
}

// Tiny all-zero buffer bound to phantom vertex slots — see
// PipelineEntry::phantomVertexSlots in webgpu_handles.h. 256 bytes is
// enough for any realistic per-vertex attribute size (largest WGSL
// scalar/vector format is 16 bytes); shaders read zero through it.
void CreateZeroVertexBuffer(WebGPUDeviceState& state) {
    wgpu::BufferDescriptor bd{};
    bd.label = "wf.zeroVtx";
    bd.size = 256;
    bd.usage = wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst;
    bd.mappedAtCreation = false;
    state.zeroVertexBuffer = state.device.CreateBuffer(&bd);
    if (state.zeroVertexBuffer) {
        std::array<u8, 256> zeros{};
        state.queue.WriteBuffer(state.zeroVertexBuffer, 0, zeros.data(), zeros.size());
    }
}

// Non-fatal: CreateBuffer falls back to per-CB dedicated buffers when
// the shared ring is missing.
void CreateSharedCbRing(WebGPUDeviceState& state) {
    wgpu::BufferDescriptor bd{};
    bd.label = "wf.sharedCb";
    bd.size = kSharedCbCapacity;
    // No MapWrite — incompatible with Uniform | Storage | Vertex | Index
    // per WebGPU spec. CPU writes go through `sharedCbShadow` and get
    // pushed to the GPU buffer via Queue::WriteBuffer from MapBuffer /
    // UpdateBuffer / UnmapBuffer (see webgpu_buffer.cpp).
    bd.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::Storage |
               wgpu::BufferUsage::Vertex | wgpu::BufferUsage::Index |
               wgpu::BufferUsage::CopySrc | wgpu::BufferUsage::CopyDst;
    bd.mappedAtCreation = false;
    state.sharedCbBuffer = state.device.CreateBuffer(&bd);
    if (!state.sharedCbBuffer) {
        std::fprintf(stderr, "[wgpu] shared CB ring allocation failed; falling back\n");
        return;
    }
    state.sharedCbShadow.assign(kSharedCbCapacity, 0);
    state.sharedCbCursor = 0;
}

} // namespace

std::vector<std::string> EnumerateAdapterNames() {
    std::vector<std::string> names;
    wgpu::Instance instance = CreateInstanceWithTimedWait();
    if (!instance)
        return names;
    wgpu::PowerPreference probes[] = {wgpu::PowerPreference::HighPerformance,
                                       wgpu::PowerPreference::LowPower};
    for (auto pp : probes) {
        wgpu::Adapter adapter = RequestAdapterSync(instance, pp);
        if (!adapter)
            continue;
        std::string name = AdapterName(adapter);
        // Dedup: HighPerformance and LowPower probes often return the
        // same adapter on integrated-only systems.
        if (std::find(names.begin(), names.end(), name) == names.end())
            names.push_back(std::move(name));
    }
    return names;
}

bool WebGPUDevice::Init(bool enableValidation) {
    auto& state = *state_;
    state.enableValidation = enableValidation;

    std::fprintf(stderr, "[wgpu] Init: creating instance + adapter\n");
    if (!CreateInstanceAndAdapter(state, deviceName_)) {
        std::fprintf(stderr, "[wgpu] Init: instance/adapter step failed\n");
        return false;
    }
    std::fprintf(stderr, "[wgpu] Init: requesting device\n");
    if (!RequestDeviceSync(state)) {
        std::fprintf(stderr, "[wgpu] Init: RequestDevice step failed\n");
        return false;
    }
    std::fprintf(stderr, "[wgpu] Init: building shared bind layouts\n");
    if (!CreateSharedBindLayouts(state)) {
        std::fprintf(stderr, "[wgpu] Init: shared bind layouts failed\n");
        return false;
    }
    std::fprintf(stderr, "[wgpu] Init: building default resources\n");
    if (!CreateDefaultResources(state)) {
        std::fprintf(stderr, "[wgpu] Init: default resources failed\n");
        return false;
    }
    CreateSharedCbRing(state); // non-fatal
    CreateZeroVertexBuffer(state); // non-fatal — pipelines just skip phantom-fill if missing

    wgpu::AdapterInfo info{};
    state.adapter.GetInfo(&info);
    std::printf("[wgpu] device='%s' vendor='%.*s' minUboAlign=%llu\n", deviceName_.c_str(),
                static_cast<int>(info.vendor.length), info.vendor.data,
                static_cast<unsigned long long>(state.minUniformBufferAlign));
    return true;
}

} // namespace whiteout::flakes::gfx::webgpu

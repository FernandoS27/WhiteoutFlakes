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
// On Emscripten/emdawnwebgpu, `emscripten_webgpu_get_device()` is declared
// inside <webgpu/webgpu.h> which webgpu_cpp.h includes transitively — no
// extra emscripten/html5_webgpu.h dependency.

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

// The gfx-factory module exposes preferred-device / WebGPU-backend
// selection to backends.
namespace whiteout::flakes::gfx {
const std::string& GetPreferredDevice();
const std::string& GetWebGPUBackend();
} // namespace whiteout::flakes::gfx

namespace whiteout::flakes::gfx::webgpu {

namespace {

// Pointer to the active device state, set at Init() time, so the free-
// function DeviceLostCallback can flip the deviceLost flag. Dawn's
// DeviceLost callback signature doesn't pass userdata in this header.
WebGPUDeviceState* g_activeState = nullptr;

void DeviceLostCallback(const wgpu::Device&, wgpu::DeviceLostReason reason,
                        wgpu::StringView message) {
    if (reason == wgpu::DeviceLostReason::Destroyed)
        return; // expected at shutdown
    std::fprintf(stderr, "[wgpu] device lost: %.*s\n", static_cast<int>(message.length),
                 message.data);
    if (g_activeState)
        g_activeState->deviceLost.store(true, std::memory_order_release);
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

// Resolve the optional --wgpu-backend CLI selection (gfx::GetWebGPUBackend)
// to a Dawn BackendType. Lets us force the adapter onto D3D11 / Vulkan /
// OpenGLES on Windows when triaging device-hung issues that only repro on
// the default D3D12 backend. Empty = "let Dawn pick".
wgpu::BackendType ResolveBackendHint() {
    const std::string& hint = gfx::GetWebGPUBackend();
    if (hint.empty())
        return wgpu::BackendType::Undefined;
    std::string s = hint;
    for (auto& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (s == "d3d11")
        return wgpu::BackendType::D3D11;
    if (s == "d3d12")
        return wgpu::BackendType::D3D12;
    if (s == "vulkan" || s == "vk")
        return wgpu::BackendType::Vulkan;
    if (s == "gl" || s == "opengl")
        return wgpu::BackendType::OpenGL;
    if (s == "gles" || s == "opengles")
        return wgpu::BackendType::OpenGLES;
    if (s == "metal")
        return wgpu::BackendType::Metal;
    std::fprintf(stderr, "[wgpu] unknown --wgpu-backend='%s' (ignored)\n", hint.c_str());
    return wgpu::BackendType::Undefined;
}

// Sync wrapper around wgpu::Instance::RequestAdapter. Returns null on
// failure. Pass `powerPreference = Undefined` for "no preference".
wgpu::Adapter RequestAdapterSync(wgpu::Instance instance, wgpu::PowerPreference powerPreference) {
    wgpu::RequestAdapterOptions opts{};
    opts.powerPreference = powerPreference;
    opts.backendType = ResolveBackendHint();

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
#if defined(__EMSCRIPTEN__)
    // emdawnwebgpu's InstanceDescriptor has no `capabilities` field — and
    // the browser can't block on WaitAny from the main thread regardless.
    // The instance is purely a surface factory on the web; device + queue
    // are taken from JS via emscripten_webgpu_get_device().
    return wgpu::CreateInstance(nullptr);
#else
    // Dawn's InstanceDescriptor nests the timedWaitAny knob inside an
    // `InstanceCapabilities` substruct (`capabilities` field).
    wgpu::InstanceDescriptor desc{};
    desc.capabilities.timedWaitAnyEnable = true;
    return wgpu::CreateInstance(&desc);
#endif
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

    // Pipeline layouts follow each shader's declarations, so the per-stage
    // counts are whatever the widest shader needs (WC3 3.0 HD PS: 13 textures,
    // 12 samplers, 3 storage buffers). Ask for up to 32 of each where the
    // adapter has them; the spec defaults are 16 / 16 / 8. The dynamic-uniform
    // cap is ~8-11 everywhere, so no binding uses hasDynamicOffset — each
    // FlushBindings bakes the ring-slot offset into the BindGroupEntry.
    auto cap = [](u32 desired, u32 adapterMax) -> u32 { return std::min(desired, adapterMax); };
    wgpu::Limits required{};
    required.maxSampledTexturesPerShaderStage = cap(32, supported.maxSampledTexturesPerShaderStage);
    required.maxSamplersPerShaderStage = cap(32, supported.maxSamplersPerShaderStage);
    required.maxUniformBuffersPerShaderStage = cap(12, supported.maxUniformBuffersPerShaderStage);
    required.maxStorageBuffersPerShaderStage = cap(16, supported.maxStorageBuffersPerShaderStage);
    required.maxBindingsPerBindGroup = cap(kMaxBindingIndex, supported.maxBindingsPerBindGroup);
    required.maxBindGroups = cap(kMaxBindGroups, supported.maxBindGroups);

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
    state.hasBlockCompression =
        state.adapter.HasFeature(wgpu::FeatureName::TextureCompressionBC);
    maybeEnable(wgpu::FeatureName::Float32Filterable, "Float32Filterable");
    // The HDR scene target uses R11G11B10_FLOAT (RG11B10Ufloat). WebGPU
    // marks this format sampleable by default but only renderable when
    // the RG11B10UfloatRenderable feature is enabled.
    maybeEnable(wgpu::FeatureName::RG11B10UfloatRenderable, "RG11B10UfloatRenderable");
    // The PSO builder picks Depth32FloatStencil8 as the engine-wide
    // depth/stencil format (matching the D3D12 / Vulkan paths). In WebGPU
    // this format is gated behind an explicit feature — without it every
    // CreateRenderPipeline with a stencil-bearing dsv fails validation.
    maybeEnable(wgpu::FeatureName::Depth32FloatStencil8, "Depth32FloatStencil8");

    // Publish the active state for DeviceLostCallback. Single-device
    // application — if that ever changes we'll need a userdata-carrying
    // overload.
    g_activeState = &state;

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
        // Ring slots back both uniform and storage bindings, so they align to both.
        state.minUniformBufferAlign =
            std::max<u64>({limits.minUniformBufferOffsetAlignment,
                           limits.minStorageBufferOffsetAlignment, 256ull});
    }
    return true;
}

// Default resources used to fill bind-group entries the renderer didn't
// populate. WebGPU rejects bind groups with holes, and every layout entry has
// an exact sampleType / viewDimension the bound resource must match, so there
// is one default per kind a WGSL declaration can ask for.
bool CreateDefaultResources(WebGPUDeviceState& state) {
    auto makeSampler = [&](const char* label, wgpu::CompareFunction cmp, wgpu::Sampler& out) {
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
    makeSampler("wf.defaultSampler", wgpu::CompareFunction::Undefined, state.defaultSampler);
    // Always-pass comparison so the shadow-PCF path returns "fully lit"
    // when the renderer hasn't bound a real shadow map yet.
    makeSampler("wf.defaultCmpSampler", wgpu::CompareFunction::Always,
                state.defaultComparisonSampler);

    // One texture per format class, each 1x1 with 6 layers so the same texture
    // serves 2D, 2D-array, cube and cube-array views. Colour texels are opaque
    // black; depth stays zero-initialised, which the always-pass comparison
    // sampler still reads as lit.
    auto makeTexture = [&](const char* label, wgpu::TextureFormat fmt, wgpu::TextureDimension dim,
                           u32 layers, const u8* texel, u32 texelBytes) {
        wgpu::TextureDescriptor td{};
        td.label = label;
        td.size = {1, 1, layers};
        td.mipLevelCount = 1;
        td.sampleCount = 1;
        td.format = fmt;
        td.dimension = dim;
        td.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
        if (fmt == wgpu::TextureFormat::Depth32Float)
            td.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::RenderAttachment;
        wgpu::Texture tex = state.device.CreateTexture(&td);
        if (tex && texel) {
            wgpu::TexelCopyBufferLayout layout{};
            layout.bytesPerRow = texelBytes;
            layout.rowsPerImage = 1;
            wgpu::Extent3D ext{1, 1, 1};
            for (u32 layer = 0; layer < layers; ++layer) {
                wgpu::TexelCopyTextureInfo dst{};
                dst.texture = tex;
                dst.origin = {0, 0, dim == wgpu::TextureDimension::e3D ? 0u : layer};
                state.queue.WriteTexture(&dst, texel, texelBytes, &layout, &ext);
                if (dim == wgpu::TextureDimension::e3D)
                    break;
            }
        }
        return tex;
    };
    const u8 black[4] = {0, 0, 0, 255};
    state.defaultTexture = makeTexture("wf.defaultTexture", wgpu::TextureFormat::RGBA8Unorm,
                                       wgpu::TextureDimension::e2D, 6, black, 4);
    state.defaultDepthTexture = makeTexture("wf.defaultDepth", wgpu::TextureFormat::Depth32Float,
                                            wgpu::TextureDimension::e2D, 6, nullptr, 0);
    state.defaultUintTexture = makeTexture("wf.defaultUint", wgpu::TextureFormat::RGBA8Uint,
                                           wgpu::TextureDimension::e2D, 6, black, 4);
    state.defaultSintTexture = makeTexture("wf.defaultSint", wgpu::TextureFormat::RGBA8Sint,
                                           wgpu::TextureDimension::e2D, 6, black, 4);
    state.defaultTexture3D = makeTexture("wf.defaultTexture3D", wgpu::TextureFormat::RGBA8Unorm,
                                         wgpu::TextureDimension::e3D, 1, black, 4);

    // Zero-filled storage buffer for unbound read-only storage declarations.
    {
        wgpu::BufferDescriptor bd{};
        bd.label = "wf.defaultStorage";
        bd.size = kDefaultStorageBufferBytes;
        bd.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst;
        state.defaultStorageBuffer = state.device.CreateBuffer(&bd);
    }

    if (!state.defaultSampler || !state.defaultComparisonSampler) {
        std::fprintf(stderr, "[wgpu] default sampler creation failed\n");
        return false;
    }
    if (!state.defaultTexture || !state.defaultDepthTexture || !state.defaultUintTexture ||
        !state.defaultSintTexture || !state.defaultTexture3D || !state.defaultStorageBuffer) {
        std::fprintf(stderr, "[wgpu] default texture/buffer creation failed\n");
        return false;
    }
    return true;
}

// Tiny all-zero buffer bound to the phantom vertex slot — see
// PipelineEntry::phantomVertexSlot in webgpu_handles.h. One 16-byte lane per
// phantom attribute (the largest WGSL scalar/vector format is 16 bytes);
// shaders read zero through it.
void CreateZeroVertexBuffer(WebGPUDeviceState& state) {
    wgpu::BufferDescriptor bd{};
    bd.label = "wf.zeroVtx";
    bd.size = kZeroVertexBufferBytes;
    bd.usage = wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst;
    bd.mappedAtCreation = false;
    state.zeroVertexBuffer = state.device.CreateBuffer(&bd);
    if (state.zeroVertexBuffer) {
        std::array<u8, kZeroVertexBufferBytes> zeros{};
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
    bd.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::Storage | wgpu::BufferUsage::Vertex |
               wgpu::BufferUsage::Index | wgpu::BufferUsage::CopySrc | wgpu::BufferUsage::CopyDst;
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

#if defined(__EMSCRIPTEN__)
// Browser path: JS pre-creates the device via navigator.gpu and registers it
// on `Module.preinitializedWebGPUDevice` before the WASM module is
// instantiated. emscripten_webgpu_get_device() then hands us back the same
// device. The adapter handle is not exposed under emdawnwebgpu — every code
// path that previously consulted `state.adapter` (GetLimits, GetInfo,
// HasFeature) is guarded under !__EMSCRIPTEN__ with WebGPU spec-default
// fallbacks.
static bool AcquireDeviceFromJS(WebGPUDeviceState& state, std::string& deviceNameOut) {
    state.instance = CreateInstanceWithTimedWait();
    if (!state.instance) {
        std::fprintf(stderr, "[wgpu] CreateInstance failed under Emscripten\n");
        return false;
    }
    WGPUDevice raw = emscripten_webgpu_get_device();
    if (!raw) {
        std::fprintf(stderr,
                     "[wgpu] emscripten_webgpu_get_device returned null — JS must set "
                     "Module.preinitializedWebGPUDevice before instantiating the module\n");
        return false;
    }
    state.device = wgpu::Device::Acquire(raw);
    state.queue = state.device.GetQueue();
    // The adapter handle isn't exposed under emdawnwebgpu, but the device
    // carries the features it was created with. Query BC support here — JS only
    // requests texture-compression-bc when the adapter has it, so on GPUs that
    // lack it (Mali / many mobile parts) this is false and the asset + IBL paths
    // decompress BCn to RGBA8 instead of creating unsupported BC textures.
    state.hasBlockCompression = state.device.HasFeature(wgpu::FeatureName::TextureCompressionBC);
    std::fprintf(stderr, "[wgpu] block-compression (BC) support: %s\n",
                 state.hasBlockCompression ? "yes" : "no");
    deviceNameOut = "WebGPU (browser)";
    // No adapter handle on the web — hardcode the WebGPU spec-default
    // minUniformBufferOffsetAlignment (256 bytes; also our internal floor).
    state.minUniformBufferAlign = 256ull;
    return true;
}
#endif

bool WebGPUDevice::Init(bool enableValidation) {
    auto& state = *state_;
    state.enableValidation = enableValidation;

#if defined(__EMSCRIPTEN__)
    std::fprintf(stderr, "[wgpu] Init: acquiring device from JS\n");
    if (!AcquireDeviceFromJS(state, deviceName_)) {
        std::fprintf(stderr, "[wgpu] Init: JS device handoff failed\n");
        return false;
    }
#else
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
#endif
    state.float32Filterable = state.device.HasFeature(wgpu::FeatureName::Float32Filterable);
    std::fprintf(stderr, "[wgpu] Init: building default resources\n");
    if (!CreateDefaultResources(state)) {
        std::fprintf(stderr, "[wgpu] Init: default resources failed\n");
        return false;
    }
    CreateSharedCbRing(state);     // non-fatal
    CreateZeroVertexBuffer(state); // non-fatal — pipelines just skip phantom-fill if missing

#if !defined(__EMSCRIPTEN__)
    wgpu::AdapterInfo info{};
    state.adapter.GetInfo(&info);
    std::printf("[wgpu] device='%s' vendor='%.*s' minUboAlign=%llu\n", deviceName_.c_str(),
                static_cast<int>(info.vendor.length), info.vendor.data,
                static_cast<unsigned long long>(state.minUniformBufferAlign));
#else
    std::printf("[wgpu] device='%s' minUboAlign=%llu\n", deviceName_.c_str(),
                static_cast<unsigned long long>(state.minUniformBufferAlign));
#endif
    return true;
}

} // namespace whiteout::flakes::gfx::webgpu

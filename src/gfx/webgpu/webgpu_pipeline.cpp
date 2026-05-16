// Shader / pipeline / sampler create + destroy. Mirrors
// src/gfx/vulkan/vulkan_pipeline.cpp's vertex-input handling: BLS
// "ATTR<N>" semantics map directly to @location(N) in WGSL; HLSL semantics
// fall back to declaration order.

#include "webgpu_device.h"
#include "webgpu_device_state.h"
#include "webgpu_handles.h"
#include "webgpu_translate.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

namespace whiteout::flakes::gfx::webgpu {

namespace {

bool IsAttrSemantic(const char* s) {
    return s && s[0] == 'A' && s[1] == 'T' && s[2] == 'T' && s[3] == 'R' && s[4] == '\0';
}

} // namespace

ShaderHandle WebGPUDevice::CreateShader(ShaderStage stage, const void* bytecode, usize size) {
    if (!bytecode || size == 0)
        return ShaderHandle::Invalid;
    auto& state = *state_;

    // The renderer hands us either WGSL source (UTF-8, null-terminated by
    // embed_shaders.cmake / BLS WGSL packer) or — when someone tries to
    // bind D3D/Vulkan bytecode by mistake — a binary blob we can't handle.
    // We treat the buffer as WGSL text; the WGSL parser will reject
    // anything that isn't.
    wgpu::ShaderSourceWGSL wgslSrc{};
    // BLS WGSL blobs include a trailing NUL; embed_shaders.cmake does the
    // same. wgpu::StringView accepts an explicit length.
    usize textLen = size;
    const char* asText = static_cast<const char*>(bytecode);
    if (textLen > 0 && asText[textLen - 1] == '\0')
        --textLen;
    wgslSrc.code = wgpu::StringView{asText, textLen};

    wgpu::ShaderModuleDescriptor smd{};
    smd.nextInChain = &wgslSrc;
    smd.label = "wf.shader";
    wgpu::ShaderModule mod = state.device.CreateShaderModule(&smd);
    if (!mod)
        return ShaderHandle::Invalid;

    ShaderEntry entry{};
    entry.module = std::move(mod);
    entry.stage = stage;
    entry.entryPoint = "main";
    return static_cast<ShaderHandle>(state.shaders.Insert(std::move(entry)));
}

PipelineHandle WebGPUDevice::CreateGraphicsPipeline(const GraphicsPipelineDesc& desc) {
    auto& state = *state_;

    auto* vs = state.shaders.Get(static_cast<u64>(desc.vs));
    auto* ps = state.shaders.Get(static_cast<u64>(desc.ps));
    if (!vs)
        return PipelineHandle::Invalid;

    // ---- Vertex layout: one VertexBufferLayout per used input slot ----
    std::array<u32, 8> slotStride{};
    std::array<bool, 8> slotUsed{};
    std::array<std::vector<wgpu::VertexAttribute>, 8> slotAttrs{};
    for (u32 i = 0; i < desc.inputLayout.size(); ++i) {
        const auto& el = desc.inputLayout[i];
        if (el.inputSlot >= slotUsed.size())
            continue;
        slotUsed[el.inputSlot] = true;
        wgpu::VertexAttribute a{};
        a.shaderLocation = IsAttrSemantic(el.semantic) ? el.semanticIndex : i;
        a.offset = el.offset;
        a.format = ToWgpuVertexFormat(el.format);
        slotAttrs[el.inputSlot].push_back(a);
        const u32 elemSize = FormatBytesPerBlock(el.format);
        slotStride[el.inputSlot] =
            std::max<u32>(slotStride[el.inputSlot], el.offset + elemSize);
    }
    std::vector<wgpu::VertexBufferLayout> buffers;
    for (u32 i = 0; i < slotUsed.size(); ++i) {
        if (!slotUsed[i])
            continue;
        wgpu::VertexBufferLayout vbl{};
        vbl.arrayStride = slotStride[i];
        vbl.stepMode = wgpu::VertexStepMode::Vertex;
        vbl.attributeCount = static_cast<u32>(slotAttrs[i].size());
        vbl.attributes = slotAttrs[i].data();
        buffers.push_back(vbl);
    }

    // ---- Color target + blend ----
    wgpu::ColorTargetState colorTarget{};
    colorTarget.format = ToWgpuFormat(desc.rtvFormat);
    wgpu::BlendState blend{};
    blend.color.srcFactor = ToWgpuBlendFactor(desc.blend.srcColor);
    blend.color.dstFactor = ToWgpuBlendFactor(desc.blend.dstColor);
    blend.color.operation = ToWgpuBlendOp(desc.blend.opColor);
    blend.alpha.srcFactor = ToWgpuBlendFactor(desc.blend.srcAlpha);
    blend.alpha.dstFactor = ToWgpuBlendFactor(desc.blend.dstAlpha);
    blend.alpha.operation = ToWgpuBlendOp(desc.blend.opAlpha);
    if (desc.blend.enable)
        colorTarget.blend = &blend;
    colorTarget.writeMask = desc.blend.colorWrite ? wgpu::ColorWriteMask::All
                                                   : wgpu::ColorWriteMask::None;

    wgpu::FragmentState frag{};
    frag.module = ps ? ps->module : wgpu::ShaderModule{};
    frag.entryPoint = ps ? ps->entryPoint.c_str() : "main";
    frag.targetCount = ps ? 1 : 0;
    frag.targets = ps ? &colorTarget : nullptr;

    // ---- Depth/stencil ----
    wgpu::DepthStencilState depth{};
    bool hasDepth = (desc.dsvFormat != Format::Unknown);
    if (hasDepth) {
        depth.format = ToWgpuFormat(desc.dsvFormat);
        depth.depthWriteEnabled = desc.depthStencil.depthWrite ? wgpu::OptionalBool::True
                                                               : wgpu::OptionalBool::False;
        depth.depthCompare = desc.depthStencil.depthTest
                                 ? ToWgpuCompare(desc.depthStencil.depthCompare)
                                 : wgpu::CompareFunction::Always;
        depth.depthBias = desc.rasterizer.depthBias;
        depth.depthBiasSlopeScale = desc.rasterizer.slopeScaledDepthBias;
        depth.depthBiasClamp = desc.rasterizer.depthBiasClamp;
    }

    wgpu::RenderPipelineDescriptor rpd{};
    rpd.label = "wf.gfxPipeline";
    rpd.layout = state.pipelineLayout;
    rpd.vertex.module = vs->module;
    rpd.vertex.entryPoint = vs->entryPoint.c_str();
    rpd.vertex.bufferCount = static_cast<u32>(buffers.size());
    rpd.vertex.buffers = buffers.data();
    rpd.primitive.topology = ToWgpuTopology(desc.topology);
    rpd.primitive.cullMode = ToWgpuCull(desc.rasterizer.cull);
    rpd.primitive.frontFace =
        desc.rasterizer.frontCCW ? wgpu::FrontFace::CCW : wgpu::FrontFace::CW;
    rpd.depthStencil = hasDepth ? &depth : nullptr;
    rpd.multisample.count = 1;
    rpd.fragment = ps ? &frag : nullptr;

    wgpu::RenderPipeline pso = state.device.CreateRenderPipeline(&rpd);
    if (!pso)
        return PipelineHandle::Invalid;

    PipelineEntry entry{};
    entry.graphics = std::move(pso);
    entry.isCompute = false;
    entry.colorFormat = colorTarget.format;
    return static_cast<PipelineHandle>(state.pipelines.Insert(std::move(entry)));
}

PipelineHandle WebGPUDevice::CreateComputePipeline(const ComputePipelineDesc& desc) {
    auto& state = *state_;
    auto* cs = state.shaders.Get(static_cast<u64>(desc.cs));
    if (!cs)
        return PipelineHandle::Invalid;

    wgpu::ComputePipelineDescriptor cpd{};
    cpd.label = "wf.computePipeline";
    cpd.layout = state.pipelineLayout;
    cpd.compute.module = cs->module;
    cpd.compute.entryPoint = cs->entryPoint.c_str();
    wgpu::ComputePipeline pso = state.device.CreateComputePipeline(&cpd);
    if (!pso)
        return PipelineHandle::Invalid;

    PipelineEntry entry{};
    entry.compute = std::move(pso);
    entry.isCompute = true;
    return static_cast<PipelineHandle>(state.pipelines.Insert(std::move(entry)));
}

SamplerHandle WebGPUDevice::CreateSampler(const SamplerDesc& desc) {
    auto& state = *state_;
    wgpu::SamplerDescriptor sd{};
    sd.label = "wf.sampler";
    sd.addressModeU = ToWgpuAddress(desc.addressU);
    sd.addressModeV = ToWgpuAddress(desc.addressV);
    sd.addressModeW = ToWgpuAddress(desc.addressW);
    sd.minFilter = ToWgpuFilter(desc.minFilter);
    sd.magFilter = ToWgpuFilter(desc.magFilter);
    sd.mipmapFilter = ToWgpuMipFilter(desc.minFilter);
    sd.lodMinClamp = 0.0f;
    sd.lodMaxClamp = 32.0f;
    sd.maxAnisotropy = 1;
    if (desc.comparison)
        sd.compare = ToWgpuCompare(desc.comparisonFunc);

    SamplerEntry entry{};
    entry.sampler = state.device.CreateSampler(&sd);
    if (!entry.sampler)
        return SamplerHandle::Invalid;
    return static_cast<SamplerHandle>(state.samplers.Insert(std::move(entry)));
}

void WebGPUDevice::Destroy(ShaderHandle h) {
    auto& state = *state_;
    auto* shader = state.shaders.Get(static_cast<u64>(h));
    if (!shader)
        return;
    ShaderEntry moved = std::move(*shader);
    state.shaders.Remove(static_cast<u64>(h));
    std::lock_guard<std::mutex> lock(state.deleteMutex);
    state.pendingDeletes.push_back(PendingDelete{
        state.pendingEpoch + 1,
        [owned = std::move(moved)]() mutable { (void)owned; },
    });
}

void WebGPUDevice::Destroy(PipelineHandle h) {
    auto& state = *state_;
    auto* pipe = state.pipelines.Get(static_cast<u64>(h));
    if (!pipe)
        return;
    PipelineEntry moved = std::move(*pipe);
    state.pipelines.Remove(static_cast<u64>(h));
    std::lock_guard<std::mutex> lock(state.deleteMutex);
    state.pendingDeletes.push_back(PendingDelete{
        state.pendingEpoch + 1,
        [owned = std::move(moved)]() mutable { (void)owned; },
    });
}

} // namespace whiteout::flakes::gfx::webgpu

// Shader / pipeline / sampler create + destroy.

#include "vulkan_device.h"
#include "vulkan_device_state.h"
#include "vulkan_handles.h"
#include "vulkan_translate.h"

#include <array>
#include <cstdio>
#include <utility>
#include <vector>

namespace whiteout::flakes::gfx::vulkan {

ShaderHandle VulkanDevice::CreateShader(ShaderStage stage, const void* bytecode, usize size) {
    // Null perms (slang strips perms with collapsed Conditional<>)
    // hit Vulkan's codeSize==0 reject — return Invalid; never bound.
    if (size == 0 || bytecode == nullptr)
        return ShaderHandle::Invalid;
    auto& state = *state_;
    auto modR = state.device.createShaderModule({
        .codeSize = size,
        .pCode = static_cast<const u32*>(bytecode),
    });
    if (modR.result != vk::Result::eSuccess) {
        return ShaderHandle::Invalid;
    }
    ShaderEntry entry{};
    entry.module = std::move(modR.value);
    entry.stage = stage;
    return static_cast<ShaderHandle>(state.shaders.Insert(std::move(entry)));
}

PipelineHandle VulkanDevice::CreateGraphicsPipeline(const GraphicsPipelineDesc& desc) {
    auto& state = *state_;

    auto* vs = state.shaders.Get(static_cast<u64>(desc.vs));
    auto* ps = state.shaders.Get(static_cast<u64>(desc.ps));
    if (!vs)
        return PipelineHandle::Invalid;

    // ---- Shader stages ----
    std::array<vk::PipelineShaderStageCreateInfo, 2> stages{};
    u32 stageCount = 0;
    stages[stageCount++] = vk::PipelineShaderStageCreateInfo{
        .stage = vk::ShaderStageFlagBits::eVertex,
        .module = *vs->module,
        .pName = "main",
    };
    if (ps) {
        stages[stageCount++] = vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eFragment,
            .module = *ps->module,
            .pName = "main",
        };
    }

    // One VkVertexInputBinding per used slot; stride = max(offset + elem).
    std::array<vk::VertexInputBindingDescription, 8> bindings{};
    std::array<u32, 8> slotStride{};
    std::array<bool, 8> slotUsed{};
    std::vector<vk::VertexInputAttributeDescription> attrs;
    attrs.reserve(desc.inputLayout.size());

    for (u32 i = 0; i < desc.inputLayout.size(); ++i) {
        const auto& el = desc.inputLayout[i];
        if (el.inputSlot >= slotUsed.size())
            continue;
        slotUsed[el.inputSlot] = true;
        // BLS "ATTR<N>" semantics map directly to Location=N.
        // HLSL semantics (POSITION/COLOR/...) fall back to declaration order.
        const bool useSemanticIndex = el.semantic && el.semantic[0] == 'A' &&
                                      el.semantic[1] == 'T' && el.semantic[2] == 'T' &&
                                      el.semantic[3] == 'R' && el.semantic[4] == '\0';
        attrs.push_back(vk::VertexInputAttributeDescription{
            .location = useSemanticIndex ? el.semanticIndex : i,
            .binding = el.inputSlot,
            .format = ToVkFormat(el.format),
            .offset = el.offset,
        });
        u32 elemSize = 4;
        switch (el.format) {
        case Format::R32G32B32A32_FLOAT:
            elemSize = 16;
            break;
        case Format::R32G32B32_FLOAT:
            elemSize = 12;
            break;
        case Format::R32G32_FLOAT:
            elemSize = 8;
            break;
        case Format::R8G8B8A8_UNORM:
        case Format::R8G8B8A8_UNORM_SRGB:
        case Format::R8G8B8A8_UINT:
        case Format::B8G8R8A8_UNORM:
        case Format::R32_FLOAT:
        case Format::R32_UINT:
        case Format::R11G11B10_FLOAT:
        case Format::R16G16_UNORM:
            elemSize = 4;
            break;
        default:
            break;
        }
        slotStride[el.inputSlot] = std::max(slotStride[el.inputSlot], el.offset + elemSize);
    }

    u32 bindingCount = 0;
    for (u32 slot = 0; slot < slotUsed.size(); ++slot) {
        if (!slotUsed[slot])
            continue;
        bindings[bindingCount++] = vk::VertexInputBindingDescription{
            .binding = slot,
            .stride = slotStride[slot],
            .inputRate = vk::VertexInputRate::eVertex,
        };
    }

    vk::PipelineVertexInputStateCreateInfo vi{
        .vertexBindingDescriptionCount = bindingCount,
        .pVertexBindingDescriptions = bindings.data(),
        .vertexAttributeDescriptionCount = static_cast<u32>(attrs.size()),
        .pVertexAttributeDescriptions = attrs.data(),
    };

    // ---- Input assembly / rasterizer / depth-stencil / blend ----
    vk::PipelineInputAssemblyStateCreateInfo ia{
        .topology = ToVkTopology(desc.topology),
    };

    vk::PipelineViewportStateCreateInfo vp{
        .viewportCount = 1,
        .scissorCount = 1,
    };

    vk::PipelineRasterizationStateCreateInfo rs{
        .polygonMode = ToVkPolygonMode(desc.rasterizer.fill),
        .cullMode = ToVkCull(desc.rasterizer.cull),
        // Winding is re-evaluated in clip space, before the negative-
        // height viewport flip, so frontCCW maps directly.
        .frontFace =
            desc.rasterizer.frontCCW ? vk::FrontFace::eCounterClockwise : vk::FrontFace::eClockwise,
        .depthBiasEnable =
            (desc.rasterizer.depthBias != 0 || desc.rasterizer.slopeScaledDepthBias != 0.0f)
                ? vk::True
                : vk::False,
        .depthBiasConstantFactor = static_cast<f32>(desc.rasterizer.depthBias),
        .depthBiasClamp = desc.rasterizer.depthBiasClamp,
        .depthBiasSlopeFactor = desc.rasterizer.slopeScaledDepthBias,
        .lineWidth = 1.0f,
    };

    vk::PipelineMultisampleStateCreateInfo ms{
        .rasterizationSamples = vk::SampleCountFlagBits::e1,
    };

    vk::PipelineDepthStencilStateCreateInfo ds{
        .depthTestEnable = desc.depthStencil.depthTest ? vk::True : vk::False,
        .depthWriteEnable = desc.depthStencil.depthWrite ? vk::True : vk::False,
        .depthCompareOp = ToVkCompareOp(desc.depthStencil.depthCompare),
    };

    vk::PipelineColorBlendAttachmentState blendAttach{
        .blendEnable = desc.blend.enable ? vk::True : vk::False,
        .srcColorBlendFactor = ToVkBlendFactor(desc.blend.srcColor),
        .dstColorBlendFactor = ToVkBlendFactor(desc.blend.dstColor),
        .colorBlendOp = ToVkBlendOp(desc.blend.opColor),
        .srcAlphaBlendFactor = ToVkBlendFactor(desc.blend.srcAlpha),
        .dstAlphaBlendFactor = ToVkBlendFactor(desc.blend.dstAlpha),
        .alphaBlendOp = ToVkBlendOp(desc.blend.opAlpha),
        .colorWriteMask = desc.blend.colorWrite
                              ? (vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                                 vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA)
                              : vk::ColorComponentFlags{},
    };
    vk::PipelineColorBlendStateCreateInfo blendState{
        .attachmentCount = (desc.rtvFormat == Format::Unknown) ? 0u : 1u,
        .pAttachments = (desc.rtvFormat == Format::Unknown) ? nullptr : &blendAttach,
    };

    const std::array<vk::DynamicState, 2> dynStates{
        vk::DynamicState::eViewport,
        vk::DynamicState::eScissor,
    };
    vk::PipelineDynamicStateCreateInfo dynState{
        .dynamicStateCount = static_cast<u32>(dynStates.size()),
        .pDynamicStates = dynStates.data(),
    };

    const vk::Format rtvFmt = ToVkFormat(desc.rtvFormat);
    const vk::Format dsvFmt = ToVkFormat(desc.dsvFormat);
    vk::PipelineRenderingCreateInfo renderingInfo{
        .colorAttachmentCount = (rtvFmt == vk::Format::eUndefined) ? 0u : 1u,
        .pColorAttachmentFormats = (rtvFmt == vk::Format::eUndefined) ? nullptr : &rtvFmt,
        .depthAttachmentFormat = dsvFmt,
    };

    vk::GraphicsPipelineCreateInfo gpci{
        .pNext = &renderingInfo,
        .stageCount = stageCount,
        .pStages = stages.data(),
        .pVertexInputState = &vi,
        .pInputAssemblyState = &ia,
        .pViewportState = &vp,
        .pRasterizationState = &rs,
        .pMultisampleState = &ms,
        .pDepthStencilState = &ds,
        .pColorBlendState = &blendState,
        .pDynamicState = &dynState,
        .layout = *state.pipelineLayout,
    };

    // Guards the case where load+create both failed at Init.
    auto pR = *state.pipelineCache ? state.device.createGraphicsPipeline(state.pipelineCache, gpci)
                                   : state.device.createGraphicsPipeline(nullptr, gpci);
    if (pR.result != vk::Result::eSuccess) {
        std::fprintf(stderr, "[vk] createGraphicsPipeline failed (%s)\n",
                     vk::to_string(pR.result).c_str());
        return PipelineHandle::Invalid;
    }
    PipelineEntry entry{};
    entry.pipeline = std::move(pR.value);
    entry.isCompute = false;
    entry.colorFormat = rtvFmt;
    return static_cast<PipelineHandle>(state.pipelines.Insert(std::move(entry)));
}

PipelineHandle VulkanDevice::CreateComputePipeline(const ComputePipelineDesc& desc) {
    auto& state = *state_;
    auto* cs = state.shaders.Get(static_cast<u64>(desc.cs));
    if (!cs) {
        std::fprintf(stderr, "[vk] CreateComputePipeline: invalid compute shader\n");
        return PipelineHandle::Invalid;
    }
    vk::ComputePipelineCreateInfo cpci{
        .stage =
            vk::PipelineShaderStageCreateInfo{
                .stage = vk::ShaderStageFlagBits::eCompute,
                .module = *cs->module,
                .pName = "main",
            },
        .layout = *state.computeLayout,
    };
    auto pR = *state.pipelineCache ? state.device.createComputePipeline(state.pipelineCache, cpci)
                                   : state.device.createComputePipeline(nullptr, cpci);
    if (pR.result != vk::Result::eSuccess) {
        std::fprintf(stderr, "[vk] createComputePipeline failed (%s)\n",
                     vk::to_string(pR.result).c_str());
        return PipelineHandle::Invalid;
    }
    PipelineEntry entry{};
    entry.pipeline = std::move(pR.value);
    entry.isCompute = true;
    return static_cast<PipelineHandle>(state.pipelines.Insert(std::move(entry)));
}

SamplerHandle VulkanDevice::CreateSampler(const SamplerDesc& desc) {
    auto& state = *state_;
    auto sR = state.device.createSampler({
        .magFilter = ToVkFilter(desc.magFilter),
        .minFilter = ToVkFilter(desc.minFilter),
        .mipmapMode = (desc.minFilter == Filter::Linear) ? vk::SamplerMipmapMode::eLinear
                                                         : vk::SamplerMipmapMode::eNearest,
        .addressModeU = ToVkAddressMode(desc.addressU),
        .addressModeV = ToVkAddressMode(desc.addressV),
        .addressModeW = ToVkAddressMode(desc.addressW),
        .compareEnable = desc.comparison ? vk::True : vk::False,
        .compareOp = ToVkCompareOp(desc.comparisonFunc),
        .maxLod = VK_LOD_CLAMP_NONE,
    });
    if (sR.result != vk::Result::eSuccess)
        return SamplerHandle::Invalid;
    SamplerEntry entry{};
    entry.sampler = std::move(sR.value);
    return static_cast<SamplerHandle>(state.samplers.Insert(std::move(entry)));
}

// Pure-raii entries: lookup, move into a deleter that just lets the
// raii members destruct on lambda exit.
template <typename Handle, typename Entry, typename Map>
static void QueueRaiiDestroy(VulkanDeviceState& state, Handle h, Map& map) {
    auto* entry = map.Get(static_cast<u64>(h));
    if (!entry)
        return;
    Entry moved = std::move(*entry);
    map.Remove(static_cast<u64>(h));
    state.pendingDeletes.push_back(
        MakePendingDelete(state.nextSubmitValue,
                          [owned = std::move(moved)](VulkanDeviceState&) mutable { (void)owned; }));
}

void VulkanDevice::Destroy(ShaderHandle h) {
    QueueRaiiDestroy<ShaderHandle, ShaderEntry>(*state_, h, state_->shaders);
}
void VulkanDevice::Destroy(PipelineHandle h) {
    QueueRaiiDestroy<PipelineHandle, PipelineEntry>(*state_, h, state_->pipelines);
}
void VulkanDevice::Destroy(SamplerHandle h) {
    QueueRaiiDestroy<SamplerHandle, SamplerEntry>(*state_, h, state_->samplers);
}

} // namespace whiteout::flakes::gfx::vulkan

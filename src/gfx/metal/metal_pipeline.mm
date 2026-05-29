// Shader / pipeline / sampler create + destroy. The split mirrors
// webgpu_pipeline.cpp: CreateShader wraps a metallib blob in an
// id<MTLLibrary> + resolves the stage's MTLFunction by name;
// CreateGraphicsPipeline builds an MTLRenderPipelineState + a separate
// MTLDepthStencilState; CreateSampler builds an MTLSamplerState.
//
// FlushBindings in metal_command_list.mm consumes these via setVertex*/
// setFragment*/setRenderPipelineState/setDepthStencilState calls.

#include "metal_device.h"
#include "metal_device_state.h"
#include "metal_handles.h"
#include "metal_translate.h"

#import <Metal/Metal.h>

#include "whiteout/flakes/gfx_types.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <utility>

namespace whiteout::flakes::gfx::metal {

namespace {

// ---- Enum translators (sampler) ----

MTLSamplerMinMagFilter ToMtlMinMag(Filter f) {
    return f == Filter::Point ? MTLSamplerMinMagFilterNearest
                              : MTLSamplerMinMagFilterLinear;
}

MTLSamplerMipFilter ToMtlMip(Filter f) {
    return f == Filter::Point ? MTLSamplerMipFilterNearest
                              : MTLSamplerMipFilterLinear;
}

MTLSamplerAddressMode ToMtlAddress(AddressMode a) {
    switch (a) {
    case AddressMode::Wrap:
        return MTLSamplerAddressModeRepeat;
    case AddressMode::Clamp:
        return MTLSamplerAddressModeClampToEdge;
    case AddressMode::Mirror:
        return MTLSamplerAddressModeMirrorRepeat;
    }
    return MTLSamplerAddressModeClampToEdge;
}

MTLCompareFunction ToMtlCompare(CompareOp op) {
    switch (op) {
    case CompareOp::Never:
        return MTLCompareFunctionNever;
    case CompareOp::Less:
        return MTLCompareFunctionLess;
    case CompareOp::LessEqual:
        return MTLCompareFunctionLessEqual;
    case CompareOp::Equal:
        return MTLCompareFunctionEqual;
    case CompareOp::Greater:
        return MTLCompareFunctionGreater;
    case CompareOp::GreaterEqual:
        return MTLCompareFunctionGreaterEqual;
    case CompareOp::Always:
        return MTLCompareFunctionAlways;
    }
    return MTLCompareFunctionAlways;
}

// ---- Enum translators (pipeline) ----

MTLBlendFactor ToMtlBlendFactor(BlendFactor f) {
    switch (f) {
    case BlendFactor::Zero:
        return MTLBlendFactorZero;
    case BlendFactor::One:
        return MTLBlendFactorOne;
    case BlendFactor::SrcAlpha:
        return MTLBlendFactorSourceAlpha;
    case BlendFactor::InvSrcAlpha:
        return MTLBlendFactorOneMinusSourceAlpha;
    case BlendFactor::SrcColor:
        return MTLBlendFactorSourceColor;
    case BlendFactor::DstColor:
        return MTLBlendFactorDestinationColor;
    case BlendFactor::InvSrcColor:
        return MTLBlendFactorOneMinusSourceColor;
    case BlendFactor::InvDstColor:
        return MTLBlendFactorOneMinusDestinationColor;
    case BlendFactor::DstAlpha:
        return MTLBlendFactorDestinationAlpha;
    case BlendFactor::InvDstAlpha:
        return MTLBlendFactorOneMinusDestinationAlpha;
    }
    return MTLBlendFactorZero;
}

MTLBlendOperation ToMtlBlendOp(BlendOp op) {
    return op == BlendOp::Subtract ? MTLBlendOperationSubtract : MTLBlendOperationAdd;
}

MTLVertexFormat ToMtlVertexFormat(Format f) {
    switch (f) {
    case Format::R8_UNORM:
        return MTLVertexFormatUCharNormalized;
    case Format::R8G8_UNORM:
        return MTLVertexFormatUChar2Normalized;
    case Format::R8G8B8A8_UNORM:
        return MTLVertexFormatUChar4Normalized;
    case Format::R8G8B8A8_UINT:
        return MTLVertexFormatUChar4;
    case Format::B8G8R8A8_UNORM:
        // Metal has no BGRA8 vertex format. Renderer-side input layouts
        // that use BGRA8 mean "shader reads RGBA"; the swizzle is on the
        // shader side. Translate to UChar4Normalized — the bytes are the
        // same on disk; the shader interprets them.
        return MTLVertexFormatUChar4Normalized;
    case Format::R16_UNORM:
        return MTLVertexFormatUShortNormalized;
    case Format::R16G16_UNORM:
        return MTLVertexFormatUShort2Normalized;
    case Format::R16G16B16A16_UNORM:
        return MTLVertexFormatUShort4Normalized;
    case Format::R16G16B16A16_FLOAT:
        return MTLVertexFormatHalf4;
    case Format::R16_UINT:
        return MTLVertexFormatUShort;
    case Format::R32_UINT:
        return MTLVertexFormatUInt;
    case Format::R32_FLOAT:
        return MTLVertexFormatFloat;
    case Format::R32G32_FLOAT:
        return MTLVertexFormatFloat2;
    case Format::R32G32B32_FLOAT:
        return MTLVertexFormatFloat3;
    case Format::R32G32B32A32_FLOAT:
        return MTLVertexFormatFloat4;
    default:
        // Anything not in this set (e.g. depth formats, BC formats) has
        // no vertex-attribute meaning — fall back to Float4 so PSO
        // creation surfaces the error rather than silently misaligning.
        return MTLVertexFormatFloat4;
    }
}

MTLPrimitiveType ToMtlPrimitive(PrimitiveTopology t) {
    switch (t) {
    case PrimitiveTopology::TriangleList:
        return MTLPrimitiveTypeTriangle;
    case PrimitiveTopology::TriangleStrip:
        return MTLPrimitiveTypeTriangleStrip;
    case PrimitiveTopology::LineList:
        return MTLPrimitiveTypeLine;
    }
    return MTLPrimitiveTypeTriangle;
}

MTLCullMode ToMtlCull(CullMode c) {
    switch (c) {
    case CullMode::None:
        return MTLCullModeNone;
    case CullMode::Back:
        return MTLCullModeBack;
    case CullMode::Front:
        return MTLCullModeFront;
    }
    return MTLCullModeNone;
}

// ---- Entry-point name resolution ----

// Pick the MTLFunction matching the stage. Slang emits stage-tagged
// names but the function metadata is canonical — every MTLFunction
// carries its functionType. When multiple match, return the first
// (typical for our libs — one entry point per stage).
id<MTLFunction> ResolveEntryFunction(id<MTLLibrary> lib, ShaderStage stage,
                                     std::string* outName) {
    if (!lib)
        return nil;
    MTLFunctionType wanted = MTLFunctionTypeVertex;
    if (stage == ShaderStage::Pixel)
        wanted = MTLFunctionTypeFragment;
    else if (stage == ShaderStage::Compute)
        wanted = MTLFunctionTypeKernel;

    NSArray<NSString*>* names = [lib functionNames];
    for (NSString* n in names) {
        id<MTLFunction> fn = [lib newFunctionWithName:n];
        if (fn && [fn functionType] == wanted) {
            if (outName)
                *outName = [n UTF8String];
            return fn;
        }
    }
    return nil;
}

} // namespace

// ============================================================
// CreateShader
// ============================================================

ShaderHandle MetalDevice::CreateShader(ShaderStage stage, const void* bytecode, usize size) {
    @autoreleasepool {
        auto& state = *state_;
        if (!bytecode || size == 0)
            return ShaderHandle::Invalid;

        // dispatch_data_create copies (with DISPATCH_DATA_DESTRUCTOR_DEFAULT)
        // — we don't have to keep `bytecode` alive past this call.
        dispatch_data_t blob = dispatch_data_create(bytecode, size,
                                                    dispatch_get_main_queue(),
                                                    DISPATCH_DATA_DESTRUCTOR_DEFAULT);
        NSError* err = nil;
        id<MTLLibrary> lib = [state.device newLibraryWithData:blob error:&err];
        if (!lib) {
            std::fprintf(stderr,
                "[gfx/metal] newLibraryWithData failed: %s\n",
                err ? [[err localizedDescription] UTF8String] : "(no error info)");
            return ShaderHandle::Invalid;
        }

        std::string entryName;
        id<MTLFunction> fn = ResolveEntryFunction(lib, stage, &entryName);
        if (!fn) {
            std::fprintf(stderr,
                "[gfx/metal] no MTLFunction matching stage=%d in library "
                "(functions=%lu)\n",
                static_cast<int>(stage),
                (unsigned long)[[lib functionNames] count]);
            return ShaderHandle::Invalid;
        }
        if (state.validationRequested)
            fn.label = [NSString stringWithUTF8String:entryName.c_str()];

        ShaderEntry entry;
        entry.library = lib;
        entry.function = fn;
        entry.stage = stage;
        entry.entryPoint = std::move(entryName);
        return static_cast<ShaderHandle>(state.shaders.Insert(std::move(entry)));
    }
}

void MetalDevice::Destroy(ShaderHandle h) {
    auto& state = *state_;
    auto* s = state.shaders.Get(static_cast<u64>(h));
    if (!s)
        return;
    ShaderEntry moved = std::move(*s);
    state.shaders.Remove(static_cast<u64>(h));

    const u64 retireAfter = state.pendingEpoch + 1;
    id<MTLLibrary> lib = moved.library;
    id<MTLFunction> fn = moved.function;
    {
        std::lock_guard<std::mutex> lock(state.pendingDeletesMutex);
        state.pendingDeletes.push_back(PendingDelete{
            retireAfter,
            [lib, fn]() {
                (void)lib;
                (void)fn;
            },
        });
    }
}

// ============================================================
// CreateGraphicsPipeline
// ============================================================

PipelineHandle MetalDevice::CreateGraphicsPipeline(const GraphicsPipelineDesc& desc) {
    @autoreleasepool {
        auto& state = *state_;

        auto* vs = state.shaders.Get(static_cast<u64>(desc.vs));
        if (!vs || !vs->function) {
            std::fprintf(stderr, "[gfx/metal] CreateGraphicsPipeline: missing VS\n");
            return PipelineHandle::Invalid;
        }
        auto* ps = state.shaders.Get(static_cast<u64>(desc.ps));
        // PS is optional (e.g. depth-only passes — though we don't ship
        // any yet on Wc3). When present it must have a valid function.

        MTLRenderPipelineDescriptor* rpd = [[MTLRenderPipelineDescriptor alloc] init];
        rpd.vertexFunction = vs->function;
        if (ps && ps->function)
            rpd.fragmentFunction = ps->function;

        // ---- Vertex input layout ----
        // Metal's stage_in path: per-slot MTLVertexBufferLayoutDescriptor
        // at index (kVertexBufferIndexBase + slot); per-attribute
        // MTLVertexAttributeDescriptor with bufferIndex pointing back at
        // the slot. Stride is computed as max(attr.offset + format-bytes).
        if (!desc.inputLayout.empty()) {
            MTLVertexDescriptor* vd = [[MTLVertexDescriptor alloc] init];

            // Per-slot byte stride. Computed as the high-water mark of
            // (offset + size) across every attribute on that slot.
            u32 slotStride[kMaxVertexBufferSlots] = {0};
            bool slotUsed[kMaxVertexBufferSlots] = {false};

            for (u32 i = 0; i < desc.inputLayout.size(); ++i) {
                const auto& el = desc.inputLayout[i];
                if (el.inputSlot >= kMaxVertexBufferSlots)
                    continue;
                slotUsed[el.inputSlot] = true;
                const u32 elemSize = FormatBytesPerBlock(el.format);
                slotStride[el.inputSlot] =
                    std::max<u32>(slotStride[el.inputSlot], el.offset + elemSize);

                MTLVertexAttributeDescriptor* a = vd.attributes[i];
                a.format = ToMtlVertexFormat(el.format);
                a.offset = el.offset;
                a.bufferIndex = kVertexBufferIndexBase + el.inputSlot;
            }
            for (u32 s = 0; s < kMaxVertexBufferSlots; ++s) {
                if (!slotUsed[s])
                    continue;
                MTLVertexBufferLayoutDescriptor* layout =
                    vd.layouts[kVertexBufferIndexBase + s];
                layout.stride = slotStride[s];
                layout.stepFunction = MTLVertexStepFunctionPerVertex;
                layout.stepRate = 1;
            }
            rpd.vertexDescriptor = vd;
        }

        // ---- Color attachment + blend ----
        MTLRenderPipelineColorAttachmentDescriptor* color = rpd.colorAttachments[0];
        const MTLPixelFormat colorFmt = ToMtlPixelFormat(desc.rtvFormat);
        color.pixelFormat = colorFmt;
        color.writeMask =
            desc.blend.colorWrite ? MTLColorWriteMaskAll : MTLColorWriteMaskNone;
        color.blendingEnabled = desc.blend.enable;
        if (desc.blend.enable) {
            color.sourceRGBBlendFactor = ToMtlBlendFactor(desc.blend.srcColor);
            color.destinationRGBBlendFactor = ToMtlBlendFactor(desc.blend.dstColor);
            color.rgbBlendOperation = ToMtlBlendOp(desc.blend.opColor);
            color.sourceAlphaBlendFactor = ToMtlBlendFactor(desc.blend.srcAlpha);
            color.destinationAlphaBlendFactor = ToMtlBlendFactor(desc.blend.dstAlpha);
            color.alphaBlendOperation = ToMtlBlendOp(desc.blend.opAlpha);
        }
        rpd.alphaToCoverageEnabled = desc.blend.alphaToCoverage;

        // ---- Depth / stencil attachment formats (on the PSO) ----
        const MTLPixelFormat depthFmt = (desc.dsvFormat == Format::Unknown)
                                            ? MTLPixelFormatInvalid
                                            : ToMtlPixelFormat(desc.dsvFormat);
        if (depthFmt != MTLPixelFormatInvalid) {
            rpd.depthAttachmentPixelFormat = depthFmt;
            if (HasStencilAspect(depthFmt))
                rpd.stencilAttachmentPixelFormat = depthFmt;
        }

        if (state.validationRequested)
            rpd.label = @"wf.pso";

        NSError* err = nil;
        id<MTLRenderPipelineState> pso =
            [state.device newRenderPipelineStateWithDescriptor:rpd error:&err];
        if (!pso) {
            std::fprintf(stderr,
                "[gfx/metal] newRenderPipelineState failed: %s\n",
                err ? [[err localizedDescription] UTF8String] : "(no error info)");
            return PipelineHandle::Invalid;
        }

        // ---- Depth-stencil state (separate Metal object) ----
        id<MTLDepthStencilState> dss = nil;
        if (depthFmt != MTLPixelFormatInvalid) {
            MTLDepthStencilDescriptor* dsd = [[MTLDepthStencilDescriptor alloc] init];
            dsd.depthCompareFunction =
                desc.depthStencil.depthTest
                    ? ToMtlCompare(desc.depthStencil.depthCompare)
                    : MTLCompareFunctionAlways;
            dsd.depthWriteEnabled = desc.depthStencil.depthWrite;
            dss = [state.device newDepthStencilStateWithDescriptor:dsd];
        }

        PipelineEntry entry;
        entry.graphics = pso;
        entry.depthStencil = dss;
        entry.isCompute = false;
        entry.colorFormat = colorFmt;
        entry.depthFormat = depthFmt;
        entry.primitive = ToMtlPrimitive(desc.topology);
        entry.cull = ToMtlCull(desc.rasterizer.cull);
        // Wc3 conventions: front-CCW unless explicitly opted into; the
        // engine flag is `frontCCW` and the gfx default is `false`
        // (so default = front-CW). Match the flag verbatim.
        entry.winding =
            desc.rasterizer.frontCCW ? MTLWindingCounterClockwise : MTLWindingClockwise;
        entry.hasFragment = ps && ps->function;

        return static_cast<PipelineHandle>(state.pipelines.Insert(std::move(entry)));
    }
}

PipelineHandle MetalDevice::CreateComputePipeline(const ComputePipelineDesc&) {
    // Compute lands in Phase G.
    return PipelineHandle::Invalid;
}

void MetalDevice::Destroy(PipelineHandle h) {
    auto& state = *state_;
    auto* p = state.pipelines.Get(static_cast<u64>(h));
    if (!p)
        return;
    PipelineEntry moved = std::move(*p);
    state.pipelines.Remove(static_cast<u64>(h));

    const u64 retireAfter = state.pendingEpoch + 1;
    id<MTLRenderPipelineState> gfx = moved.graphics;
    id<MTLComputePipelineState> comp = moved.compute;
    id<MTLDepthStencilState> dss = moved.depthStencil;
    {
        std::lock_guard<std::mutex> lock(state.pendingDeletesMutex);
        state.pendingDeletes.push_back(PendingDelete{
            retireAfter,
            [gfx, comp, dss]() {
                (void)gfx;
                (void)comp;
                (void)dss;
            },
        });
    }
}

// ============================================================
// CreateSampler
// ============================================================

SamplerHandle MetalDevice::CreateSampler(const SamplerDesc& desc) {
    @autoreleasepool {
        auto& state = *state_;

        MTLSamplerDescriptor* sd = [[MTLSamplerDescriptor alloc] init];
        sd.minFilter = ToMtlMinMag(desc.minFilter);
        sd.magFilter = ToMtlMinMag(desc.magFilter);
        sd.mipFilter = ToMtlMip(desc.minFilter);
        sd.sAddressMode = ToMtlAddress(desc.addressU);
        sd.tAddressMode = ToMtlAddress(desc.addressV);
        sd.rAddressMode = ToMtlAddress(desc.addressW);
        sd.maxAnisotropy = 1;
        sd.lodMinClamp = 0.0f;
        sd.lodMaxClamp = FLT_MAX;
        if (desc.comparison)
            sd.compareFunction = ToMtlCompare(desc.comparisonFunc);

        id<MTLSamplerState> sampler = [state.device newSamplerStateWithDescriptor:sd];
        if (!sampler)
            return SamplerHandle::Invalid;
        if (state.validationRequested)
            sd.label = @"wf.sampler";

        SamplerEntry entry;
        entry.sampler = sampler;
        return static_cast<SamplerHandle>(state.samplers.Insert(std::move(entry)));
    }
}

void MetalDevice::Destroy(SamplerHandle h) {
    auto& state = *state_;
    auto* s = state.samplers.Get(static_cast<u64>(h));
    if (!s)
        return;
    SamplerEntry moved = std::move(*s);
    state.samplers.Remove(static_cast<u64>(h));

    const u64 retireAfter = state.pendingEpoch + 1;
    id<MTLSamplerState> ms = moved.sampler;
    {
        std::lock_guard<std::mutex> lock(state.pendingDeletesMutex);
        state.pendingDeletes.push_back(PendingDelete{
            retireAfter,
            [ms]() { (void)ms; },
        });
    }
}

} // namespace whiteout::flakes::gfx::metal

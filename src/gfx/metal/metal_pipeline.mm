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

// Map MTLDataType (the type of a single [[attribute]] field as the
// reflection API reports it) to a matching MTLVertexFormat. The
// phantom-attribute path feeds zeros, so the only constraint is that
// the format's element count + element type match what the shader
// declared — Metal's PSO build rejects format/type mismatches.
MTLVertexFormat PhantomFormatFor(MTLDataType dt) {
    switch (dt) {
    case MTLDataTypeFloat:    return MTLVertexFormatFloat;
    case MTLDataTypeFloat2:   return MTLVertexFormatFloat2;
    case MTLDataTypeFloat3:   return MTLVertexFormatFloat3;
    case MTLDataTypeFloat4:   return MTLVertexFormatFloat4;
    case MTLDataTypeInt:      return MTLVertexFormatInt;
    case MTLDataTypeInt2:     return MTLVertexFormatInt2;
    case MTLDataTypeInt3:     return MTLVertexFormatInt3;
    case MTLDataTypeInt4:     return MTLVertexFormatInt4;
    case MTLDataTypeUInt:     return MTLVertexFormatUInt;
    case MTLDataTypeUInt2:    return MTLVertexFormatUInt2;
    case MTLDataTypeUInt3:    return MTLVertexFormatUInt3;
    case MTLDataTypeUInt4:    return MTLVertexFormatUInt4;
    case MTLDataTypeShort:    return MTLVertexFormatShort;
    case MTLDataTypeShort2:   return MTLVertexFormatShort2;
    case MTLDataTypeShort4:   return MTLVertexFormatShort4;
    case MTLDataTypeUShort:   return MTLVertexFormatUShort;
    case MTLDataTypeUShort2:  return MTLVertexFormatUShort2;
    case MTLDataTypeUShort4:  return MTLVertexFormatUShort4;
    case MTLDataTypeChar:     return MTLVertexFormatChar;
    case MTLDataTypeChar2:    return MTLVertexFormatChar2;
    case MTLDataTypeChar4:    return MTLVertexFormatChar4;
    case MTLDataTypeUChar:    return MTLVertexFormatUChar;
    case MTLDataTypeUChar2:   return MTLVertexFormatUChar2;
    case MTLDataTypeUChar4:   return MTLVertexFormatUChar4;
    case MTLDataTypeHalf:     return MTLVertexFormatHalf;
    case MTLDataTypeHalf2:    return MTLVertexFormatHalf2;
    case MTLDataTypeHalf4:    return MTLVertexFormatHalf4;
    default:
        // Unknown: a 4-float read of zeros is valid for any vec3/vec4
        // shader use the slang side might decay this to.
        return MTLVertexFormatFloat4;
    }
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

        // Capture declared vertex attributes so CreateGraphicsPipeline
        // can synthesize phantom layouts for any not in InputLayout.
        // [function vertexAttributes] is nil for non-vertex stages.
        if (stage == ShaderStage::Vertex) {
            NSArray<MTLVertexAttribute*>* attrs = [fn vertexAttributes];
            entry.declaredVertexAttrs.reserve([attrs count]);
            for (MTLVertexAttribute* a in attrs) {
                ShaderEntry::VertexAttr va;
                va.index = static_cast<u32>([a attributeIndex]);
                va.format = PhantomFormatFor([a attributeType]);
                entry.declaredVertexAttrs.push_back(va);
            }
        }

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
        //
        // Phantom-attribute synthesis: slangc's Metal emit always declares
        // every [[attribute(N)]] in the input struct even when
        // specialization eliminates the body that reads it. Metal
        // validates the [[stage_in]] declaration strictly — any
        // declared attribute not in InputLayout fails PSO creation
        // (`Vertex attribute X is missing from the vertex descriptor`).
        // We mirror the WebGPU backend's fix: every declared attribute
        // with no InputLayout entry gets its own one-attribute
        // VertexBufferLayout at a high buffer index, fed by the
        // device-wide zero buffer at draw time.
        std::vector<u32> phantomBufferIndices;
        const bool needVertexDescriptor =
            !desc.inputLayout.empty() || (vs && !vs->declaredVertexAttrs.empty());
        if (needVertexDescriptor) {
            MTLVertexDescriptor* vd = [[MTLVertexDescriptor alloc] init];

            // Per-slot byte stride. Computed as the high-water mark of
            // (offset + size) across every attribute on that slot.
            u32 slotStride[kMaxVertexBufferSlots] = {0};
            bool slotUsed[kMaxVertexBufferSlots] = {false};

            // Track which attribute indices the InputLayout supplies so
            // the phantom pass can identify the misses.
            std::vector<u32> suppliedAttrIndices;
            suppliedAttrIndices.reserve(desc.inputLayout.size());

            // Slang's Metal emit assigns [[attribute(N)]] to each VS
            // input struct field where N is the HLSL semantic index
            // (e.g. ATTR3 → [[attribute(3)]]). The MTLVertexDescriptor
            // attribute slot index MUST match that, NOT the InputLayout
            // array order — otherwise the engine's "ATTR7 → tangent at
            // inputSlot 1" lands on vd.attributes[<some other index>]
            // and the shader's [[attribute(7)]] reads from a phantom
            // zero buffer instead. Mirrors webgpu_pipeline.cpp's
            // shaderLocation = el.semanticIndex pattern.
            auto attrIndexFor = [](const InputElement& el, u32 fallback) -> u32 {
                const char* s = el.semantic;
                const bool isAttr = s && s[0] == 'A' && s[1] == 'T' &&
                                    s[2] == 'T' && s[3] == 'R' && s[4] == '\0';
                return isAttr ? el.semanticIndex : fallback;
            };

            for (u32 i = 0; i < desc.inputLayout.size(); ++i) {
                const auto& el = desc.inputLayout[i];
                if (el.inputSlot >= kMaxVertexBufferSlots)
                    continue;
                slotUsed[el.inputSlot] = true;
                const u32 elemSize = FormatBytesPerBlock(el.format);
                slotStride[el.inputSlot] =
                    std::max<u32>(slotStride[el.inputSlot], el.offset + elemSize);

                const u32 attrIdx = attrIndexFor(el, i);
                MTLVertexAttributeDescriptor* a = vd.attributes[attrIdx];
                a.format = ToMtlVertexFormat(el.format);
                a.offset = el.offset;
                a.bufferIndex = kVertexBufferIndexBase + el.inputSlot;
                suppliedAttrIndices.push_back(attrIdx);
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

            // Synthesize phantom layouts for every declared attribute
            // missing from the InputLayout. Each phantom gets its own
            // buffer index in [kPhantomVertexBufferIndexBase,
            // kPhantomVertexBufferIndexBase + kMaxPhantomVertexAttrs);
            // FlushBindings binds state.zeroVertexBuffer to every
            // active phantom index before each Draw.
            u32 nextPhantomIdx = kPhantomVertexBufferIndexBase;
            if (vs) {
                for (const auto& va : vs->declaredVertexAttrs) {
                    bool supplied = false;
                    for (u32 idx : suppliedAttrIndices) {
                        if (idx == va.index) {
                            supplied = true;
                            break;
                        }
                    }
                    if (supplied)
                        continue;
                    if (nextPhantomIdx >= kPhantomVertexBufferIndexBase +
                                              kMaxPhantomVertexAttrs) {
                        std::fprintf(stderr,
                            "[gfx/metal] phantom-attribute budget exhausted "
                            "(attr=%u). Bump kMaxPhantomVertexAttrs.\n",
                            va.index);
                        break;
                    }
                    MTLVertexAttributeDescriptor* a = vd.attributes[va.index];
                    a.format = va.format;
                    a.offset = 0;
                    a.bufferIndex = nextPhantomIdx;

                    MTLVertexBufferLayoutDescriptor* layout =
                        vd.layouts[nextPhantomIdx];
                    layout.stride = 16;
                    // Constant step = same 16 bytes read for every
                    // vertex. With a zero-filled source buffer this
                    // gives zeros for every attribute read. Metal
                    // requires stepRate=0 when stepFunction=Constant
                    // (the validation error message is explicit).
                    layout.stepFunction = MTLVertexStepFunctionConstant;
                    layout.stepRate = 0;
                    phantomBufferIndices.push_back(nextPhantomIdx);
                    ++nextPhantomIdx;
                }
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

        // Color-only variant: same descriptor with depth/stencil
        // attachment formats cleared. Built lazily — only when the
        // primary variant declares a depth format (otherwise the
        // primary variant IS the color-only variant and no second
        // build is needed). Metal's strict format match between PSO
        // and render pass means a pass with no depth attachment can
        // only consume a PSO whose depth attachment format is
        // Invalid; the renderer happily binds the same PSO into both
        // pass types, so we keep two ready.
        id<MTLRenderPipelineState> psoColorOnly = nil;
        if (depthFmt != MTLPixelFormatInvalid) {
            rpd.depthAttachmentPixelFormat = MTLPixelFormatInvalid;
            rpd.stencilAttachmentPixelFormat = MTLPixelFormatInvalid;
            if (state.validationRequested)
                rpd.label = @"wf.pso.color-only";
            NSError* err2 = nil;
            psoColorOnly =
                [state.device newRenderPipelineStateWithDescriptor:rpd error:&err2];
            if (!psoColorOnly) {
                std::fprintf(stderr,
                    "[gfx/metal] color-only PSO variant build failed: %s\n",
                    err2 ? [[err2 localizedDescription] UTF8String] : "(no error)");
                // Non-fatal — the depth-aware PSO is still usable for depth
                // passes. Binding it inside a color-only pass will hit the
                // same validation error we were trying to avoid, but the
                // depth-pass path is unaffected.
            }
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
        entry.graphicsColorOnly = psoColorOnly;
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
        entry.phantomBufferIndices = std::move(phantomBufferIndices);

        return static_cast<PipelineHandle>(state.pipelines.Insert(std::move(entry)));
    }
}

PipelineHandle MetalDevice::CreateComputePipeline(const ComputePipelineDesc& desc) {
    @autoreleasepool {
        auto& state = *state_;

        auto* cs = state.shaders.Get(static_cast<u64>(desc.cs));
        if (!cs || !cs->function) {
            std::fprintf(stderr, "[gfx/metal] CreateComputePipeline: missing CS\n");
            return PipelineHandle::Invalid;
        }

        // Compute PSO is essentially just the kernel MTLFunction wrapped
        // by Metal — no descriptors / attachments / vertex shape, unlike
        // the graphics path. The threadgroup size travels separately
        // (PipelineEntry::computeThreads{X,Y,Z}); see the constants in
        // metal_handles.h for why the default is (8,8,1).
        NSError* err = nil;
        id<MTLComputePipelineState> pso =
            [state.device newComputePipelineStateWithFunction:cs->function error:&err];
        if (!pso) {
            std::fprintf(stderr,
                "[gfx/metal] newComputePipelineState failed: %s\n",
                err ? [[err localizedDescription] UTF8String] : "(no error info)");
            return PipelineHandle::Invalid;
        }

        PipelineEntry entry;
        entry.compute = pso;
        entry.isCompute = true;
        // Defaults (8,8,1) — see kDefaultComputeThreads* docstring.
        entry.computeThreadsX = kDefaultComputeThreadsX;
        entry.computeThreadsY = kDefaultComputeThreadsY;
        entry.computeThreadsZ = kDefaultComputeThreadsZ;

        return static_cast<PipelineHandle>(state.pipelines.Insert(std::move(entry)));
    }
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
    id<MTLRenderPipelineState> gfxColorOnly = moved.graphicsColorOnly;
    id<MTLComputePipelineState> comp = moved.compute;
    id<MTLDepthStencilState> dss = moved.depthStencil;
    {
        std::lock_guard<std::mutex> lock(state.pendingDeletesMutex);
        state.pendingDeletes.push_back(PendingDelete{
            retireAfter,
            [gfx, gfxColorOnly, comp, dss]() {
                (void)gfx;
                (void)gfxColorOnly;
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

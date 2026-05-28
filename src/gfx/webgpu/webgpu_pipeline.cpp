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

// Skip ASCII whitespace + WGSL line/block comments.
const char* SkipWsAndComments(const char* p, const char* end) {
    while (p < end) {
        if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
            ++p;
            continue;
        }
        if (end - p >= 2 && p[0] == '/' && p[1] == '/') {
            while (p < end && *p != '\n')
                ++p;
            continue;
        }
        if (end - p >= 2 && p[0] == '/' && p[1] == '*') {
            p += 2;
            while (p + 1 < end && !(p[0] == '*' && p[1] == '/'))
                ++p;
            if (p + 1 < end)
                p += 2;
            continue;
        }
        break;
    }
    return p;
}

bool IsIdent(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}

// Pick a vertex format that matches the WGSL field type the entry
// declares — Dawn validates that VertexBufferLayout::format is type-
// compatible with the shader's @location declaration (f32 vs u32 vs
// i32, and the component count). Zero buffer reads as all zeros for
// every supported format so the exact representation doesn't matter.
wgpu::VertexFormat PickPhantomFormat(const std::string& typeName) {
    auto isI = typeName == "i32";
    auto isU = typeName == "u32";
    auto vec = [&](char which, int n) -> wgpu::VertexFormat {
        switch (n) {
        case 1:
            return which == 'u'   ? wgpu::VertexFormat::Uint32
                   : which == 'i' ? wgpu::VertexFormat::Sint32
                                  : wgpu::VertexFormat::Float32;
        case 2:
            return which == 'u'   ? wgpu::VertexFormat::Uint32x2
                   : which == 'i' ? wgpu::VertexFormat::Sint32x2
                                  : wgpu::VertexFormat::Float32x2;
        case 3:
            // WebGPU has no x3 integer formats — pick x4 since the
            // buffer is all-zeros anyway and Dawn rounds up.
            return which == 'u'   ? wgpu::VertexFormat::Uint32x4
                   : which == 'i' ? wgpu::VertexFormat::Sint32x4
                                  : wgpu::VertexFormat::Float32x3;
        default:
            return which == 'u'   ? wgpu::VertexFormat::Uint32x4
                   : which == 'i' ? wgpu::VertexFormat::Sint32x4
                                  : wgpu::VertexFormat::Float32x4;
        }
    };
    if (isU)
        return vec('u', 1);
    if (isI)
        return vec('i', 1);
    if (typeName == "f32")
        return vec('f', 1);
    // vecN<T> form.
    if (typeName.size() >= 8 && typeName[0] == 'v' && typeName[1] == 'e' && typeName[2] == 'c') {
        const int n = typeName[3] - '0';
        // Find <T> between '<' and '>'.
        const auto lt = typeName.find('<');
        const auto gt = typeName.find('>');
        if (n >= 1 && n <= 4 && lt != std::string::npos && gt != std::string::npos && gt > lt) {
            const std::string t = typeName.substr(lt + 1, gt - lt - 1);
            if (t == "u32")
                return vec('u', n);
            if (t == "i32")
                return vec('i', n);
            return vec('f', n);
        }
    }
    // Fallback — generous Float32x4 is the WebGPU default attribute width.
    return wgpu::VertexFormat::Float32x4;
}

// Pull `<word>` immediately after a `:` (the field's type identifier).
// Stops at the first non-type character so vec types with `<...>` are
// captured whole.
std::string ParseTypeAfterColon(const char* p, const char* end) {
    p = SkipWsAndComments(p, end);
    std::string out;
    while (p < end) {
        const char c = *p;
        const bool ok = IsIdent(c) || c == '<' || c == '>' || c == ',' || c == ' ';
        if (!ok)
            break;
        if (c == ',' || (c == ' ' && !out.empty() && out.back() != '<' && out.back() != ',' &&
                         out.find('<') == std::string::npos))
            break;
        if (c != ' ')
            out.push_back(c);
        ++p;
    }
    return out;
}

// Parse every `@location(N) name : type` annotation between [p, end)
// into `out`, deduped by location. Used for both inline-arg annotations
// on the entry signature and member annotations inside an input struct.
void CollectLocationFieldsIn(const char* p, const char* end,
                             std::vector<VertexInputLocation>& out) {
    while (p < end) {
        const char* hit = static_cast<const char*>(std::memchr(p, '@', end - p));
        if (!hit)
            break;
        p = hit + 1;
        const char* kw = "location";
        const usize kwLen = 8;
        if (end - p < static_cast<ptrdiff_t>(kwLen + 1))
            break;
        if (std::memcmp(p, kw, kwLen) != 0)
            continue;
        p += kwLen;
        p = SkipWsAndComments(p, end);
        if (p >= end || *p != '(')
            continue;
        ++p;
        p = SkipWsAndComments(p, end);
        u32 v = 0;
        bool any = false;
        while (p < end && *p >= '0' && *p <= '9') {
            v = v * 10 + static_cast<u32>(*p - '0');
            ++p;
            any = true;
        }
        if (!any)
            continue;
        // Walk to the next `:` (the field's type follows) and grab it.
        const char* colon = static_cast<const char*>(std::memchr(p, ':', end - p));
        std::string typeName;
        if (colon)
            typeName = ParseTypeAfterColon(colon + 1, end);
        bool dup = false;
        for (auto& f : out)
            if (f.location == v) {
                dup = true;
                break;
            }
        if (!dup) {
            VertexInputLocation field;
            field.location = v;
            field.typeName = std::move(typeName);
            out.push_back(std::move(field));
        }
    }
}

// Scan WGSL VS source for the @location set the entry function consumes
// Find the entry-point function name for the given stage by scanning for
// the appropriate `@vertex` / `@fragment` / `@compute` attribute followed
// by `fn <name>(`. Compile_all_slang.py post-processes slang's WGSL output
// to rename the entry to `main`, but its regex requires `@stage` to be
// *immediately* followed by `fn` — slangc emits `@compute @workgroup_size(...) fn name`
// for every compute shader, so the rename silently no-ops on those and the
// original `<entry>_main`-style name survives. Scanning the WGSL at load
// time picks up whatever name is actually present, regardless of which
// slangc + post-processor combo produced the bundle.
std::string FindEntryPointName(const char* src, usize len, ShaderStage stage) {
    if (!src || len == 0)
        return "main";
    const char* end = src + len;
    const char* attr = nullptr;
    usize attrLen = 0;
    switch (stage) {
    case ShaderStage::Vertex:
        attr = "@vertex";
        attrLen = 7;
        break;
    case ShaderStage::Pixel:
        attr = "@fragment";
        attrLen = 9;
        break;
    case ShaderStage::Compute:
        attr = "@compute";
        attrLen = 8;
        break;
    default:
        return "main";
    }
    for (const char* p = src; p + attrLen + 4 < end; ++p) {
        if (std::memcmp(p, attr, attrLen) != 0)
            continue;
        // Walk forward, skipping whitespace, comments, and other `@…(…)`
        // attributes (e.g. `@workgroup_size(8, 8, 1)`), until we hit `fn`.
        const char* q = p + attrLen;
        while (q < end) {
            q = SkipWsAndComments(q, end);
            if (q >= end)
                break;
            if (*q == '@') {
                ++q;
                while (q < end && IsIdent(*q))
                    ++q;
                if (q < end && *q == '(') {
                    int d = 0;
                    for (; q < end; ++q) {
                        if (*q == '(')
                            ++d;
                        else if (*q == ')') {
                            --d;
                            if (d == 0) {
                                ++q;
                                break;
                            }
                        }
                    }
                }
                continue;
            }
            if (end - q >= 2 && q[0] == 'f' && q[1] == 'n' && (q + 2 >= end || !IsIdent(q[2])))
                break;
            // Anything else — give up, this isn't a stage-attribute we know.
            q = end;
        }
        if (q >= end)
            continue;
        // q points at `fn`; advance past it and any whitespace, read ident.
        q = SkipWsAndComments(q + 2, end);
        const char* nameStart = q;
        while (q < end && IsIdent(*q))
            ++q;
        if (q > nameStart)
            return std::string(nameStart, q - nameStart);
    }
    return "main";
}

// — i.e. the input struct's members, plus any inline `@location` on the
// entry's argument list. The post-processor in compile_all_slang.py
// rewrites slang's `@vertex fn vs_main` down to `fn main`, so we look
// for the first `fn main(` and walk its parameter list.
//
// This is a lexical scan, not a real WGSL parser: enough because slangc
// emits each binding annotation as the literal token `@location(<int>)`
// with no preprocessor mangling. We deliberately ignore @location on
// VS *outputs* (after `->`) and on `VSOutput_0`-style structs whose
// names aren't named as the entry's argument type.
std::vector<VertexInputLocation> ScanVertexLocations(const char* src, usize len,
                                                     const std::string& entryName) {
    std::vector<VertexInputLocation> out;
    if (!src || len == 0 || entryName.empty())
        return out;
    const char* end = src + len;

    // Find `fn <entryName>(` — the entry-point name varies across slangc
    // versions and post-processors (see FindEntryPointName).
    const std::string needle = "fn " + entryName;
    const char* fnMain = nullptr;
    for (const char* p = src; p + static_cast<isize>(needle.size()) + 1 < end; ++p) {
        if (std::memcmp(p, needle.data(), needle.size()) != 0)
            continue;
        if (p > src && IsIdent(p[-1]))
            continue;
        const char* after = p + needle.size();
        if (after < end && IsIdent(*after))
            continue;
        const char* q = SkipWsAndComments(after, end);
        if (q < end && *q == '(') {
            fnMain = q;
            break;
        }
    }
    if (!fnMain)
        return out;

    // Walk to the matching `)`.
    const char* parenEnd = fnMain;
    int depth = 0;
    for (const char* p = fnMain; p < end; ++p) {
        if (*p == '(')
            ++depth;
        else if (*p == ')') {
            --depth;
            if (depth == 0) {
                parenEnd = p;
                break;
            }
        }
    }
    if (parenEnd == fnMain)
        return out;

    // Inline annotations on the entry's arg list.
    CollectLocationFieldsIn(fnMain + 1, parenEnd, out);

    // Collect parameter type names — every `: <Identifier>` inside the
    // param list — and pull @location members out of each one's struct
    // body. Lets us handle both the inline-arg form and the
    // `fn main(in : VsInput)` form.
    for (const char* p = fnMain + 1; p < parenEnd; ++p) {
        if (*p != ':')
            continue;
        const char* q = SkipWsAndComments(p + 1, parenEnd);
        const char* nameStart = q;
        while (q < parenEnd && IsIdent(*q))
            ++q;
        if (q == nameStart)
            continue;
        std::string typeName(nameStart, q - nameStart);
        // Find `struct <typeName>` somewhere in the source.
        const std::string needle = "struct " + typeName;
        const char* structPos = nullptr;
        for (const char* r = src; r + needle.size() < end; ++r) {
            if (std::memcmp(r, needle.data(), needle.size()) != 0)
                continue;
            const char* after = r + needle.size();
            if (after < end && IsIdent(*after))
                continue;
            structPos = r;
            break;
        }
        if (!structPos)
            continue;
        const char* brace = static_cast<const char*>(std::memchr(structPos, '{', end - structPos));
        if (!brace)
            continue;
        int sd = 0;
        const char* braceEnd = brace;
        for (const char* r = brace; r < end; ++r) {
            if (*r == '{')
                ++sd;
            else if (*r == '}') {
                --sd;
                if (sd == 0) {
                    braceEnd = r;
                    break;
                }
            }
        }
        CollectLocationFieldsIn(brace, braceEnd, out);
    }
    return out;
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
    entry.entryPoint = FindEntryPointName(asText, textLen, stage);
    if (stage == ShaderStage::Vertex)
        entry.vertexLocations = ScanVertexLocations(asText, textLen, entry.entryPoint);
    return static_cast<ShaderHandle>(state.shaders.Insert(std::move(entry)));
}

PipelineHandle WebGPUDevice::CreateGraphicsPipeline(const GraphicsPipelineDesc& desc) {
    auto& state = *state_;

    auto* vs = state.shaders.Get(static_cast<u64>(desc.vs));
    auto* ps = state.shaders.Get(static_cast<u64>(desc.ps));
    if (!vs)
        return PipelineHandle::Invalid;

    // ---- Vertex layout: one VertexBufferLayout per used input slot ----
    constexpr u32 kMaxRealSlots = 8;
    std::array<u32, kMaxRealSlots> slotStride{};
    std::array<bool, kMaxRealSlots> slotUsed{};
    std::array<std::vector<wgpu::VertexAttribute>, kMaxRealSlots> slotAttrs{};
    std::vector<u32> declaredLocations;
    declaredLocations.reserve(desc.inputLayout.size());
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
        slotStride[el.inputSlot] = std::max<u32>(slotStride[el.inputSlot], el.offset + elemSize);
        declaredLocations.push_back(a.shaderLocation);
    }

    // Phantom-attribute synthesis: every VS @location() the renderer's
    // InputLayout didn't supply gets its own one-attribute layout fed by
    // the device's shared zero buffer at draw time. Without this, Dawn
    // rejects the pipeline as soon as the shader entry references an
    // unbound @location. Format is picked to match the shader's declared
    // type so Dawn's type-compat check passes. Skipped silently when
    // zeroVertexBuffer didn't create (init.cpp logs separately).
    std::vector<VertexInputLocation> phantomFields;
    std::vector<u32> phantomSlots;
    if (state.zeroVertexBuffer) {
        for (const auto& vl : vs->vertexLocations) {
            if (std::find(declaredLocations.begin(), declaredLocations.end(), vl.location) ==
                declaredLocations.end())
                phantomFields.push_back(vl);
        }
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
    // Phantom layouts: one VertexBufferLayout each, format derived from
    // the VS field's declared WGSL type. The zero buffer reads as zero
    // for every supported format, so accuracy beyond type-compat
    // doesn't matter — Dawn just needs format & shader type to agree.
    std::vector<wgpu::VertexAttribute> phantomAttrs(phantomFields.size());
    for (usize i = 0; i < phantomFields.size(); ++i) {
        const u32 slot = static_cast<u32>(buffers.size());
        phantomSlots.push_back(slot);
        phantomAttrs[i].shaderLocation = phantomFields[i].location;
        phantomAttrs[i].offset = 0;
        phantomAttrs[i].format = PickPhantomFormat(phantomFields[i].typeName);
        wgpu::VertexBufferLayout vbl{};
        vbl.arrayStride = 16;
        vbl.stepMode = wgpu::VertexStepMode::Instance;
        vbl.attributeCount = 1;
        vbl.attributes = &phantomAttrs[i];
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
    colorTarget.writeMask =
        desc.blend.colorWrite ? wgpu::ColorWriteMask::All : wgpu::ColorWriteMask::None;

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
        depth.depthWriteEnabled =
            desc.depthStencil.depthWrite ? wgpu::OptionalBool::True : wgpu::OptionalBool::False;
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
    rpd.primitive.frontFace = desc.rasterizer.frontCCW ? wgpu::FrontFace::CCW : wgpu::FrontFace::CW;
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
    entry.phantomVertexSlots = std::move(phantomSlots);
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

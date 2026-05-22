// Texture create / destroy + render targets + initial-data upload.
// Mirrors src/gfx/vulkan/vulkan_texture.cpp; WebGPU's queue.WriteTexture
// removes the staging buffer we manage by hand in the Vulkan path.

#include "webgpu_device.h"
#include "webgpu_device_state.h"
#include "webgpu_handles.h"
#include "webgpu_translate.h"

#include "whiteout/flakes/gfx_types.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace whiteout::flakes::gfx::webgpu {

namespace {

wgpu::TextureUsage ToWgpuTextureUsage(TextureUsage u, bool wantUpload) {
    wgpu::TextureUsage out = wgpu::TextureUsage::None;
    if (hasFlag(u, TextureUsage::ShaderResource))
        out |= wgpu::TextureUsage::TextureBinding;
    if (hasFlag(u, TextureUsage::RenderTarget))
        out |= wgpu::TextureUsage::RenderAttachment;
    if (hasFlag(u, TextureUsage::DepthStencil))
        out |= wgpu::TextureUsage::RenderAttachment;
    if (wantUpload)
        out |= wgpu::TextureUsage::CopyDst;
    return out;
}

void UploadInitialPixels(WebGPUDeviceState& state, const wgpu::Texture& tex,
                         const TextureDesc& desc, const void* pixels) {
    if (!pixels)
        return;
    const u32 mipLevels = std::max(1u, static_cast<u32>(desc.mipLevels));
    const u32 layers = std::max(1u, static_cast<u32>(desc.arraySize));
    const u32 bpp = FormatBytesPerBlock(desc.format);
    const bool block = IsBlockCompressed(desc.format);

    const u8* cursor = static_cast<const u8*>(pixels);
    for (u32 layer = 0; layer < layers; ++layer) {
        u32 w = desc.width;
        u32 h = desc.height;
        for (u32 mip = 0; mip < mipLevels; ++mip) {
            const u32 blocksW = block ? std::max(1u, (w + 3) / 4) : w;
            const u32 blocksH = block ? std::max(1u, (h + 3) / 4) : h;
            const u32 rowBytes = blocksW * bpp;
            const u64 sliceBytes = static_cast<u64>(rowBytes) * blocksH;

            wgpu::TexelCopyTextureInfo dst{};
            dst.texture = tex;
            dst.mipLevel = mip;
            dst.origin = {0, 0, layer};
            dst.aspect = wgpu::TextureAspect::All;

            wgpu::TexelCopyBufferLayout layout{};
            layout.offset = 0;
            layout.bytesPerRow = rowBytes;
            layout.rowsPerImage = blocksH;

            // For BC formats, copy extents must be block-aligned. When
            // the mip is smaller than the block (e.g. 2×2 / 1×1 on BC1),
            // pad up to the next 4-multiple — the texture's physical
            // storage at that level is one block anyway.
            const u32 extentW = block ? std::max(4u, (w + 3) & ~3u) : w;
            const u32 extentH = block ? std::max(4u, (h + 3) & ~3u) : h;
            wgpu::Extent3D extent{extentW, extentH, 1};
            state.queue.WriteTexture(&dst, cursor, sliceBytes, &layout, &extent);

            cursor += sliceBytes;
            w = std::max(1u, w / 2);
            h = std::max(1u, h / 2);
        }
    }
}

} // namespace

TextureHandle WebGPUDevice::CreateTexture(const TextureDesc& desc, const void* initialPixels) {
    auto& state = *state_;
    const wgpu::TextureFormat fmt = ToWgpuFormat(desc.format);
    if (fmt == wgpu::TextureFormat::Undefined)
        return TextureHandle::Invalid;

    wgpu::TextureDescriptor td{};
    td.size = {static_cast<u32>(desc.width), static_cast<u32>(desc.height),
               static_cast<u32>(std::max(1, desc.arraySize))};
    td.mipLevelCount = static_cast<u32>(std::max(1, desc.mipLevels));
    td.sampleCount = 1;
    td.format = fmt;
    td.dimension = wgpu::TextureDimension::e2D;
    td.usage = ToWgpuTextureUsage(desc.usage, initialPixels != nullptr);

    wgpu::Texture tex = state.device.CreateTexture(&td);
    if (!tex)
        return TextureHandle::Invalid;

    wgpu::TextureViewDescriptor vd{};
    vd.format = fmt;
    // Cube vs CubeArray distinction. Cube view requires exactly 6
    // layers; CubeArray needs a multiple of 6. Plain 2D / 2D-array
    // otherwise. The IBL pipeline ships 2-cube arrays (12 layers).
    if (desc.isCube) {
        vd.dimension = (td.size.depthOrArrayLayers > 6) ? wgpu::TextureViewDimension::CubeArray
                                                        : wgpu::TextureViewDimension::Cube;
    } else if (td.size.depthOrArrayLayers > 1) {
        vd.dimension = wgpu::TextureViewDimension::e2DArray;
    } else {
        vd.dimension = wgpu::TextureViewDimension::e2D;
    }
    vd.baseMipLevel = 0;
    vd.mipLevelCount = td.mipLevelCount;
    vd.baseArrayLayer = 0;
    vd.arrayLayerCount = td.size.depthOrArrayLayers;
    vd.aspect = wgpu::TextureAspect::All;
    wgpu::TextureView view = tex.CreateView(&vd);

    if (initialPixels)
        UploadInitialPixels(state, tex, desc, initialPixels);

    TextureEntry entry{};
    entry.texture = std::move(tex);
    entry.view = std::move(view);
    entry.format = fmt;
    entry.width = desc.width;
    entry.height = desc.height;
    entry.mipLevels = desc.mipLevels;
    entry.arraySize = desc.arraySize;
    entry.ownsTexture = true;
    return static_cast<TextureHandle>(state.textures.Insert(std::move(entry)));
}

TextureHandle WebGPUDevice::CreateColorTarget(i32 w, i32 h, Format f) {
    TextureDesc d{};
    d.width = w;
    d.height = h;
    d.mipLevels = 1;
    d.arraySize = 1;
    d.format = f;
    d.usage = TextureUsage::ShaderResource | TextureUsage::RenderTarget;
    return CreateTexture(d, nullptr);
}

TextureHandle WebGPUDevice::CreateDepthTarget(i32 w, i32 h, Format f) {
    auto& state = *state_;
    const wgpu::TextureFormat fmt = ToWgpuFormat(f);
    if (fmt == wgpu::TextureFormat::Undefined)
        return TextureHandle::Invalid;

    wgpu::TextureDescriptor td{};
    td.size = {static_cast<u32>(w), static_cast<u32>(h), 1};
    td.mipLevelCount = 1;
    td.sampleCount = 1;
    td.format = fmt;
    td.dimension = wgpu::TextureDimension::e2D;
    td.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding;

    wgpu::Texture tex = state.device.CreateTexture(&td);
    if (!tex)
        return TextureHandle::Invalid;

    wgpu::TextureViewDescriptor vd{};
    vd.format = fmt;
    vd.dimension = wgpu::TextureViewDimension::e2D;
    vd.baseMipLevel = 0;
    vd.mipLevelCount = 1;
    vd.baseArrayLayer = 0;
    vd.arrayLayerCount = 1;
    vd.aspect = wgpu::TextureAspect::All;
    wgpu::TextureView view = tex.CreateView(&vd);

    TextureEntry entry{};
    entry.texture = std::move(tex);
    entry.view = std::move(view);
    entry.format = fmt;
    entry.width = w;
    entry.height = h;
    entry.ownsTexture = true;
    entry.isDepth = true;
    return static_cast<TextureHandle>(state.textures.Insert(std::move(entry)));
}

void WebGPUDevice::Destroy(TextureHandle h) {
    auto& state = *state_;
    auto* texture = state.textures.Get(static_cast<u64>(h));
    if (!texture)
        return;
    // Swap-chain proxies don't own their texture; the SwapChainEntry does.
    if (!texture->ownsTexture) {
        state.textures.Remove(static_cast<u64>(h));
        return;
    }
    TextureEntry moved = std::move(*texture);
    state.textures.Remove(static_cast<u64>(h));
    std::lock_guard<std::mutex> lock(state.deleteMutex);
    state.pendingDeletes.push_back(PendingDelete{
        state.pendingEpoch + 1,
        [owned = std::move(moved)]() mutable { (void)owned; },
    });
}

void WebGPUDevice::Destroy(SamplerHandle h) {
    auto& state = *state_;
    auto* sampler = state.samplers.Get(static_cast<u64>(h));
    if (!sampler)
        return;
    SamplerEntry moved = std::move(*sampler);
    state.samplers.Remove(static_cast<u64>(h));
    std::lock_guard<std::mutex> lock(state.deleteMutex);
    state.pendingDeletes.push_back(PendingDelete{
        state.pendingEpoch + 1,
        [owned = std::move(moved)]() mutable { (void)owned; },
    });
}

} // namespace whiteout::flakes::gfx::webgpu

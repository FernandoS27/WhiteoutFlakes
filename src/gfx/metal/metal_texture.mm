// Texture create / destroy + render targets + initial-data upload.
// Mirrors src/gfx/webgpu/webgpu_texture.cpp; Metal's
// [MTLTexture replaceRegion:...] takes the place of queue.WriteTexture.
//
// All textures with initial pixels use MTLStorageModeShared so we can
// memcpy the bytes directly via replaceRegion (a thin host→GPU memcpy
// on Apple Silicon's unified memory). Render targets default to
// MTLStorageModePrivate.

#include "metal_device.h"
#include "metal_device_state.h"
#include "metal_handles.h"
#include "metal_translate.h"

#import <Metal/Metal.h>

#include "whiteout/flakes/gfx_types.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace whiteout::flakes::gfx::metal {

namespace {

u64 TextureBytes(const TextureDesc& desc) {
    const u32 mipLevels = std::max(1u, static_cast<u32>(desc.mipLevels));
    const u32 layers = std::max(1u, static_cast<u32>(desc.arraySize));
    const u32 bpp = FormatBytesPerBlock(desc.format);
    const bool block = IsBlockCompressed(desc.format);
    u64 total = 0;
    for (u32 layer = 0; layer < layers; ++layer) {
        u32 w = static_cast<u32>(desc.width);
        u32 h = static_cast<u32>(desc.height);
        for (u32 mip = 0; mip < mipLevels; ++mip) {
            const u32 blocksW = block ? std::max(1u, (w + 3) / 4) : w;
            const u32 blocksH = block ? std::max(1u, (h + 3) / 4) : h;
            total += static_cast<u64>(blocksW) * blocksH * bpp;
            w = std::max(1u, w / 2);
            h = std::max(1u, h / 2);
        }
    }
    return total;
}

void UploadInitialPixels(id<MTLTexture> tex, const TextureDesc& desc, const void* pixels) {
    if (!pixels)
        return;
    const u32 mipLevels = std::max(1u, static_cast<u32>(desc.mipLevels));
    const u32 layers = std::max(1u, static_cast<u32>(desc.arraySize));
    const u32 bpp = FormatBytesPerBlock(desc.format);
    const bool block = IsBlockCompressed(desc.format);

    const uint8_t* cursor = static_cast<const uint8_t*>(pixels);
    for (u32 layer = 0; layer < layers; ++layer) {
        u32 w = desc.width;
        u32 h = desc.height;
        for (u32 mip = 0; mip < mipLevels; ++mip) {
            const u32 blocksW = block ? std::max(1u, (w + 3) / 4) : w;
            const u32 blocksH = block ? std::max(1u, (h + 3) / 4) : h;
            const u32 rowBytes = blocksW * bpp;
            const u64 sliceBytes = static_cast<u64>(rowBytes) * blocksH;

            // Mip extent in *pixels* (not blocks). Metal's replaceRegion
            // wants the texel-space rect, even for BC formats — it
            // rounds up to whole blocks internally. For BC mips smaller
            // than 4×4 we still pass the natural pixel extent (1×1, 2×2,
            // 4×4) and Metal handles the block-padding.
            MTLRegion region = MTLRegionMake2D(0, 0, w, h);
            [tex replaceRegion:region
                   mipmapLevel:mip
                         slice:layer
                     withBytes:cursor
                   bytesPerRow:rowBytes
                 bytesPerImage:sliceBytes];

            cursor += sliceBytes;
            w = std::max(1u, w / 2);
            h = std::max(1u, h / 2);
        }
    }
}

MTLTextureUsage ToMtlTextureUsage(TextureUsage u) {
    MTLTextureUsage out = MTLTextureUsageUnknown;
    if (hasFlag(u, TextureUsage::ShaderResource))
        out |= MTLTextureUsageShaderRead;
    if (hasFlag(u, TextureUsage::RenderTarget) || hasFlag(u, TextureUsage::DepthStencil))
        out |= MTLTextureUsageRenderTarget;
    return out == MTLTextureUsageUnknown ? MTLTextureUsageShaderRead : out;
}

TextureHandle InsertTexture(MetalDeviceState& state, id<MTLTexture> tex,
                            const TextureDesc& desc, MTLPixelFormat fmt, bool ownsTexture) {
    TextureEntry entry;
    entry.texture = tex;
    entry.format = fmt;
    entry.width = desc.width;
    entry.height = desc.height;
    entry.mipLevels = std::max(1, desc.mipLevels);
    entry.arraySize = std::max(1, desc.arraySize);
    entry.ownsTexture = ownsTexture;
    entry.isDepth = IsDepthStencilFormat(fmt);
    entry.byteSize = ownsTexture ? TextureBytes(desc) : 0;
    if (ownsTexture)
        state.gpuBytesAlloc.fetch_add(entry.byteSize, std::memory_order_relaxed);
    return static_cast<TextureHandle>(state.textures.Insert(std::move(entry)));
}

} // namespace

TextureHandle MetalDevice::CreateTexture(const TextureDesc& desc, const void* initialPixels) {
    @autoreleasepool {
        auto& state = *state_;

        MTLPixelFormat mfmt = ToMtlPixelFormat(desc.format);
        if (mfmt == MTLPixelFormatInvalid)
            return TextureHandle::Invalid;

        MTLTextureDescriptor* td = [[MTLTextureDescriptor alloc] init];
        if (desc.isCube) {
            td.textureType = (desc.arraySize > 6) ? MTLTextureTypeCubeArray
                                                  : MTLTextureTypeCube;
            td.arrayLength = std::max(1u, static_cast<u32>(desc.arraySize) / 6u);
        } else if (desc.arraySize > 1) {
            td.textureType = MTLTextureType2DArray;
            td.arrayLength = static_cast<NSUInteger>(desc.arraySize);
        } else {
            td.textureType = MTLTextureType2D;
            td.arrayLength = 1;
        }
        td.pixelFormat = mfmt;
        td.width = static_cast<NSUInteger>(desc.width);
        td.height = static_cast<NSUInteger>(desc.height);
        td.mipmapLevelCount = std::max(1u, static_cast<u32>(desc.mipLevels));
        td.usage = ToMtlTextureUsage(desc.usage);
        // Initial-data path needs host-writable storage. Render targets
        // and pure GPU resources stay Private.
        const bool wantUpload = initialPixels != nullptr;
        td.storageMode = wantUpload ? MTLStorageModeShared : MTLStorageModePrivate;

        id<MTLTexture> tex = [state.device newTextureWithDescriptor:td];
        if (!tex)
            return TextureHandle::Invalid;
        if (state.validationRequested)
            tex.label = @"wf.tex";

        if (initialPixels)
            UploadInitialPixels(tex, desc, initialPixels);

        return InsertTexture(state, tex, desc, mfmt, /*ownsTexture=*/true);
    }
}

TextureHandle MetalDevice::CreateColorTarget(i32 w, i32 h, Format f) {
    TextureDesc d{};
    d.width = w;
    d.height = h;
    d.mipLevels = 1;
    d.arraySize = 1;
    d.format = f;
    d.usage = TextureUsage::ShaderResource | TextureUsage::RenderTarget;
    return CreateTexture(d, nullptr);
}

TextureHandle MetalDevice::CreateDepthTarget(i32 w, i32 h, Format f) {
    TextureDesc d{};
    d.width = w;
    d.height = h;
    d.mipLevels = 1;
    d.arraySize = 1;
    d.format = f;
    d.usage = TextureUsage::DepthStencil | TextureUsage::ShaderResource;
    return CreateTexture(d, nullptr);
}

void MetalDevice::Destroy(TextureHandle h) {
    auto& state = *state_;
    auto* tex = state.textures.Get(static_cast<u64>(h));
    if (!tex)
        return;

    // Swap-chain proxies don't own their MTLTexture (it lives on the
    // drawable). Just drop the slot.
    if (!tex->ownsTexture || tex->swapChainProxy != SwapChainHandle::Invalid) {
        state.textures.Remove(static_cast<u64>(h));
        return;
    }

    TextureEntry moved = std::move(*tex);
    state.textures.Remove(static_cast<u64>(h));

    const u64 retireAfter = state.pendingEpoch + 1;
    const u64 bytes = moved.byteSize;
    id<MTLTexture> mtl = moved.texture;
    id<MTLTexture> mtlLinear = moved.viewLinear;
    {
        std::lock_guard<std::mutex> lock(state.pendingDeletesMutex);
        state.pendingDeletes.push_back(PendingDelete{
            retireAfter,
            [mtl, mtlLinear, &state, bytes]() {
                (void)mtl;
                (void)mtlLinear;
                state.gpuBytesFreed.fetch_add(bytes, std::memory_order_relaxed);
            },
        });
    }
}

} // namespace whiteout::flakes::gfx::metal

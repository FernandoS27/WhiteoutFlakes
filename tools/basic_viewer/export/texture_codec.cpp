// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "texture_codec.h"

#include <whiteout/textures/blp/writer.h>
#include <whiteout/textures/bmp/writer.h>
#include <whiteout/textures/dds/writer.h>
#include <whiteout/textures/jpeg/writer.h>
#include <whiteout/textures/png/writer.h>
#include <whiteout/textures/tga/writer.h>
#include <whiteout/textures/tiff/writer.h>

#include <exception>
#include <optional>
#include <stdexcept>
#include <span>

namespace whiteout::flakes {

namespace {

namespace tx = ::whiteout::textures;

/// Level 0 and every level below it, rebuilt. Why not, when it could not be.
std::optional<std::string> GenerateFullChain(tx::Texture& texture) {
    return texture.generateMipmaps(
        tx::computeMaxMipCount(texture.width(), texture.height(), texture.depth()));
}

/// The mip generator reports rather than throws; an encode that cannot build
/// its chain must not write a file without one.
void ThrowIfFailed(const std::optional<std::string>& error) {
    if (error)
        throw std::runtime_error("mip generation: " + *error);
}

/// @p writer's output for @p texture, or why there is none.
template <typename WriteFn>
EncodedTexture Encode(WriteFn&& write) {
    EncodedTexture out;
    try {
        out.bytes = write();
        if (out.bytes.empty()) {
            out.problem = "the writer produced nothing";
        }
    } catch (const std::exception& e) {
        out.bytes.clear();
        out.problem = e.what();
    }
    return out;
}

} // namespace

bool HasAlpha(const tx::Texture& texture) {
    const std::span<const u8> pixels = texture.mipData(0);
    for (std::size_t i = 3; i < pixels.size(); i += 4) {
        if (pixels[i] != 0xFF) {
            return true;
        }
    }
    return false;
}

EncodedTexture EncodeGameDds(const tx::Texture& source) {
    return Encode([&] {
        tx::Texture texture = source;
        texture.format(tx::PixelFormat::RGBA8);
        const bool alpha = HasAlpha(texture);
        ThrowIfFailed(GenerateFullChain(texture));
        texture.setSrgb(false);
        texture.format(alpha ? tx::PixelFormat::BC3 : tx::PixelFormat::BC1);
        tx::dds::Writer writer;
        return writer.write(texture);
    });
}

EncodedTexture EncodeClassicBlp(const tx::Texture& source) {
    return Encode([&] {
        tx::Texture texture = source;
        texture.format(tx::PixelFormat::RGBA8);
        const bool alpha = HasAlpha(texture);
        ThrowIfFailed(GenerateFullChain(texture));
        tx::blp::SaveOptions options;
        options.version = tx::blp::BlpVersion::BLP1;
        options.encoding = tx::blp::BlpEncoding::Palettized;
        options.alpha = alpha ? tx::blp::BlpAlphaDepth::Eight : tx::blp::BlpAlphaDepth::Zero;
        tx::blp::Writer writer;
        return writer.write(texture, options);
    });
}

EncodedTexture EncodeBaked(const BakedTexture& baked) {
    std::string note;
    EncodedTexture out = Encode([&] {
        tx::Texture texture = baked.texture;
        switch (baked.mips) {
        case BakedTexture::Mips::Authored:
            if (texture.mipCount() <= 1)
                ThrowIfFailed(GenerateFullChain(texture));
            break;
        case BakedTexture::Mips::Box:
            texture.setKind(tx::TextureKind::Other);
            ThrowIfFailed(GenerateFullChain(texture));
            break;
        case BakedTexture::Mips::ByKind:
            if (texture.kind() == tx::TextureKind::Other)
                note = "declares no channel kinds, so its mips were box-filtered";
            ThrowIfFailed(GenerateFullChain(texture));
            break;
        }
        // Cleared after the chain, not before: a colour channel is filtered in
        // linear light only while the flag says its values are sRGB. It goes
        // for the write because the DDS writer turns it into a DX10 header,
        // and the legacy FourCC is what both games' tools read.
        texture.setSrgb(false);
        texture.format(baked.format);
        tx::dds::Writer writer;
        return writer.write(texture);
    });
    out.note = std::move(note);
    return out;
}

EncodedTexture EncodeTextureAs(const tx::Texture& source, std::string_view extension) {
    const auto as = [&](tx::Writer&& writer) {
        return Encode([&] {
            tx::Texture texture = source;
            texture.format(tx::PixelFormat::RGBA8);
            return writer.write(texture);
        });
    };
    if (extension == ".png") {
        return as(tx::png::Writer{});
    }
    if (extension == ".tga") {
        return as(tx::tga::Writer{});
    }
    if (extension == ".blp") {
        return as(tx::blp::Writer{});
    }
    if (extension == ".dds") {
        return as(tx::dds::Writer{});
    }
    if (extension == ".bmp") {
        return as(tx::bmp::Writer{});
    }
    if (extension == ".jpg" || extension == ".jpeg") {
        return as(tx::jpeg::Writer{});
    }
    if (extension == ".tif" || extension == ".tiff") {
        return as(tx::tiff::Writer{});
    }
    return {{}, "no writer for '" + std::string(extension) + "'"};
}

} // namespace whiteout::flakes

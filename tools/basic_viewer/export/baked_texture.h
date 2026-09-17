#pragma once

// ============================================================================
// A texture an export writes out of nothing a file holds: made from two or
// three of the source's own maps a moment ago, keyed by the document texture
// it stands for, and encoded by `EncodeBaked` in place of a provider read.
//
// Its mip chain is rebuilt from level 0, and what a channel holds decides how:
// `Texture::generateMipmaps` filters a colour in linear light, a normal
// through Toksvig, a binary mask by max-pooling it so a cutout stays a
// cutout. So a bake says what its channels are when it is made — the
// constructor asks — and the few no kind describes say that instead.
// ============================================================================

#include "whiteout/flakes/types.h"

#include <whiteout/textures/texture.h>

#include <utility>

namespace whiteout::flakes {

/// What a bake's channels hold.
struct BakedChannels {
    ::whiteout::textures::TextureKind rgb;
    ::whiteout::textures::TextureKind alpha;
    /// The colour channels are display-referred (sRGB-encoded) values.
    bool srgb = false;
};

class BakedTexture {
public:
    /// How the chain is built.
    enum class Mips : u8 {
        /// From level 0, through the kinds the texture declares.
        ByKind,
        /// From level 0 with a box filter, on purpose: no kind describes the
        /// channels (an X-in-alpha normal, a projected panorama).
        Box,
        /// Kept as the bake made it: a pre-filtered probe's levels are its
        /// roughness blur, which no kind rebuilds for a cube.
        Authored,
    };

    /// A bake whose channels hold @p channels.
    BakedTexture(::whiteout::textures::Texture texture, ::whiteout::textures::PixelFormat format,
                 const BakedChannels& channels)
        : texture(std::move(texture)), format(format), mips(Mips::ByKind) {
        using ::whiteout::textures::Channel;
        using ::whiteout::textures::TextureKind;
        this->texture.setKind(TextureKind::Multikind);
        this->texture.setChannelKind(Channel::R, channels.rgb);
        this->texture.setChannelKind(Channel::G, channels.rgb);
        this->texture.setChannelKind(Channel::B, channels.rgb);
        this->texture.setChannelKind(Channel::A, channels.alpha);
        this->texture.setSrgb(channels.srgb);
    }

    /// A bake whose texture already declares its kinds — the library's
    /// `pbr_bake.h` bakes all do.
    static BakedTexture Declared(::whiteout::textures::Texture texture,
                                 ::whiteout::textures::PixelFormat format) {
        return BakedTexture(std::move(texture), format, Mips::ByKind);
    }

    /// A bake no kind describes: box-filtered, knowingly.
    static BakedTexture BoxFiltered(::whiteout::textures::Texture texture,
                                    ::whiteout::textures::PixelFormat format) {
        return BakedTexture(std::move(texture), format, Mips::Box);
    }

    /// A bake that brings the chain it is to be written with.
    static BakedTexture WithAuthoredChain(::whiteout::textures::Texture texture,
                                          ::whiteout::textures::PixelFormat format) {
        return BakedTexture(std::move(texture), format, Mips::Authored);
    }

    ::whiteout::textures::Texture texture;
    /// The container it wants. BC5 for a two-channel normal — which is what
    /// Reforged's own normal maps are — and BC3 for an ORM, whose alpha is the
    /// team-colour mask and so cannot be dropped.
    ::whiteout::textures::PixelFormat format;
    Mips mips;

private:
    BakedTexture(::whiteout::textures::Texture texture, ::whiteout::textures::PixelFormat format,
                 Mips mips)
        : texture(std::move(texture)), format(format), mips(mips) {}
};

} // namespace whiteout::flakes

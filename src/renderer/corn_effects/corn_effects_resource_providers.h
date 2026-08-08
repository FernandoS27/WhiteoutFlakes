#pragma once

// ============================================================================
// Content-provider-backed cornflakes resource providers.
//
// cornflakes resolves three kinds of sampler resource at BIND time, through
// synchronous callbacks that hand back spans the binder keeps pointing into:
//
//   Mesh Shape samplers        → .pkmm bytes      (IMeshResourceProvider)
//   External Turbulence        → .pkvf bytes      (IVectorFieldProvider)
//   Texture samplers           → decoded texels   (ITextureResourceProvider)
//
// That contract rules out AssetManager, which is push-based: a slot is only
// populated after the host pump has fetched it, and bind can't wait. So these
// go straight to IContentProvider::ReadFile, which is the blocking read the
// rest of the renderer (BLS cache, MDX parse, DNC) already uses.
//
// Both classes own every buffer they hand out and never evict, since the
// returned spans have to stay valid for as long as any runtime bound against
// them. Failures are cached too — a missing resource is looked up once, not
// once per emitter spawn. cornflakes treats an empty result as "resource did
// not resolve" and emits a bind warning, which is the same thing the engine
// does for a missing descriptor.
// ============================================================================

#include "whiteout/flakes/types.h"

#include <cornflakes/interface/asset/mesh_provider.hpp>
#include <cornflakes/interface/asset/texture_provider.hpp>
#include <cornflakes/interface/asset/vector_field_provider.hpp>

#include <cstddef>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace whiteout::flakes::io {
class IContentProvider;
}

namespace whiteout::flakes::renderer::corn_effects {

/// @brief Serves .pkmm meshes and .pkvf vector fields out of the scene's
///        content provider. One object covers both interfaces because the
///        two resource kinds ship the same way (baked beside the effect)
///        and differ only in what cornflakes parses them into.
class CornEffectsMeshProvider final : public ::whiteout::cornflakes::IMeshResourceProvider,
                                      public ::whiteout::cornflakes::IVectorFieldProvider {
public:
    explicit CornEffectsMeshProvider(io::IContentProvider* content) : content_(content) {}

    void SetContentProvider(io::IContentProvider* content);

    std::span<const std::byte> readMesh(std::string_view path) override;
    std::span<const std::byte> readVectorField(std::string_view path) override {
        return readMesh(path);
    }

private:
    mutable std::mutex mutex_;
    io::IContentProvider* content_ = nullptr;
    // Values are stable in memory across rehash (node-based container), which
    // the span contract requires.
    std::unordered_map<std::string, std::vector<std::byte>> cache_;
};

/// @brief Serves texture-sampler texels out of the scene's content provider.
///
/// Distinct from the renderer's diffuse textures: a Texture *sampler* is read
/// by the simulation on the CPU, so this decodes to texels rather than
/// uploading to the GPU. BC1 is passed through compressed — cornflakes samples
/// DXT1 blocks in place, as the engine does — and everything else is
/// normalised to RGBA8.
class CornEffectsTextureProvider final : public ::whiteout::cornflakes::ITextureResourceProvider {
public:
    explicit CornEffectsTextureProvider(io::IContentProvider* content) : content_(content) {}

    void SetContentProvider(io::IContentProvider* content);

    ::whiteout::cornflakes::TextureImageDesc readTexture(std::string_view path) override;

private:
    struct Image {
        std::vector<std::byte> texels;
        u32 width = 0;
        u32 height = 0;
        ::whiteout::cornflakes::TextureSourceFormat format =
            ::whiteout::cornflakes::TextureSourceFormat::Rgba8;
        bool srgb = false;
    };

    mutable std::mutex mutex_;
    io::IContentProvider* content_ = nullptr;
    std::unordered_map<std::string, Image> cache_;
};

} // namespace whiteout::flakes::renderer::corn_effects

#pragma once

#include "whiteout/flakes/types.h"
#include "renderer/types.h"
#include "renderer/bls/bls_cb_layout.h"
#include "renderer/bls/bls_mat_params.h"
#include "../gfx/gfx.h"

#include <cornflakes/interface/render/render_backend.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace whiteout::flakes::renderer::assets {
class TextureAssetManager;
class SamplerAssetManager;
}

namespace whiteout::flakes::renderer::bls {
struct BlsProgram;
class  BlsPsoBuilder;
}

namespace whiteout::flakes::renderer::corn_effects {

struct CornEffectsFrameInputs {
    gfx::IGFXCommandList* cmd          = nullptr;
    Matrix44f             view         = Matrix44f::identity();
    Matrix44f             projection   = Matrix44f::identity();
    Matrix44f             world        = Matrix44f::identity();
    Vector4f              viewportRect = {1280, 720, 0, 0};
    f32                   effectTime   = 0.0f;
    f32                   cornEffectsScale = 100.0f;
    gfx::Format           rtvFormat    = gfx::Format::R16G16B16A16_FLOAT;
};

using TextureResolver = std::function<gfx::TextureHandle(std::string_view path)>;

class CornEffectsGfxBackend final : public ::whiteout::cornflakes::IRenderBackend {
public:
    struct Init {
        gfx::IGFXDevice*               device      = nullptr;
        const bls::BlsProgram*         program     = nullptr;
        bls::BlsPsoBuilder*            psoBuilder  = nullptr;
        assets::TextureAssetManager*   textures    = nullptr;
        assets::SamplerAssetManager*   samplers    = nullptr;
        TextureResolver                resolver;
    };

    explicit CornEffectsGfxBackend(const Init& init);
    ~CornEffectsGfxBackend() override;

    CornEffectsGfxBackend(const CornEffectsGfxBackend&) = delete;
    CornEffectsGfxBackend& operator=(const CornEffectsGfxBackend&) = delete;

    void SetFrameInputs(const CornEffectsFrameInputs& inputs) { frame_ = inputs; }

    bool prepare(std::span<const ::whiteout::cornflakes::LayerProgram> layers,
                 ::whiteout::cornflakes::IssueBag& issues) override;
    void submit(std::span<const ::whiteout::cornflakes::RenderPacket> packets,
                const ::whiteout::cornflakes::ViewParams& view,
                ::whiteout::cornflakes::IssueBag& issues) override;
    void shutdown(::whiteout::cornflakes::IssueBag& issues) override;

private:
    struct LayerState {
        gfx::TextureHandle diffuse     = gfx::TextureHandle::Invalid;
        bool               isDistortion = false;
        bool               renderable   = false;
        u16                atlasX       = 0;
        u16                atlasY       = 0;
        bool               flipU        = false;
        bool               flipV        = false;
        bool               rotate       = false;
        bool               size2D       = false;
    };

    bool EnsureVertexBuffer(u32 vertexCount);
    bool EnsureIndexBuffer(u32 indexCount);
    void EnsureCbs();

    static bls::GxMatAlpha BlendModeToGxAlpha(u8 blendMode);

    gfx::IGFXDevice*             device_       = nullptr;
    const bls::BlsProgram*       program_      = nullptr;
    bls::BlsPsoBuilder*          psoBuilder_   = nullptr;
    assets::TextureAssetManager* textures_     = nullptr;
    assets::SamplerAssetManager* samplers_     = nullptr;
    TextureResolver              resolver_;

    std::vector<LayerState> layerStates_;

    gfx::BufferHandle      vb_       = gfx::BufferHandle::Invalid;
    u32                    vbCap_    = 0;
    gfx::BufferHandle      ib_       = gfx::BufferHandle::Invalid;
    u32                    ibCap_    = 0;

    gfx::BufferHandle      vsCb_     = gfx::BufferHandle::Invalid;
    gfx::BufferHandle      psCb_     = gfx::BufferHandle::Invalid;

    CornEffectsFrameInputs     frame_;
};

}

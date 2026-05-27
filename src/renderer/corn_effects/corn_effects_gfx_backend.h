#pragma once

#include "../gfx/gfx.h"
#include "renderer/bls/bls_cb_layout.h"
#include "renderer/bls/bls_mat_params.h"
#include "renderer/corn_effects/corn_effects_vertex.h"
#include "renderer/types.h"
#include "whiteout/flakes/types.h"

#include <cornflakes/interface/render/render_backend.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace whiteout::flakes::renderer::assets {
class AssetManager;
class TextureAssetManager;
class SamplerAssetManager;
} // namespace whiteout::flakes::renderer::assets

namespace whiteout::flakes::renderer::bls {
struct BlsProgram;
class BlsPsoBuilder;
} // namespace whiteout::flakes::renderer::bls

namespace whiteout::flakes::renderer::corn_effects {

struct CornEffectsFrameInputs {
    gfx::IGFXCommandList* cmd = nullptr;
    Matrix44f view = Matrix44f::identity();
    Matrix44f projection = Matrix44f::identity();
    Matrix44f world = Matrix44f::identity();
    Vector4f viewportRect = {1280, 720, 0, 0};
    f32 effectTime = 0.0f;
    f32 cornEffectsScale = 100.0f;
    gfx::Format rtvFormat = gfx::Format::R11G11B10_FLOAT;
    gfx::Format dsvFormat = gfx::Format::D24_UNORM_S8_UINT;
};

// Slot acquirer hook — corn_fx invokes it once per layer during
// prepare() to get a stable AssetManager slot for that layer's
// diffuse texture. Returning 0 means "no slot, render white fallback".
using TextureSlotAcquirer = std::function<std::uint32_t(std::string_view path)>;

class CornEffectsGfxBackend final : public ::whiteout::cornflakes::IRenderBackend {
public:
    struct Init {
        gfx::IGFXDevice* device = nullptr;
        const bls::BlsProgram* program = nullptr;
        bls::BlsPsoBuilder* psoBuilder = nullptr;
        assets::TextureAssetManager* textures = nullptr;
        assets::SamplerAssetManager* samplers = nullptr;
        assets::AssetManager* assets = nullptr;
        TextureSlotAcquirer slotAcquire;
    };

    explicit CornEffectsGfxBackend(const Init& init);
    ~CornEffectsGfxBackend() override;

    CornEffectsGfxBackend(const CornEffectsGfxBackend&) = delete;
    CornEffectsGfxBackend& operator=(const CornEffectsGfxBackend&) = delete;

    void SetFrameInputs(const CornEffectsFrameInputs& inputs) {
        frame_ = inputs;
    }

    bool prepare(std::span<const ::whiteout::cornflakes::LayerProgram> layers,
                 ::whiteout::cornflakes::IssueBag& issues) override;

    // IRenderBackend::submit. Called by EffectRuntime::tick() — does the
    // CPU per-particle work (depth sort, quad expansion, atlas UVs) and
    // stashes the result in `pending_`. No GPU calls — the owning
    // CornEffectsService merges every emitter's pending_ into one shared
    // VB/IB per frame and issues draws in one consolidated pass.
    void submit(std::span<const ::whiteout::cornflakes::RenderPacket> packets,
                const ::whiteout::cornflakes::ViewParams& view,
                ::whiteout::cornflakes::IssueBag& issues) override;
    void shutdown(::whiteout::cornflakes::IssueBag& issues) override;

    // CPU vert/index/draw bundle produced by `submit`. Lives on the
    // backend so it survives EffectRuntime::tick re-entry; cleared by
    // the service at the start of each frame's batch.
    struct PendingBatch {
        struct Draw {
            u32 indexFirst = 0; // local — service offsets via baseIndex
            u32 indexCount = 0;
            u32 layerIdx = 0;
            u8  blendMode = 0;
        };
        std::vector<CornEffectsVertex> verts;
        std::vector<u16> indices; // local 0..verts.size(); service offsets via baseVertex
        std::vector<Draw> draws;
        bool Empty() const {
            return draws.empty();
        }
        void Clear() {
            verts.clear();
            indices.clear();
            draws.clear();
        }
    };
    PendingBatch& Pending() {
        return pending_;
    }
    const PendingBatch& Pending() const {
        return pending_;
    }

    // Service hooks. Texture slot of the layer for a given draw; the
    // service binds the texture before DrawIndexed because it changes
    // per draw within a batch.
    std::uint32_t LayerDiffuseSlot(u32 layerIdx) const;

    // Map a packet's blend-mode byte to the matching BLS alpha mode.
    // Called by the service when building MatParams for each draw.
    static bls::GxMatAlpha BlendModeToGxAlpha(u8 blendMode);

private:
    struct LayerState {
        // AssetManager slot for the diffuse. Acquired once during
        // prepare() (per layer); resolved live at submit() time via
        // TextureOf(). When the slot's payload hasn't arrived yet the
        // resolution returns the shared white placeholder — no
        // per-frame retry needed, no missing-list spam.
        std::uint32_t diffuseSlot = 0;
        bool isDistortion = false;
        bool renderable = false;
        u16 atlasX = 0;
        u16 atlasY = 0;
        bool flipU = false;
        bool flipV = false;
        bool rotate = false;
        bool size2D = false;
    };

    gfx::IGFXDevice* device_ = nullptr;
    const bls::BlsProgram* program_ = nullptr;
    bls::BlsPsoBuilder* psoBuilder_ = nullptr;
    assets::TextureAssetManager* textures_ = nullptr;
    assets::SamplerAssetManager* samplers_ = nullptr;
    assets::AssetManager* assets_ = nullptr;
    TextureSlotAcquirer slotAcquire_;

    std::vector<LayerState> layerStates_;
    PendingBatch pending_;

    CornEffectsFrameInputs frame_;
};

} // namespace whiteout::flakes::renderer::corn_effects

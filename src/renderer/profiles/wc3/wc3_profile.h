#pragma once

// Wc3SdProfile / Wc3HdProfile — the two WC3 frames, declared.
//
// RenderViewport walks these: the order below is the order the frame runs in,
// and each entry's predicate is the gate the chain checks before entering the
// stage. ValidateProfile runs over them at first frame, so a declaration that
// reads a target nobody writes fails loudly instead of at whichever frame first
// notices.

#include "core/render_profile.h"
#include "renderer/render_settings.h"

#include <array>
#include <functional>
#include <utility>
#include <vector>

namespace whiteout::flakes::renderer::profiles::wc3 {

using core::PassEntry;
using core::PassSlot;
using core::TargetBit;
using core::TargetBits;
using core::TargetDesc;
using core::TargetSlot;

// Gamma, LDR, one colour target and one depth target. The whole point of the
// abstraction is that this case stays this small.
class Wc3SdProfile final : public core::IRenderProfile {
public:
    // `sceneHdrInSd` answers "does THIS frame's SD colour route through the
    // HDR target?" — supplied by the pipeline as the ACTIVE SCENE's effective
    // flag rather than read off RenderSettings directly, because the opt-in is
    // per scene now (a pinned-HDR thumbnail cell and a classic gamma document
    // render through this same profile object in one frame). The single-arg
    // form reads the global flag — for tests and hosts with no scene overrides.
    explicit Wc3SdProfile(RenderSettings& settings)
        : Wc3SdProfile(settings, [&s = settings] { return s.SceneHdrInSd(); }) {}
    Wc3SdProfile(RenderSettings& settings, std::function<bool()> sceneHdrInSd)
        : settings_(settings), sceneHdrInSd_(std::move(sceneHdrInSd)) {
        targets_ = {
            TargetDesc{TargetSlot::SceneColor, gfx::Format::R8G8B8A8_UNORM, 1.0f},
            TargetDesc{TargetSlot::Depth, gfx::Format::D24_UNORM_S8_UINT, 1.0f},
            TargetDesc{TargetSlot::ShadowMap, gfx::Format::D32_FLOAT, 1.0f},
            TargetDesc{TargetSlot::Backbuffer, gfx::Format::R8G8B8A8_UNORM, 1.0f},
        };
        passes_.push_back({PassSlot::ShadowMap,
                           [] { return false; }, // replaced via SetPassPredicate
                           0,
                           0,
                           TargetBit(TargetSlot::ShadowMap)});
        passes_.push_back({PassSlot::OpaqueColor,
                           nullptr,
                           0,
                           TargetBit(TargetSlot::ShadowMap),
                           TargetBits({TargetSlot::SceneColor, TargetSlot::Depth})});
        passes_.push_back({PassSlot::TransparentScene,
                           nullptr,
                           TargetBit(TargetSlot::Depth),
                           0,
                           TargetBit(TargetSlot::SceneColor)});
        // SD is gamma-LDR and normally lands straight on the backbuffer. The
        // SceneHdrInSd opt-in routes it through the HDR target + tonemap so
        // additive content stops clipping to white — an optional pass whose
        // consumer must therefore not be unconditional.
        passes_.push_back({PassSlot::Tonemap,
                           [this] { return sceneHdrInSd_(); },
                           TargetBit(TargetSlot::SceneColor),
                           0,
                           TargetBit(TargetSlot::Backbuffer)});
        passes_.push_back({PassSlot::ImGui,
                           nullptr,
                           0,
                           0,
                           TargetBit(TargetSlot::Backbuffer)});
    }

    const char* Name() const override {
        return "Wc3Sd";
    }
    std::span<const PassEntry> Passes() const override {
        return passes_;
    }
    std::span<const TargetDesc> TargetSet() const override {
        return targets_;
    }
    gfx::Format SceneColorFormat() const override {
        return sceneHdrInSd_() ? gfx::Format::R11G11B10_FLOAT
                               : gfx::Format::R8G8B8A8_UNORM;
    }
    bool LinearShading() const override {
        return false;
    }
    f32 WorldScale() const override {
        return 1.0f; // identity: WC3 authors in renderer units
    }
    CoordSpace SourceSpace() const override {
        return kDefaultCoordSpace;
    }
    std::span<shading::IShadingModel* const> ShadingModels() const override {
        return models_;
    }
    void SetShadingModels(std::vector<shading::IShadingModel*> models) {
        models_ = std::move(models);
    }
    // Each gated pass's OUTER gate — the check the frame chain makes before
    // entering the stage at all. Installed by the host because they read
    // services (shadow, GTAO, DoF, post-process) rather than RenderSettings.
    // The semantic toggles stay inside those services, alongside the per-frame
    // param push that has to happen whether or not the pass draws.
    void SetPassPredicate(PassSlot slot, std::function<bool()> pred) {
        for (auto& e : passes_) {
            if (e.slot == slot)
                e.condition = pred;
        }
    }

private:
    RenderSettings& settings_;
    std::function<bool()> sceneHdrInSd_;
    std::vector<PassEntry> passes_;
    std::vector<TargetDesc> targets_;
    std::vector<shading::IShadingModel*> models_;
};

// Linear, HDR, a G-buffer for GTAO, and a tonemap that is not optional.
class Wc3HdProfile final : public core::IRenderProfile {
public:
    explicit Wc3HdProfile(RenderSettings& settings) : settings_(settings) {
        targets_ = {
            TargetDesc{TargetSlot::SceneColor, gfx::Format::R11G11B10_FLOAT, 1.0f},
            TargetDesc{TargetSlot::Depth, gfx::Format::D24_UNORM_S8_UINT, 1.0f},
            TargetDesc{TargetSlot::LinearDepth, gfx::Format::R32_FLOAT, 1.0f},
            TargetDesc{TargetSlot::Normal, gfx::Format::R8G8B8A8_UNORM, 1.0f},
            TargetDesc{TargetSlot::ShadowMap, gfx::Format::D32_FLOAT, 1.0f},
            TargetDesc{TargetSlot::AmbientOcclusion, gfx::Format::R8G8B8A8_UNORM, 1.0f},
            TargetDesc{TargetSlot::Bloom, gfx::Format::R11G11B10_FLOAT, 0.5f},
            TargetDesc{TargetSlot::Backbuffer, gfx::Format::R8G8B8A8_UNORM, 1.0f},
        };
        passes_.push_back({PassSlot::ShadowMap,
                           [] { return false; }, // replaced via SetPassPredicate
                           0,
                           0,
                           TargetBit(TargetSlot::ShadowMap)});
        // One MRT pass, not two: the HD opaque shader forward-shades *and*
        // writes linear depth + normal in the same draw, selected by what is
        // today the WC3_IS_MRT permutation. That is exactly the (surface, pass)
        // output mask IShadingModel::Emits describes.
        passes_.push_back({PassSlot::GBuffer,
                           nullptr,
                           0,
                           TargetBit(TargetSlot::ShadowMap),
                           TargetBits({TargetSlot::SceneColor, TargetSlot::Depth,
                                       TargetSlot::LinearDepth, TargetSlot::Normal})});
        // Submitted inside the same render-pass block as the G-buffer pass,
        // so it lands before GTAO multiplies AO into the composited colour.
        passes_.push_back({PassSlot::TransparentScene,
                           nullptr,
                           TargetBit(TargetSlot::Depth),
                           0,
                           TargetBit(TargetSlot::SceneColor)});
        passes_.push_back({PassSlot::Gtao,
                           [this] { return settings_.AoEnabled(); },
                           TargetBits({TargetSlot::LinearDepth, TargetSlot::Normal}),
                           0,
                           TargetBits({TargetSlot::AmbientOcclusion, TargetSlot::SceneColor})});
        // 3.0.0's post order is Bloom -> Distortion -> DoF (WC3_HD_PIPELINE_3_0_RE.md,
        // frame order): bloom composites into the scene colour first, so depth
        // of field blurs the bloomed image. WC3 has no distortion pass here.
        passes_.push_back({PassSlot::Bloom,
                           [this] { return settings_.BloomEnabled(); },
                           TargetBit(TargetSlot::SceneColor),
                           0,
                           TargetBits({TargetSlot::Bloom, TargetSlot::SceneColor})});
        passes_.push_back({PassSlot::Dof,
                           [this] { return settings_.DofEnabled(); },
                           TargetBits({TargetSlot::SceneColor, TargetSlot::LinearDepth}),
                           0,
                           TargetBit(TargetSlot::SceneColor)});
        passes_.push_back({PassSlot::Tonemap,
                           nullptr,
                           TargetBit(TargetSlot::SceneColor),
                           0,
                           TargetBit(TargetSlot::Backbuffer)});
        passes_.push_back({PassSlot::ImGui,
                           nullptr,
                           0,
                           0,
                           TargetBit(TargetSlot::Backbuffer)});
    }

    const char* Name() const override {
        return "Wc3Hd";
    }
    std::span<const PassEntry> Passes() const override {
        return passes_;
    }
    std::span<const TargetDesc> TargetSet() const override {
        return targets_;
    }
    gfx::Format SceneColorFormat() const override {
        return gfx::Format::R11G11B10_FLOAT;
    }
    bool LinearShading() const override {
        return true;
    }
    f32 WorldScale() const override {
        return 1.0f;
    }
    CoordSpace SourceSpace() const override {
        return kDefaultCoordSpace;
    }
    std::span<shading::IShadingModel* const> ShadingModels() const override {
        return models_;
    }
    void SetShadingModels(std::vector<shading::IShadingModel*> models) {
        models_ = std::move(models);
    }
    // Each gated pass's OUTER gate — the check the frame chain makes before
    // entering the stage at all. Installed by the host because they read
    // services (shadow, GTAO, DoF, post-process) rather than RenderSettings.
    // The semantic toggles stay inside those services, alongside the per-frame
    // param push that has to happen whether or not the pass draws.
    void SetPassPredicate(PassSlot slot, std::function<bool()> pred) {
        for (auto& e : passes_) {
            if (e.slot == slot)
                e.condition = pred;
        }
    }

private:
    RenderSettings& settings_;
    std::vector<PassEntry> passes_;
    std::vector<TargetDesc> targets_;
    std::vector<shading::IShadingModel*> models_;
};

} // namespace whiteout::flakes::renderer::profiles::wc3

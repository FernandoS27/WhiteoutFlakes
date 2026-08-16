#pragma once

// WowProfile — the World of Warcraft frame.
//
// Named for the game, not the format, matching Wc3SdProfile / Wc3HdProfile: a
// profile is a *frame*, and one game can ship more than one format over its
// lifetime (WoW alone went `.mdx` → `.m2`) while one format can serve more than
// one game (`.m3` is both StarCraft II and Heroes). The format axis lives on
// the adapter side — M2ModelAdapter, next to MdxModelAdapter.
//
// Reuses Wc3SdProfile's pass list verbatim: gamma, LDR, one colour target, one
// depth target. That is deliberate rather than lazy. At this stage WoW models
// render untextured white through UnlitShading, which needs no G-buffer, no
// GTAO and no tonemap, and picking the *simpler* of the two existing shapes is
// what keeps "the simple case stays simple" an acceptance test for
// IRenderProfile rather than an aspiration. Sc2HeroesProfile takes the HD shape
// (P10), so the two new profiles between them exercise both.
//
// What is genuinely this profile's own is WorldScale. A WoW creature is 2–5
// yards against camera constants sized in the hundreds; without this the model
// is a sub-pixel dot that a golden image cannot tell apart from a load failure.

#include "core/render_profile.h"
#include "renderer/render_settings.h"

#include <vector>

namespace whiteout::flakes::renderer::profiles::wow {

using core::PassEntry;
using core::PassSlot;
using core::TargetBit;
using core::TargetBits;
using core::TargetDesc;
using core::TargetSlot;

class WowProfile final : public core::IRenderProfile {
public:
    // WoW model units → renderer units. One WoW unit is 100 Warcraft III
    // units; StarCraft II and Heroes use the same ratio, which is why
    // Sc2HeroesProfile carries the same number rather than a second measured
    // one.
    //
    // This replaced a *framing* constant of 20, reverse-engineered from corpus
    // bounding boxes so models looked right against WC3's camera defaults
    // (kDefaultDistance = 350). That number made things look plausible and was
    // not the conversion — with auto-framing driving the camera off the actor's
    // own bounds, the real ratio is what belongs here and the framing takes
    // care of itself.
    static constexpr f32 kWowUnitsToRendererUnits = 100.0f;

    explicit WowProfile(RenderSettings& settings) : settings_(settings) {
        targets_ = {
            TargetDesc{TargetSlot::SceneColor, gfx::Format::R8G8B8A8_UNORM, 1.0f},
            TargetDesc{TargetSlot::Depth, gfx::Format::D24_UNORM_S8_UINT, 1.0f},
            TargetDesc{TargetSlot::Backbuffer, gfx::Format::R8G8B8A8_UNORM, 1.0f},
        };
        // No ShadowMap pass: nothing casts one yet, and declaring a target
        // nobody writes is what ValidateProfile exists to reject.
        passes_.push_back({PassSlot::OpaqueColor,
                           nullptr,
                           0,
                           0,
                           TargetBits({TargetSlot::SceneColor, TargetSlot::Depth})});
        passes_.push_back({PassSlot::TransparentScene,
                           nullptr,
                           TargetBit(TargetSlot::Depth),
                           0,
                           TargetBit(TargetSlot::SceneColor)});
        passes_.push_back({PassSlot::ImGui, nullptr, 0, 0, TargetBit(TargetSlot::Backbuffer)});
    }

    const char* Name() const override {
        return "WoW";
    }
    std::span<const PassEntry> Passes() const override {
        return passes_;
    }
    std::span<const TargetDesc> TargetSet() const override {
        return targets_;
    }
    gfx::Format SceneColorFormat() const override {
        return gfx::Format::R8G8B8A8_UNORM;
    }
    bool LinearShading() const override {
        return false;
    }
    f32 WorldScale() const override {
        return kWowUnitsToRendererUnits;
    }
    core::UnlitLightingModel UnlitLighting() const override {
        // Per-pixel Blinn-Phong. Not WoW's shading — WoW's is a texture
        // combiner chain this phase has no materials for. It is here because
        // it reads the vertex normal, which is exactly the attribute the
        // pre-MeshBuffer upload path discarded: a wrong layout description
        // shows up as wrong shading, where flat white showed nothing.
        return core::UnlitLightingModel::BlinnPhong;
    }
    core::RibbonBehavior Ribbons() const override {
        // The one place the two ribbon runtimes are asked to differ. Everything
        // the MDX path already did — the interpolation blend, the edge count,
        // the `g*t^2` fall — was measured identical in WoW 6.0.1 and stays
        // shared; see core/ribbon_dialect.h for each divergence this selects.
        return core::RibbonBehavior::Wow();
    }
    core::ParticleBehavior Particles() const override {
        // Same relationship as Ribbons(): WoW's CParticleEmitter2 and WC3's are
        // the same emitter evolved — the emission accumulator, the pool, the
        // squirt burst and the cell math are shared, and only the divergences
        // in core/particle_dialect.h are selected here.
        return core::ParticleBehavior::Wow();
    }
    CoordSpace SourceSpace() const override {
        // Genuinely identity: World of Warcraft shares Warcraft III's axes
        // (+X forward, +Y left, +Z up), so nothing is rebased. StarCraft II is
        // the one that diverges — see Sc2HeroesProfile.
        return CoordSpace::Blizzard;
    }
    std::span<shading::IShadingModel* const> ShadingModels() const override {
        return models_;
    }
    void SetShadingModels(std::vector<shading::IShadingModel*> models) {
        models_ = std::move(models);
    }

private:
    RenderSettings& settings_;
    std::vector<PassEntry> passes_;
    std::vector<TargetDesc> targets_;
    std::vector<shading::IShadingModel*> models_;
};

} // namespace whiteout::flakes::renderer::profiles::wow

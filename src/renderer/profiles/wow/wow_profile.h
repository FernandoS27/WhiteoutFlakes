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
    // WoW model units → renderer units, measured rather than assumed.
    //
    // Bounding-box extents across the `.m2` corpus: a cow is 3.8 units, a
    // humanoid 5–6, a mid-size creature 10–15, and Alexstrasza — a dragon
    // aspect, the largest thing in the set — 60. Warcraft III authors the same
    // silhouettes in the 90–300 range, which is what every camera constant
    // here is tuned against (kDefaultDistance = 350, kMinDistance = 15). That
    // puts the ratio at roughly 20, consistently, across the size range.
    //
    // A framing constant, not a physical conversion: nothing here claims to
    // know how long a yard is, only what it takes for a model authored in WoW
    // units to be visible next to a Warcraft III camera. An earlier draft of
    // this file guessed 40 from the design's "2–5 yards" note without
    // measuring, which would have rendered every creature at twice the size it
    // should be — visible, and therefore easy to mistake for correct.
    static constexpr f32 kWowUnitsToRendererUnits = 20.0f;

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
    CoordSpace SourceSpace() const override {
        // Identity for now. M2 authors Z-up right-handed like MDX; a real
        // divergence would show as a rotated model, which is visible in the
        // white render rather than silent.
        return kDefaultCoordSpace;
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

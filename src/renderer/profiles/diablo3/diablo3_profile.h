#pragma once

// Diablo3Profile — the Diablo III frame.
//
// Named for the game, not the format, matching Wc3SdProfile / WowProfile /
// Sc2HeroesProfile. The format axis lives on the adapter side (D3ModelAdapter).
//
// Shaped on WowProfile (which copies SD's) rather than on Sc2HeroesProfile
// (which copies HD's), and for a reason rather than for economy: D3's material
// model is a fixed-function `MaterialColors` — diffuse, specular, emissive,
// ambient, shininess — with no HDR multiplier anywhere in it, and nothing to
// feed a G-buffer. Gamma LDR is what the data is.
//
// What is genuinely this profile's own is WorldScale. See below.

#include "core/render_profile.h"
#include "renderer/render_settings.h"

#include <functional>
#include <vector>

namespace whiteout::flakes::renderer::profiles::diablo3 {

using core::PassEntry;
using core::PassSlot;
using core::TargetBit;
using core::TargetBits;
using core::TargetDesc;
using core::TargetSlot;

class Diablo3Profile final : public core::IRenderProfile {
public:
    // Diablo III model units -> renderer units.
    //
    // Unlike WoW's and SC2's 100, this is **not** a unit conversion: it is a
    // framing constant, and that difference is the whole of what WorldScale is
    // for. A Barbarian's head bone sits at z = 7.30 raw units against WC3
    // camera constants fitted to characters in the 90-120 band, so 7.30 x 17
    // lands where the framing expects.
    static constexpr f32 kD3UnitsToRendererUnits = 17.0f;

    explicit Diablo3Profile(RenderSettings& settings) : settings_(settings) {
        targets_ = {
            TargetDesc{TargetSlot::SceneColor, gfx::Format::R8G8B8A8_UNORM, 1.0f},
            TargetDesc{TargetSlot::Depth, gfx::Format::D24_UNORM_S8_UINT, 1.0f},
            TargetDesc{TargetSlot::Bloom, gfx::Format::R11G11B10_FLOAT, 0.5f},
            TargetDesc{TargetSlot::Backbuffer, gfx::Format::R8G8B8A8_UNORM, 1.0f},
        };
        // No ShadowMap pass and no shadow target: nothing casts one in v1, and
        // declaring a target no pass writes is what ValidateProfile exists to
        // reject.
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
        // Bloom and Tonemap: D3 ALWAYS renders through the float scene target
        // and a tonemap, because the real game does — its shading is gamma-LDR
        // but its frame buffer is HDR, and its additive/emissive materials
        // (skin sheen, gold trim, item glows) sum well past 1.0 and roll off
        // in the tonemap instead of clipping to opaque white. This is NOT the
        // host `SceneHdrInSd` opt-in that WC3 SD uses: for D3 it is intrinsic,
        // so the tonemap must not hinge on a host flag that a viewer only
        // flips on a profile *change* — the bug that left a dressed character
        // a white blob, its bright cloth clipped at write while the scene
        // target was still UNORM. Bloom still needs its own enable so its
        // target is not written by a frame that will never read it.
        // Screen-space distortion, between the transparent scene and the
        // tonemap — where `sub_741760` runs the post-effect chain, and where
        // the buffer's contents are complete but the frame is not yet graded.
        //
        // Reads the depth (the phase-3 draws are depth-tested against the
        // finished scene, never writing it) and writes the SceneColor it bends,
        // which it also samples. The buffer itself is the service's, not a
        // profile target: nothing else in the frame reads or writes it, and
        // declaring a target one pass owns end to end would only make
        // ValidateProfile police a private allocation.
        passes_.push_back({PassSlot::Distortion,
                           nullptr,
                           TargetBits({TargetSlot::SceneColor, TargetSlot::Depth}),
                           0,
                           TargetBit(TargetSlot::SceneColor)});
        passes_.push_back({PassSlot::Bloom,
                           [this] { return settings_.BloomEnabled(); },
                           TargetBit(TargetSlot::SceneColor),
                           0,
                           TargetBit(TargetSlot::Bloom)});
        passes_.push_back({PassSlot::Tonemap,
                           nullptr, // always: D3's frame buffer is HDR
                           TargetBit(TargetSlot::SceneColor),
                           0,
                           TargetBit(TargetSlot::Backbuffer)});
        passes_.push_back({PassSlot::ImGui, nullptr, 0, 0, TargetBit(TargetSlot::Backbuffer)});
    }

    const char* Name() const override {
        return "Diablo3";
    }
    std::span<const PassEntry> Passes() const override {
        return passes_;
    }
    std::span<const TargetDesc> TargetSet() const override {
        return targets_;
    }
    gfx::Format SceneColorFormat() const override {
        // Always the float target — D3 tonemaps unconditionally (see the pass
        // list). A gamma-LDR UNORM scene target would clip every additive and
        // bright-albedo material before the tonemap could touch it.
        return gfx::Format::R11G11B10_FLOAT;
    }
    bool LinearShading() const override {
        // The material is fixed-function and authored against a gamma pipeline;
        // shading it linearly would be re-grading art that was signed off in
        // the space it was authored in.
        return false;
    }
    f32 WorldScale() const override {
        return kD3UnitsToRendererUnits;
    }
    core::UnlitLightingModel UnlitLighting() const override {
        // What an unresolved surface falls back to. Blinn-Phong rather than
        // flat white because it reads the vertex normal, which is exactly the
        // attribute a wrong layout description silently drops.
        return core::UnlitLightingModel::BlinnPhong;
    }
    CoordSpace SourceSpace() const override {
        // Measured from the shipped skeleton, not taken from the APP spec:
        // Barbarian_Male's left_* bones sit at +Y and right_* at -Y with up at
        // +Z, which for a right-handed Z-up space puts forward at +X. That is
        // CoordSpace::Blizzard, identical to Warcraft III and World of Warcraft.
        return CoordSpace::Blizzard;
    }
    std::span<shading::IShadingModel* const> ShadingModels() const override {
        return models_;
    }
    void SetShadingModels(std::vector<shading::IShadingModel*> models) {
        models_ = std::move(models);
    }
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

} // namespace whiteout::flakes::renderer::profiles::diablo3

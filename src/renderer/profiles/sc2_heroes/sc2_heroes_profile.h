#pragma once

// Sc2HeroesProfile — the StarCraft II / Heroes of the Storm frame.
//
// One profile, two games, because they ship the same format through the same
// frame. Named for the games rather than the format for the same reason
// WowProfile is: a profile is a *frame*, and the format name belongs on the
// adapter (M3ModelAdapter, next to M2ModelAdapter and MdxModelAdapter).
//
// Reuses Wc3HdProfile's shape — linear shading, an HDR scene target, one MRT
// G-buffer pass, and a tonemap that is not optional — where WowProfile copies
// SD's. That split is the point: between them the two new profiles exercise
// both existing frame shapes, so "the abstraction fits a second game" is
// checked against the complicated case as well as the simple one.
//
// Consequence, stated up front because it changes what the gate asserts: the
// P10 golden is *tonemapped* white, not #FFFFFF. A linear 1.0 through the
// tonemap and bloom chain does not land at 1.0. That is why the definition of
// done says "flat untextured constant colour" rather than "white".

#include "core/render_profile.h"
#include "renderer/render_settings.h"

#include <functional>
#include <vector>

namespace whiteout::flakes::renderer::profiles::sc2_heroes {

using core::PassEntry;
using core::PassSlot;
using core::TargetBit;
using core::TargetBits;
using core::TargetDesc;
using core::TargetSlot;

class Sc2HeroesProfile final : public core::IRenderProfile {
public:
    // SC2 model units → renderer units. One SC2 unit is 100 Warcraft III
    // units — the same ratio World of Warcraft uses, so this is deliberately
    // the same number as WowProfile::kWowUnitsToRendererUnits rather than an
    // independently measured one.
    //
    // Replaced a framing constant of 30 that had been fitted to corpus
    // bounding boxes (Zealot 1.59 units, Marine 4.06, Thor 12.08) so infantry
    // landed near WC3's 90–120 range. That made models look right against a
    // fixed camera and was not the conversion.
    static constexpr f32 kSc2UnitsToRendererUnits = 100.0f;

    explicit Sc2HeroesProfile(RenderSettings& settings) : settings_(settings) {
        targets_ = {
            TargetDesc{TargetSlot::SceneColor, gfx::Format::R11G11B10_FLOAT, 1.0f},
            TargetDesc{TargetSlot::Depth, gfx::Format::D24_UNORM_S8_UINT, 1.0f},
            TargetDesc{TargetSlot::LinearDepth, gfx::Format::R32_FLOAT, 1.0f},
            TargetDesc{TargetSlot::Normal, gfx::Format::R8G8B8A8_UNORM, 1.0f},
            TargetDesc{TargetSlot::AmbientOcclusion, gfx::Format::R8G8B8A8_UNORM, 1.0f},
            TargetDesc{TargetSlot::Bloom, gfx::Format::R11G11B10_FLOAT, 0.5f},
            TargetDesc{TargetSlot::Backbuffer, gfx::Format::R8G8B8A8_UNORM, 1.0f},
        };
        // No ShadowMap pass or target: nothing casts one yet, and declaring a
        // target nobody writes is what ValidateProfile exists to reject.
        //
        // The G-buffer pass declares all four attachments even though the only
        // model that can draw an `.m3` today writes SV_Target0 alone. That is
        // not a lie: the declaration describes the *pass*, and the pass really
        // does bind three colour attachments — UnlitShading::Emits is where
        // "this model fills one of them" is stated, per model, which is the
        // whole reason Emits is per-model rather than per-pass.
        passes_.push_back({PassSlot::GBuffer,
                           nullptr,
                           0,
                           0,
                           TargetBits({TargetSlot::SceneColor, TargetSlot::Depth,
                                       TargetSlot::LinearDepth, TargetSlot::Normal})});
        passes_.push_back({PassSlot::TransparentScene,
                           nullptr,
                           TargetBit(TargetSlot::Depth),
                           0,
                           TargetBit(TargetSlot::SceneColor)});
        // GTAO and DoF are declared and gated OFF, not omitted. Both read
        // LinearDepth (GTAO also Normal), and at this stage nothing writes
        // either: the G-buffer slots hold their clear values — a 1e5 "no draw
        // landed here" sentinel and a flat encoded +Z. GTAO over that computes
        // no occlusion, and DoF over it blurs by a far-plane CoC. Declaring
        // them keeps the frame's real shape visible and makes enabling them,
        // once an `.m3` shading model writes a normal, a predicate change
        // rather than a profile change.
        passes_.push_back({PassSlot::Gtao,
                           [] { return false; },
                           TargetBits({TargetSlot::LinearDepth, TargetSlot::Normal}),
                           0,
                           TargetBits({TargetSlot::AmbientOcclusion, TargetSlot::SceneColor})});
        passes_.push_back({PassSlot::Dof,
                           [] { return false; },
                           TargetBits({TargetSlot::SceneColor, TargetSlot::LinearDepth}),
                           0,
                           TargetBit(TargetSlot::SceneColor)});
        // Bloom stays live: it reads SceneColor only, so it is well-defined on
        // an unlit frame, and it is half of why the golden is tonemapped white
        // rather than white.
        passes_.push_back({PassSlot::Bloom,
                           [this] { return settings_.BloomEnabled(); },
                           TargetBit(TargetSlot::SceneColor),
                           0,
                           TargetBit(TargetSlot::Bloom)});
        passes_.push_back({PassSlot::Tonemap,
                           nullptr,
                           TargetBit(TargetSlot::SceneColor),
                           0,
                           TargetBit(TargetSlot::Backbuffer)});
        passes_.push_back({PassSlot::ImGui, nullptr, 0, 0, TargetBit(TargetSlot::Backbuffer)});
    }

    const char* Name() const override {
        return "Sc2Heroes";
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
        return kSc2UnitsToRendererUnits;
    }
    core::UnlitLightingModel UnlitLighting() const override {
        // Per-pixel Lambert — the diffuse half of WoW's Blinn-Phong, chosen
        // so the two profiles are distinguishable at a glance. Same purpose:
        // it reads the normal, so the `.m3` layout description is either
        // right or visibly wrong. Not SC2's shading model.
        return core::UnlitLightingModel::Lambert;
    }
    CoordSpace SourceSpace() const override {
        // The one profile that is not renderer-native. StarCraft II and Heroes
        // author −Y forward / +X left (3ds Max's axes, which their exporter
        // kept), where Warcraft III and WoW use +X forward / +Y left — a 90°
        // yaw. Applied on the actor transform, not baked into the vertices,
        // because an `.m3` reaches the GPU as a verbatim MeshBuffer with no
        // CPU-side copy left to rotate.
        return CoordSpace::Sc2;
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

} // namespace whiteout::flakes::renderer::profiles::sc2_heroes

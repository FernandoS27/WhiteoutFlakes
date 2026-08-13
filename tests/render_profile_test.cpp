// The profile's startup validation, and the two identity claims WC3 makes.
//
// Device-free by construction: a profile is a declaration, so the check that a
// pass only reads what an earlier pass wrote is arithmetic on bitmasks. That
// makes it the one part of the frame chain that can be a CI gate.

#include <catch2/catch_test_macros.hpp>

#include "core/render_profile.h"
#include "profiles/wc3/wc3_profile.h"
#include "renderer/render_settings.h"

using namespace whiteout::flakes::renderer::core;
namespace gfx = whiteout::flakes::gfx;
namespace wfr = whiteout::flakes::renderer;
using whiteout::flakes::renderer::RenderSettings;
using whiteout::flakes::renderer::profiles::wc3::Wc3HdProfile;
using whiteout::flakes::renderer::profiles::wc3::Wc3SdProfile;

namespace {

// A profile assembled field by field, so a test can build an invalid chain
// without a real product having to contain one.
class FakeProfile final : public IRenderProfile {
public:
    std::vector<PassEntry> passes;
    std::vector<TargetDesc> targets;

    const char* Name() const override { return "Fake"; }
    std::span<const PassEntry> Passes() const override { return passes; }
    std::span<const TargetDesc> TargetSet() const override { return targets; }
    gfx::Format SceneColorFormat() const override { return gfx::Format::R8G8B8A8_UNORM; }
    bool LinearShading() const override { return false; }
    float WorldScale() const override { return 1.0f; }
    wfr::CoordSpace SourceSpace() const override {
        return wfr::kDefaultCoordSpace;
    }
    std::span<wfr::shading::IShadingModel* const> ShadingModels()
        const override {
        return {};
    }
};

TargetDesc Target(TargetSlot s) {
    return TargetDesc{s, gfx::Format::R8G8B8A8_UNORM, 1.0f};
}

} // namespace

TEST_CASE("Both WC3 profiles declare a coherent chain") {
    RenderSettings settings;
    Wc3SdProfile sd(settings);
    Wc3HdProfile hd(settings);

    auto sdResult = ValidateProfile(sd);
    INFO(sdResult.error);
    REQUIRE(sdResult.ok);

    auto hdResult = ValidateProfile(hd);
    INFO(hdResult.error);
    REQUIRE(hdResult.ok);
}

TEST_CASE("A pass reading a target nothing has written is rejected") {
    FakeProfile p;
    p.targets = {Target(TargetSlot::SceneColor), Target(TargetSlot::AmbientOcclusion)};
    p.passes.push_back({PassSlot::OpaqueColor, nullptr, TargetBit(TargetSlot::AmbientOcclusion), 0,
                        TargetBit(TargetSlot::SceneColor)});

    const auto r = ValidateProfile(p);
    REQUIRE_FALSE(r.ok);
    REQUIRE(r.error.find("AmbientOcclusion") != std::string::npos);
}

TEST_CASE("A pass using a target outside the declared set is rejected") {
    FakeProfile p;
    p.targets = {Target(TargetSlot::SceneColor)};
    p.passes.push_back({PassSlot::OpaqueColor, nullptr, 0, 0,
                        TargetBits({TargetSlot::SceneColor, TargetSlot::Normal})});

    const auto r = ValidateProfile(p);
    REQUIRE_FALSE(r.ok);
    REQUIRE(r.error.find("Normal") != std::string::npos);
}

TEST_CASE("An unconditional pass may not depend on a switchable producer") {
    // The rule that earns its keep: this chain works until someone turns the
    // conditional pass off, and then the consumer reads a target nobody wrote.
    FakeProfile p;
    p.targets = {Target(TargetSlot::SceneColor), Target(TargetSlot::AmbientOcclusion),
                 Target(TargetSlot::Backbuffer)};
    p.passes.push_back({PassSlot::Gtao, [] { return false; }, 0, 0,
                        TargetBit(TargetSlot::AmbientOcclusion)});
    p.passes.push_back({PassSlot::Tonemap, nullptr, TargetBit(TargetSlot::AmbientOcclusion), 0,
                        TargetBit(TargetSlot::Backbuffer)});

    const auto r = ValidateProfile(p);
    REQUIRE_FALSE(r.ok);
    REQUIRE(r.error.find("conditional") != std::string::npos);
}

TEST_CASE("A conditional consumer of a conditional producer is fine") {
    FakeProfile p;
    p.targets = {Target(TargetSlot::SceneColor), Target(TargetSlot::AmbientOcclusion)};
    p.passes.push_back({PassSlot::Gtao, [] { return false; }, 0, 0,
                        TargetBit(TargetSlot::AmbientOcclusion)});
    p.passes.push_back({PassSlot::Dof, [] { return false; },
                        TargetBit(TargetSlot::AmbientOcclusion), 0,
                        TargetBit(TargetSlot::SceneColor)});

    REQUIRE(ValidateProfile(p).ok);
}

TEST_CASE("WorldScale and SourceSpace are exactly identity for WC3") {
    // Plumbed but inert. Pinning them here is what stops them drifting before
    // a format that needs them arrives — a WoW creature is 2-5 yards against
    // WC3 camera constants sized in the hundreds, so a WorldScale that silently
    // became non-identity would move every WC3 model.
    RenderSettings settings;
    Wc3SdProfile sd(settings);
    Wc3HdProfile hd(settings);

    REQUIRE(sd.WorldScale() == 1.0f);
    REQUIRE(hd.WorldScale() == 1.0f);
    REQUIRE(sd.SourceSpace() == wfr::kDefaultCoordSpace);
    REQUIRE(hd.SourceSpace() == wfr::kDefaultCoordSpace);
}

TEST_CASE("The two WC3 profiles disagree on format and linearity, as they must") {
    // SD and HD are not interchangeable: mixing them in one scene needs
    // SceneHdrInSd, which is a fidelity change rather than a free win.
    RenderSettings settings;
    Wc3SdProfile sd(settings);
    Wc3HdProfile hd(settings);

    REQUIRE(sd.SceneColorFormat() == gfx::Format::R8G8B8A8_UNORM);
    REQUIRE(hd.SceneColorFormat() == gfx::Format::R11G11B10_FLOAT);
    REQUIRE_FALSE(sd.LinearShading());
    REQUIRE(hd.LinearShading());

    settings.SetSceneHdrInSd(true);
    REQUIRE(sd.SceneColorFormat() == gfx::Format::R11G11B10_FLOAT);
}

TEST_CASE("An optional read of a switchable producer is allowed") {
    // The distinction optionalReads exists for, and the reason WC3's real
    // chain validates: the scene pass samples the shadow map when shadows are
    // on and compiles a permutation without it when they are off.
    FakeProfile p;
    p.targets = {Target(TargetSlot::SceneColor), Target(TargetSlot::ShadowMap)};
    p.passes.push_back({PassSlot::ShadowMap, [] { return false; }, 0, 0,
                        TargetBit(TargetSlot::ShadowMap)});
    p.passes.push_back({PassSlot::OpaqueColor, nullptr, 0, TargetBit(TargetSlot::ShadowMap),
                        TargetBit(TargetSlot::SceneColor)});

    REQUIRE(ValidateProfile(p).ok);
}

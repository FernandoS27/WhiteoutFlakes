// The profile's startup validation, and the two identity claims WC3 makes.
//
// Device-free by construction: a profile is a declaration, so the check that a
// pass only reads what an earlier pass wrote is arithmetic on bitmasks. That
// makes it the one part of the frame chain that can be a CI gate.

#include <catch2/catch_test_macros.hpp>

#include "core/render_profile.h"
#include "profiles/wc3/wc3_profile.h"
#include "renderer/render_settings.h"
#if WDX_ENABLE_M2
#include "profiles/wow/wow_profile.h"
#endif
#if WDX_ENABLE_M3
#include "profiles/sc2_heroes/sc2_heroes_profile.h"
#endif
#if WDX_ENABLE_D3
#include "profiles/diablo3/diablo3_profile.h"
#endif

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

#if WDX_ENABLE_M2 || WDX_ENABLE_M3
TEST_CASE("The non-WC3 profiles agree on units and disagree on axes") {
    RenderSettings settings;
#if WDX_ENABLE_M2
    whiteout::flakes::renderer::profiles::wow::WowProfile wow(settings);
    // One game unit is 100 Warcraft III units. Both products, same ratio —
    // asserted rather than assumed, because these used to be two independently
    // fitted framing constants (20 and 30) and the equality is the claim.
    REQUIRE(wow.WorldScale() == 100.0f);
    // WoW shares WC3's axes; only SC2 diverges.
    REQUIRE(wow.SourceSpace() == wfr::CoordSpace::Blizzard);
    REQUIRE(wow.SourceSpace() == wfr::kDefaultCoordSpace);
#endif
#if WDX_ENABLE_M3
    whiteout::flakes::renderer::profiles::sc2_heroes::Sc2HeroesProfile sc2(settings);
    REQUIRE(sc2.WorldScale() == 100.0f);
    REQUIRE(sc2.SourceSpace() == wfr::CoordSpace::Sc2);
    // The whole point of the field: SC2 is the one profile that is not
    // renderer-native, so a change that quietly made it identity would put
    // every `.m3` back to facing 90° wrong with nothing to catch it.
    REQUIRE(sc2.SourceSpace() != wfr::kDefaultCoordSpace);
#endif
#if WDX_ENABLE_M2 && WDX_ENABLE_M3
    REQUIRE(wow.WorldScale() == sc2.WorldScale());
#endif
}
#endif

#if WDX_ENABLE_D3
TEST_CASE("Diablo III keeps WC3's on-screen scale, not the other profiles' 100") {
    // D3 shares WC3's and WoW's axes but NOT the 100 unit scale the other
    // non-WC3 profiles use. A D3 character is ~6.4 raw units, so 100 would put
    // it at ~640 (in line with WoW ~960 / SC2 ~400) but ~6x a WC3 hero; 17
    // lands it at ~110, inside the 90-120 band the shared camera is fitted to.
    // Pinned because "match the other profiles -> 100" is the plausible wrong
    // change, and the on-screen size is what disproves it.
    RenderSettings settings;
    whiteout::flakes::renderer::profiles::diablo3::Diablo3Profile d3(settings);
    REQUIRE(d3.WorldScale() == 17.0f);
    REQUIRE(d3.SourceSpace() == wfr::CoordSpace::Blizzard);
}
#endif

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

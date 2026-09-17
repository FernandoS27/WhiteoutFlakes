// ============================================================================
// The viewer's scalar setting descriptors
// (tools/basic_viewer/settings/setting_descriptors.h) against the `[Display]`
// keys and value spellings WhiteoutFlakes.ini has always used.
//
// tests/data/viewer_settings_display.ini holds every descriptor-driven key under
// the name and in the format the hand-written loader and saver used (floats at
// three decimals, bools as 1/0). Loading it must set each value; saving must
// write each key back byte for byte.
// ============================================================================

#include "settings/setting_descriptors.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <string>
#include <vector>

using namespace whiteout::flakes;
using namespace whiteout::flakes::settings;
using Catch::Matchers::WithinAbs;

namespace {

ini::IniMap LoadFixture() {
    ini::IniMap map;
    map.Load(WDX_VIEWER_SETTINGS_FIXTURE);
    REQUIRE(!map.values.empty());
    return map;
}

void LoadAll(RenderSettings& s, const ini::IniMap& map) {
    Load(s, map, kPhysicsOverlays);
    Load(s, map, kClothDeform);
    Load(s, map, kPhysicsSubstepping);
    Load(s, map, kExposure);
    Load(s, map, kGraphicsDebug);
    Load(s, map, kWowPage);
    Load(s, map, kAoEnabled);
    Load(s, map, kAoBentBoost);
    Load(s, map, kBloomEnabled);
    Load(s, map, kBloomLevels);
    Load(s, map, kDofEnabled);
    Load(s, map, kDofLevels);
    Load(s, map, kDofFarFieldOnly);
}

void SaveAll(const RenderSettings& s, ini::IniMap& map) {
    Save(s, map, kPhysicsOverlays);
    Save(s, map, kClothDeform);
    Save(s, map, kPhysicsSubstepping);
    Save(s, map, kExposure);
    Save(s, map, kGraphicsDebug);
    Save(s, map, kWowPage);
    Save(s, map, kAoEnabled);
    Save(s, map, kAoBentBoost);
    Save(s, map, kBloomEnabled);
    Save(s, map, kBloomLevels);
    Save(s, map, kDofEnabled);
    Save(s, map, kDofLevels);
    Save(s, map, kDofFarFieldOnly);
    // Not persisted, so never written.
    Save(s, map, kD3LazyAnimations);
}

} // namespace

TEST_CASE("Descriptor settings load the recorded [Display] values", "[viewer][settings]") {
    const ini::IniMap fixture = LoadFixture();
    RenderSettings s;
    LoadAll(s, fixture);

    CHECK(s.ShowPhysicsDynamic());
    CHECK_FALSE(s.ShowPhysicsKinematic());
    CHECK(s.ShowPhysicsStatic());
    CHECK(s.ShowPhysicsCloth());
    CHECK_FALSE(s.ClothDeform());
    CHECK_FALSE(s.PhysicsSubstepping());
    CHECK_THAT(s.GetTonemapExposure(), WithinAbs(1.75, 1e-6));
    CHECK(s.GraphicsDebug());
    CHECK(s.M2LazyAnimations());
    CHECK(s.M2DistanceSortGeometry());
    CHECK_FALSE(s.M2ModelLights());
    CHECK_FALSE(s.AoEnabled());
    CHECK_THAT(s.AoBentBoost(), WithinAbs(0.125, 1e-6));
    CHECK(s.BloomEnabled());
    CHECK_THAT(s.BloomThreshold(), WithinAbs(1.5, 1e-6));
    CHECK_THAT(s.BloomIntensity(), WithinAbs(2.5, 1e-6));
    CHECK_THAT(s.BloomSaturation(), WithinAbs(0.75, 1e-6));
    CHECK(s.DofEnabled());
    CHECK_THAT(s.DofFocusDistance(), WithinAbs(812.0, 1e-4));
    CHECK_THAT(s.DofFocusScale(), WithinAbs(64.5, 1e-6));
    CHECK_THAT(s.DofMaxBlurSize(), WithinAbs(12.0, 1e-6));
    CHECK_THAT(s.DofRadiusScale(), WithinAbs(2.25, 1e-6));
    CHECK(s.DofFarFieldOnly());
}

TEST_CASE("Descriptor settings save every key back as recorded", "[viewer][settings]") {
    const ini::IniMap fixture = LoadFixture();
    RenderSettings s;
    LoadAll(s, fixture);

    ini::IniMap saved;
    SaveAll(s, saved);
    // Same keys, same spellings, and nothing else.
    CHECK(saved.values.size() == fixture.values.size());
    for (const auto& [key, value] : fixture.values) {
        INFO(key);
        const std::string* written = saved.Get(key);
        REQUIRE(written);
        CHECK(*written == value);
    }
}

TEST_CASE("Descriptor settings keep each key's own read rules", "[viewer][settings]") {
    RenderSettings s;
    ini::IniMap map;

    // The physics toggles only ever took "1"; the other flags take ParseBool's words.
    map.Set("Display.ShowPhysicsDynamic", "true");
    map.Set("Display.M2LazyAnimations", "true");
    Load(s, map, kPhysicsOverlays);
    Load(s, map, kWowPage);
    CHECK_FALSE(s.ShowPhysicsDynamic());
    CHECK(s.M2LazyAnimations());

    // A word ParseBool does not know leaves the setting where it was.
    s.SetAoEnabled(true);
    map.Set("Display.AoEnabled", "yes");
    Load(s, map, kAoEnabled);
    CHECK(s.AoEnabled());

    // Exposure clamps on load; the others store what they read.
    map.Set("Display.Exposure", "9.000");
    map.Set("Display.BloomIntensity", "9.000");
    Load(s, map, kExposure);
    Load(s, map, kBloomLevels);
    CHECK_THAT(s.GetTonemapExposure(), WithinAbs(3.0, 1e-6));
    CHECK_THAT(s.BloomIntensity(), WithinAbs(9.0, 1e-6));

    // Reset puts the page's value back.
    Reset(s, kBloomIntensity);
    CHECK_THAT(s.BloomIntensity(), WithinAbs(1.25, 1e-6));
}

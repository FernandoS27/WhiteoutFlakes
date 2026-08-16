// CM2Lighting — the per-model light accumulator, device-free.
//
// Transcribed from CM2Lighting::AddDiffuse @ 0x100f4fbc0, AddLight @ 0x100f4fdb0,
// SetupSunlight @ 0x100f50190 and CShaderEffect::SetLocalLighting @ 0x100e9a2e0
// in a WoW 6.0.1 client.

#include "renderer/profiles/wow/m2_lighting.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace whiteout::flakes::renderer::profiles::wow;
using whiteout::Vector3f;
using Catch::Approx;

namespace {

M2LightInput PointAt(const Vector3f& p, const Vector3f& color) {
    M2LightInput in;
    in.positional = true;
    in.positionWS = p;
    in.diffuse = color;
    return in;
}

} // namespace

TEST_CASE("The portrait preset resolves to the client's key light", "[m2][lighting]") {
    M2Lighting acc({0.0f, 0.0f, 0.0f});
    acc.AddAmbient(kM2PortraitAmbient);
    acc.AddDiffuse(kM2PortraitDiffuse, kM2PortraitDirection);
    const M2LightingResult r = acc.Resolve();

    CHECK(r.ambient.x == Approx(0.45f));
    CHECK(r.diffuse.x == Approx(1.0f));
    // (-1, 0, -1) arrives unnormalised; SetupSunlight is what normalises it.
    CHECK(r.directionWS.x == Approx(-0.70710678f));
    CHECK(r.directionWS.y == Approx(0.0f));
    CHECK(r.directionWS.z == Approx(-0.70710678f));
    CHECK(r.pointCount == 0u);
}

TEST_CASE("Ambient accumulates and the directional term does not", "[m2][lighting]") {
    M2Lighting acc({0.0f, 0.0f, 0.0f});
    acc.AddAmbient({0.1f, 0.1f, 0.1f});
    acc.AddAmbient({0.2f, 0.2f, 0.2f});
    acc.AddDiffuse({1.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f});
    acc.AddDiffuse({0.0f, 0.5f, 0.0f}, {0.0f, 0.0f, 1.0f});
    const M2LightingResult r = acc.Resolve();

    CHECK(r.ambient.x == Approx(0.3f));
    // AddDiffuse assigns. Two directional lights leave only the second lighting
    // the model — which is why CM2Model::SetupLighting runs the environment
    // callback *after* the scene's own lights.
    CHECK(r.diffuse.x == Approx(0.0f));
    CHECK(r.diffuse.y == Approx(0.5f));
    CHECK(r.directionWS.z == Approx(1.0f));
}

TEST_CASE("Resolve clamps the diffuse colour and leaves the ambient alone",
          "[m2][lighting]") {
    M2Lighting acc({0.0f, 0.0f, 0.0f});
    acc.AddAmbient({1.5f, 0.2f, 0.0f});
    acc.AddDiffuse({2.0f, 0.25f, 1.0f}, {0.0f, 0.0f, -1.0f});
    const M2LightingResult r = acc.Resolve();

    // fminf on the one, a plain copy on the other. An interior stacking several
    // ambient lights really does exceed 1 in the client.
    CHECK(r.diffuse.x == Approx(1.0f));
    CHECK(r.diffuse.y == Approx(0.25f));
    CHECK(r.ambient.x == Approx(1.5f));
}

TEST_CASE("A degenerate direction falls back to (0, 0, -1)", "[m2][lighting]") {
    M2Lighting acc({0.0f, 0.0f, 0.0f});
    // Never told about a directional light at all: SetupSunlight's zero-length
    // branch, which writes -1 into z rather than leaving the vector at zero.
    const M2LightingResult r = acc.Resolve();
    CHECK(r.directionWS.x == Approx(0.0f));
    CHECK(r.directionWS.y == Approx(0.0f));
    CHECK(r.directionWS.z == Approx(-1.0f));
}

TEST_CASE("Point lights keep the three nearest, nearest first", "[m2][lighting]") {
    M2Lighting acc({0.0f, 0.0f, 0.0f});
    acc.AddLight(PointAt({30.0f, 0.0f, 0.0f}, {0.3f, 0.0f, 0.0f}));
    acc.AddLight(PointAt({10.0f, 0.0f, 0.0f}, {0.1f, 0.0f, 0.0f}));
    acc.AddLight(PointAt({20.0f, 0.0f, 0.0f}, {0.2f, 0.0f, 0.0f}));

    SECTION("a fourth further away is dropped") {
        acc.AddLight(PointAt({40.0f, 0.0f, 0.0f}, {0.4f, 0.0f, 0.0f}));
        const M2LightingResult r = acc.Resolve();
        REQUIRE(r.pointCount == kM2MaxPointLights);
        CHECK(r.points[0].diffuse.x == Approx(0.1f));
        CHECK(r.points[1].diffuse.x == Approx(0.2f));
        CHECK(r.points[2].diffuse.x == Approx(0.3f));
    }

    SECTION("a fourth nearer one evicts the farthest") {
        acc.AddLight(PointAt({5.0f, 0.0f, 0.0f}, {0.5f, 0.0f, 0.0f}));
        const M2LightingResult r = acc.Resolve();
        REQUIRE(r.pointCount == kM2MaxPointLights);
        CHECK(r.points[0].diffuse.x == Approx(0.5f));
        CHECK(r.points[1].diffuse.x == Approx(0.1f));
        CHECK(r.points[2].diffuse.x == Approx(0.2f));
    }
}

TEST_CASE("Distance is measured from the model centre, not the origin",
          "[m2][lighting]") {
    M2Lighting acc({100.0f, 0.0f, 0.0f});
    acc.AddLight(PointAt({0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}));
    acc.AddLight(PointAt({101.0f, 0.0f, 0.0f}, {2.0f, 0.0f, 0.0f}));
    const M2LightingResult r = acc.Resolve();
    REQUIRE(r.pointCount == 2u);
    CHECK(r.points[0].diffuse.x == Approx(2.0f));
}

TEST_CASE("An invisible light contributes nothing", "[m2][lighting]") {
    M2Lighting acc({0.0f, 0.0f, 0.0f});

    M2LightInput dark = PointAt({1.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f});
    dark.visible = false;
    acc.AddLight(dark);

    M2LightInput dirLight;
    dirLight.visible = false;
    dirLight.ambient = {0.9f, 0.9f, 0.9f};
    dirLight.diffuse = {0.9f, 0.9f, 0.9f};
    acc.AddLight(dirLight);

    const M2LightingResult r = acc.Resolve();
    CHECK(r.pointCount == 0u);
    CHECK(r.ambient.x == Approx(0.0f));
    CHECK(r.diffuse.x == Approx(0.0f));
}

TEST_CASE("A directional light folds into the ambient and diffuse terms",
          "[m2][lighting]") {
    M2Lighting acc({0.0f, 0.0f, 0.0f});
    M2LightInput L;
    L.positional = false;
    L.ambient = {0.1f, 0.1f, 0.1f};
    L.diffuse = {0.8f, 0.7f, 0.6f};
    L.directionWS = {0.0f, 2.0f, 0.0f};
    acc.AddLight(L);
    const M2LightingResult r = acc.Resolve();

    CHECK(r.pointCount == 0u);
    CHECK(r.ambient.y == Approx(0.1f));
    CHECK(r.diffuse.y == Approx(0.7f));
    CHECK(r.directionWS.y == Approx(1.0f));
}

TEST_CASE("Point lights carry the client's fixed attenuation", "[m2][lighting]") {
    // Not file data: AnimateMT animates a light block's colour and visibility
    // and never reads attenuationStart / attenuationEnd, so every `.m2` point
    // light falls back to what CM2Light's constructor left behind.
    M2Lighting acc({0.0f, 0.0f, 0.0f});
    acc.AddLight(PointAt({1.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}));
    const M2LightingResult r = acc.Resolve();
    REQUIRE(r.pointCount == 1u);
    CHECK(r.points[0].attenuation.x == Approx(0.0f));
    CHECK(r.points[0].attenuation.y == Approx(0.7f));
    CHECK(r.points[0].attenuation.z == Approx(0.03f));
}

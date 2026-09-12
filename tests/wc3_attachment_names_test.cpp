// ============================================================================
// Warcraft III attachment names -> StarCraft II `Ref_` names
// (WC3_TO_SC2_COMPLETION_PLAN.md §4.1). Every row of the table, the suffix
// spellings the shipped corpus actually carries (C0.5's histogram), and the
// rule a name nobody listed falls through to.
// ============================================================================

#include "wc3_attachment_names.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <utility>
#include <variant>
#include <vector>

using whiteout::flakes::Wc3AttachmentNameToSc2;
using whiteout::flakes::Wc3TargetVolumeOf;
namespace wem = whiteout::models::wem;

TEST_CASE("Warcraft III attachment names take StarCraft II's", "[wc3_sc2][attachments]") {
    const std::vector<std::pair<std::string, std::string>> rows = {
        {"Origin Ref", "Ref_Origin"},
        {"OverHead Ref ", "Ref_Overhead"},
        {"Chest Ref", "Ref_Chest"},
        {"Head Ref", "Ref_Head"},
        {"Head - Ref", "Ref_Head"},
        {"Hand Left Ref", "Ref_Hand Left"},
        {"Hand Right Ref", "Ref_Hand Right"},
        {"Foot Left Ref", "Ref_Foot Left"},
        {"Foot Right Rear Ref", "Ref_Foot Right Rear"},
        {"Weapon Ref", "Ref_Weapon"},
        {"Weapon - Ref", "Ref_Weapon"},
        {"Weapon Left Ref", "Ref_Weapon Left"},
        {"Chest Mount Rear Ref", "Ref_Chest Mount Rear"},
        {"Head Mount - Ref", "Ref_Head Mount"},
        {"Sprite First Ref", "Ref_Hardpoint 01"},
        {"Sprite Sixth Ref", "Ref_Hardpoint 06"},
        {"Sprite RallyPoint Ref", "Ref_RallyPoint"},
        {"Sprite RalyPoint Ref", "Ref_RallyPoint"},
        {"sprite rallypoint", "Ref_RallyPoint"},
        {"Sprite Medium Ref", "Ref_Hardpoint Medium"},
        {"Sprite EatTree Ref", "Ref_Hardpoint EatTree"},
        {"Chest Alternate Ref", "Ref_Chest Alternate"},
        {"Chest Ref01", "Ref_Chest"},
        {"Hand Right Ref 01", "Ref_Hand Right"},
        {"Head - Ref01", "Ref_Head"},
        {"Origin Ref03", "Ref_Origin"},
        {"Foot Right  Ref", "Ref_Foot Right"},
    };
    for (const auto& [wc3, sc2] : rows) {
        INFO(wc3);
        const auto renamed = Wc3AttachmentNameToSc2(wc3);
        CHECK(renamed.name == sc2);
        CHECK(renamed.known);
    }
}

TEST_CASE("an attachment name no table row lists still takes the rule, and says so",
          "[wc3_sc2][attachments]") {
    const auto birth = Wc3AttachmentNameToSc2("BirthLink");
    CHECK(birth.name == "Ref_Birthlink");
    CHECK_FALSE(birth.known);
    const auto grip = Wc3AttachmentNameToSc2("Weapon R Ref");
    CHECK(grip.name == "Ref_Weapon R");
    CHECK_FALSE(grip.known);
    // A word that merely ends in "ref" is not the suffix.
    const auto shelf = Wc3AttachmentNameToSc2("Shelfref");
    CHECK(shelf.name == "Ref_Shelfref");
}

TEST_CASE("the targeting volume wraps every collision shape, box corners from their pivot",
          "[wc3_sc2][attachments]") {
    wem::Model model;
    CHECK_FALSE(Wc3TargetVolumeOf(model).fromCollision);
    CHECK(Wc3TargetVolumeOf(model).scale.x == Catch::Approx(1.2f));
    CHECK(Wc3TargetVolumeOf(model).center.z == Catch::Approx(0.0f));

    wem::Node sphere;
    sphere.kind = wem::NodeKind::CollisionShape;
    sphere.resetPayloadForKind();
    auto& round = std::get<wem::CollisionPayload>(sphere.payload).shape;
    round.kind = wem::CollisionShapeKind::Sphere;
    round.sphere.center = whiteout::Vector3f{10, 20, 30};
    round.sphere.radius = 5;
    sphere.pivot = round.sphere.center;
    model.nodes.add(sphere);

    wem::Node box;
    box.kind = wem::NodeKind::CollisionShape;
    box.resetPayloadForKind();
    auto& square = std::get<wem::CollisionPayload>(box.payload).shape;
    square.kind = wem::CollisionShapeKind::Box;
    square.box.minimum = whiteout::Vector3f{10, 20, 40}; // corners in either order
    square.box.maximum = whiteout::Vector3f{-10, -20, 0};
    box.pivot = whiteout::Vector3f{100, 0, 0};
    model.nodes.add(box);

    // x 5..110, y -20..25, z 0..40.
    const auto volume = Wc3TargetVolumeOf(model);
    CHECK(volume.fromCollision);
    CHECK(volume.center.x == Catch::Approx(57.5f));
    CHECK(volume.center.y == Catch::Approx(2.5f));
    CHECK(volume.center.z == Catch::Approx(20.0f));
    // 1.44 x the half extents / 100, StarCraft II's x being Warcraft III's y.
    CHECK(volume.scale.x == Catch::Approx(0.0144f * 22.5f));
    CHECK(volume.scale.y == Catch::Approx(0.0144f * 52.5f));
    CHECK(volume.scale.z == Catch::Approx(0.0144f * 20.0f));
}

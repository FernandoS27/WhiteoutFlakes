// ============================================================================
// `.m3` non-bone animated channels: bone visibility, geoset gating, lights.
//
// Visibility is the interesting one. It is not a per-bone boolean the consumer
// ANDs together later — the engine propagates it down the parent chain while
// sampling, and skips sampling a bone entirely once an ancestor is hidden
// (M3Anim_EvaluateBoneVisibility tests the parent's visible bit first). It is
// also sampled in *override* mode rather than blended, because a bone is not
// forty percent visible.
// ============================================================================

#include "io/m3/m3_model_adapter.h"
#include "m3_anim_builders.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace whiteout;
using namespace whiteout::flakes;
using namespace whiteout::flakes::io;
using Catch::Approx;

namespace {

renderer::model::FrameState EvalAt(M3ModelAdapter& a, std::vector<ClipRef> clips) {
    PoseRequest req;
    req.clips = clips;
    return a.Evaluate(req);
}

ClipRef Clip(i32 seq, i32 elapsed, f32 weight = 1.0f) {
    ClipRef c;
    c.sequence = seq;
    c.timeMs = elapsed;
    c.elapsedMs = elapsed;
    c.weight = weight;
    c.loop = false;
    return c;
}

// A bone whose visibility is driven by `animId`, keyed on then off.
m3::Model VisibilityFixture(u32 animId, bool childOwnVisibility) {
    m3fix::ModelBuilder mb;
    m3::Bone root;
    root.name = "root";
    root.parentIndex = 0xFFFF;
    root.flags = m3::BoneFlag::Animated;
    root.position = m3fix::ConstRef(Vector3f{0, 0, 0});
    root.rotation = m3fix::ConstRef(Quaternion{0, 0, 0, 1});
    root.scale = m3fix::ConstRef(Vector3f{1, 1, 1});
    root.visibility = m3fix::Ref<u32>(animId, 1u, /*interpType*/ 0);

    m3::Bone child;
    child.name = "child";
    child.parentIndex = 0;
    child.flags = m3::BoneFlag::Animated;
    child.position = m3fix::ConstRef(Vector3f{0, 0, 0});
    child.rotation = m3fix::ConstRef(Quaternion{0, 0, 0, 1});
    child.scale = m3fix::ConstRef(Vector3f{1, 1, 1});
    // The child either claims it is visible, or has no track at all. Neither
    // may rescue it from a hidden parent.
    child.visibility = childOwnVisibility ? m3fix::ConstRef<u32>(1u) : m3fix::ConstRef<u32>(1u);

    m3fix::StcBuilder s("s", 1, false);
    s.U32(animId, m3fix::Block<u32>({0, 500}, {1u, 0u}));

    m3::Model model;
    model.bones.push_back(std::move(root));
    model.bones.push_back(std::move(child));
    model.subTrackCollections.push_back(s.Build());

    m3::Sequence seq;
    seq.name = "Vis";
    seq.startFrame = 0;
    seq.endFrame = 1000;
    model.sequences.push_back(std::move(seq));
    m3::AnimationGroup g;
    g.subtrackIndices = {0};
    model.animationGroups.push_back(std::move(g));

    model.initialReference.assign(model.bones.size(), m3::InitialReference{});
    for (auto& r : model.initialReference)
        r.matrix = Matrix44f::identity();
    return model;
}

// One region hanging off `rootBone`, with a vertex blob big enough for it.
void AddRegion(m3::Model& model, u16 rootBone) {
    m3::Region region;
    region.firstVertex = 0;
    region.vertexCount = 4;
    region.firstIndex = 0;
    region.indexCount = 3;
    region.firstBoneLookup = 0;
    region.boneLookupCount = 1;
    region.rootBone = rootBone;
    if (model.divisions.empty()) {
        m3::MeshDivision div;
        div.faces = {0, 1, 2};
        model.divisions.push_back(std::move(div));
        model.boneLookup = {0};
        model.vertices.flags = m3::VertexFormatFlag::UV1;
        model.vertices.data.assign(4 * 32, 0);
        model.vertices.initialize();
    }
    model.divisions[0].regions.push_back(region);
}

} // namespace

TEST_CASE("Bone visibility is discrete — it holds, it does not fade", "[m3chan]") {
    m3::Model model = VisibilityFixture(300, true);
    AddRegion(model, 0);
    M3ModelAdapter a(std::move(model));

    // Keys are visible@0 and hidden@500. A blended channel would read as a
    // partial alpha in between; an override-sampled discrete one holds the
    // left key right up to the switch.
    REQUIRE(EvalAt(a, {Clip(0, 250)}).geosetAlphas[0] == Approx(1.0f));
    REQUIRE(EvalAt(a, {Clip(0, 499)}).geosetAlphas[0] == Approx(1.0f));
    REQUIRE(EvalAt(a, {Clip(0, 500)}).geosetAlphas[0] == Approx(0.0f));
}

TEST_CASE("A hidden parent hides its whole subtree", "[m3chan]") {
    m3::Model model = VisibilityFixture(300, /*childOwnVisibility*/ true);
    AddRegion(model, 1); // the region hangs off the CHILD
    M3ModelAdapter a(std::move(model));

    SECTION("visible while the parent's track says so") {
        const auto fs = EvalAt(a, {Clip(0, 0)});
        REQUIRE(fs.geosetAlphas.size() == 1);
        REQUIRE(fs.geosetAlphas[0] == Approx(1.0f));
    }
    SECTION("hidden once the parent's track goes to zero") {
        // The child insists it is visible; the parent overrules it, which is
        // what makes this hierarchical rather than per-bone.
        const auto fs = EvalAt(a, {Clip(0, 600)});
        REQUIRE(fs.geosetAlphas.size() == 1);
        REQUIRE(fs.geosetAlphas[0] == Approx(0.0f));
    }
}

TEST_CASE("A geoset is gated by its region's root bone", "[m3chan]") {
    m3::Model model = VisibilityFixture(300, true);
    AddRegion(model, 0); // hangs off the root, which is the animated bone
    M3ModelAdapter a(std::move(model));

    REQUIRE(EvalAt(a, {Clip(0, 0)}).geosetAlphas[0] == Approx(1.0f));
    REQUIRE(EvalAt(a, {Clip(0, 600)}).geosetAlphas[0] == Approx(0.0f));
}

TEST_CASE("An unanimated visibility reference holds its init value", "[m3chan]") {
    m3fix::ModelBuilder mb;
    mb.StaticBone("root", -1);
    m3::Model model = mb.Build();
    model.bones[0].visibility = m3fix::ConstRef<u32>(0u); // authored hidden
    AddRegion(model, 0);
    M3ModelAdapter a(std::move(model));

    const auto fs = EvalAt(a, {});
    REQUIRE(fs.geosetAlphas.size() == 1);
    REQUIRE(fs.geosetAlphas[0] == Approx(0.0f));
}

TEST_CASE("Lights sample their colour and intensity", "[m3chan]") {
    m3fix::ModelBuilder mb;
    mb.StaticBone("root", -1);
    m3fix::StcBuilder s("s", 1, false);
    s.Vec3(400, m3fix::Block<Vector3f>({0, 1000}, {{1, 0, 0}, {0, 0, 1}}));
    s.Float(401, m3fix::Block<f32>({0, 1000}, {0.0f, 4.0f}));
    const u32 idx = mb.AddStc(s.Build());
    mb.Sequence("Glow", 0, 1000, {idx});
    m3::Model model = mb.Build();

    m3::Light light;
    light.lightType = m3::LightType::Omni;
    light.boneIndex = 0;
    light.diffuseColor = m3fix::Ref<Vector3f>(400, {1, 1, 1});
    light.intensityMultiplier = m3fix::Ref<f32>(401, 1.0f);
    light.attenuationStart = m3fix::ConstRef<f32>(2.0f);
    light.attenuationEnd = 9.0f;
    model.lights.push_back(std::move(light));

    M3ModelAdapter a(std::move(model));
    const auto fs = EvalAt(a, {Clip(0, 500)});
    REQUIRE(fs.lights.size() == 1);
    const auto& l = fs.lights[0];

    REQUIRE(l.kind == renderer::model::FrameState::LightKind::Omni);
    // Colour lerps to (0.5, 0, 0.5); intensity lerps to 2.0. The stored
    // diffuse is the product, which is what LightState documents as
    // shader-ready.
    REQUIRE(l.dirIntensity == Approx(2.0f));
    REQUIRE(l.diffuse.x == Approx(1.0f));
    REQUIRE(l.diffuse.y == Approx(0.0f));
    REQUIRE(l.diffuse.z == Approx(1.0f));
    REQUIRE(l.attenStart == Approx(2.0f));
    REQUIRE(l.attenEnd == Approx(9.0f));
}

TEST_CASE("A light rides its bone's world transform", "[m3chan]") {
    m3fix::ModelBuilder mb;
    mb.StaticBone("root", -1, {0, 0, 0});
    mb.StaticBone("arm", 0, {3, 4, 5});
    m3::Model model = mb.Build();

    m3::Light light;
    light.lightType = m3::LightType::Omni;
    light.boneIndex = 1;
    light.diffuseColor = m3fix::ConstRef(Vector3f{1, 1, 1});
    light.intensityMultiplier = m3fix::ConstRef<f32>(1.0f);
    light.attenuationStart = m3fix::ConstRef<f32>(0.0f);
    model.lights.push_back(std::move(light));

    M3ModelAdapter a(std::move(model));
    const auto fs = EvalAt(a, {});
    REQUIRE(fs.lights.size() == 1);
    REQUIRE(fs.lights[0].worldPos.x == Approx(3.0f));
    REQUIRE(fs.lights[0].worldPos.y == Approx(4.0f));
    REQUIRE(fs.lights[0].worldPos.z == Approx(5.0f));
}

TEST_CASE("A light on a hidden bone is disabled, not moved", "[m3chan]") {
    m3::Model model = VisibilityFixture(300, true);
    m3::Light light;
    light.lightType = m3::LightType::Directional;
    light.boneIndex = 0;
    light.diffuseColor = m3fix::ConstRef(Vector3f{1, 1, 1});
    light.intensityMultiplier = m3fix::ConstRef<f32>(1.0f);
    light.attenuationStart = m3fix::ConstRef<f32>(0.0f);
    model.lights.push_back(std::move(light));
    M3ModelAdapter a(std::move(model));

    REQUIRE(EvalAt(a, {Clip(0, 0)}).lights[0].enabled);
    REQUIRE_FALSE(EvalAt(a, {Clip(0, 600)}).lights[0].enabled);
}

TEST_CASE("Spot lights are carried as omni rather than dropped", "[m3chan]") {
    m3fix::ModelBuilder mb;
    mb.StaticBone("root", -1);
    m3::Model model = mb.Build();
    m3::Light light;
    light.lightType = m3::LightType::Spot;
    light.boneIndex = 0;
    light.diffuseColor = m3fix::ConstRef(Vector3f{1, 1, 1});
    light.intensityMultiplier = m3fix::ConstRef<f32>(1.0f);
    light.attenuationStart = m3fix::ConstRef<f32>(0.0f);
    model.lights.push_back(std::move(light));
    M3ModelAdapter a(std::move(model));

    const auto fs = EvalAt(a, {});
    REQUIRE(fs.lights.size() == 1);
    REQUIRE(fs.lights[0].kind == renderer::model::FrameState::LightKind::Omni);
}

TEST_CASE("A model with no lights emits none", "[m3chan]") {
    m3fix::ModelBuilder mb;
    mb.StaticBone("root", -1);
    M3ModelAdapter a(mb.Build());
    REQUIRE(EvalAt(a, {}).lights.empty());
}

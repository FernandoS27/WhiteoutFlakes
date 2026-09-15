// The 3.0.0 HD lighting inputs: the cluster cull radius, the 40-byte light
// record, the single-tile cluster set, and the main light block's rows.
//
// The expectations are the engine's arithmetic (WC3_30_LIGHTING_DESIGN.md §3),
// checked by bytes at shader offsets where a field name could agree with a
// wrong layout.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "bls/bls_frame.h"
#include "ibl/env_probe.h"
#include "profiles/wc3/wc3_lighting_frame.h"
#include "shadow/shadow_service.h"

#include <cmath>
#include <cstring>
#include <vector>

using Catch::Approx;
using whiteout::flakes::f32;
using whiteout::flakes::i32;
using whiteout::flakes::u32;
using whiteout::flakes::usize;
using whiteout::flakes::Matrix44f;
using whiteout::flakes::Vector3f;
using whiteout::flakes::renderer::model::FrameState;
namespace bls = whiteout::flakes::renderer::bls;
namespace wc3 = whiteout::flakes::renderer::profiles::wc3;

namespace {

f32 Falloff(f32 d, f32 q, f32 l, f32 e) {
    return std::exp(-e * d * d) / (1.0f + l * d + q * d * d);
}

// The radius by bisection on the falloff itself, independent of the solver.
f32 BisectRadius(f32 radiance, f32 q, f32 l, f32 e) {
    const f32 target = (1.0f / 255.0f) / radiance;
    double lo = 0.0, hi = 1.0e6;
    for (i32 i = 0; i < 200; ++i) {
        const double mid = 0.5 * (lo + hi);
        if (Falloff(static_cast<f32>(mid), q, l, e) > target)
            lo = mid;
        else
            hi = mid;
    }
    return static_cast<f32>(0.5 * (lo + hi));
}

f32 FloatAt(const void* base, usize offset) {
    f32 v;
    std::memcpy(&v, static_cast<const char*>(base) + offset, sizeof(v));
    return v;
}

i32 IntAt(const void* base, usize offset) {
    i32 v;
    std::memcpy(&v, static_cast<const char*>(base) + offset, sizeof(v));
    return v;
}

FrameState::LightState Omni(Vector3f pos, Vector3f color, f32 intensity) {
    FrameState::LightState L;
    L.kind = FrameState::LightKind::Omni;
    L.worldPos = pos;
    L.diffuse = {color.x * intensity, color.y * intensity, color.z * intensity};
    L.dirIntensity = intensity;
    return L;
}

} // namespace

TEST_CASE("Cluster cull radius: closed form without damping", "[wc3][lighting]") {
    // 1 / (1 + q d^2) = 1/255  =>  d = sqrt(254 / q)
    const f32 r = wc3::ClusterCullRadius(1.0f, 0.0005f, 0.0f, 0.0f);
    CHECK(r == Approx(std::sqrt(254.0f / 0.0005f)).epsilon(1e-4));
    CHECK(Falloff(r, 0.0005f, 0.0f, 0.0f) == Approx(1.0f / 255.0f).epsilon(1e-3));

    // Linear only.
    CHECK(wc3::ClusterCullRadius(2.0f, 0.0f, 0.01f, 0.0f) ==
          Approx((2.0f * 255.0f - 1.0f) / 0.01f).epsilon(1e-4));
}

TEST_CASE("Cluster cull radius: Newton with damping", "[wc3][lighting]") {
    // The game's substitute defaults for a pre-v1600 light, and a stronger
    // damping that dominates the rational term.
    for (f32 e : {0.00001f, 0.0001f}) {
        const f32 r = wc3::ClusterCullRadius(1.5f, 0.0005f, 0.0f, e);
        CHECK(r == Approx(BisectRadius(1.5f, 0.0005f, 0.0f, e)).epsilon(0.01));
    }
}

TEST_CASE("Cluster cull radius: edge cases", "[wc3][lighting]") {
    CHECK(wc3::ClusterCullRadius(0.0f, 0.0005f, 0.0f, 0.0f) == 0.0f);
    // Radiance at or below one 8-bit step never reaches the frame buffer.
    CHECK(wc3::ClusterCullRadius(1.0f / 255.0f, 0.0005f, 0.0f, 0.0f) == 0.0f);
    // No falloff at all.
    CHECK(wc3::ClusterCullRadius(1.0f, 0.0f, 0.0f, 0.0f) == 100000.0f);
}

TEST_CASE("Clustered light record matches the 40-byte layout", "[wc3][lighting]") {
    FrameState::LightState L = Omni({10, 20, 30}, {0.5f, 0.25f, 1.0f}, 2.0f);
    L.quadraticFalloff = 0.001f;
    L.linearFalloff = 0.02f;
    L.damping = 0.0003f;

    Matrix44f view = Matrix44f::identity();
    view.data[3][0] = 1.0f; // translate x by +1
    const bls::HdClusterLight rec = wc3::PackClusterLight(L, view);

    // EnvSet truncates each channel to a byte; no sRGB conversion.
    CHECK(FloatAt(&rec, 0) == Approx(127.0f / 255.0f * 2.0f));
    CHECK(FloatAt(&rec, 4) == Approx(63.0f / 255.0f * 2.0f));
    CHECK(FloatAt(&rec, 8) == Approx(2.0f));
    CHECK(IntAt(&rec, 12) == -1);
    CHECK(FloatAt(&rec, 16) == Approx(11.0f));
    CHECK(FloatAt(&rec, 20) == Approx(20.0f));
    CHECK(FloatAt(&rec, 24) == Approx(30.0f));
    CHECK(FloatAt(&rec, 28) == Approx(0.001f));
    CHECK(FloatAt(&rec, 32) == Approx(0.02f));
    CHECK(FloatAt(&rec, 36) == Approx(0.0003f));
}

TEST_CASE("Single-tile cluster set lists every visible omni light", "[wc3][lighting]") {
    std::vector<FrameState::LightState> lights;
    lights.push_back(Omni({0, 0, 0}, {1, 1, 1}, 1.0f));
    FrameState::LightState dir;
    dir.kind = FrameState::LightKind::Directional;
    dir.diffuse = {1, 1, 1};
    dir.dirIntensity = 1.0f;
    lights.push_back(dir);
    FrameState::LightState off = Omni({0, 0, 0}, {1, 1, 1}, 1.0f);
    off.enabled = false;
    lights.push_back(off);
    lights.push_back(Omni({5, 0, 0}, {1, 0, 0}, 3.0f));
    lights.push_back(Omni({9, 0, 0}, {0, 0, 0}, 1.0f)); // black: culled

    wc3::ClusterSet set;
    wc3::BuildSingleTileClusterSet(lights, Matrix44f::identity(), set);

    REQUIRE(set.tiles.size() == 1);
    CHECK((set.tiles[0] >> bls::kClusterCountBits) == 0u);
    CHECK((set.tiles[0] & bls::kClusterMaxCount) == 2u);
    REQUIRE(set.lights.size() == 2);
    CHECK(set.lights[1].posX == Approx(5.0f));
    REQUIRE(set.indices.size() == 1);
    CHECK((set.indices[0] & 0xFFFFu) == 0u);
    CHECK((set.indices[0] >> 16) == 1u);

    bls::HdPsClusteredCb cb{};
    wc3::FillClusterConstants(set, 1280.0f, 720.0f, cb);
    CHECK(IntAt(&cb, 41 * 16 + 4) == 1); // grid stride
    CHECK(IntAt(&cb, 41 * 16 + 8) == 1); // grid rows
    CHECK(FloatAt(&cb, 40 * 16) == 1.0f);
    CHECK(FloatAt(&cb, 42 * 16 + 8) == 1280.0f);
    CHECK(FloatAt(&cb, 42 * 16 + 12) == 720.0f);

    // An empty scene still yields bindable buffers and a zero count.
    wc3::BuildSingleTileClusterSet({}, Matrix44f::identity(), set);
    CHECK(set.tiles[0] == 0u);
    CHECK(set.lights.size() == 1);
    CHECK(set.indices.size() == 1);
}

TEST_CASE("Cluster grid follows 16-px tiles with the long side capped", "[wc3][lighting]") {
    u32 w = 0, h = 0;
    wc3::ClusterGridDims(1280, 720, w, h);
    CHECK(w == 80u);
    CHECK(h == 45u);
    wc3::ClusterGridDims(512, 512, w, h);
    CHECK(w == 32u);
    CHECK(h == 32u);
    wc3::ClusterGridDims(720, 1280, w, h);
    CHECK(w == 45u);
    CHECK(h == 80u);
    wc3::ClusterGridDims(8000, 1000, w, h);
    CHECK(w == 400u);
    CHECK(h == 50u);
}

TEST_CASE("Binned cluster set lists every light that reaches a pixel", "[wc3][lighting]") {
    // The gate for "images identical to the single tile": a light may only be
    // missing from a tile where no point of that tile is inside its radius.
    constexpr u32 kW = 640, kH = 360;
    const Matrix44f proj = Matrix44f::perspective_diag_sgcompat(1.0f, f32(kW) / f32(kH), 8.0f,
                                                                4000.0f);
    std::vector<FrameState::LightState> lights;
    u32 seed = 12345u;
    auto rnd = [&seed](f32 lo, f32 hi) {
        seed = seed * 1664525u + 1013904223u;
        return lo + (hi - lo) * (static_cast<f32>(seed >> 8) / static_cast<f32>(1u << 24));
    };
    for (i32 i = 0; i < 50; ++i) {
        FrameState::LightState L = Omni({rnd(-600, 600), rnd(-400, 400), rnd(-50, 1500)},
                                        {rnd(0.2f, 1), rnd(0.2f, 1), rnd(0.2f, 1)}, rnd(0.5f, 3));
        L.quadraticFalloff = rnd(0.00005f, 0.002f);
        L.linearFalloff = rnd(0.0f, 0.01f);
        L.damping = (i % 3 == 0) ? rnd(0.000001f, 0.0001f) : 0.0f;
        lights.push_back(L);
    }

    wc3::ClusterSet set;
    wc3::BuildBinnedClusterSet(lights, Matrix44f::identity(), proj, kW, kH, set);
    REQUIRE(set.gridWidth == 40u);
    REQUIRE(set.gridHeight == 23u);
    REQUIRE(set.tiles.size() == 40u * 23u);
    REQUIRE(set.lightCount > 0u);

    auto tileHas = [&](u32 tile, u32 light) {
        const u32 offset = set.tiles[tile] >> bls::kClusterCountBits;
        const u32 count = set.tiles[tile] & bls::kClusterMaxCount;
        for (u32 k = 0; k < count; ++k) {
            const u32 slot = offset + k;
            const u32 packed = set.indices[slot >> 1];
            const u32 idx = (slot & 1u) ? (packed >> 16) : (packed & 0xFFFFu);
            if (idx == light)
                return true;
        }
        return false;
    };

    i32 checked = 0;
    for (i32 s = 0; s < 20000; ++s) {
        const Vector3f p = {rnd(-900, 900), rnd(-600, 600), rnd(10, 2000)};
        const f32 cx = p.x * proj.data[0][0] + p.z * proj.data[2][0] + proj.data[3][0];
        const f32 cy = p.y * proj.data[1][1] + p.z * proj.data[2][1] + proj.data[3][1];
        const f32 cw = p.z * proj.data[2][3] + proj.data[3][3];
        const f32 nx = cx / cw, ny = cy / cw;
        if (nx < -1 || nx >= 1 || ny <= -1 || ny > 1)
            continue;
        // The pixel the point lands in, then hdLookupCluster on its centre.
        const u32 px = static_cast<u32>((nx * 0.5f + 0.5f) * kW);
        const u32 py = static_cast<u32>((0.5f - ny * 0.5f) * kH);
        const u32 tx = std::min(
            static_cast<u32>((static_cast<f32>(px) + 0.5f) / kW * set.gridWidth), set.gridWidth - 1);
        const u32 ty = std::min(
            static_cast<u32>((static_cast<f32>(py) + 0.5f) / kH * set.gridHeight), set.gridHeight - 1);
        const u32 tile = ty * set.gridWidth + tx;
        for (u32 li = 0; li < set.lightCount; ++li) {
            const auto& rec = set.lights[li];
            const f32 radiance = std::sqrt(rec.colorR * rec.colorR + rec.colorG * rec.colorG +
                                           rec.colorB * rec.colorB);
            const f32 radius =
                wc3::ClusterCullRadius(radiance, rec.quadAtten, rec.linAtten, rec.expAtten);
            const f32 dx = p.x - rec.posX, dy = p.y - rec.posY, dz = p.z - rec.posZ;
            if (dx * dx + dy * dy + dz * dz > radius * radius)
                continue;
            ++checked;
            if (!tileHas(tile, li)) {
                FAIL("light " << li << " reaches (" << p.x << ", " << p.y << ", " << p.z
                              << ") but tile " << tile << " does not list it");
            }
        }
    }
    CHECK(checked > 1000);
}

TEST_CASE("Cube shadow faces follow the D3D cube layout", "[wc3][shadow]") {
    // A direction the shader samples picks face and texel by the D3D rules:
    // for each face, the major axis, then sc / tc along these axes (tc grows
    // downward, so it is -NDC y). A face camera that disagrees would store
    // depth where the lookup never reads.
    namespace sh = whiteout::flakes::renderer::shadow;
    const Vector3f eye = {100, -50, 30};
    const Vector3f major[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    const Vector3f sc[6] = {{0, 0, -1}, {0, 0, 1}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {-1, 0, 0}};
    const Vector3f tc[6] = {{0, -1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}, {0, -1, 0}, {0, -1, 0}};
    auto project = [](const Matrix44f& m, const Vector3f& p) {
        const f32 x = p.x * m.data[0][0] + p.y * m.data[1][0] + p.z * m.data[2][0] + m.data[3][0];
        const f32 y = p.x * m.data[0][1] + p.y * m.data[1][1] + p.z * m.data[2][1] + m.data[3][1];
        const f32 z = p.x * m.data[0][2] + p.y * m.data[1][2] + p.z * m.data[2][2] + m.data[3][2];
        const f32 w = p.x * m.data[0][3] + p.y * m.data[1][3] + p.z * m.data[2][3] + m.data[3][3];
        return Vector3f{x / w, y / w, z / w};
    };
    for (i32 f = 0; f < 6; ++f) {
        const Matrix44f vp = sh::CubeFaceViewProj(eye, f, 5.0f, 500.0f);
        const f32 d = 100.0f;
        const Vector3f centre = {eye.x + major[f].x * d, eye.y + major[f].y * d,
                                 eye.z + major[f].z * d};
        const Vector3f c = project(vp, centre);
        CHECK(c.x == Approx(0.0f).margin(1e-4));
        CHECK(c.y == Approx(0.0f).margin(1e-4));
        CHECK(c.z > 0.0f);
        CHECK(c.z < 1.0f);
        // 45 degrees along sc lands on the right edge, along tc on the bottom.
        const Vector3f right = {centre.x + sc[f].x * d, centre.y + sc[f].y * d, centre.z + sc[f].z * d};
        const Vector3f down = {centre.x + tc[f].x * d, centre.y + tc[f].y * d, centre.z + tc[f].z * d};
        CHECK(project(vp, right).x == Approx(1.0f).epsilon(1e-3));
        CHECK(project(vp, down).y == Approx(-1.0f).epsilon(1e-3));
    }
    // Depth runs near 0 -> far 1.
    const Matrix44f vp = sh::CubeFaceViewProj(eye, 0, 5.0f, 500.0f);
    CHECK(project(vp, {eye.x + 5.0f, eye.y, eye.z}).z == Approx(0.0f).margin(1e-4));
    CHECK(project(vp, {eye.x + 500.0f, eye.y, eye.z}).z == Approx(1.0f).epsilon(1e-4));
}

TEST_CASE("Point shadow slots: Key_ShadowCast first, then nearest, faded", "[wc3][shadow]") {
    namespace sh = whiteout::flakes::renderer::shadow;
    sh::ShadowService svc(nullptr);
    svc.SetEnabled(true);

    auto caster = [](Vector3f pos, bool priority) {
        FrameState::LightState L = Omni(pos, {1, 1, 1}, 1.0f);
        L.shadowCasting = true;
        L.shadowCastingStart = 1.0f; // floored to 5
        L.shadowCastingEnd = 300.0f;
        L.shadowPriority = priority;
        return L;
    };
    std::vector<FrameState::LightState> lights = {
        caster({600, 0, 0}, false), caster({50, 0, 0}, false), caster({900, 0, 0}, true),
        caster({100, 0, 0}, false), caster({400, 0, 0}, false), caster({200, 0, 0}, false),
        Omni({10, 0, 0}, {1, 1, 1}, 1.0f), // not a caster
    };
    const Vector3f camera = {0, 0, 0};

    svc.UpdatePointShadows(lights, camera, 0.25f);
    auto slots = svc.PointShadows();
    REQUIRE(slots.size() == 4);
    CHECK(slots[0].position.x == 900.0f); // the priority key, though farthest
    CHECK(slots[1].position.x == 50.0f);
    CHECK(slots[2].position.x == 100.0f);
    CHECK(slots[3].position.x == 200.0f);
    CHECK(slots[0].nearZ == 5.0f);
    CHECK(slots[0].farZ == 300.0f);
    CHECK(slots[0].strength == Approx(0.5f));
    CHECK(svc.PointShadowSlotFor({100, 0, 0}) == 2);
    CHECK(svc.PointShadowSlotFor({400, 0, 0}) == -1);

    svc.UpdatePointShadows(lights, camera, 0.5f);
    CHECK(svc.PointShadows()[0].strength == 1.0f);

    // Drop the light at 50: it fades out over the next half second while the
    // one at 400 fades in, and wanted casters keep the low slots.
    lights.erase(lights.begin() + 1);
    svc.UpdatePointShadows(lights, camera, 0.25f);
    slots = svc.PointShadows();
    REQUIRE(slots.size() == 4);
    CHECK(slots[3].position.x == 400.0f);
    CHECK(slots[3].strength == Approx(0.5f));
    svc.UpdatePointShadows(lights, camera, 0.5f);
    slots = svc.PointShadows();
    REQUIRE(slots.size() == 4);
    for (const auto& s : slots)
        CHECK(s.position.x != 50.0f);

    // The cluster records carry the slot, -2 for a caster without one.
    wc3::ClusterSet set;
    std::vector<Vector3f> positions;
    for (const auto& s : svc.PointShadows())
        positions.push_back(s.position);
    wc3::BuildBinnedClusterSet(lights, Matrix44f::identity(),
                               Matrix44f::perspective_diag_sgcompat(1.0f, 1.0f, 8.0f, 4000.0f), 64,
                               64, set, positions);
    i32 withSlot = 0, withoutSlot = 0, none = 0;
    for (u32 i = 0; i < set.lightCount; ++i) {
        const i32 idx = IntAt(&set.lights[i], 12);
        withSlot += idx >= 0;
        withoutSlot += idx == -2;
        none += idx == -1;
    }
    CHECK(withSlot == 4);
    CHECK(withoutSlot == 1);
    CHECK(none == 1);
}

TEST_CASE("Main light rows: ambient in 28, colour in 29", "[wc3][lighting]") {
    // The shader repo names row 28 `ambientAdd` and row 29 `ambientColor`; the
    // engine writes the ambient into 28 and the light colour into 29.
    bls::FrameInputs in;
    in.mainLight = {.ambient = {0.1f, 0.2f, 0.3f},
                    .shadowIntensity = 0.15f,
                    .color = {1.0f, 0.5f, 0.25f},
                    .dirToLightVS = {0.0f, 0.6f, 0.8f},
                    .enabled = true};
    in.envFromMipCount = 10.0f;
    in.envToMipCount = 9.0f;
    bls::MatParams mat;
    bls::HdPsCb cb{};
    bls::BuildHdPsCb(cb, in, mat);

    CHECK(FloatAt(&cb, 28 * 16) == Approx(0.1f));
    CHECK(FloatAt(&cb, 28 * 16 + 8) == Approx(0.3f));
    CHECK(FloatAt(&cb, 28 * 16 + 12) == Approx(0.15f));
    CHECK(FloatAt(&cb, 29 * 16) == Approx(1.0f));
    CHECK(FloatAt(&cb, 29 * 16 + 4) == Approx(0.5f));
    CHECK(FloatAt(&cb, 30 * 16 + 4) == Approx(0.6f));
    CHECK(IntAt(&cb, 27 * 16 + 4) == 1);          // main light enable
    CHECK(FloatAt(&cb, 25 * 16) == 10.0f);        // IBL mip counts
    CHECK(FloatAt(&cb, 25 * 16 + 4) == 9.0f);
    CHECK(FloatAt(&cb, 22 * 16) == 1.0f);         // output alpha enable
    CHECK(FloatAt(&cb, 27 * 16) == 1.0f);         // normal strength

    // An unlit material never enables it.
    bls::MatParams unlit;
    unlit.disables = bls::kDisableLighting;
    bls::BuildHdPsCb(cb, in, unlit);
    CHECK(IntAt(&cb, 27 * 16 + 4) == 0);
}

TEST_CASE("SD VS bank: fog depth is the distance ahead of an RH camera", "[wc3][fog]") {
    // The SD PS fogs on mul(world, pos).z. Under the right-handed camera the SD
    // passes use, view z is negative ahead, so the bank turns the view space to
    // the engine's left-handed one, lights included.
    auto xform = [](const Matrix44f& m, const Vector3f& p) {
        return Vector3f{p.x * m.data[0][0] + p.y * m.data[1][0] + p.z * m.data[2][0] + m.data[3][0],
                        p.x * m.data[0][1] + p.y * m.data[1][1] + p.z * m.data[2][1] + m.data[3][1],
                        p.x * m.data[0][2] + p.y * m.data[1][2] + p.z * m.data[2][2] + m.data[3][2]};
    };
    auto sub = [](const Vector3f& a, const Vector3f& b) {
        return Vector3f{a.x - b.x, a.y - b.y, a.z - b.z};
    };
    auto dot = [](const Vector3f& a, const Vector3f& b) { return a.x * b.x + a.y * b.y + a.z * b.z; };

    const Vector3f eye = {300, -40, 120};
    const Vector3f target = {0, 0, 40};
    const Vector3f lightWS = {60, 50, 90};
    const Vector3f pointWS = {10, -20, 60};
    const Vector3f normalWS = {0.6f, 0.0f, 0.8f};

    bls::FrameInputs in;
    in.view = Matrix44f::look_at_rh(eye, target, {0, 0, 1});
    in.projection = Matrix44f::perspective_fov_rh(0.8f, 1.5f, 8.0f, 5000.0f);
    in.numLights = 1;
    const Vector3f lightVS = xform(in.view, lightWS);
    in.lights[0].position = {lightVS.x, lightVS.y, lightVS.z, 1.0f};

    bls::SdVsCbA cb{};
    bls::BuildSdVsCbA(cb, in, bls::MatParams{});

    const Vector3f forward = sub(target, eye);
    const f32 ahead = dot(sub(pointWS, eye), forward) / std::sqrt(dot(forward, forward));
    const Vector3f p = xform(cb.world, pointWS);
    CHECK(p.z == Approx(ahead).epsilon(1e-4));
    CHECK(ahead > 0.0f);

    // The per-vertex light sees the same vector it did in world space.
    const Vector3f l = {cb.lights[0].position.x, cb.lights[0].position.y, cb.lights[0].position.z};
    const Vector3f toLight = sub(l, p);
    const Vector3f toLightWS = sub(lightWS, pointWS);
    CHECK(dot(toLight, toLight) == Approx(dot(toLightWS, toLightWS)).epsilon(1e-4));
    Vector3f n = sub(xform(cb.world, normalWS), xform(cb.world, {0, 0, 0}));
    CHECK(dot(n, toLight) == Approx(dot(normalWS, toLightWS)).epsilon(1e-3));
    CHECK(cb.lights[0].position.w == 1.0f);

    // A left-handed projection is already the engine's space: untouched.
    in.projection = Matrix44f::perspective_diag_sgcompat(1.0f, 1.5f, 8.0f, 5000.0f);
    bls::BuildSdVsCbA(cb, in, bls::MatParams{});
    CHECK(cb.world.data[3][2] == in.view.data[3][2]);
    CHECK(cb.lights[0].position.z == lightVS.z);
}

TEST_CASE("IBL mip count is the full chain length", "[wc3][lighting]") {
    namespace ibl = whiteout::flakes::renderer::ibl;
    CHECK(ibl::EngineProbeMipCount(512) == 10.0f);
    CHECK(ibl::EngineProbeMipCount(256) == 9.0f);
    CHECK(ibl::EngineProbeMipCount(16) == 5.0f);
    CHECK(ibl::EngineProbeMipCount(1) == 1.0f);
    CHECK(ibl::EngineProbeMipCount(0) == 0.0f);
}

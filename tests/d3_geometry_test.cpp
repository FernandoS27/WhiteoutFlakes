// ============================================================================
// D3 geometry and skeleton — gates D3-G1 and D3-G2, offline.
//
// Runs against the extracted corpus under `C:/Projects/WhiteoutLib/Corpus/D3`;
// override the root with WDX_TEST_D3_CORPUS, and skip loudly without it.
// Skipped is not passed.
//
// The corpus is 11,347 `.app` totalling 14.4 GB with a p95 of 8.3 MB and a max
// of 37.1 MB, so a full sweep on every run is not something a unit test should
// do. WDX_TEST_D3_LIMIT caps the count (default 300, deterministically the
// first N in sorted order); set it to 0 for the whole set. What was skipped is
// printed, because a bounded sweep that reads like full coverage is worse than
// an honest partial one.
//
// ---------------------------------------------------------------------------
// The two gates
//
// **D3-G1 (geometry)**: every index is below its sub-object's vertex count and
// every nBoneIndex below the bone count. Failures are *counted and named*
// rather than asserted one by one — a sweep over shipped content that reports
// zero failures over eleven thousand files is a sweep that is not reading what
// it thinks it is.
//
// **D3-G2 (bind poses)**: the highest-risk claim in the format. A BoneStructure
// carries five PRSTransforms and three of the APP spec's five labels are wrong:
//
//   tTransform1 == inverse(tTransform0)   model-space bind pose A and its inverse
//   tTransform4 == inverse(tTransform3)   model-space bind pose B and its inverse
//   compose(parent.tTransform0, tTransform2) == tTransform0    t2 is LOCAL
//
// and, critically, **A and B are not the same pose**. The gate reports the
// count of bones where they diverge and the largest delta. A run that reports
// **zero** divergent bones means the sampler is not reading what it thinks it
// is — the expected order is ~8% of bones with deltas to ~20 units. That number
// is the blast radius of skinning with tTransform1 instead of tTransform4:
// 92% of every model would be correct and the rest would detonate, which reads
// as "the exporter is broken" rather than as a wrong field.
// ============================================================================

#include <catch2/catch_test_macros.hpp>

#include "io/d3/d3_model_adapter.h"
#include "io/d3/d3_sno_cache.h"

#include <whiteout/sno/d3/native/d3_native.h>
#include <whiteout/sno/d3/native/geometry.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace d3n = ::whiteout::sno::d3::native;
using namespace ::whiteout;
using ::whiteout::flakes::renderer::model::VertexSemantic;

namespace {

fs::path CorpusRoot() {
    if (const char* v = std::getenv("WDX_TEST_D3_CORPUS"); v && *v)
        return fs::path(v);
    return fs::path("C:/Projects/WhiteoutLib/Corpus/D3");
}

std::size_t SweepLimit() {
    if (const char* v = std::getenv("WDX_TEST_D3_LIMIT"); v && *v)
        return static_cast<std::size_t>(std::strtoul(v, nullptr, 10));
    return 300;
}

std::vector<fs::path> FindFiles(const fs::path& dir, const char* ext) {
    std::vector<fs::path> out;
    std::error_code ec;
    if (!fs::is_directory(dir, ec))
        return out;
    for (fs::directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
        if (ec)
            break;
        if (it->is_regular_file(ec) && it->path().extension() == ext)
            out.push_back(it->path());
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<u8> ReadAll(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f)
        return {};
    f.seekg(0, std::ios::end);
    const auto n = static_cast<std::size_t>(f.tellg());
    f.seekg(0);
    std::vector<u8> b(n);
    f.read(reinterpret_cast<char*>(b.data()), static_cast<std::streamsize>(n));
    return b;
}

// ---- PRSTransform arithmetic, as Skeleton_ComposeWorldPose states it -------
//
//   trans = parentTrans + parentScale * (parentRot * localTrans)
//   rot   = parentRot * localRot
//   scale = parentScale * localScale
//
// Written out here rather than routed through the adapter's matrix helper on
// purpose: the gate has to be able to disagree with the adapter.

struct Prs {
    Vector3f t{0.0f, 0.0f, 0.0f};
    Quaternion r{0.0f, 0.0f, 0.0f, 1.0f};
    f32 s = 1.0f;
};

Prs Of(const d3n::PRSTransform& p) {
    return Prs{p.vTranslation, Quaternion{p.qRotation.x, p.qRotation.y, p.qRotation.z, p.qRotation.w},
               p.flScale};
}

Quaternion Mul(const Quaternion& a, const Quaternion& b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

Vector3f Rotate(const Quaternion& q, const Vector3f& v) {
    const Vector3f u{q.x, q.y, q.z};
    const Vector3f uv{u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x};
    const Vector3f uuv{u.y * uv.z - u.z * uv.y, u.z * uv.x - u.x * uv.z, u.x * uv.y - u.y * uv.x};
    return {v.x + 2.0f * (q.w * uv.x + uuv.x), v.y + 2.0f * (q.w * uv.y + uuv.y),
            v.z + 2.0f * (q.w * uv.z + uuv.z)};
}

Prs Compose(const Prs& parent, const Prs& local) {
    Prs o;
    const Vector3f rt = Rotate(parent.r, local.t);
    o.t = {parent.t.x + parent.s * rt.x, parent.t.y + parent.s * rt.y, parent.t.z + parent.s * rt.z};
    o.r = Mul(parent.r, local.r);
    o.s = parent.s * local.s;
    return o;
}

Prs Inverse(const Prs& a) {
    Prs o;
    o.s = (std::fabs(a.s) > 1e-8f) ? 1.0f / a.s : 0.0f;
    o.r = {-a.r.x, -a.r.y, -a.r.z, a.r.w};
    const Vector3f rt = Rotate(o.r, a.t);
    o.t = {-o.s * rt.x, -o.s * rt.y, -o.s * rt.z};
    return o;
}

// Distance between two transforms as the worst displacement they give a set of
// probe points. Comparing raw quaternion components would call q and -q
// different when they are the same rotation.
f32 Delta(const Prs& a, const Prs& b) {
    const Vector3f probes[4] = {
        {0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}};
    f32 worst = 0.0f;
    for (const Vector3f& p : probes) {
        const Vector3f ra = Rotate(a.r, p);
        const Vector3f rb = Rotate(b.r, p);
        const Vector3f pa{a.t.x + a.s * ra.x, a.t.y + a.s * ra.y, a.t.z + a.s * ra.z};
        const Vector3f pb{b.t.x + b.s * rb.x, b.t.y + b.s * rb.y, b.t.z + b.s * rb.z};
        const f32 d = std::sqrt((pa.x - pb.x) * (pa.x - pb.x) + (pa.y - pb.y) * (pa.y - pb.y) +
                                (pa.z - pb.z) * (pa.z - pb.z));
        worst = (std::max)(worst, d);
    }
    return worst;
}

const Prs kIdentity{};

} // namespace

TEST_CASE("D3 corpus: every appearance parses and its geometry is in range", "[d3][corpus]") {
    const fs::path root = CorpusRoot() / "Appearances";
    const auto files = FindFiles(root, ".app");
    if (files.empty()) {
        WARN("No D3 corpus at " << root.string()
                                << " (set WDX_TEST_D3_CORPUS). SKIPPED, not passed.");
        return;
    }
    const std::size_t limit = SweepLimit();
    const std::size_t take = (limit == 0) ? files.size() : (std::min)(limit, files.size());

    std::size_t parsed = 0, parseFailed = 0;
    std::size_t subObjects = 0, vertices = 0, indices = 0;
    std::size_t badIndex = 0, badSubObjectBone = 0, badInfluenceBone = 0, compressed = 0;
    std::size_t bonelessApps = 0, bonelessSubObjects = 0;
    std::vector<std::string> failedNames;

    for (std::size_t i = 0; i < take; ++i) {
        const auto bytes = ReadAll(files[i]);
        REQUIRE_FALSE(bytes.empty());
        // Every SNO asset opens 0xDEADBEEF, and the corpus is grouped by
        // directory, so a file here that is not an Appearance is a corpus bug.
        REQUIRE(flakes::io::LooksLikeD3(bytes));

        auto app = d3n::parseAppearances(bytes);
        if (!app) {
            ++parseFailed;
            if (failedNames.size() < 20)
                failedNames.push_back(files[i].filename().string());
            continue;
        }
        ++parsed;

        const i32 boneCount = static_cast<i32>(app->arBones.size());
        // A boneless appearance is a real thing — a static prop with no
        // skeleton at all — and its sub-objects' nBoneIndex means nothing.
        // Counted apart from a genuinely out-of-range reference, because
        // lumping the two turns "content has no rig" into "the parser is
        // reading the wrong field".
        if (boneCount == 0)
            ++bonelessApps;
        const d3n::GeoSet* sets[2] = {&app->tGeoSet0, &app->tGeoSet1};
        for (const auto* set : sets) {
            for (const auto& sub : set->arSubObjects) {
                ++subObjects;
                vertices += sub.arVertices.size();
                indices += sub.arIndices.size();
                if ((static_cast<u32>(sub.dwVertexFormat) & d3n::kSubObjectCompressedMask) != 0)
                    ++compressed;
                for (u16 idx : sub.arIndices) {
                    if (static_cast<std::size_t>(idx) >= sub.arVertices.size()) {
                        ++badIndex;
                        break;
                    }
                }
                if (boneCount == 0)
                    ++bonelessSubObjects;
                else if (sub.nBoneIndex >= boneCount)
                    ++badSubObjectBone;
                for (const auto& inf : sub.arVertexInfluences) {
                    const d3n::Influence* three[3] = {&inf.tInfluence0, &inf.tInfluence1,
                                                      &inf.tInfluence2};
                    for (const auto* one : three) {
                        if (one->flWeight > 0.0f && one->nBoneIndex >= boneCount) {
                            ++badInfluenceBone;
                            break;
                        }
                    }
                }
            }
        }
    }

    std::printf("[d3-g1] %zu/%zu files: %zu parsed, %zu failed | %zu sub-objects, %zu vertices, "
                "%zu indices | %zu out-of-range index runs, %zu compressed-format sub-objects\n",
                take, files.size(), parsed, parseFailed, subObjects, vertices, indices, badIndex,
                compressed);
    std::printf("[d3-g1] bone refs: %zu out-of-range nBoneIndex, %zu out-of-range influences | "
                "%zu boneless appearances carrying %zu sub-objects\n",
                badSubObjectBone, badInfluenceBone, bonelessApps, bonelessSubObjects);
    for (const auto& n : failedNames)
        std::printf("[d3-g1]   parse failed: %s\n", n.c_str());
    if (take < files.size())
        std::printf("[d3-g1] %zu files NOT swept (WDX_TEST_D3_LIMIT)\n", files.size() - take);

    // The sweep read something.
    CHECK(parsed > 0);
    CHECK(subObjects > 0);
    CHECK(vertices > 0);
    // And what it read is addressable.
    CHECK(badIndex == 0);
    // A rigged appearance whose sub-object names a bone past the end would mean
    // the bone array and the geoset disagree, which is a parse bug. A boneless
    // one is content and is reported above rather than asserted away — the
    // adapter clamps it to bone 0 and skips the weight stream entirely.
    CHECK(badSubObjectBone == 0);
    CHECK(badInfluenceBone == 0);
    // kSubObjectCompressedMask is post-v260 and never set in shipped content.
    // The adapter refuses such a sub-object with a named warning rather than
    // reading 44-byte records out of a 28-byte stream; if this ever fires, that
    // refusal is what is being exercised.
    CHECK(compressed == 0);
}

TEST_CASE("D3 corpus: the two bind poses, and the fact that they differ", "[d3][corpus]") {
    const fs::path root = CorpusRoot() / "Appearances";
    const auto files = FindFiles(root, ".app");
    if (files.empty()) {
        WARN("No D3 corpus at " << root.string()
                                << " (set WDX_TEST_D3_CORPUS). SKIPPED, not passed.");
        return;
    }
    const std::size_t limit = SweepLimit();
    const std::size_t take = (limit == 0) ? files.size() : (std::min)(limit, files.size());

    std::size_t bones = 0;
    std::size_t badInv0 = 0, badInv3 = 0, badLocal = 0;
    std::size_t divergent = 0;
    f32 worstInv0 = 0.0f, worstInv3 = 0.0f, worstLocal = 0.0f, worstDivergence = 0.0f;
    std::string worstDivergenceFile;

    for (std::size_t i = 0; i < take; ++i) {
        const auto bytes = ReadAll(files[i]);
        auto app = d3n::parseAppearances(bytes);
        if (!app)
            continue;
        const auto& b = app->arBones;
        for (std::size_t n = 0; n < b.size(); ++n) {
            ++bones;
            const Prs t0 = Of(b[n].tTransform0);
            const Prs t1 = Of(b[n].tTransform1);
            const Prs t2 = Of(b[n].tTransform2);
            const Prs t3 = Of(b[n].tTransform3);
            const Prs t4 = Of(b[n].tTransform4);

            // t1 is the exact inverse of t0, t4 of t3 — composing each pair
            // must land on identity.
            const f32 d1 = Delta(Compose(t0, t1), kIdentity);
            const f32 d4 = Delta(Compose(t3, t4), kIdentity);
            worstInv0 = (std::max)(worstInv0, d1);
            worstInv3 = (std::max)(worstInv3, d4);
            if (d1 > 1e-3f)
                ++badInv0;
            if (d4 > 1e-3f)
                ++badInv3;

            // t2 is the LOCAL, parent-relative bind pose: composing it onto the
            // parent's t0 reproduces this bone's t0. A root bone's t2 is its t0.
            const i32 p = b[n].nParentIndex;
            const Prs parent =
                (p >= 0 && static_cast<std::size_t>(p) < b.size()) ? Of(b[p].tTransform0) : kIdentity;
            const f32 dl = Delta(Compose(parent, t2), t0);
            worstLocal = (std::max)(worstLocal, dl);
            if (dl > 1e-3f)
                ++badLocal;

            // The measurement the whole decision rests on.
            const f32 dv = Delta(t0, t3);
            if (dv > 1e-4f) {
                ++divergent;
                if (dv > worstDivergence) {
                    worstDivergence = dv;
                    worstDivergenceFile = files[i].filename().string();
                }
            }
        }
    }

    const double pct = bones ? (100.0 * static_cast<double>(divergent) / static_cast<double>(bones))
                             : 0.0;
    std::printf("[d3-g2] %zu bones over %zu files | inverse(t0)!=t1: %zu (worst %.6f) | "
                "inverse(t3)!=t4: %zu (worst %.6f) | compose(parent.t0, t2)!=t0: %zu "
                "(worst %.6f)\n",
                bones, take, badInv0, static_cast<double>(worstInv0), badInv3,
                static_cast<double>(worstInv3), badLocal, static_cast<double>(worstLocal));
    std::printf("[d3-g2] DIVERGENT BIND POSES: %zu of %zu bones (%.2f%%), max delta %.3f units"
                " (%s)\n",
                divergent, bones, pct, static_cast<double>(worstDivergence),
                worstDivergenceFile.empty() ? "-" : worstDivergenceFile.c_str());

    REQUIRE(bones > 0);
    CHECK(badInv0 == 0);
    CHECK(badInv3 == 0);
    CHECK(badLocal == 0);

    // Reported as a number AND asserted non-zero. Zero divergent bones would
    // mean the sampler is not reading what it thinks it is: the expected order
    // is ~8% of bones with deltas up to ~20 units on a ~7-unit character, and a
    // measurement that says "they are always equal" would silently make
    // tTransform1 and tTransform4 interchangeable — which is exactly the bug
    // this gate exists to keep out.
    CHECK(divergent > 0);
}

TEST_CASE("D3 vertex record: the offsets the shader's input layout assumes", "[d3]") {
    // The record is a *repack*, not a passthrough — only the position is plain
    // float data in the file — so nothing on disk pins these. What does pin
    // them is the PSO, which merges the two UV sets into one float4 element at
    // set 0's offset and would silently scramble texture coordinates if they
    // ever stopped being adjacent.
    const auto attrs = flakes::io::DescribeD3Vertex();
    auto find = [&](VertexSemantic s, u8 idx) -> const flakes::renderer::model::VertexAttribute* {
        for (const auto& a : attrs)
            if (a.semantic == s && a.semanticIndex == idx)
                return &a;
        return nullptr;
    };
    const auto* pos = find(VertexSemantic::Position, 0);
    const auto* nrm = find(VertexSemantic::Normal, 0);
    const auto* uv0 = find(VertexSemantic::TexCoord, 0);
    const auto* uv1 = find(VertexSemantic::TexCoord, 1);
    const auto* tan = find(VertexSemantic::Tangent, 0);
    const auto* col = find(VertexSemantic::Color, 0);
    REQUIRE(pos);
    REQUIRE(nrm);
    REQUIRE(uv0);
    REQUIRE(uv1);
    REQUIRE(tan);
    REQUIRE(col);
    CHECK(pos->offset == 0);
    CHECK(uv1->offset == uv0->offset + 8);
    CHECK(uv0->format == flakes::gfx::Format::R32G32_FLOAT);
    CHECK(uv1->format == flakes::gfx::Format::R32G32_FLOAT);
    CHECK(tan->format == flakes::gfx::Format::R32G32B32A32_FLOAT);
    // Both vertex colours in one element: a second COLOR would need semantic
    // index 1, which this toolchain cannot round-trip (Slang emits COLOR10).
    CHECK(col->format == flakes::gfx::Format::R16G16B16A16_UNORM);
}

TEST_CASE("D3 vertex decoders: the packing the geometry test cannot see", "[d3]") {
    // The colour swizzle happens in the adapter at the point of packing, so it
    // is invisible in a render until P4 and invisible in a geometry sweep
    // forever. What is checkable here is that the decoders themselves agree
    // with the byte order the header documents.
    const d3n::VertexColor c = d3n::unpackVertexColor(0x44332211u);
    CHECK(c.r == 0x11);
    CHECK(c.g == 0x22);
    CHECK(c.b == 0x33);
    CHECK(c.a == 0x44);

    // n = b * 2/255 - 1: 0 -> -1, 255 -> +1, 128 -> ~0.
    const Vector3f n = d3n::unpackVertexVector(0x00FF8000u);
    CHECK(n.x == -1.0f);
    CHECK(std::fabs(n.y - 0.003921569f) < 1e-6f);
    CHECK(n.z == 1.0f);

    // coord = u16 / 512 - 64.
    const Vector2f uv = d3n::unpackTexCoord((32768u << 16) | 32768u);
    CHECK(std::fabs(uv.x - 0.0f) < 1e-4f);
    CHECK(std::fabs(uv.y - 0.0f) < 1e-4f);
}

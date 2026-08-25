// ============================================================================
// Which way do a format's vertex normals point?
//
// M3_NORMAL_ORIENTATION_PLAN.md P0. The renderer had a per-pixel
// `dot(n, toEye) < 0 ? -n : n` in the `.m3` shader, and it was firing on
// 74-87% of every visible model — which means the normals reaching the shader
// disagreed with the surface, and the flip was manufacturing the shading
// normal rather than correcting a rare back-facing pixel.
//
// Two candidate causes with opposite remedies: the normals are stored (or
// decoded) inverted, or our winding convention is inverted. Guessing wrong
// makes backface culling remove the side you can see, so this measures the
// question on the CPU, from the parsed arrays alone — no device, no camera,
// no projection, nothing that could impose a convention of its own.
//
// The answer turned out to be "decoded": the bytes are UNORM (retail's
// `ubyte4n`, `2 * v - 1`), and an SNORM read of them scores 4.8% here while
// being 0.73..1.41 long — per component its dot with the true value is
// v^2 - |v| <= 0, so it reads as "inverted" without being the inverse of
// anything. Decoded UNORM, `.m3` scores the same as `.m2`.
//
// Two independent metrics per mesh:
//
//   1. Winding agreement. For each triangle, `cross(p1-p0, p2-p0)` against the
//      mean of its three vertex normals. This is the relationship the
//      rasterizer's front-face bit keys on, but it is only meaningful
//      *relative* to a known-good format, which is why `.m2` is measured the
//      same way in the same run.
//
//   2. Outwardness. `dot(n, vertex - centroid)`, which needs no winding
//      convention at all: for any roughly star-shaped body — every character
//      mesh in both corpora — outward normals score positive and inward
//      normals score negative. This is the metric that decides the question;
//      metric 1 says whether winding is implicated too.
//
// Device-free G0 test. Skips without the corpora, and skipped is not passed.
// ============================================================================

#include <catch2/catch_test_macros.hpp>

#include "whiteout/flakes/model_types.h"

#if WDX_ENABLE_M2
#include "io/file_content_provider.h"
#include "io/m2/m2_model_adapter.h"
#endif
#if WDX_ENABLE_M3
#include "io/m3/m3_model_adapter.h"
#endif
#include "whiteout/flakes/content_ref.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using whiteout::f32;
using whiteout::f64;
using whiteout::flakes::ContentRef;
using whiteout::flakes::MeshData;
using whiteout::Vector3f;
namespace io = whiteout::flakes::io;
namespace fs = std::filesystem;

namespace {

// What one format's normals do, pooled over every mesh measured.
struct Orientation {
    std::size_t triangles = 0;
    std::size_t windingAgrees = 0; ///< cross(p1-p0, p2-p0) . meanNormal > 0
    std::size_t vertices = 0;
    std::size_t outward = 0; ///< n . (v - centroid) > 0
    std::size_t meshes = 0;

    f64 WindingAgreeFraction() const {
        return triangles ? static_cast<f64>(windingAgrees) / static_cast<f64>(triangles) : 0.0;
    }
    f64 OutwardFraction() const {
        return vertices ? static_cast<f64>(outward) / static_cast<f64>(vertices) : 0.0;
    }
};

Vector3f Sub(const Vector3f& a, const Vector3f& b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
Vector3f Cross(const Vector3f& a, const Vector3f& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
f32 Dot(const Vector3f& a, const Vector3f& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

const whiteout::flakes::VertexAttribute* Find(const whiteout::flakes::MeshBuffer& b,
                                              whiteout::flakes::VertexSemantic s) {
    for (const auto& a : b.attributes)
        if (a.semantic == s && a.semanticIndex == 0)
            return &a;
    return nullptr;
}

// Decode one vec3 attribute out of the interleaved record. Both adapters bake,
// and a baked mesh leaves `MeshData::normals` empty — the normals only exist
// inside the blob, which is also the copy the GPU actually reads, so decoding
// them here measures the bytes the shader sees rather than a CPU-side echo.
Vector3f DecodeVec3(const whiteout::flakes::MeshBuffer& b, std::size_t v,
                    const whiteout::flakes::VertexAttribute& a) {
    const whiteout::u8* rec = b.data.data() + v * b.stride + a.offset;
    switch (a.format) {
    case whiteout::flakes::gfx::Format::R32G32B32_FLOAT:
    case whiteout::flakes::gfx::Format::R32G32B32A32_FLOAT: {
        Vector3f r{};
        std::memcpy(&r, rec, sizeof(r));
        return r;
    }
    case whiteout::flakes::gfx::Format::R8G8B8A8_SNORM:
        // D3D's SNORM decode, clamped the way the hardware does it.
        return {std::max(static_cast<f32>(static_cast<std::int8_t>(rec[0])) / 127.0f, -1.0f),
                std::max(static_cast<f32>(static_cast<std::int8_t>(rec[1])) / 127.0f, -1.0f),
                std::max(static_cast<f32>(static_cast<std::int8_t>(rec[2])) / 127.0f, -1.0f)};
    case whiteout::flakes::gfx::Format::R8G8B8A8_UNORM:
        // The `.m3` basis: UNORM bytes, decoded `2 * v - 1` by the shader.
        return {static_cast<f32>(rec[0]) / 255.0f * 2.0f - 1.0f,
                static_cast<f32>(rec[1]) / 255.0f * 2.0f - 1.0f,
                static_cast<f32>(rec[2]) / 255.0f * 2.0f - 1.0f};
    default:
        return {0.0f, 0.0f, 0.0f};
    }
}

void Measure(const MeshData& mesh, Orientation& o) {
    std::vector<Vector3f> p = mesh.positions;
    std::vector<Vector3f> n = mesh.normals;

    if (n.size() != p.size() || p.empty()) {
        const auto& b = mesh.baked;
        if (!b.Valid() || b.stride == 0)
            return;
        const auto* pa = Find(b, whiteout::flakes::VertexSemantic::Position);
        const auto* na = Find(b, whiteout::flakes::VertexSemantic::Normal);
        if (!pa || !na)
            return;
        const std::size_t count = b.VertexCount();
        p.resize(count);
        n.resize(count);
        for (std::size_t v = 0; v < count; ++v) {
            p[v] = DecodeVec3(b, v, *pa);
            n[v] = DecodeVec3(b, v, *na);
        }
    }
    if (p.empty() || n.size() != p.size())
        return;

    // The centroid stands in for "inside". A vertex mean is biased toward
    // dense regions, which is fine here: the metric only needs a point that is
    // genuinely interior, not the centre of mass.
    Vector3f c{0.0f, 0.0f, 0.0f};
    for (const auto& v : p) {
        c.x += v.x;
        c.y += v.y;
        c.z += v.z;
    }
    const f32 inv = 1.0f / static_cast<f32>(p.size());
    c = {c.x * inv, c.y * inv, c.z * inv};

    for (std::size_t i = 0; i < p.size(); ++i) {
        const Vector3f r = Sub(p[i], c);
        // Skip vertices sitting on the centroid and degenerate normals: both
        // give a meaningless sign rather than a wrong one.
        if (Dot(r, r) < 1e-12f || Dot(n[i], n[i]) < 1e-6f)
            continue;
        ++o.vertices;
        if (Dot(n[i], r) > 0.0f)
            ++o.outward;
    }

    for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        const auto i0 = mesh.indices[i], i1 = mesh.indices[i + 1], i2 = mesh.indices[i + 2];
        if (i0 >= p.size() || i1 >= p.size() || i2 >= p.size())
            continue;
        const Vector3f geo = Cross(Sub(p[i1], p[i0]), Sub(p[i2], p[i0]));
        if (Dot(geo, geo) < 1e-12f)
            continue; // degenerate triangle
        const Vector3f mean{n[i0].x + n[i1].x + n[i2].x, n[i0].y + n[i1].y + n[i2].y,
                            n[i0].z + n[i1].z + n[i2].z};
        if (Dot(mean, mean) < 1e-6f)
            continue;
        ++o.triangles;
        if (Dot(geo, mean) > 0.0f)
            ++o.windingAgrees;
    }
    ++o.meshes;
}

void Report(const char* what, const Orientation& o) {
    UNSCOPED_INFO(what << ": " << o.meshes << " meshes, " << o.triangles << " triangles, winding-agree "
                       << (100.0 * o.WindingAgreeFraction()) << "%, outward "
                       << (100.0 * o.OutwardFraction()) << "%");
}

std::vector<fs::path> FindByExt(const fs::path& root, const char* ext, std::size_t limit) {
    std::vector<fs::path> out;
    std::error_code ec;
    if (!fs::is_directory(root, ec))
        return out;
    for (fs::recursive_directory_iterator it(root, ec), end; it != end; it.increment(ec)) {
        if (ec)
            break;
        if (!it->is_regular_file(ec))
            continue;
        auto e = it->path().extension().string();
        std::transform(e.begin(), e.end(), e.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (e == ext)
            out.push_back(it->path());
    }
    std::sort(out.begin(), out.end());
    // Capped deliberately: a full `.m2` sweep drives the parser into multi-GB
    // resizes, and this measurement converges long before the corpus ends. The
    // callers stop earlier still, once enough meshes have been measured — most
    // of the `.m3` corpus is effect-only files with no geometry at all, so the
    // cap has to be on what was *measured*, not on what was opened.
    if (out.size() > limit)
        out.resize(limit);
    return out;
}

std::vector<whiteout::u8> ReadAll(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f)
        return {};
    return std::vector<whiteout::u8>((std::istreambuf_iterator<char>(f)),
                                     std::istreambuf_iterator<char>());
}

} // namespace

#if WDX_ENABLE_M3
namespace {
fs::path Sc2Corpus() {
    if (const char* v = std::getenv("WDX_TEST_SC2_CORPUS"); v && *v)
        return fs::path(v);
    return fs::path("C:/Projects/WhiteoutLib/Corpus/Sc2M3");
}
} // namespace

namespace {
fs::path HotsCorpus() {
    if (const char* v = std::getenv("WDX_TEST_HOTS_CORPUS"); v && *v)
        return fs::path(v);
    return fs::path("C:/Projects/WhiteoutLib/Corpus/HotSM3");
}

void MeasureM3Corpus(const fs::path& root, const char* what) {
    const auto models = FindByExt(root, ".m3", 400);
    if (models.empty())
        SKIP("no .m3 files under " + root.string());

    Orientation o;
    for (const auto& path : models) {
        if (o.meshes >= 60)
            break;
        const auto bytes = ReadAll(path);
        if (bytes.empty())
            continue;
        auto adapter =
            io::M3ModelAdapter::Load(ContentRef::FromPath(path.string()),
                                     std::span<const whiteout::u8>(bytes));
        if (!adapter)
            continue; // effect-only / physics-only `.m3`
        for (const auto& mesh : adapter->GetMeshes())
            Measure(mesh, o);
    }

    REQUIRE(o.meshes > 0);
    REQUIRE(o.vertices > 0);
    Report(what, o);

    // Decoded UNORM, `.m3` stores the vertex normal PARALLEL to
    // `cross(p1-p0, p2-p0)` — the relationship `.m2` has, and `.m2` renders
    // correctly through the same renderer, rasterizer state and winding. So
    // the shader must not negate. Pinned as a range rather than an exact
    // figure: it is a corpus-wide vote, and degenerate triangles legitimately
    // land on either side.
    CHECK(o.WindingAgreeFraction() > 0.85);
}
} // namespace

TEST_CASE("m3 vertex normals point outward", "[m3][normals]") {
    MeasureM3Corpus(Sc2Corpus(), "m3 (sc2)");
}

// The HotS corpus is MODL v29 throughout where SC2's is mostly v23: one vertex
// format, two generations of exporter, and the models the misread was noticed on.
TEST_CASE("m3 vertex normals point outward (hots)", "[m3][normals]") {
    MeasureM3Corpus(HotsCorpus(), "m3 (hots)");
}

// How much of the corpus is two-sided decides whether retail's TwoSided-gated
// flip could be the thing that corrects the stored orientation (it is not — the
// flag is a minority), which is why the negation below is unconditional rather
// than riding the two-sided path.
TEST_CASE("m3 TwoSided is a minority of materials", "[m3][normals]") {
    const auto models = FindByExt(Sc2Corpus(), ".m3", 400);
    if (models.empty())
        SKIP("no .m3 files under " + Sc2Corpus().string());

    std::size_t materials = 0, twoSided = 0, files = 0;
    for (const auto& path : models) {
        if (files >= 40)
            break;
        const auto bytes = ReadAll(path);
        if (bytes.empty())
            continue;
        auto adapter =
            io::M3ModelAdapter::Load(ContentRef::FromPath(path.string()),
                                     std::span<const whiteout::u8>(bytes));
        if (!adapter || adapter->GetMeshes().empty())
            continue;
        ++files;
        for (const auto& mat : adapter->SourceModel().standardMaterials) {
            ++materials;
            if ((static_cast<whiteout::u32>(mat.flags) &
                 static_cast<whiteout::u32>(::whiteout::m3::MaterialFlag::TwoSided)) != 0)
                ++twoSided;
        }
    }

    REQUIRE(materials > 0);
    const f64 frac = static_cast<f64>(twoSided) / static_cast<f64>(materials);
    UNSCOPED_INFO("m3: " << twoSided << " / " << materials << " materials TwoSided ("
                         << (100.0 * frac) << "%)");
    CHECK(frac < 0.5);
}
#endif // WDX_ENABLE_M3

#if WDX_ENABLE_M2
namespace {
fs::path WowCorpus() {
    if (const char* v = std::getenv("WDX_TEST_WOW_CORPUS"); v && *v)
        return fs::path(v);
    return fs::path("C:/Projects/WhiteoutLib/Corpus/WoW");
}
} // namespace

// The calibration arm. `.m2` renders correctly today, so whatever it scores is
// what "right" looks like in this codebase's conventions — which is the only
// way to read the winding number, since a cross product's sign is meaningless
// in isolation.
TEST_CASE("m2 vertex normals point outward", "[m2][normals]") {
    const auto models = FindByExt(WowCorpus(), ".m2", 40);
    if (models.empty())
        SKIP("no .m2 files under " + WowCorpus().string());

    io::FileContentProvider provider;
    provider.SetGame(whiteout::flakes::ProductId::Wow);

    Orientation o;
    for (const auto& path : models) {
        if (o.meshes >= 60)
            break;
        provider.SetBasePath(path.parent_path());
        auto bytes = provider.ReadFile(path.string());
        if (!bytes.has_value())
            continue;
        auto adapter = io::M2ModelAdapter::Load(
            ContentRef::FromPath(path.string()),
            std::span<const whiteout::u8>(bytes->data(), bytes->size()), &provider);
        if (!adapter)
            continue;
        for (const auto& mesh : adapter->GetMeshes())
            Measure(mesh, o);
    }

    REQUIRE(o.meshes > 0);
    REQUIRE(o.vertices > 0);
    Report("m2", o);

    // The reference reading. Note it is the *winding* metric that is clean
    // (~99.7%) and the outwardness one that is only suggestive (~80%): a
    // character mesh is not convex, so capes, fingers and shoulder plates
    // legitimately score inward against a single centroid. That is exactly why
    // the m3 assertion above is on winding agreement too — the same metric,
    // read against the format that is known to render correctly.
    CHECK(o.WindingAgreeFraction() > 0.85);
}
#endif // WDX_ENABLE_M2

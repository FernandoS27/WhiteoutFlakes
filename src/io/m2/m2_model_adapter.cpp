#include "io/m2/m2_model_adapter.h"

#include "io/m2/m2_animation.h"
#include "renderer/profiles/wow/m2_material.h"
#include "renderer/animation/anim_math.h"
#if WDX_HAS_PHYSICS
#include "renderer/profiles/wow/wow_physics.h"
#endif

#include <whiteout/models/m2/parser.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <type_traits>
#include <unordered_map>

namespace whiteout::flakes::io {

using renderer::model::MeshBuffer;
using renderer::model::MeshData;
using renderer::model::SequenceInfo;
using renderer::model::SkinWeightData;
using renderer::model::SkeletonData;
using renderer::model::VertexAttribute;
using renderer::model::VertexInfluence;
using renderer::model::VertexSemantic;

namespace {

using M2Vertex = ::whiteout::m2::Vertex;

// M2Material::blendingMode → the renderer's FilterMode. The two enums are the
// same five states in a different order; anything unknown blends, which is what
// a ribbon almost always wants.
i32 M2BlendToFilterMode(u16 blendingMode) {
    switch (blendingMode) {
    case 0:
        return ::whiteout::flakes::FILTER_NONE;
    case 1:
        return ::whiteout::flakes::FILTER_TRANSPARENT;
    case 2:
        return ::whiteout::flakes::FILTER_BLEND;
    case 3:
        return ::whiteout::flakes::FILTER_ADDITIVE;
    case 4:
        return ::whiteout::flakes::FILTER_ADD_ALPHA;
    case 5:
        return ::whiteout::flakes::FILTER_MODULATE;
    case 6:
        return ::whiteout::flakes::FILTER_MODULATE_2X;
    default:
        return ::whiteout::flakes::FILTER_BLEND;
    }
}

// `whiteout::m2::Vertex` IS the on-disk record: 48 bytes, no padding, in the
// order the file stores them. That is what makes the M2 path a copy rather
// than a repack — but it is also invisible, so it is asserted. If WhiteoutLib
// ever reorders or pads that struct, every baked buffer silently becomes
// garbage and this is the only place that would catch it.
static_assert(std::is_standard_layout_v<M2Vertex>, "m2::Vertex must be memcpy-able");
static_assert(sizeof(M2Vertex) == 48, "m2::Vertex must match the on-disk record");
static_assert(offsetof(M2Vertex, position) == 0);
static_assert(offsetof(M2Vertex, boneWeights) == 12);
static_assert(offsetof(M2Vertex, boneIndices) == 16);
static_assert(offsetof(M2Vertex, normal) == 20);
static_assert(offsetof(M2Vertex, texCoords) == 32);

// A submesh's first index into `skin.indices`.
//
// `SkinSection::indexStart` is a u16 and a skin profile routinely holds more
// than 65535 indices, so the format carries the missing high word in the
// neighbouring `level` field rather than widening the struct. Reading
// `indexStart` alone silently draws another submesh's triangles: it is in
// bounds, so nothing faults — `humanmale_hd00.skin` has 147966 indices and 74
// of its 113 submeshes past the first wrap. Verified on that file: with the
// high word folded in, the submeshes tile [0, 147966) exactly and without it
// they collide at the 65536 boundary.
//
// `vertexStart` gets no such treatment. It tiles contiguously on its own in
// the same file, and a profile is capped below 65536 vertices for it.
std::size_t M2IndexStart(const ::whiteout::m2::SkinSection& sec) {
    return (static_cast<std::size_t>(sec.level) << 16) | sec.indexStart;
}

// The record above, described for the GPU. Fixed — unlike `.m3`, `.m2` has no
// per-model vertex format flags.
std::vector<VertexAttribute> DescribeM2Vertex() {
    return {
        {VertexSemantic::Position, 0, gfx::Format::R32G32B32_FLOAT, 0},
        {VertexSemantic::BoneWeights, 0, gfx::Format::R8G8B8A8_UNORM, 12},
        {VertexSemantic::BoneIndices, 0, gfx::Format::R8G8B8A8_UINT, 16},
        {VertexSemantic::Normal, 0, gfx::Format::R32G32B32_FLOAT, 20},
        {VertexSemantic::TexCoord, 0, gfx::Format::R32G32_FLOAT, 32},
        {VertexSemantic::TexCoord, 1, gfx::Format::R32G32_FLOAT, 40},
    };
}

// One bone's local transform, in the renderer's row-vector convention:
//
//     p' = ((p - pivot) · S · R) + pivot + t
//
// which is `AnimateMT`'s `M = R; M.Scale(s); M.row3 += pivot + t;
// M.Translate(-pivot)` read back out — C44Matrix::Translate and ::Scale both
// *pre*-multiply, so the sequence composes to `T(-pivot) · S · R · T(pivot+t)`.
// Warcraft III's node transform is the same expression, so both now call one
// kernel: `renderer::animation::ComposePivotSRT`. The copies existed only
// because MDX's version took `mdx::` types and pulled that format's structure
// header in behind it.
Matrix44f M2BoneLocal(const Vector3f& t, const Quaternion& r, const Vector3f& s,
                      const Vector3f& pivot) {
    return renderer::animation::ComposePivotSRT(t, r, s, pivot);
}

// The four billboard bits, as one value. Not a bitmask any more: the client
// dispatches on `flags & 0x78` with a switch that has no `default`, so a bone
// setting two of them billboards not at all. Six corpus bones do (two 0x18,
// four 0x48) and this is why they are inert rather than picking a winner.
enum class M2Billboard { None, Spherical, LockX, LockY, LockZ };

M2Billboard M2BillboardOf(u32 flags) {
    using ::whiteout::m2::BoneFlag;
    switch (flags & (static_cast<u32>(BoneFlag::SphericalBillboard) |
                     static_cast<u32>(BoneFlag::CylindricalBillboardX) |
                     static_cast<u32>(BoneFlag::CylindricalBillboardY) |
                     static_cast<u32>(BoneFlag::CylindricalBillboardZ))) {
    case static_cast<u32>(BoneFlag::SphericalBillboard):
        return M2Billboard::Spherical;
    case static_cast<u32>(BoneFlag::CylindricalBillboardX):
        return M2Billboard::LockX;
    case static_cast<u32>(BoneFlag::CylindricalBillboardY):
        return M2Billboard::LockY;
    case static_cast<u32>(BoneFlag::CylindricalBillboardZ):
        return M2Billboard::LockZ;
    default:
        return M2Billboard::None;
    }
}

Vector3f RowOf(const Matrix44f& m, i32 r) {
    return {m.data[r][0], m.data[r][1], m.data[r][2]};
}

void SetRow(Matrix44f& m, i32 r, const Vector3f& v) {
    m.data[r][0] = v.x;
    m.data[r][1] = v.y;
    m.data[r][2] = v.z;
}

// C3Vector::SafeNormalize: unit length, or left alone when there is no length
// to divide by. The client billboards through it three times per bone and never
// checks the result, so a degenerate row has to survive as-is.
Vector3f SafeNormalize(const Vector3f& v) {
    const f32 lenSq = v.x * v.x + v.y * v.y + v.z * v.z;
    if (lenSq <= 1e-12f)
        return v;
    return v * (1.0f / std::sqrt(lenSq));
}

// The camera's ORIENTATION in model space, and its inverse — the basis a
// billboarded bone is aligned against.
//
// Orthonormalised, not just de-translated. An actor's world transform carries
// the profile's unit scale (40x for a WoW creature) and its axis rebase, and
// neither belongs in a rotation: the bone's row lengths are measured through
// this basis and mapped back through its inverse, so any scale left in it gets
// SQUARED — measured at 1600 on a 40x actor, which is a sprite covering the
// screen. Orthonormal also makes the inverse a transpose, exactly.
//
// Then forced left-handed. The basis the client installs for an unrotated bone
// is rows `(0,0,-1) (1,0,0) (0,1,0)`, whose determinant is -1; a billboard
// cannot mirror a model, so the view basis those constants are written against
// is left-handed too and the two cancel. Ours is `look_at_rh` composed with a
// rebase of unknown handedness, so the sign is normalised here rather than
// assumed — feeding the client's constants a right-handed basis inverts every
// billboarded bone's frame, which flips its skinned normals and blows the
// shading out to white.
bool M2CameraBasis(const Matrix44f& world, const Matrix44f& view, Matrix44f& basis,
                   Matrix44f& inverse) {
    const Matrix44f mv = world * view;
    Vector3f r[3] = {RowOf(mv, 0), RowOf(mv, 1), RowOf(mv, 2)};

    // Gram-Schmidt, which keeps the handedness the input had.
    for (i32 k = 0; k < 3; ++k) {
        for (i32 j = 0; j < k; ++j)
            r[k] = r[k] - r[j] * r[k].dot(r[j]);
        const f32 len = r[k].length();
        if (len <= 1e-8f)
            return false;  // degenerate world transform: nothing to align to
        r[k] = r[k] * (1.0f / len);
    }
    if (r[0].dot(whiteout::cross(r[1], r[2])) > 0.0f) {
        for (i32 k = 0; k < 3; ++k)
            r[k].z = -r[k].z;
    }

    basis = Matrix44f::identity();
    inverse = Matrix44f::identity();
    for (i32 k = 0; k < 3; ++k) {
        SetRow(basis, k, r[k]);
        // Orthonormal, so the inverse is the transpose.
        inverse.data[0][k] = r[k].x;
        inverse.data[1][k] = r[k].y;
        inverse.data[2][k] = r[k].z;
    }
    return true;
}

// Screen-align one bone. `world` is the bone's model-space matrix as the parent
// chain left it, `local` the local transform that produced it, and `viewBasis`
// the model→view 3x3 with `viewBasisInv` its inverse.
//
// The client (`AnimateMT`, the `flags & 0x78` block) does this with no camera
// vector at all, because it composes the whole palette in VIEW space: a root
// bone's parent is `model x view`, so overwriting a bone's basis with one that
// is constant in view space *is* the billboard. Ours is a model-space palette,
// so the same three steps have to be conjugated through `viewBasis` — the axes
// are read in view space, replaced there, and mapped back.
//
// Both halves of the client's fix-up are kept because both are load-bearing:
// the row lengths are restored from the composed matrix (a billboard must not
// also rescale the bone), and the translation is rebuilt so the pivot lands
// where the pre-billboard matrix put it (a billboard rotates in place).
Matrix44f M2BillboardBone(const Matrix44f& world, const Matrix44f& local, M2Billboard mode,
                          const Matrix44f& viewBasis, const Matrix44f& viewBasisInv,
                          const Vector3f& pivot) {
    // The bone's axes as the camera sees them.
    const Matrix44f viewSpace = world * viewBasis;
    const f32 len[3] = {RowOf(viewSpace, 0).length(), RowOf(viewSpace, 1).length(),
                        RowOf(viewSpace, 2).length()};
    const Vector3f anchor = whiteout::transform_point(pivot, world);

    Matrix44f bb = Matrix44f::identity();
    switch (mode) {
    case M2Billboard::Spherical: {
        // Every axis replaced, from the bone's own LOCAL rotation with its
        // columns permuted: row k becomes `(local[k][1], local[k][2],
        // -local[k][0])`. Reinterpreting a local basis as a view-space one is
        // the client's, not a reading of it — and it is what lets a bone that
        // animates its own rotation spin in the screen plane instead of
        // freezing. An unsampled bone's local transform is identity, which
        // constant-folds to the fixed basis the client keeps in a literal.
        for (i32 r = 0; r < 3; ++r)
            SetRow(bb, r,
                   SafeNormalize({local.data[r][1], local.data[r][2], -local.data[r][0]}));
        break;
    }
    case M2Billboard::LockX: {
        // Cylindrical: the named axis survives, the other two swing to face the
        // camera. `(y, -x, 0)` is the locked axis turned a quarter turn inside
        // the screen plane, which is what puts the third axis nearest the eye.
        const Vector3f x = SafeNormalize(RowOf(viewSpace, 0));
        const Vector3f y = SafeNormalize({x.y, -x.x, 0.0f});
        SetRow(bb, 0, x);
        SetRow(bb, 1, y);
        SetRow(bb, 2, whiteout::cross(y, x));
        break;
    }
    case M2Billboard::LockY: {
        const Vector3f y = SafeNormalize(RowOf(viewSpace, 1));
        const Vector3f x = SafeNormalize({-y.y, y.x, 0.0f});
        SetRow(bb, 0, x);
        SetRow(bb, 1, y);
        SetRow(bb, 2, whiteout::cross(y, x));
        break;
    }
    case M2Billboard::LockZ: {
        const Vector3f z = SafeNormalize(RowOf(viewSpace, 2));
        const Vector3f y = SafeNormalize({z.y, -z.x, 0.0f});
        SetRow(bb, 0, whiteout::cross(z, y));
        SetRow(bb, 1, y);
        SetRow(bb, 2, z);
        break;
    }
    case M2Billboard::None:
        return world;
    }

    for (i32 r = 0; r < 3; ++r)
        SetRow(bb, r, RowOf(bb, r) * len[r]);

    Matrix44f out = bb * viewBasisInv;
    const Vector3f rebased = whiteout::transform_normal(pivot, out);
    out.data[3][0] = anchor.x - rebased.x;
    out.data[3][1] = anchor.y - rebased.y;
    out.data[3][2] = anchor.z - rebased.z;
    return out;
}

// The parent matrix bone `i` composes against, after its three "ignore parent"
// flags have had their say.
//
// The client builds this in camera space against the model's own world
// transform (`AnimateMT`'s `v379` block). Here every bone matrix is model-space
// and the model transform is applied by the vertex shader, so that world
// transform degenerates to identity: its basis rows are unit length, which
// collapses the client's rescale-to-parent-magnitude step to a plain
// normalisation, and its translation row is the origin.
Matrix44f M2ParentFor(const Matrix44f& parent, u32 flags, const Vector3f& pivot) {
    using ::whiteout::m2::BoneFlag;
    constexpr u32 kIgnoreMask = static_cast<u32>(BoneFlag::IgnoreParentTranslate) |
                                static_cast<u32>(BoneFlag::IgnoreParentScale) |
                                static_cast<u32>(BoneFlag::IgnoreParentRotation);
    if ((flags & kIgnoreMask) == 0)
        return parent;

    Matrix44f m = parent;
    const u32 basis = flags & (static_cast<u32>(BoneFlag::IgnoreParentScale) |
                               static_cast<u32>(BoneFlag::IgnoreParentRotation));
    if (basis == static_cast<u32>(BoneFlag::IgnoreParentScale)) {
        for (i32 row = 0; row < 3; ++row) {
            Vector3f v{m.data[row][0], m.data[row][1], m.data[row][2]};
            const f32 len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
            if (len > 1e-6f)
                v *= 1.0f / len;
            m.data[row][0] = v.x;
            m.data[row][1] = v.y;
            m.data[row][2] = v.z;
        }
    } else if (basis == static_cast<u32>(BoneFlag::IgnoreParentRotation)) {
        // Rotation only: the model's basis direction, at the magnitude the
        // parent had. The client writes `MV.row_k * (|parent.row_k| /
        // |MV.row_k|)`, so dropping the rotation must not also drop the scale.
        for (i32 row = 0; row < 3; ++row) {
            const Vector3f v{m.data[row][0], m.data[row][1], m.data[row][2]};
            const f32 len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
            for (i32 col = 0; col < 3; ++col)
                m.data[row][col] = (row == col) ? len : 0.0f;
        }
    } else if (basis != 0) {
        // Both ignored: the model's own basis verbatim, i.e. identity.
        for (i32 row = 0; row < 3; ++row)
            for (i32 col = 0; col < 3; ++col)
                m.data[row][col] = (row == col) ? 1.0f : 0.0f;
    }

    if (flags & static_cast<u32>(BoneFlag::IgnoreParentTranslate)) {
        m.data[3][0] = m.data[3][1] = m.data[3][2] = 0.0f;
    } else {
        // Keep the pivot where the unmodified parent put it, so replacing the
        // basis rotates the bone in place instead of flinging it.
        const Vector3f anchored = whiteout::transform_point(pivot, parent);
        const Vector3f rebased = whiteout::transform_normal(pivot, m);
        m.data[3][0] = anchored.x - rebased.x;
        m.data[3][1] = anchored.y - rebased.y;
        m.data[3][2] = anchored.z - rebased.z;
    }
    return m;
}

} // namespace

std::vector<::whiteout::u8> ContentProviderCascFs::readFile(::whiteout::u32 fileId) const {
    if (!provider_)
        return {};
    auto bytes = provider_->ReadFile(ContentRef::FromFileId(fileId));
    if (!bytes)
        return {};
    return std::move(*bytes);
}

bool ContentProviderCascFs::fileExists(::whiteout::u32 fileId) const {
    // No cheaper probe than a read: IContentProvider has no existence query,
    // and adding one for this would push a CASC-shaped concept into an
    // interface three unrelated hosts implement. The parser calls this rarely.
    return !readFile(fileId).empty();
}

std::vector<::whiteout::u8> ContentProviderPathFs::readFile(const std::string& path) const {
    if (!provider_)
        return {};
    auto bytes = provider_->ReadFile(path);
    if (!bytes)
        return {};
    return std::move(*bytes);
}

bool ContentProviderPathFs::fileExists(const std::string& path) const {
    return !readFile(path).empty();
}

std::shared_ptr<M2ModelAdapter> M2ModelAdapter::Load(const ContentRef& ref,
                                                     std::span<const ::whiteout::u8> bytes,
                                                     IContentProvider* provider,
                                                     bool lazyAnimations) {
    if (bytes.empty())
        return nullptr;
    ::whiteout::m2::Parser parser;
    parser.setLazyAnimations(lazyAnimations);
    ::whiteout::m2::Model model;
    // Heap-allocated rather than a local, because a lazy parse reads `.anim`
    // siblings through this wrapper long after Load returns.
    std::shared_ptr<void> fsKeepAlive;
    try {
        // The ref's discriminant picks the route, because it is the same
        // question: a model named by id has id-named siblings, a model named
        // by path has its siblings on disk beside it.
        if (ref.IsFileId()) {
            auto fs = std::make_shared<ContentProviderCascFs>(provider);
            model = parser.parse(*fs, bytes);
            fsKeepAlive = std::move(fs);
        } else {
            auto fs = std::make_shared<ContentProviderPathFs>(provider);
            model = parser.parse(*fs, ref.path);
            fsKeepAlive = std::move(fs);
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[m2] parse failed for '%s': %s\n", ref.Describe().c_str(), e.what());
        return nullptr;
    }
    if (parser.hasIssues()) {
        // Issues are not necessarily fatal — the parser reports what it
        // skipped. Surface them rather than letting a half-read model look
        // like a clean one.
        for (const auto& issue : parser.getIssues())
            std::fprintf(stderr, "[m2] %s\n", issue.c_str());
    }
    if (model.skinProfiles.empty() && !ref.IsFileId() && provider) {
        // A path-addressed model is not necessarily a model with path-addressed
        // siblings. Anything Legion or later names its skins by fileDataID in
        // SFID, so a loose `.m2` sitting on disk beside no `.skin` at all is
        // normal — those skins live in the install's CASC and are reachable by
        // id, which is the route this retry takes. Pre-Legion models never get
        // here: their positional `<name>NN.skin` siblings resolved above.
        ::whiteout::m2::Parser byId;
        byId.setLazyAnimations(lazyAnimations);
        try {
            auto fs = std::make_shared<ContentProviderCascFs>(provider);
            ::whiteout::m2::Model retry = byId.parse(*fs, bytes);
            if (!retry.skinProfiles.empty()) {
                model = std::move(retry);
                fsKeepAlive = std::move(fs);
            }
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[m2] id-route retry failed for '%s': %s\n",
                         ref.Describe().c_str(), e.what());
        }
    }
    if (model.skinProfiles.empty()) {
        // Both routes are exhausted: the skins are named by id and no storage
        // could resolve them. A configuration problem, not a malformed model.
        std::fprintf(stderr, "[m2] no skin profile resolved for '%s'; is a WoW CASC configured?\n",
                     model.modelName.c_str());
        return nullptr;
    }
    if (!lazyAnimations)
        fsKeepAlive.reset();
    return std::make_shared<M2ModelAdapter>(std::move(model), std::move(fsKeepAlive));
}

M2ModelAdapter::M2ModelAdapter(::whiteout::m2::Model model, std::shared_ptr<void> fsKeepAlive)
    : model_(std::move(model)), fsKeepAlive_(std::move(fsKeepAlive)) {
    // Profile 0 is the highest detail level. LOD selection is a later phase;
    // taking one and saying so beats taking whichever happens to be first
    // without noticing there were others.
    profileIndex_ = 0;
    if (profileIndex_ < model_.skinProfiles.size())
        submeshCount_ = model_.skinProfiles[profileIndex_].submeshes.size();
    globalLoops_.reserve(model_.globalLoops.size());
    for (const auto& g : model_.globalLoops)
        globalLoops_.push_back(g.timestamp);
}

std::vector<MeshData> M2ModelAdapter::GetMeshes() {
    std::vector<MeshData> out;
    if (profileIndex_ >= model_.skinProfiles.size())
        return out;
    const auto& skin = model_.skinProfiles[profileIndex_];
    out.reserve(skin.submeshes.size());

    // Two levels of indirection, which is the whole trick of the format:
    //   skin.indices[i]            → an index into skin.vertices
    //   skin.vertices[thatIndex]   → an index into model_.vertices
    // A submesh names a contiguous run of both. We flatten each submesh into
    // its own mesh with indices rebased to zero, because MeshData is one
    // vertex array per mesh and the renderer's geoset upload assumes that.
    emittedSections_.clear();
    emittedSections_.reserve(skin.submeshes.size());
    for (std::size_t s = 0; s < skin.submeshes.size(); ++s) {
        const auto& sec = skin.submeshes[s];
        if (sec.vertexCount == 0 || sec.indexCount == 0)
            continue;

        const std::size_t vBegin = sec.vertexStart;
        const std::size_t vEnd = vBegin + sec.vertexCount;
        const std::size_t iBegin = M2IndexStart(sec);
        const std::size_t iEnd = iBegin + sec.indexCount;
        if (vEnd > skin.vertices.size() || iEnd > skin.indices.size())
            continue; // truncated skin; skip rather than read out of bounds

        MeshData mesh;
        mesh.geosetId = static_cast<i32>(s);
        mesh.materialId = -1; // no materials in this phase; UnlitShading draws it
        mesh.lod = 0;
        // A gather, not a slice: the skin indirection means a submesh's
        // vertices are scattered through the global array. Still verbatim —
        // what moves is whole 48-byte records, never a decoded attribute.
        mesh.positions.reserve(sec.vertexCount);
        mesh.baked.stride = sizeof(M2Vertex);
        mesh.baked.attributes = DescribeM2Vertex();
        mesh.baked.data.resize(sec.vertexCount * sizeof(M2Vertex));
        u8* dst = mesh.baked.data.data();
        for (std::size_t v = vBegin; v < vEnd; ++v, dst += sizeof(M2Vertex)) {
            const std::size_t gv = skin.vertices[v];
            if (gv >= model_.vertices.size()) {
                // Out-of-range index: a zeroed record, matching the zeroed
                // position the CPU copy gets. Degenerate, but in-bounds.
                std::memset(dst, 0, sizeof(M2Vertex));
                mesh.positions.push_back({0.0f, 0.0f, 0.0f});
                continue;
            }
            const M2Vertex& src = model_.vertices[gv];
            std::memcpy(dst, &src, sizeof(M2Vertex));
            mesh.positions.push_back(src.position);
        }

        mesh.indices.reserve(sec.indexCount);
        for (std::size_t i = iBegin; i < iEnd; ++i) {
            const std::size_t local = skin.indices[i];
            // skin.indices is profile-global; rebase into this submesh.
            mesh.indices.push_back(static_cast<u32>(local - vBegin));
        }
        emittedSections_.push_back(sec.skinSectionId);
        out.push_back(std::move(mesh));
    }
    RebuildGeosetVisibility();
    return out;
}

void M2ModelAdapter::SetVisibleGeosets(std::vector<u16> skinSectionIds) {
    visibleSections_ = std::move(skinSectionIds);
    std::sort(visibleSections_.begin(), visibleSections_.end());
    visibleSections_.erase(std::unique(visibleSections_.begin(), visibleSections_.end()),
                           visibleSections_.end());
    hasVisibleSet_ = true;
    RebuildGeosetVisibility();
}

void M2ModelAdapter::RebuildGeosetVisibility() {
    geosetHidden_.clear();
    // No selection is the common case — a creature, a prop, a doodad. Leaving
    // the vector empty is what keeps Evaluate from writing geosetHidden at all,
    // so nothing pays for a feature only character models use.
    if (!hasVisibleSet_ || emittedSections_.empty())
        return;
    geosetHidden_.reserve(emittedSections_.size());
    for (const u16 section : emittedSections_) {
        const bool on = std::binary_search(visibleSections_.begin(), visibleSections_.end(),
                                           section);
        // A flag rather than a skip: the geoset stays uploaded, so a host can
        // switch hairstyle without re-spawning the actor. And a flag rather than
        // alpha zero, because `M2ClassifySurface` exempts additive batches from
        // the alpha cull on purpose — an eye glow hidden that way kept drawing
        // over the face.
        geosetHidden_.push_back(on ? u8{0} : u8{1});
    }
}

std::vector<TextureData> M2ModelAdapter::GetTextures() {
    std::vector<TextureData> out;
    out.reserve(model_.textures.size());
    for (usize i = 0; i < model_.textures.size(); ++i) {
        const auto& tex = model_.textures[i];
        TextureData td;
        td.textureId = static_cast<i32>(i);
        // Zero, not the M2 texture *type*: replaceableId is WC3's
        // team-colour/glow slot space, nothing maps the two, and AddModel reads
        // *any* non-zero value as "hand this slot to the replaceable manager",
        // which then owns the binding. M2's own customisation slots are carried
        // by the empty sharedKey below instead.
        td.replaceableId = 0;
        // Bit 0 wrap-U, bit 1 wrap-V — the same encoding StagedTexture uses.
        td.wrapFlags = tex.flags & 0x3u;

        // A composited slot outranks everything below it. Nothing names these
        // pixels — the game built them this load — so they ride the staging
        // path, which uploads a texture with no `sharedKey` from its own bytes.
        // First, so a character body whose type also has a TXID entry (an
        // authoring leftover on some models) takes the composite rather than
        // whatever that entry points at.
        const auto composite =
            std::find_if(composed_.begin(), composed_.end(),
                         [&](const M2ComposedTexture& c) { return c.textureType == tex.type; });
        if (composite != composed_.end() && !composite->rgba.empty()) {
            td.width = static_cast<i32>(composite->width);
            td.height = static_cast<i32>(composite->height);
            td.mipLevels = 1;
            td.format = gfx::Format::R8G8B8A8_UNORM;
            td.pixels = composite->rgba;
            out.push_back(std::move(td));
            continue;
        }

        if (!tex.filename.empty()) {
            td.sharedKey = tex.filename;
        } else if (i < model_.texture_ids.size() && model_.texture_ids[i] != 0) {
            // Chunked models name their textures by fileDataID in TXID and
            // leave `filename` a lone NUL. `#<id>` is ContentRef::Describe's
            // own spelling, which UploadStagedTextures reverses.
            td.sharedKey = "#" + std::to_string(model_.texture_ids[i]);
        } else if (tex.type < replaceableByType_.size() && !replaceableByType_[tex.type].empty()) {
            // A replaceable slot the game filled in. TXID carries a 0 for these
            // — the file that belongs here is a property of what the model was
            // spawned as, not of the model — so this branch is reached only
            // once the WoW profile has said what it is.
            td.sharedKey = replaceableByType_[tex.type];
        }
        // Anything left with an empty key is a slot nothing resolved (character
        // customisation, or a creature opened without the client databases in
        // reach) or a genuinely nameless texture; both bind the white default.
        out.push_back(std::move(td));
    }
    return out;
}

::whiteout::flakes::ModelBounds M2ModelAdapter::GetBounds() {
    ::whiteout::flakes::ModelBounds b;
    const auto& e = model_.bounding;
    const bool degenerate = e.maximum.x <= e.minimum.x && e.maximum.y <= e.minimum.y &&
                            e.maximum.z <= e.minimum.z;
    if (!degenerate) {
        b.min = {e.minimum.x, e.minimum.y, e.minimum.z};
        b.max = {e.maximum.x, e.maximum.y, e.maximum.z};
        b.valid = true;
        return b;
    }
    // Fall through to the interface's union-over-positions default when the
    // model carries no usable box of its own.
    return IModelSource::GetBounds();
}

SkeletonData M2ModelAdapter::GetSkeleton() {
    SkeletonData sk;
    // Every `.m2` has at least one bone — the client asserts `data->bones.count
    // > 0` before it animates anything. Synthesising one for a model that
    // somehow has none keeps the invariant total, so the draw path never has to
    // handle a skinned geoset with no palette behind it.
    const usize boneCount = std::max<usize>(model_.bones.size(), 1);
    sk.nodeCount = static_cast<i32>(boneCount);
    sk.inverseBindMatrices.assign(boneCount, Matrix44f::identity());
    sk.nodePivots.assign(boneCount, Vector3f{0.0f, 0.0f, 0.0f});
    sk.nodeParents.assign(boneCount, -1);
    sk.billboardFlags.assign(boneCount, BONE_BILLBOARD_NONE);

    using ::whiteout::m2::BoneFlag;
    for (usize i = 0; i < model_.bones.size(); ++i) {
        const auto& b = model_.bones[i];
        sk.nodePivots[i] = b.pivot;
        sk.nodeParents[i] =
            (b.parentBoneId >= 0 && static_cast<usize>(b.parentBoneId) < model_.bones.size())
                ? static_cast<i32>(b.parentBoneId)
                : -1;
        // Routed through the same decode `Evaluate` billboards with, so what
        // this publishes is what the bone actually does — a bone setting two
        // bits reads as NONE here because that is how it renders.
        switch (M2BillboardOf(b.flags)) {
        case M2Billboard::Spherical: sk.billboardFlags[i] = BONE_BILLBOARD_FULL; break;
        case M2Billboard::LockX: sk.billboardFlags[i] = BONE_BILLBOARD_LOCK_X; break;
        case M2Billboard::LockY: sk.billboardFlags[i] = BONE_BILLBOARD_LOCK_Y; break;
        case M2Billboard::LockZ: sk.billboardFlags[i] = BONE_BILLBOARD_LOCK_Z; break;
        case M2Billboard::None: break;
        }
    }
    return sk;
}

std::vector<SkinWeightData> M2ModelAdapter::GetSkinWeights() {
    std::vector<SkinWeightData> out;
    if (profileIndex_ >= model_.skinProfiles.size())
        return out;
    const auto& skin = model_.skinProfiles[profileIndex_];
    const i32 boneCount = static_cast<i32>(model_.bones.size());
    out.reserve(skin.submeshes.size());

    for (usize s = 0; s < skin.submeshes.size(); ++s) {
        const auto& sec = skin.submeshes[s];
        // Same skips GetMeshes takes, so geoset ids line up one for one.
        if (sec.vertexCount == 0 || sec.indexCount == 0)
            continue;
        const usize vBegin = sec.vertexStart;
        const usize vEnd = vBegin + sec.vertexCount;
        if (vEnd > skin.vertices.size())
            continue;

        SkinWeightData sw;
        sw.geosetId = static_cast<i32>(s);
        sw.influences.resize(sec.vertexCount);

        // Bone 0 always occupies slot 0, so a submesh whose vertices are all
        // unweighted still has a palette to point at.
        std::unordered_map<i32, i32> globalToLocal;
        sw.subsetNodeIndices.push_back(0);
        globalToLocal.emplace(0, 0);

        for (usize v = vBegin; v < vEnd; ++v) {
            VertexInfluence& inf = sw.influences[v - vBegin];
            const usize gv = skin.vertices[v];
            if (gv >= model_.vertices.size())
                continue; // zeroed record, matching GetMeshes
            const M2Vertex& src = model_.vertices[gv];
            for (i32 k = 0; k < 4; ++k) {
                const f32 w = static_cast<f32>(src.boneWeights[k]) * (1.0f / 255.0f);
                inf.weight[k] = w;
                if (w <= 0.0f)
                    continue;
                // The `.m2` vertex names its bones globally — what
                // CM2Model::TransformVerticesNoUVSelect_cpp indexes the bone
                // matrix array with directly. (The `.skin`'s own `bones` array
                // is the *section-local* twin the client's shader path
                // substitutes, resolved through boneCombos; either reaches the
                // same bone, and the global one needs no second table.)
                i32 g = static_cast<i32>(src.boneIndices[k]);
                if (g < 0 || g >= boneCount)
                    g = 0;
                auto [it, inserted] = globalToLocal.emplace(g, 0);
                if (inserted) {
                    it->second = static_cast<i32>(sw.subsetNodeIndices.size());
                    sw.subsetNodeIndices.push_back(g);
                }
                inf.boneIdx[k] = it->second;
            }
        }
        out.push_back(std::move(sw));
    }
    return out;
}

std::vector<u32> M2ModelAdapter::GetGlobalSequences() {
    std::vector<u32> out;
    out.reserve(model_.globalLoops.size());
    for (const auto& g : model_.globalLoops)
        out.push_back(g.timestamp);
    return out;
}

std::vector<SequenceInfo> M2ModelAdapter::GetSequences() const {
    std::vector<SequenceInfo> out;
    out.reserve(model_.sequences.size());
    for (const auto& seq : model_.sequences) {
        SequenceInfo s;
        const std::string_view name = M2AnimationName(seq.id);
        s.name = name.empty() ? ("Anim" + std::to_string(seq.id)) : std::string(name);
        // Variations share an id and are distinguished only by their index, so
        // the number has to be in the name or a host's list reads as duplicates.
        if (seq.variationIndex != 0)
            s.name += " (" + std::to_string(seq.variationIndex) + ")";
        // Each `.m2` sequence is its own timeline from zero — unlike MDX, where
        // every sequence is a window into one global one.
        s.startMs = 0;
        s.endMs = static_cast<i32>(seq.duration);
        s.moveSpeed = seq.movespeed;
        // `rarity` stays 0. M2's `frequency` is a selection *probability* and
        // MDX's rarity runs the other way, so feeding one to the other would
        // make a host's random pick prefer exactly the wrong variations.
        //
        // Bit 0, not WhiteoutLib's `SequenceFlag::Looping` (0x20): 0x20 is part
        // of the `flags & 0x130` mask that says where a sequence's *keys* live
        // and is set on Stand and Attack1H alike. What decides looping is bit 0
        // — `CM2Model::AnimateMTSimple` clamps the clock to the sequence's
        // window when it is set and wraps `now % duration` when it is not.
        s.nonLooping = (static_cast<u32>(seq.flags) & 0x1u) != 0;
        out.push_back(std::move(s));
    }
    if (out.empty()) {
        // A model with no sequence table still needs one entry: the actor's
        // clock advances against it, and global-sequence tracks run regardless.
        SequenceInfo s;
        s.name = "Stand";
        s.endMs = 1000;
        out.push_back(std::move(s));
    }
    return out;
}

renderer::model::FrameState M2ModelAdapter::Evaluate(const PoseRequest& req) const {
    renderer::model::FrameState fs;
    const ClipRef clip = req.PrimaryClip();

    // A lazily parsed model has not read this sequence's `.anim` yet. Asking
    // first keeps a model whose siblings are missing from re-reading them every
    // frame — sequenceKeysPending is false once a load has been tried.
    if (clip.sequence >= 0 &&
        ::whiteout::m2::sequenceKeysPending(model_, static_cast<u32>(clip.sequence))) {
        ::whiteout::m2::loadSequence(model_, static_cast<u32>(clip.sequence));
    }

    M2AnimTime at;
    at.sequence = clip.sequence;
    at.timeMs = clip.timeMs;
    at.globalTimeMs = (req.globalTimeMs >= 0) ? req.globalTimeMs : clip.timeMs;
    at.globalLoops = std::span<const u32>(globalLoops_);
    // `sequence < 0` is the bind pose: no clip, so every track answers with its
    // animref default and every local transform is identity. That reproduces the
    // stored vertex positions exactly, because an `.m2` stores them posed.
    const bool bindPose = clip.sequence < 0;

    EvaluateBones(at, bindPose, req, fs);
    EvaluateTextureTransforms(at, bindPose, fs);
    EvaluateSurfaces(at, bindPose, fs);
    // Positional, and it has to be: RenderModel::ApplyGeosetStates pairs
    // `geosetHidden[i]` with `gpuGeosets[i]`, and that vector is drained from a
    // map keyed by the ids GetMeshes handed out — which ascend in emission
    // order. So the i-th entry here is the i-th mesh GetMeshes emitted, not the
    // i-th submesh of the profile; the two differ whenever a submesh is skipped.
    if (!geosetHidden_.empty())
        fs.geosetHidden = geosetHidden_;
    // Lights are evaluated at the bind pose too, unlike surfaces: nothing about
    // an `.m2` stores them pre-posed, so there is no constant to fall back to.
    EvaluateLights(at, req.world, fs);
    EvaluateRibbons(at, req.world, fs);
    EvaluateParticles(at, req.world, fs);
    return fs;
}

void M2ModelAdapter::EvaluateBones(const M2AnimTime& at, bool bindPose, const PoseRequest& req,
                                   renderer::model::FrameState& fs) const {
    using ::whiteout::m2::BoneFlag;
    const usize boneCount = std::max<usize>(model_.bones.size(), 1);
    fs.boneWorldMatrices.assign(boneCount, Matrix44f::identity());

    // `externallyDriven` + a `replace` override is the host saying "this bone's
    // model-space matrix is mine, do not sample it and do not compose the parent
    // chain into it". That is what a collections model needs: its thirteen-bone
    // rig is posed bone-for-bone from the character it rides, by key bone, and
    // the matrices arriving are already in the character's model space.
    //
    // A driven bone still acts as a parent for the bones below it, so an
    // undriven one in the middle of the chain (there is one) composes onto a
    // driven ancestor exactly as it would onto a sampled one.
    std::vector<const ::whiteout::flakes::NodeOverride*> driven;
    if (!req.overrides.empty()) {
        driven.assign(boneCount, nullptr);
        for (const auto& o : req.overrides) {
            if (o.replace && o.node >= 0 && static_cast<usize>(o.node) < boneCount)
                driven[static_cast<usize>(o.node)] = &o;
        }
    }

    // Flag coverage, for the ones nothing below acts on. `0x080` enables the
    // sampled path alongside `Transformed` and multiplies in a host matrix, and
    // `0x100` pairs with it — neither is set on a single bone in the 530283-bone
    // corpus, so there is nothing to drive. `0x800` (608 bones), `0x1000`
    // HelmetAnimScaled (143) and everything above `0x10000` (16548) are not read
    // by `AnimateMT` at all: 6.0.1 is Warlords and the corpus is Legion+, so
    // these are content the reference binary predates rather than behaviour it
    // declines to implement. Inert until a binary that reads them says what for.

    // Built once per pose; false means the caller's world transform is
    // degenerate, and a bone with no camera basis to align to keeps the one the
    // parent chain gave it.
    Matrix44f viewBasis;
    Matrix44f viewBasisInv;
    const bool canBillboard = M2CameraBasis(req.world, req.view, viewBasis, viewBasisInv);

    for (usize i = 0; i < model_.bones.size(); ++i) {
        if (!driven.empty() && driven[i]) {
            fs.boneWorldMatrices[i] = driven[i]->m;
            continue;
        }
        const auto& b = model_.bones[i];
        const i32 parent = b.parentBoneId;
        // `parentIndex < boneIndex` is the client's own assertion, so one
        // forward pass suffices; a violation would read a matrix this pass has
        // not written, which the bounds test below turns into "no parent".
        const Matrix44f parentM =
            (parent >= 0 && static_cast<usize>(parent) < i)
                ? M2ParentFor(fs.boneWorldMatrices[static_cast<usize>(parent)], b.flags, b.pivot)
                : Matrix44f::identity();

        // Only a bone the file marks `Transformed`, and not `Kinematic`, is
        // sampled. Both halves are the client's own gate — `(flags & 0x280) ==
        // 0 || (flags & 0x400) != 0` sends a bone down the unsampled path,
        // where it takes the solver's matrix if it has one and its parent's
        // otherwise. An unflagged bone inheriting its parent is what an
        // identity local transform composes to anyway.
        //
        // `Kinematic` reaching this at all is narrow: of the corpus's 2162 such
        // bones, 2142 carry no keys to sample, and the 20 that do (all in
        // `moargbrute_boss`) also ship a `.phys`, so the physics claim above
        // has already taken them. The gate is what makes that true when the
        // solver is off rather than only when it happens to run.
        const bool sampled = !bindPose &&
                             hasFlag(static_cast<BoneFlag>(b.flags), BoneFlag::Transformed) &&
                             !hasFlag(static_cast<BoneFlag>(b.flags), BoneFlag::Kinematic);
        Matrix44f local = Matrix44f::identity();
        if (sampled) {
            const Vector3f t = SampleM2Vec3(b.translation, at, {0.0f, 0.0f, 0.0f});
            const Quaternion r = SampleM2Quat(b.rotation, at, Quaternion{0.0f, 0.0f, 0.0f, 1.0f});
            const Vector3f s = SampleM2Vec3(b.scale, at, {1.0f, 1.0f, 1.0f});
            local = M2BoneLocal(t, r, s, b.pivot);
        }
        Matrix44f world = sampled ? (local * parentM) : parentM;

        // Outside the sampled test on purpose, matching the client: four bones
        // in five that billboard are not `Transformed` at all, and they still
        // billboard — off an identity local transform. Applied here rather than
        // downstream so a billboarded bone carries its whole subtree with it,
        // which is what makes one flagged helper reorient a model's every
        // attached sprite.
        const M2Billboard bb = M2BillboardOf(b.flags);
        if (bb != M2Billboard::None && canBillboard)
            world = M2BillboardBone(world, local, bb, viewBasis, viewBasisInv, b.pivot);

        fs.boneWorldMatrices[i] = world;
    }

    // Place the `.phys` wireframes on their bones for the Collisions view. The palette is a
    // *skinning* matrix, so a shape's model-space placement is `T(pivot) * palette` — the same
    // conversion the physics stage does, and the reason a shape drawn straight from the palette
    // sits at the origin.
    //
    // These are one frame behind the solver: physics runs after `Evaluate` and its result
    // arrives here through the next frame's claim overrides. That is invisible in an overlay
    // and keeps the fill in one place.
    if (!physicsShapeBones_.empty()) {
        fs.collisionTransforms.resize(physicsShapeBones_.size());
        for (usize k = 0; k < physicsShapeBones_.size(); ++k) {
            const auto bone = static_cast<usize>(physicsShapeBones_[k]);
            if (bone >= fs.boneWorldMatrices.size()) {
                fs.collisionTransforms[k] = Matrix44f::identity();
                continue;
            }
            const Matrix44f& m = fs.boneWorldMatrices[bone];
            const Vector3f& p = model_.bones[bone].pivot;
            Matrix44f placed = m;
            placed.data[3][0] = p.x * m.data[0][0] + p.y * m.data[1][0] + p.z * m.data[2][0] +
                                m.data[3][0];
            placed.data[3][1] = p.x * m.data[0][1] + p.y * m.data[1][1] + p.z * m.data[2][1] +
                                m.data[3][1];
            placed.data[3][2] = p.x * m.data[0][2] + p.y * m.data[1][2] + p.z * m.data[2][2] +
                                m.data[3][2];
            // A `BOXS` keeps its orientation in its own frame rather than in its corners,
            // so that frame goes on the inside of the placement.
            fs.collisionTransforms[k] =
                k < physicsShapeLocals_.size() ? physicsShapeLocals_[k] * placed : placed;
        }
    }
}

void M2ModelAdapter::CreatePoseStages(renderer::animation::PoseStageList& out) const {
#if WDX_HAS_PHYSICS
    // Physics is last in the list by contract: solvers correct the animated
    // pose and physics consumes the corrected one (`pose_stage.h`). Nothing
    // else appends here yet, so "last" costs nothing to honour today and is
    // the ordering to keep when something does.
    if (auto stage = renderer::profiles::wow::CreateWowPhysicsStage(model_)) {
        out.push_back(std::move(stage));
    }
#else
    (void)out;
#endif
}

void M2ModelAdapter::EvaluateTextureTransforms(const M2AnimTime& at, bool bindPose,
                                               renderer::model::FrameState& fs) const {
    fs.texAnimMatrices.reserve(model_.textureTransforms.size());
    for (usize i = 0; i < model_.textureTransforms.size(); ++i) {
        const auto& tt = model_.textureTransforms[i];
        Vector3f t{0.0f, 0.0f, 0.0f};
        Quaternion r{0.0f, 0.0f, 0.0f, 1.0f};
        Vector3f s{1.0f, 1.0f, 1.0f};
        if (!bindPose) {
            t = SampleM2Vec3(tt.translation, at, t);
            r = SampleM2Quat(tt.rotation, at, r);
            s = SampleM2Vec3(tt.scaling, at, s);
        }

        // CM2Model::AnimateTextureTransformMT composes, in order, a rotation
        // about (0.5, 0.5), a scale about the same pivot, and a translation —
        // all three *pre*-multiplied, which flattens to
        //
        //     uv' = ((uv + t - 0.5) · S · R) + 0.5
        //
        // the same shape WC3's texture animation takes, which is why the two
        // share FrameState::TexAnimMatrix. Only rows 0 and 1 matter: the
        // shader feeds (u, v, 0, 1) and reads .xy back, so the third column and
        // row can never reach the output.
        const Matrix44f rot = Matrix44f::rotation(r).transpose();
        const f32 a = s.x * rot.data[0][0];
        const f32 bb = s.y * rot.data[1][0];
        const f32 d = s.x * rot.data[0][1];
        const f32 e = s.y * rot.data[1][1];
        const f32 px = t.x - 0.5f;
        const f32 py = t.y - 0.5f;

        renderer::model::FrameState::TexAnimMatrix m{};
        m.textureAnimId = static_cast<i32>(i);
        m.row0[0] = a;
        m.row0[1] = bb;
        m.row0[2] = 0.0f;
        m.row0[3] = a * px + bb * py + 0.5f;
        m.row1[0] = d;
        m.row1[1] = e;
        m.row1[2] = 0.0f;
        m.row1[3] = d * px + e * py + 0.5f;
        fs.texAnimMatrices.push_back(m);
    }
}

void M2ModelAdapter::EvaluateSurfaces(const M2AnimTime& at, bool bindPose,
                                      renderer::model::FrameState& fs) const {
    // Nothing at the bind pose: an empty channel is what makes the draw path
    // read M2SurfaceTable's own constants, and those *are* each track's first
    // key. Filling this with animref defaults instead would quietly override
    // them with 1.0.
    if (bindPose || profileIndex_ >= model_.skinProfiles.size())
        return;
    const auto& skin = model_.skinProfiles[profileIndex_];
    fs.surfaceStates.reserve(skin.batches.size());

    // `surface` is the batch's index in this profile, which is exactly what
    // BuildM2SurfaceTable numbers its entries by. The two walk the same array in
    // the same order; that is the whole of the contract between them.
    for (usize i = 0; i < skin.batches.size(); ++i) {
        const auto& batch = skin.batches[i];
        renderer::model::FrameState::SurfaceState st;
        st.surface = static_cast<i32>(i);

        const u32 units = std::clamp<u32>(batch.textureCount, 1u, 4u);
        for (u32 u = 0; u < units; ++u) {
            const u64 ci = static_cast<u64>(batch.textureWeightComboIndex) + u;
            if (ci >= model_.textureWeightCombos.size())
                continue;
            const u32 w = model_.textureWeightCombos[ci];
            if (w >= model_.textureWeights.size())
                continue;
            st.unitWeights[u] = SampleM2Fixed16(model_.textureWeights[w].weight, at, 1.0f);
        }

        // Whole-element alpha takes unit 0's weight whatever the texture count;
        // the rest reach the shader as the per-unit float4. Batch flag 0x40
        // drops the weight from the product entirely.
        st.alpha = (batch.flags & 0x40u) ? 1.0f : st.unitWeights[0];
        if (batch.colorIndex >= 0 && static_cast<usize>(batch.colorIndex) < model_.colors.size()) {
            const auto& c = model_.colors[static_cast<usize>(batch.colorIndex)];
            st.color = SampleM2Vec3(c.color, at, {1.0f, 1.0f, 1.0f});
            st.alpha *= SampleM2Fixed16(c.alpha, at, 1.0f);
        }
        st.alpha = std::clamp(st.alpha, 0.0f, 1.0f);
        fs.surfaceStates.push_back(st);
    }
}

void M2ModelAdapter::EvaluateLights(const M2AnimTime& at, const Matrix44f& world,
                                    renderer::model::FrameState& fs) const {
    using LightKind = renderer::model::FrameState::LightKind;
    if (model_.lights.empty())
        return;
    fs.lights.reserve(model_.lights.size());

    for (const auto& L : model_.lights) {
        renderer::model::FrameState::LightState st;
        // Type 1 is the positional one, in the file and at runtime alike:
        // CM2Model::AnimateST calls SetPosition on `type == 1` and SetDirection
        // otherwise, and CM2Lighting::AddLight routes on the same value.
        st.kind = (L.type == 1) ? LightKind::Omni : LightKind::Directional;

        Matrix44f bone = Matrix44f::identity();
        if (L.boneId >= 0 && static_cast<usize>(L.boneId) < fs.boneWorldMatrices.size())
            bone = fs.boneWorldMatrices[static_cast<usize>(L.boneId)];
        const Matrix44f toWorld = bone * world;

        st.worldPos = whiteout::transform_point(L.position, toWorld);
        // A directional light aims along its bone's *negative* Z. AnimateST
        // negates the bone matrix's third row and rotates it out to world; the
        // result is the direction the light travels, which is the sign
        // CM2Lighting keeps and GLDevice::SetLight flips on the way to
        // GL_POSITION.
        const Vector3f boneZ{toWorld.data[2][0], toWorld.data[2][1], toWorld.data[2][2]};
        st.worldDir = {-boneZ.x, -boneZ.y, -boneZ.z};

        // `colour × intensity`, the product CM2Model::AnimateMT forms before
        // handing either to CM2Light. Both default to zero, so a light whose
        // intensity track is absent contributes nothing — the client's own
        // behaviour, not a fallback.
        const f32 ambI = SampleM2Float(L.ambientIntensity, at, 0.0f);
        const f32 diffI = SampleM2Float(L.diffuseIntensity, at, 0.0f);
        const Vector3f ambC = SampleM2Vec3(L.ambientColor, at, {0.0f, 0.0f, 0.0f});
        const Vector3f diffC = SampleM2Vec3(L.diffuseColor, at, {0.0f, 0.0f, 0.0f});

        st.ambientColor = ambC;
        st.ambIntensity = ambI;
        st.diffuse = {diffC.x * diffI, diffC.y * diffI, diffC.z * diffI};
        st.dirIntensity = diffI;

        // Carried because the file has them and a future consumer may want
        // them; the WoW shading path does not read them. 6.0.1 never animates
        // these two tracks — AnimateMT walks the other five and skips this pair
        // — so every `.m2` point light attenuates by CM2Light's fixed
        // constants instead. See kM2Attenuation.
        st.attenStart = SampleM2Float(L.attenuationStart, at, 0.0f);
        st.attenEnd = SampleM2Float(L.attenuationEnd, at, 0.0f);

        st.enabled = SampleM2U8(L.visibility, at, 1) != 0;
        fs.lights.push_back(st);
    }
}

std::vector<renderer::effects::RibbonEmitterConfig> M2ModelAdapter::GetRibbonConfigs() {
    std::vector<renderer::effects::RibbonEmitterConfig> out;
    out.reserve(model_.ribbonEmitters.size());

    for (const auto& r : model_.ribbonEmitters) {
        renderer::effects::RibbonEmitterConfig cfg;
        // `textureIndices` indexes the model's texture array directly, which is
        // the same space GetTextures numbers its output in. Only the FIRST is
        // drawn, and the rest are deliberately dropped.
        //
        // 6.0.1's loader disagrees: CM2Model::InitializeLoaded sizes the
        // emitter's CRibbonMat / CTexture / replaces arrays to
        // `textureIndices.count` (`lea r14,[rec+0x14]`, then SetCount([r14]) on
        // all three) and Render @0x100e7e1d0 loops that count, rebinding
        // texture and blend over the same strip. It pairs layer i with
        // `materialIndices[i]` unbounded — every shipped ribbon has exactly one
        // material index, and the u16 it reads past the end is a literal 0 on
        // all 5926 such layers in the corpus, so it lands on material 0.
        //
        // Drawing those extra passes is wrong on the content we have, and the
        // textures say why: across this model every layer-0 texture's alpha
        // reaches 0 (a shaped mask) while every layer-1/2 texture has a high
        // alpha FLOOR — 118, 128, 50, 30, 40 — and two are 8x8/32x32 tiles that
        // are opaque everywhere. Those are combiner inputs, not coverage masks;
        // alpha-compositing them over the strip buries the base under a solid
        // band. 6.0.1 is Warlords and this content is Legion+, the same version
        // gap that made the record's zSource look live, so the pass loop is
        // recorded here rather than acted on.
        renderer::effects::RibbonLayer layer;
        layer.textureId = r.textureIndices.empty() ? -1 : static_cast<i32>(r.textureIndices[0]);
        layer.filterMode = ::whiteout::flakes::FILTER_BLEND;
        if (!r.materialIndices.empty() && r.materialIndices[0] < model_.materials.size()) {
            const auto& mat = model_.materials[r.materialIndices[0]];
            layer.filterMode = M2BlendToFilterMode(mat.blendingMode);
            // Render derives all three from the material rather than assuming
            // them: SetLightingEnabled(flags & 1) on a bit the loader stores as
            // !unlit, SetFogEnabled likewise. 356 of the corpus's ribbon
            // materials are NOT unlit and 178 not two-sided, so hardcoding them
            // was wrong for those.
            layer.unshaded = (mat.flags & renderer::profiles::wow::kM2Unlit) != 0;
            layer.unfogged = (mat.flags & renderer::profiles::wow::kM2Unfogged) != 0;
            layer.twoSided = (mat.flags & renderer::profiles::wow::kM2TwoSided) != 0;
        }
        cfg.layers.push_back(layer);

        cfg.textureId = layer.textureId;
        cfg.filterMode = layer.filterMode;
        cfg.rows = (r.textureRows > 0) ? r.textureRows : 1;
        cfg.cols = (r.textureCols > 0) ? r.textureCols : 1;
        cfg.emission = r.edgesPerSecond;
        cfg.life = r.edgeLifetime;
        cfg.gravity = r.gravity;
        cfg.priorityPlane = r.priorityPlane;
        cfg.unshaded = layer.unshaded;
        cfg.twoSided = layer.twoSided;
        out.push_back(cfg);
    }
    return out;
}

void M2ModelAdapter::EvaluateRibbons(const M2AnimTime& at, const Matrix44f& world,
                                     renderer::model::FrameState& fs) const {
    if (model_.ribbonEmitters.empty())
        return;
    fs.ribbonStates.reserve(model_.ribbonEmitters.size());

    // Same factor the particle path reads, for the same reason: the record's
    // heights are model units (0.04..6.9 across the corpus, mean 0.6) while the
    // edges this feeds live in renderer units. Unscaled they draw a sub-pixel
    // sliver — WC3's own authored half-widths are ~20, which is what 0.22 model
    // units becomes at the wow profile's 100.
    const f32 unitScale = std::sqrt(world.data[0][0] * world.data[0][0] +
                                    world.data[0][1] * world.data[0][1] +
                                    world.data[0][2] * world.data[0][2]);

    for (usize i = 0; i < model_.ribbonEmitters.size(); ++i) {
        const auto& r = model_.ribbonEmitters[i];
        renderer::model::FrameState::RibbonFrameState st;
        st.emitterId = static_cast<i32>(i);

        Matrix44f bone = Matrix44f::identity();
        if (r.boneId < model_.bones.size() && r.boneId < fs.boneWorldMatrices.size())
            bone = fs.boneWorldMatrices[static_cast<usize>(r.boneId)];
        // The emitter's frame IS the bone's, translated by the record's own
        // offset. RibbonEmitter::SetState reads position from the translation,
        // forward from row 2 and vertical from row 1 — the same decomposition
        // CRibbonEmitter::SetPos does on the orientation it is handed.
        Matrix44f local = Matrix44f::identity();
        local.data[3][0] = r.position.x;
        local.data[3][1] = r.position.y;
        local.data[3][2] = r.position.z;
        st.transform = local * bone * world;

        st.above = SampleM2Float(r.heightAbove, at, 0.0f);
        st.below = SampleM2Float(r.heightBelow, at, 0.0f);
        st.color = SampleM2Vec3(r.colorTrack, at, {1.0f, 1.0f, 1.0f});
        st.alpha = SampleM2Fixed16(r.alphaTrack, at, 1.0f);
        st.visibility = SampleM2U8(r.visibility, at, 1) != 0 ? 1.0f : 0.0f;
        // CM2Model::AnimateST feeds this straight to CRibbonEmitter::SetTexSlot
        // every frame. No corpus ribbon is anything but 1x1 with slot 0, so
        // this is correctness rather than a visible change.
        st.slot = static_cast<i32>(SampleM2U16(r.texSlot, at, 0));
        st.unitScale = (unitScale > 0.0f) ? unitScale : 1.0f;

        // The record's index is into textureTransformCombos, not into
        // textureTransforms: over the corpus's 5293 ribbons every non-zero
        // index is in range for the combos array and 278 are not for the direct
        // one. Both misses below are silent by design — 1353 of those indices
        // resolve to the combos array's 0xFFFF "no transform" sentinel, which
        // leaves the identity the state already holds. 213 name a real one.
        if (r.textureTransformIndex >= 0 &&
            static_cast<usize>(r.textureTransformIndex) < model_.textureTransformCombos.size()) {
            const u16 combo =
                model_.textureTransformCombos[static_cast<usize>(r.textureTransformIndex)];
            if (combo < fs.texAnimMatrices.size()) {
                const auto& m = fs.texAnimMatrices[combo];
                std::copy(std::begin(m.row0), std::end(m.row0), std::begin(st.texAnimRow0));
                std::copy(std::begin(m.row1), std::end(m.row1), std::begin(st.texAnimRow1));
            }
        }
        fs.ribbonStates.push_back(st);
    }
}

namespace {

using ::whiteout::m2::ParticleFlag;

bool HasParticleFlag(ParticleFlag flags, u32 bit) {
    return (static_cast<u32>(flags) & bit) != 0;
}

// An M2 string array counts its terminator, so every name the parser hands back
// ends in a NUL that std::string keeps as a character. Left in, it reaches the
// content provider as part of the path and nothing ever resolves.
std::string TrimTrailingNuls(std::string s) {
    while (!s.empty() && s.back() == '\0')
        s.pop_back();
    return s;
}

// The `.m2` emitter @p index spawns instead of quads, empty when it spawns
// quads. A pre-Legion record names it inline; a chunked one leaves the name
// empty and puts a fileDataID in GPID, one entry per emitter — the same
// substitution TXID makes for texture names, spelled the same way
// (`ContentRef::Describe`). No record in the corpus uses the inline form.
//
// One function because two callers have to agree exactly: the config decides
// which emitter class gets built, and the frame state decides which id space
// its animated values are sent to.
std::string GeometryModelKey(const ::whiteout::m2::Model& model, usize index) {
    if (index >= model.particleEmitters.size())
        return {};
    std::string name = TrimTrailingNuls(model.particleEmitters[index].particleModelFilename);
    if (!name.empty())
        return name;
    if (index < model.geometryParticleModelIds.size() &&
        model.geometryParticleModelIds[index] != 0) {
        return "#" + std::to_string(model.geometryParticleModelIds[index]);
    }
    return {};
}

// The same lookup for RPID — the `.m2` whose emitters trail every particle of
// this one. Same two forms as GeometryModelKey and the same trailing-NUL trap:
// both strings come out of the file with their terminator inside the field.
std::string RecursionModelKey(const ::whiteout::m2::Model& model, usize index) {
    if (index >= model.particleEmitters.size())
        return {};
    std::string name = TrimTrailingNuls(model.particleEmitters[index].childEmittersModelFilename);
    if (!name.empty())
        return name;
    if (index < model.recursiveParticleModelIds.size() &&
        model.recursiveParticleModelIds[index] != 0) {
        return "#" + std::to_string(model.recursiveParticleModelIds[index]);
    }
    return {};
}

// A particle track's 16-bit fields are the client's `fixed16`: raw * 1/32767,
// so a full-scale key is 0x7FFF and not 0xFFFF. WhiteoutLib types them
// `unorm16`, whose float conversion divides by 65535 — half of what the client
// reads (`InterpolateAllTracks` @0x1016a1b70 multiplies by 0.000030518509).
// Taking `.value` and rescaling here is what keeps a track that ends at 1.0
// from ending at 0.5.
constexpr f32 kFixed16 = 1.0f / 32767.0f;

template <class U>
inline f32 Fixed16(U v) {
    return static_cast<f32>(v.value) * kFixed16;
}

// Copy one M2 particle lifetime track. Times are fixed16 over [0,1] and the
// last one is asserted to be 1.0 by the client, so they need no rescaling.
template <class Track, class Out, class Conv>
void CopyParticleTrack(const Track& track, std::vector<f32>& times, Out& values, Conv conv) {
    const usize n = track.values.size();
    times.reserve(n);
    values.reserve(n);
    for (usize i = 0; i < n; ++i) {
        times.push_back(i < track.timestamps.size() ? Fixed16(track.timestamps[i])
                                                    : (n > 1 ? static_cast<f32>(i) / (n - 1) : 0.0f));
        values.push_back(conv(track.values[i]));
    }
}

} // namespace

std::vector<renderer::M2ParticleEmitterConfig> M2ModelAdapter::GetM2ParticleConfigs() {
    std::vector<renderer::M2ParticleEmitterConfig> out;
    out.reserve(model_.particleEmitters.size());

    for (usize ei = 0; ei < model_.particleEmitters.size(); ++ei) {
        const auto& p = model_.particleEmitters[ei];
        renderer::M2ParticleEmitterConfig cfg;
        const u32 f = static_cast<u32>(p.flags);

        // A plain emitter indexes the model's texture array with the whole
        // 16-bit field (`InitializeLoaded` @0x100f57c29 loads it and indexes
        // CM2Shared's handle array directly). A MultiTexture one packs three
        // 5-bit indices into the same field; only the first is bound here,
        // because the multi-texture shading path is out of scope.
        cfg.textureId = ((f & 0x10000000u) != 0) ? static_cast<i32>(p.textureId & 0x1Fu)
                                                 : static_cast<i32>(p.textureId);
        cfg.filterMode = static_cast<i32>(p.blendingType);
        cfg.rows = (p.rows > 0) ? p.rows : 1;
        cfg.cols = (p.columns > 0) ? p.columns : 1;
        // Shaded/Unshaded are two separate bits in the record; the loader turns
        // them into the lighting bit of CParticleMat.
        cfg.unshaded = (f & 0x8u) != 0 || (f & 0x1u) == 0;
        cfg.unfogged = (f & 0x100000u) != 0;

        cfg.generator = static_cast<renderer::M2ParticleEmitterConfig::Generator>(
            static_cast<u8>(p.emitterType));
        cfg.boneId = static_cast<i32>(p.boneId);
        cfg.position = p.position;

        cfg.lifespanVariation = p.lifespanVariation;
        cfg.emissionRateVariation = p.emissionRateVariation;
        cfg.tailLength = p.tailLength;

        // HeadStyle / TailStyle. A record with neither draws nothing, which is
        // faithful — SetParticleStyle leaves both quad flags clear.
        cfg.hasHead = (f & 0x20000u) != 0;
        cfg.hasTail = (f & 0x40000u) != 0;
        if (!cfg.hasHead && !cfg.hasTail)
            cfg.hasHead = true;

        // Despite the name, file 0x10 is what makes a particle RIDE its
        // emitter. It maps to runtime 0x200 (`InitializeLoaded` @0x100f553d0:
        // set on 0x10, explicitly cleared otherwise), and 0x200 is the bit
        // `CreateParticle` @0x1016a0640 tests to decide whether to bake the
        // spawn into world space — it bakes when the bit is CLEAR. So set means
        // "stay local, transform at draw", which is exactly modelSpace here,
        // and clear means the particle is stamped into the world at birth and
        // trails behind a moving emitter.
        cfg.modelSpace = (f & 0x10u) != 0;
        cfg.sortZ = (f & 0x2u) != 0;
        cfg.xyQuad = (f & 0x1000u) != 0;
        cfg.squirt = (f & 0x8000u) != 0;
        cfg.hemisphereUp = (f & 0x100u) != 0;
        cfg.followPosition = (f & 0x4000u) != 0;
        cfg.randomEmissionSpacing = (f & 0x800u) != 0;
        cfg.inheritVelocity = (f & 0x40u) != 0;
        cfg.lodIgnoreDistance = (f & 0x4000000u) != 0;
        // 6.0.1 keys the implosion filter off the DynamicWind sign bit for
        // sphere emitters, not the documented 0x80 — a quirk of that build,
        // reproduced rather than corrected.
        cfg.implosionFilter =
            cfg.generator == renderer::M2ParticleEmitterConfig::Generator::Sphere &&
            (f & 0x80000000u) != 0;
        cfg.inheritVelocityScale = p.inheritVelocityScale;

        // Appearance. The file→runtime flag map is the loader's own
        // (`InitializeLoaded` @0x100f553d0); the runtime bit numbers differ from
        // the file ones, which is why these are read by file bit here and
        // carried as named booleans rather than as a flag word.
        cfg.velocityOrient = (f & 0x4u) != 0;
        cfg.inheritBoneScale = (f & 0x20u) != 0;
        cfg.negateSpinRandom = (f & 0x200u) != 0;
        cfg.clampTailToAge = (f & 0x400u) != 0;
        cfg.chooseRandomTexture = (f & 0x10000u) != 0;
        cfg.unscaledSizeVariation = (f & 0x80000u) != 0;
        cfg.randFlipbookStart = (f & 0x200000u) != 0;
        cfg.offsetHeadBySpin = (f & 0x8000000u) != 0;

        cfg.baseSpin = p.baseSpin;
        cfg.baseSpinVariation = p.baseSpinVariation;
        cfg.spinSpeed = p.spinSpeed;
        cfg.spinSpeedVariation = p.spinSpeedVariation;
        cfg.twinkleSpeed = p.twinkleSpeed;
        cfg.twinklePercent = p.twinklePercent;
        cfg.twinkleScale = p.twinkleScale;

        cfg.drag = p.drag;
        // The static wind vector applies only when DynamicWind is clear.
        if ((f & 0x80000000u) == 0)
            cfg.windVector = p.windVector;
        cfg.followSpeed1 = p.followSpeed1;
        cfg.followScale1 = p.followScale1;
        cfg.followSpeed2 = p.followSpeed2;
        cfg.followScale2 = p.followScale2;
        cfg.splinePoints = p.splinePoints;
        cfg.priorityPlane = p.textureTilerotation;

        cfg.geometryModelPath = GeometryModelKey(model_, ei);
        cfg.recursionModelPath = RecursionModelKey(model_, ei);
        cfg.tumbleMin = p.tumble.minimum;
        cfg.tumbleMax = p.tumble.maximum;

        // Sampled through the same sampler the animated path uses, at the start
        // of sequence 0. Only a trail emitter reads these: it belongs to a model
        // nothing animates, so the tracks are never walked for it.
        {
            const M2AnimTime at0{};
            cfg.initial.emissionRate = (std::max)(SampleM2Float(p.emissionRate, at0, 0.0f), 0.0f);
            cfg.initial.speed = SampleM2Float(p.emissionSpeed, at0, 0.0f);
            cfg.initial.variation = SampleM2Float(p.speedVariation, at0, 0.0f);
            cfg.initial.coneAngle = SampleM2Float(p.verticalRange, at0, 0.0f);
            cfg.initial.horizontalRange = SampleM2Float(p.horizontalRange, at0, 0.0f);
            cfg.initial.width = SampleM2Float(p.emissionAreaWidth, at0, 0.0f);
            cfg.initial.length = SampleM2Float(p.emissionAreaLength, at0, 0.0f);
            cfg.initial.zSource =
                p.extension ? p.extension->zSource : SampleM2Float(p.zSource, at0, 0.0f);
            cfg.initial.lifeSpan = SampleM2Float(p.lifespan, at0, 1.0f);
            cfg.initial.gravityVector = SampleM2ParticleGravity(
                p.gravity, at0,
                HasParticleFlag(p.flags, static_cast<u32>(ParticleFlag::CompressedGravity)));
        }

        CopyParticleTrack(p.colorTrack, cfg.colorTimes, cfg.colorValues, [](const Vector3f& v) {
            // Record colours are 0..255 display-referred.
            return Vector3f{v.x / 255.0f, v.y / 255.0f, v.z / 255.0f};
        });
        CopyParticleTrack(p.alphaTrack, cfg.alphaTimes, cfg.alphaValues,
                          [](auto v) { return Fixed16(v); });
        CopyParticleTrack(p.scaleTrack, cfg.scaleTimes, cfg.scaleValues,
                          [](const Vector2f& v) { return v; });
        cfg.scaleVariation = p.scaleVary;
        // Despite their `unorm16` type and their name, the two cell tracks hold
        // raw sprite-sheet cell indices — the client reads them as plain u16 and
        // masks them into the sheet. Normalising them would make every cell 0.
        CopyParticleTrack(p.headUVScroll, cfg.headCellTimes, cfg.headCellValues,
                          [](auto v) { return static_cast<f32>(v.value); });
        CopyParticleTrack(p.tailUVScroll, cfg.tailCellTimes, cfg.tailCellValues,
                          [](auto v) { return static_cast<f32>(v.value); });

        out.push_back(std::move(cfg));
    }
    return out;
}

// The bone-emitter table, per model rather than per emitter — exactly as
// `CBoneGeneratorBase::CreateBoneEmitterTable` @0x10169d1a0 builds it.
//
// Membership is "every bone the rendered geometry actually uses, minus the ones
// flagged 0x800". The client walks the skin's submeshes and maps their bone
// ranges through the bone-combo table rather than taking every bone in the
// model, which is what keeps attachment and helper bones out of the spray.
void M2ModelAdapter::BuildBoneSpawnTable(renderer::model::FrameState& fs) const {
    fs.boneSpawnTable.clear();
    const bool anyBoneGenerator =
        std::any_of(model_.particleEmitters.begin(), model_.particleEmitters.end(),
                    [](const auto& p) {
                        return p.emitterType == ::whiteout::m2::ParticleEmitterType::Bone;
                    });
    if (!anyBoneGenerator || model_.skinProfiles.empty())
        return;

    // Bit 0x800 of the bone flags. Skipped by the client, and the reason a
    // billboarded bone never sprays.
    constexpr u32 kBoneExcluded = 0x800u;

    std::vector<u8> used(model_.bones.size(), 0);
    const auto& skin = model_.skinProfiles.front();
    for (const auto& sec : skin.submeshes) {
        for (u32 k = 0; k < sec.boneCount; ++k) {
            const usize combo = static_cast<usize>(sec.boneComboIndex) + k;
            if (combo >= model_.boneCombos.size())
                continue;
            const u16 bone = model_.boneCombos[combo];
            if (bone < used.size() && (model_.bones[bone].flags & kBoneExcluded) == 0)
                used[bone] = 1;
        }
    }

    for (usize b = 0; b < used.size(); ++b) {
        // The pose is what carries the positions, so a bone the evaluator did
        // not produce a matrix for has nothing to spawn from.
        if (!used[b] || b >= fs.boneWorldMatrices.size())
            continue;
        renderer::model::FrameState::BoneSpawn e{};
        const Matrix44f& m = fs.boneWorldMatrices[b];
        e.pos = {m.data[3][0], m.data[3][1], m.data[3][2]};
        e.parentPos = e.pos;

        const i16 parent = model_.bones[b].parentBoneId;
        if (parent >= 0 && static_cast<usize>(parent) < fs.boneWorldMatrices.size()) {
            const Matrix44f& pm = fs.boneWorldMatrices[static_cast<usize>(parent)];
            e.parentPos = {pm.data[3][0], pm.data[3][1], pm.data[3][2]};
            e.hasParent = true;
            // The bone's own direction, and any perpendicular to it: together
            // the plane ApplyParams scatters the spawn in. Both fall back the
            // way the client's SafeNormalize does when the bone is degenerate.
            Vector3f dir{e.pos.x - e.parentPos.x, e.pos.y - e.parentPos.y,
                         e.pos.z - e.parentPos.z};
            const f32 len = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
            e.axisA = (len > 1e-6f) ? Vector3f{dir.x / len, dir.y / len, dir.z / len}
                                    : Vector3f{1.0f, 0.0f, 0.0f};
            const Vector3f up = (std::abs(e.axisA.z) > 0.99f) ? Vector3f{1.0f, 0.0f, 0.0f}
                                                              : Vector3f{0.0f, 0.0f, 1.0f};
            Vector3f perp{e.axisA.y * up.z - e.axisA.z * up.y, e.axisA.z * up.x - e.axisA.x * up.z,
                          e.axisA.x * up.y - e.axisA.y * up.x};
            const f32 pl = std::sqrt(perp.x * perp.x + perp.y * perp.y + perp.z * perp.z);
            e.axisB = (pl > 1e-6f) ? Vector3f{perp.x / pl, perp.y / pl, perp.z / pl}
                                   : Vector3f{0.0f, 0.0f, 1.0f};
        }
        fs.boneSpawnTable.push_back(e);
    }
}

void M2ModelAdapter::EvaluateParticles(const M2AnimTime& at, const Matrix44f& world,
                                       renderer::model::FrameState& fs) const {
    if (model_.particleEmitters.empty())
        return;
    fs.particleStates.reserve(model_.particleEmitters.size());

    // `world` is the actor's scaled transform, so the length of its first basis
    // row is exactly the model-unit → renderer-unit factor (100 under the wow
    // profile). Read off the actor rather than the bone: the emitter's own bone
    // scale is a separate, opt-in effect (InheritBoneScale).
    const f32 unitScale = std::sqrt(world.data[0][0] * world.data[0][0] +
                                    world.data[0][1] * world.data[0][1] +
                                    world.data[0][2] * world.data[0][2]);

    BuildBoneSpawnTable(fs);

    for (usize i = 0; i < model_.particleEmitters.size(); ++i) {
        const auto& p = model_.particleEmitters[i];
        renderer::model::FrameState::ParticleFrameState st{};
        st.emitterId = static_cast<i32>(i);
        st.boneGenerator = p.emitterType == ::whiteout::m2::ParticleEmitterType::Bone;
        st.modelParticle = !GeometryModelKey(model_, i).empty();

        Matrix44f bone = Matrix44f::identity();
        if (p.boneId < model_.bones.size() && p.boneId < fs.boneWorldMatrices.size())
            bone = fs.boneWorldMatrices[static_cast<usize>(p.boneId)];
        Matrix44f local = Matrix44f::identity();
        local.data[3][0] = p.position.x;
        local.data[3][1] = p.position.y;
        local.data[3][2] = p.position.z;

        // CM2Model::AnimateParticleST pre-multiplies by a constant 90-degree
        // rotation about Z before the bone chain. Without it every generator's
        // azimuth-zero points the wrong way and emitters spray sideways — and
        // no unit test can catch that, only looking at the model.
        Matrix44f baseFlip = Matrix44f::identity();
        baseFlip.data[0][0] = 0.0f;
        baseFlip.data[0][1] = 1.0f;
        baseFlip.data[1][0] = -1.0f;
        baseFlip.data[1][1] = 0.0f;
        st.transform = baseFlip * local * bone * world;

        st.worldPosition = {st.transform.data[3][0], st.transform.data[3][1],
                            st.transform.data[3][2]};

        st.emissionRate = SampleM2Float(p.emissionRate, at, 0.0f);
        st.speed = SampleM2Float(p.emissionSpeed, at, 0.0f);
        st.variation = SampleM2Float(p.speedVariation, at, 0.0f);
        st.coneAngle = SampleM2Float(p.verticalRange, at, 0.0f);
        st.horizontalRange = SampleM2Float(p.horizontalRange, at, 0.0f);
        st.width = SampleM2Float(p.emissionAreaWidth, at, 0.0f);
        st.length = SampleM2Float(p.emissionAreaLength, at, 0.0f);
        // The record track is dead once EXPT/EXP2 is present — it holds the
        // sentinel 255 on 30718 corpus emitters, which aims them down a virtual
        // source overhead and discards the authored cone. The record field is
        // the pre-Legion fallback. See M2_PARTICLE_PLAN.md.
        st.zSource = p.extension ? p.extension->zSource : SampleM2Float(p.zSource, at, 0.0f);
        st.lifeSpan = SampleM2Float(p.lifespan, at, 1.0f);

        // Gravity is a direction in M2, not just a magnitude — but only when
        // the emitter says so. Both forms arrive as f32 keys; the compressed
        // one is four packed bytes wearing a float's clothes.
        st.gravityVector = SampleM2ParticleGravity(
            p.gravity, at,
            HasParticleFlag(p.flags, static_cast<u32>(ParticleFlag::CompressedGravity)));
        st.gravity = -st.gravityVector.z;
        st.hasGravityVector = true;

        // Two distinct gates: `enabledIn` drives emission (and clamps a
        // negative rate to zero), while visibility decides whether the emitter
        // is simulated at all. Model alpha is neither — it scales the drawn
        // particle and never stops emission.
        st.enabled = SampleM2U8(p.enabledIn, at, 1) != 0;
        if (!st.enabled || st.emissionRate < 0.0f)
            st.emissionRate = 0.0f;
        st.visibility = 1.0f;
        // The client drives this from the model's own fade (`m_alphaScale`,
        // written by CM2Model::AnimateParticleST). Nothing in the viewer fades a
        // model, so it stays 1 — the multiply lives in the builder either way,
        // so a host that does fade one gets the behaviour for free.
        st.modelAlpha = 1.0f;
        st.unitScale = (unitScale > 0.0f) ? unitScale : 1.0f;
        // The emitter's static Squirt property, exactly as the MDX adapter
        // reports it: "this emitter bursts rather than streams". The rate's
        // rising edge is detected at the actor layer, shared by both formats.
        st.squirting = (static_cast<u32>(p.flags) & 0x8000u) != 0;

        fs.particleStates.push_back(st);
    }
}

std::vector<renderer::model::CollisionShapeData> M2ModelAdapter::GetCollisionShapes() {
    physicsShapeBones_.clear();
    physicsShapeLocals_.clear();
    std::vector<renderer::model::CollisionShapeData> out;
#if WDX_HAS_PHYSICS
    // "Debug -> Collision Markers is on and nothing draws" has three causes that look
    // identical on screen: physics compiled out, no `.phys` in the model, every shape
    // skipped. One gated line separates them.
    const bool physDebug = std::getenv("WDX_PHYSICS_DEBUG") != nullptr;
    if (!model_.physics.has_value()) {
        if (physDebug)
            std::fprintf(stderr, "[phys] collision draw: model has no .phys\n");
        return out;
    }
    namespace w2 = ::whiteout::m2;
    const w2::PhysicsData& phys = *model_.physics;

    for (const w2::PhysicsBody& pb : phys.bodies) {
        if (pb.boneIndex >= model_.bones.size())
            continue;
        const usize first = static_cast<usize>(std::max(0, pb.shapeIndex));
        const usize count = static_cast<usize>(std::max(0, pb.shapeCount));
        for (usize s = first; s < first + count && s < phys.shapes.size(); ++s) {
            const w2::PhysicsShape& ps = phys.shapes[s];
            const usize idx = static_cast<usize>(std::max<i16>(0, ps.shapeIndex));
            renderer::model::CollisionShapeData d{};
            Matrix44f local = Matrix44f::identity();
            bool ok = false;
            switch (ps.shapeType) {
            case w2::PhysicsShapeType::Capsule:
                if (idx < phys.capsuleShapes.size()) {
                    const w2::CapsuleShape& c = phys.capsuleShapes[idx];
                    // The two fields are the **cap centres**, which is what
                    // `CollisionShapeType::Capsule` wants — so the hemisphere the fixture
                    // reaches past each of them is drawn rather than left implied.
                    d.type = static_cast<i32>(renderer::model::CollisionShapeType::Capsule);
                    d.vertices[0] = c.localPosition1;
                    d.vertices[1] = c.localPosition2;
                    d.radius = c.radius;
                    ok = true;
                }
                break;
            case w2::PhysicsShapeType::Sphere:
                if (idx < phys.sphereShapes.size()) {
                    const w2::SphereShape& sp = phys.sphereShapes[idx];
                    d.type = static_cast<i32>(renderer::model::CollisionShapeType::Sphere);
                    d.vertices[0] = sp.localPosition;
                    d.radius = sp.radius;
                    ok = true;
                }
                break;
            case w2::PhysicsShapeType::Box:
                if (idx < phys.boxShapes.size()) {
                    // Half extents about the frame's origin with the frame carried in `local` —
                    // the same `MakeBox(halfExtents, frame)` the fixture is built from. Spanning
                    // the corners in bone space instead, which is what this did, throws the
                    // rotation away, and a `.phys` box is turned onto the limb it wraps rather
                    // than aligned to it.
                    const w2::BoxShape& b = phys.boxShapes[idx];
                    d.type = static_cast<i32>(renderer::model::CollisionShapeType::Box);
                    d.vertices[0] = {-b.halfExtents.x, -b.halfExtents.y, -b.halfExtents.z};
                    d.vertices[1] = b.halfExtents;
                    // Axes down the **rows**: a `PhysicsFrame` axis is the image of a basis
                    // vector, and in a row-vector matrix that is a row. Down the columns instead
                    // transposes the frame, which on an orthonormal basis is its inverse — a box
                    // rotated the wrong way, and right-looking at rest (`wow_physics.cpp`).
                    local.data[0][0] = b.frame.axisX.x;
                    local.data[0][1] = b.frame.axisX.y;
                    local.data[0][2] = b.frame.axisX.z;
                    local.data[1][0] = b.frame.axisY.x;
                    local.data[1][1] = b.frame.axisY.y;
                    local.data[1][2] = b.frame.axisY.z;
                    local.data[2][0] = b.frame.axisZ.x;
                    local.data[2][1] = b.frame.axisZ.y;
                    local.data[2][2] = b.frame.axisZ.z;
                    local.data[3][0] = b.frame.origin.x;
                    local.data[3][1] = b.frame.origin.y;
                    local.data[3][2] = b.frame.origin.z;
                    ok = true;
                }
                break;
            case w2::PhysicsShapeType::Polytope:
                if (idx < phys.polytopeShapes.size()) {
                    const w2::PolytopeShape& hull = phys.polytopeShapes[idx];
                    if (!hull.vertices.empty()) {
                        // The hull itself, from the `PLYT` half-edges: twins sit at adjacent
                        // indices, so taking only the ones that step *forward* walks every
                        // undirected edge exactly once. Drawn as the box it spans — which is
                        // what this did — a torso plate and a shoulder plate are the same
                        // picture, and both far larger than the collider.
                        d.type = static_cast<i32>(renderer::model::CollisionShapeType::Hull);
                        d.hullPoints = hull.vertices;
                        d.hullEdges = renderer::profiles::wow::WowPolytopeEdges(hull);
                        ok = true;
                    }
                }
                break;
            }
            if (ok) {
                // `.phys` encodes only these two — there is no static body in any
                // version of the format, so the Static overlay stays empty for `.m2`.
                d.bodyKind = static_cast<i32>(pb.type == w2::PhysicsBodyType::Dynamic
                                                  ? renderer::model::CollisionBodyKind::Dynamic
                                                  : renderer::model::CollisionBodyKind::Kinematic);
                out.push_back(d);
                physicsShapeBones_.push_back(static_cast<i32>(pb.boneIndex));
                physicsShapeLocals_.push_back(local);
            }
        }
    }
    if (physDebug) {
        std::fprintf(stderr, "[phys] collision draw: %zu shapes from %zu bodies\n", out.size(),
                     phys.bodies.size());
    }
#endif
    return out;
}

} // namespace whiteout::flakes::io

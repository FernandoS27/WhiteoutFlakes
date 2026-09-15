#include "io/m3/m3_model_adapter.h"

#include "io/m3/m3_billboard.h"
#include "io/m3/m3_pose_solvers.h"
#if WDX_HAS_PHYSICS
#include "renderer/particle/particle_stages_sc2.h"
#include "renderer/profiles/sc2_heroes/sc2_cloth.h"
#include "renderer/profiles/sc2_heroes/sc2_physics.h"
#endif

#include <whiteout/models/m3/engine_compat.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string_view>
#include <unordered_map>

namespace whiteout::flakes::io {

using renderer::model::MeshBuffer;
using renderer::model::MeshData;
using renderer::model::SequenceInfo;
using renderer::model::SkeletonData;
using renderer::model::SkinWeightData;
using renderer::model::VertexAttribute;
using renderer::model::VertexSemantic;

namespace {

// The `.m3` vertex record, described rather than decoded. Offsets mirror
// VertexBuffer::initialize() exactly — see whiteout/models/m3/types.cpp:
//
//   0  position      f32 x3
//   12 boneWeights   u8  x4  (/255)
//   16 boneIndices   u8  x4
//   20 normal        u8  x4  UNORM (v/255*2-1); .w is the bitangent handedness
//   24 colour        u8  x4  BGRA, only when the VertexColor flag is set
//   .. uv0..uv4      i16 x2  each, one per UV flag
//   -4 tangent       u8  x4  UNORM, always the last four bytes; .w unused (255)
//
// UNORM, not SNORM: retail declares both basis vectors `ubyte4n` and decodes
// them `2 * v - 1` (vsmodelvertexformat.fx TranslateVert). Read as i8/127 the
// same bytes are 0.73..1.41 long and point the wrong way — that was the
// "inverted .m3 normals" the shader used to negate.
//
// Deriving this a second time here rather than asking the parser is the one
// genuinely risky thing in the M3 path, which is why mesh_buffer_test asserts
// the description agrees with getPositions()/getNormals() byte for byte.
std::vector<VertexAttribute> DescribeM3Vertex(const ::whiteout::m3::VertexBuffer& vb) {
    const u16 stride = static_cast<u16>(vb.vertexSize());
    const u16 uvBase = vb.hasVertexColors() ? 28 : 24;

    std::vector<VertexAttribute> attrs;
    attrs.push_back({VertexSemantic::Position, 0, gfx::Format::R32G32B32_FLOAT, 0});
    attrs.push_back({VertexSemantic::BoneWeights, 0, gfx::Format::R8G8B8A8_UNORM, 12});
    attrs.push_back({VertexSemantic::BoneIndices, 0, gfx::Format::R8G8B8A8_UINT, 16});
    attrs.push_back({VertexSemantic::Normal, 0, gfx::Format::R8G8B8A8_UNORM, 20});
    if (vb.hasVertexColors())
        attrs.push_back({VertexSemantic::Color, 0, gfx::Format::R8G8B8A8_UNORM, 24});
    for (u8 i = 0; i < static_cast<u8>(vb.UVsNum()); ++i) {
        // Declared, but nothing consumes it yet: `.m3` UVs are i16/2048, and
        // SNORM decodes /32767. The scale — plus REGN v5+'s per-region
        // uvMultiply/uvOffset — is material work, so the honest description
        // here is the storage layout, not a usable texture coordinate.
        attrs.push_back({VertexSemantic::TexCoord, i, gfx::Format::R16G16_SNORM,
                         static_cast<u16>(uvBase + i * 4)});
    }
    attrs.push_back(
        {VertexSemantic::Tangent, 0, gfx::Format::R8G8B8A8_UNORM, static_cast<u16>(stride - 4)});
    return attrs;
}

// The local matrix, built exactly as M3Anim_EvaluateBoneTransform builds it:
// the quaternion's row-vector rotation matrix with row i scaled by scale[i],
// and the translation in row 3. `M3RotationRows` is that rotation, shared with
// the billboard code; `Matrix44f::rotation` is the column-vector form and
// composes the wrong way round for every `.m3` matrix.
Matrix44f M3ComposeLocal(const Vector3f& t, const Quaternion& q, const Vector3f& s) {
    Matrix44f m = M3RotationRows(q);
    const f32 sc[3] = {s.x, s.y, s.z};
    for (i32 r = 0; r < 3; ++r)
        for (i32 c = 0; c < 3; ++c)
            m.data[r][c] *= sc[r];
    m.data[3][0] = t.x;
    m.data[3][1] = t.y;
    m.data[3][2] = t.z;
    return m;
}

// ---- per-type block access and interpolation -------------------------------

const ::whiteout::m3::AnimBlock<Vector2f>* BlockOf(const ::whiteout::m3::SubTrackContainer& stc,
                                                   M3TrackHandle h, const Vector2f*) {
    return h.slot == M3SdSlot::Vec2 ? &stc.sd2v[h.block] : nullptr;
}
const ::whiteout::m3::AnimBlock<Vector3f>* BlockOf(const ::whiteout::m3::SubTrackContainer& stc,
                                                   M3TrackHandle h, const Vector3f*) {
    return h.slot == M3SdSlot::Vec3 ? &stc.sd3v[h.block] : nullptr;
}
const ::whiteout::m3::AnimBlock<Quaternion>* BlockOf(const ::whiteout::m3::SubTrackContainer& stc,
                                                     M3TrackHandle h, const Quaternion*) {
    return h.slot == M3SdSlot::Quat ? &stc.sd4q[h.block] : nullptr;
}
const ::whiteout::m3::AnimBlock<f32>* BlockOf(const ::whiteout::m3::SubTrackContainer& stc,
                                              M3TrackHandle h, const f32*) {
    return h.slot == M3SdSlot::Float ? &stc.sdr3[h.block] : nullptr;
}
const ::whiteout::m3::AnimBlock<::whiteout::m3::ColorBGRA>*
BlockOf(const ::whiteout::m3::SubTrackContainer& stc, M3TrackHandle h,
        const ::whiteout::m3::ColorBGRA*) {
    return h.slot == M3SdSlot::Color ? &stc.sdcc[h.block] : nullptr;
}
const ::whiteout::m3::AnimBlock<::whiteout::u16>*
BlockOf(const ::whiteout::m3::SubTrackContainer& stc, M3TrackHandle h, const ::whiteout::u16*) {
    return h.slot == M3SdSlot::U16 ? &stc.sdu6[h.block] : nullptr;
}
const ::whiteout::m3::AnimBlock<::whiteout::i16>*
BlockOf(const ::whiteout::m3::SubTrackContainer& stc, M3TrackHandle h, const ::whiteout::i16*) {
    return h.slot == M3SdSlot::S16 ? &stc.sds6[h.block] : nullptr;
}

Vector3f MixValue(const Vector3f& a, const Vector3f& b, f32 t) {
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
}
Vector2f MixValue(const Vector2f& a, const Vector2f& b, f32 t) {
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
}
// Track-level: the raw componentwise lerp the engine uses (see m3_animation.h).
Quaternion MixTrack(const Quaternion& a, const Quaternion& b, f32 t) {
    return M3LerpQuatRaw(a, b, t);
}
Vector3f MixTrack(const Vector3f& a, const Vector3f& b, f32 t) {
    return MixValue(a, b, t);
}
Vector2f MixTrack(const Vector2f& a, const Vector2f& b, f32 t) {
    return MixValue(a, b, t);
}
f32 MixTrack(f32 a, f32 b, f32 t) {
    return a + (b - a) * t;
}
// Colours lerp per channel in float and round back to the byte — the raw
// componentwise blend, no colour-space cleverness.
::whiteout::m3::ColorBGRA MixColor(const ::whiteout::m3::ColorBGRA& a,
                                   const ::whiteout::m3::ColorBGRA& b, f32 t) {
    const auto ch = [t](u8 x, u8 y) {
        return static_cast<u8>(
            std::clamp(static_cast<f32>(x) + (static_cast<f32>(y) - static_cast<f32>(x)) * t +
                           0.5f,
                       0.0f, 255.0f));
    };
    ::whiteout::m3::ColorBGRA out;
    out.b = ch(a.b, b.b);
    out.g = ch(a.g, b.g);
    out.r = ch(a.r, b.r);
    out.a = ch(a.a, b.a);
    return out;
}
::whiteout::m3::ColorBGRA MixTrack(const ::whiteout::m3::ColorBGRA& a,
                                   const ::whiteout::m3::ColorBGRA& b, f32 t) {
    return MixColor(a, b, t);
}
// Cross-layer: rotations really do slerp when several layers combine.
Quaternion MixLayers(const Quaternion& a, const Quaternion& b, f32 t) {
    return M3SlerpQuat(a, b, t);
}
Vector3f MixLayers(const Vector3f& a, const Vector3f& b, f32 t) {
    return MixValue(a, b, t);
}
Vector2f MixLayers(const Vector2f& a, const Vector2f& b, f32 t) {
    return MixValue(a, b, t);
}
f32 MixLayers(f32 a, f32 b, f32 t) {
    return a + (b - a) * t;
}
::whiteout::m3::ColorBGRA MixLayers(const ::whiteout::m3::ColorBGRA& a,
                                    const ::whiteout::m3::ColorBGRA& b, f32 t) {
    return MixColor(a, b, t);
}

} // namespace

template <typename T>
T M3ModelAdapter::SampleRef(const ::whiteout::m3::AnimRef<T>& ref,
                            std::span<const M3Layer> layers) const {
    // An unbound reference is a constant, and its own init value is the
    // constant. Both sentinels appear in shipped data: WhiteoutLib documents
    // 0, the runtime tests for 0xFFFFFFFF.
    if (ref.animId == 0 || ref.animId == 0xFFFFFFFFu || layers.empty())
        return ref.initValue;

    const i32 row = tables_.RowOf(ref.animId);
    if (row < 0)
        return ref.initValue;

    // Bit 4 of `flags` is the *only* thing that makes a track step —
    // `M3Anim_BlendQuat_Weighted` at 0x10285442f and its F32 twin both compute
    // `interpolate = !(animRef->flags & 0x10)` and pass nothing else. The
    // field WhiteoutLib calls `interpType` is not an interpolation type at
    // sample time at all: the loader overwrites that u16 with the row this
    // property occupies in the flattened track table, which is why both
    // blenders read it as `trackTable[*ref * stride + globalStc]` and treat
    // 0xFFFF as "no track bound".
    //
    // Reading it as an interp type does not merely add a redundant test, it
    // breaks the models that need this most. A model with no sequences of its
    // own ships that u16 zeroed — there was no track for the exporter to
    // number — and animation reaches it entirely through an attached `.m3a`.
    // Across the corpus that is 75.5% of bone SRT refs on `.m3a`-driven models
    // (28868 of 38217, 175 models) against 11.4% on self-animated ones, so the
    // extra test stepped every bone of every hero model and left the shipped
    // step bit — set on 330 refs in 291027 — doing nothing it was not already
    // doing.
    //
    // The row does decide one thing, once, on the way in: each chunk's load
    // fixup runs `if (interpType == 0) flags |= 0x10` before overwriting it
    // (SC2Editor: BONE `sub_141EAB2B0`, LAYR `sub_141EABA80`), and our own
    // exporter states a stepped track exactly that way, so Zhao Yun's DontInterp
    // flame flipbook slid between atlas cells here. The fold is taken only
    // where it cannot reach the models above: the ref says its own file's
    // tracks drive it (bit 2) and the contribution is one of that file's
    // sequences. No moving shipped track states a zero row (0 over 50,068
    // `.m3` and 1,110 `.m3a` files).
    const bool stepBit = (ref.flags & 0x10u) != 0;
    const bool rowSteps = ref.interpType == 0 && (ref.flags & 0x4u) != 0;
    const std::size_t ownSequences = model_.sequences.size();

    struct Contribution {
        T value;
        f32 w;
    };
    Contribution contribs[16];
    int count = 0;
    f32 budget = kM3StartBudget;
    ::whiteout::u64 visited = 0;

    for (const M3Layer& l : layers) {
        if (l.play < 64 && ((visited >> l.play) & 1u) != 0)
            continue; // this play already contributed

        const M3TrackHandle h = tables_.At(row, l.stc);
        // Global index: the container may live in the model or in any attached
        // `.m3a`, so it is resolved through the tables, never by indexing
        // `model_`.
        const ::whiteout::m3::SubTrackContainer* coll = tables_.StcAt(l.stc);
        const ::whiteout::m3::AnimBlock<T>* blk =
            (h.Valid() && coll) ? BlockOf(*coll, h, static_cast<const T*>(nullptr)) : nullptr;

        // A transparent layer with no track abstains entirely: it neither
        // contributes nor spends budget, so lower layers show through. This is
        // the whole of split body.
        if (!blk && l.transparent)
            continue;

        // Past that filter the bracket is claimed either way — an opaque
        // default-fill counts, verified at the stamp site in
        // M3Anim_BlendVec3_Weighted.
        if (l.play < 64)
            visited |= (::whiteout::u64{1} << l.play);

        T value;
        if (blk && !blk->keys.empty()) {
            const bool interpolate = !stepBit && !(rowSteps && l.sequence < ownSequences);
            const M3KeySpan sp = M3LocateKey(blk->timestamps, l.timeMs, l.loop, interpolate);
            if (!sp.valid) {
                // A bound but unusable track spends its weight and contributes
                // nothing — reproduced, not repaired.
                budget -= l.weight;
                if (budget <= kM3BudgetEpsilon)
                    break;
                continue;
            }
            const std::size_t last = blk->keys.size() - 1;
            const std::size_t i0 = (std::min)(sp.i0, last);
            const std::size_t i1 = (std::min)(sp.i1, last);
            value = (i0 == i1) ? blk->keys[i0] : MixTrack(blk->keys[i0], blk->keys[i1], sp.frac);
        } else {
            // Opaque layer with no track: force the property's default, which
            // pulls the channel back toward bind pose at full weight.
            value = ref.initValue;
        }

        if (count < 16)
            contribs[count++] = {value, l.weight};
        budget -= l.weight;
        if (budget <= kM3BudgetEpsilon)
            break;
    }

    if (count == 0)
        return ref.initValue;
    if (count == 1 || budget > kM3SettledBudget)
        return contribs[0].value;

    // Overspend is absorbed by the lowest-priority contributor so the weights
    // sum to exactly one.
    if (budget < 0.0f)
        contribs[count - 1].w += budget;

    // Combine lowest priority first, each step smoothstepped against the
    // weight accumulated so far.
    T out = contribs[count - 1].value;
    f32 acc = contribs[count - 1].w;
    for (int i = count - 2; i >= 0; --i) {
        out = MixLayers(out, contribs[i].value, M3SmoothstepFactor(acc, contribs[i].w));
        acc += contribs[i].w;
    }
    return out;
}

::whiteout::u32 M3ModelAdapter::SampleRefOverride(const ::whiteout::m3::AnimRef<::whiteout::u32>& ref,
                                                  std::span<const M3Layer> layers) const {
    // Override mode: every contributor enters at a flat weight of 1.0, so the
    // first one past the transparent/opaque filter wins outright and nothing is
    // blended. Discrete channels have no meaningful midpoint — a bone is not
    // 40% visible — which is why the engine samples them through
    // M3Anim_BlendBool32_Override_Flags rather than the weighted worker.
    if (ref.animId == 0 || ref.animId == 0xFFFFFFFFu || layers.empty())
        return ref.initValue;

    const i32 row = tables_.RowOf(ref.animId);
    if (row < 0)
        return ref.initValue;

    ::whiteout::u64 visited = 0;
    for (const M3Layer& l : layers) {
        if (l.play < 64 && ((visited >> l.play) & 1u) != 0)
            continue;
        const M3TrackHandle h = tables_.At(row, l.stc);
        const ::whiteout::m3::SubTrackContainer* collPtr = tables_.StcAt(l.stc);
        if (!collPtr)
            continue;
        const auto& coll = *collPtr;

        // **A discrete u32 channel lands in `SDFG`, not `SDU3`.** Both hold
        // four-byte keys and the engine reaches them through one generic
        // indexer, so the file is free to pick either — and it always picks
        // slot 11: measured over the 54724-model corpus, 30617 keyed bone
        // visibilities and all 598 keyed `PHRB.dynamicState` resolve there, and
        // not one to slot 10. Accepting only the u32 slot compiles, runs, and
        // silently answers `initValue` for every keyed channel in shipped
        // content, which renders as an animation that simply has no visibility
        // track.
        const bool isFlag = h.Valid() && h.slot == M3SdSlot::Flag &&
                            h.block < coll.sdfg.size() && !coll.sdfg[h.block].keys.empty();
        const bool isU32 = h.Valid() && h.slot == M3SdSlot::U32 && h.block < coll.sdu3.size() &&
                           !coll.sdu3[h.block].keys.empty();
        if (!isFlag && !isU32 && l.transparent)
            continue;
        if (l.play < 64)
            visited |= (::whiteout::u64{1} << l.play);
        if (!isFlag && !isU32)
            return ref.initValue;

        // Discrete: never interpolated, whatever the ref's interp type says.
        const std::vector<::whiteout::i32>& stamps =
            isFlag ? coll.sdfg[h.block].timestamps : coll.sdu3[h.block].timestamps;
        const M3KeySpan sp = M3LocateKey(stamps, l.timeMs, l.loop, /*interpolate*/ false);
        if (!sp.valid)
            return ref.initValue;
        if (isFlag) {
            const auto& keys = coll.sdfg[h.block].keys;
            return keys[(std::min)(sp.i0, keys.size() - 1)].value;
        }
        const auto& keys = coll.sdu3[h.block].keys;
        return keys[(std::min)(sp.i0, keys.size() - 1)];
    }
    return ref.initValue;
}

std::shared_ptr<M3ModelAdapter> M3ModelAdapter::Load(const ContentRef& ref,
                                                     std::span<const ::whiteout::u8> bytes) {
    if (bytes.empty())
        return nullptr;
    ::whiteout::m3::Parser parser;
    ::whiteout::m3::Model model;
    // The web build compiles -fno-exceptions, where `try` is a hard error.
    // The parser collects issues instead of throwing, so the handler only
    // ever sees what the STL raised on malformed input.
#if defined(__cpp_exceptions)
    try {
        model = parser.parse(bytes);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[m3] parse failed for '%s': %s\n", ref.Describe().c_str(), e.what());
        return nullptr;
    }
#else
    model = parser.parse(bytes);
#endif
    if (parser.hasIssues()) {
        // Issues are not necessarily fatal — the parser reports what it
        // skipped. Surface them rather than letting a half-read model look
        // like a clean one.
        for (const auto& issue : parser.getIssues())
            std::fprintf(stderr, "[m3] %s\n", issue.c_str());
    }
    // An `.m3` is also the container for effect-only and physics-only assets,
    // which carry no drawable mesh at all. That used to end the load, because
    // a mesh was the only thing this renderer could draw out of one. An
    // emitter draws now, and the effect-only files are exactly where the small
    // single-emitter cases live — `ZealotWeaponImpact.m3` is one `PAR_`, one
    // bone and no `REGN`, and refusing it made the whole content class
    // invisible rather than merely mesh-less. So the refusal is now about
    // having NOTHING to draw, not about having no mesh.
    const bool hasMesh = !model.divisions.empty() && model.vertices.vertexCount() != 0;
    const bool hasEffects = !model.particleEmitters.empty() || !model.ribbonEmitters.empty();
    if (!hasMesh && !hasEffects) {
        std::fprintf(stderr, "[m3] no geometry and no emitters in '%s'\n",
                     ref.Describe().c_str());
        return nullptr;
    }
    return std::make_shared<M3ModelAdapter>(std::move(model));
}

M3ModelAdapter::M3ModelAdapter(::whiteout::m3::Model model) : model_(std::move(model)) {
    // Before anything reads a material: past this point the model has no
    // data-driven maps left that could have been standard ones.
    M3RestoreDataDrivenMaterials(model_);

    // Division 0 is the highest detail level. LOD selection is a later phase;
    // taking one and saying so beats taking whichever happens to be first
    // without noticing there were others.
    divisionIndex_ = 0;
    if (divisionIndex_ < model_.divisions.size())
        regionCount_ = model_.divisions[divisionIndex_].regions.size();
    BuildEmittedRegions();
    BuildBoneBillboards();
    RebuildAnimationTables();
#if WDX_HAS_PHYSICS
    if (auto build = renderer::profiles::sc2_heroes::Sc2BuildCloth(model_); !build.pieces.empty()) {
        cloth_ =
            std::make_shared<const renderer::profiles::sc2_heroes::Sc2ClothBuild>(std::move(build));
    }
#endif
    BuildClothGeosetMap();
}

void M3ModelAdapter::BuildBoneBillboards() {
    boneBillboard_.clear();
    if (model_.billboardBehaviors.empty() || model_.bones.empty())
        return;
    boneBillboard_.assign(model_.bones.size(), -1);
    bool any = false;
    for (std::size_t i = 0; i < model_.billboardBehaviors.size(); ++i) {
        const std::size_t bone = model_.billboardBehaviors[i].boneIndex;
        if (bone >= boneBillboard_.size())
            continue;
        boneBillboard_[bone] = static_cast<i32>(i);
        any = true;
    }
    if (!any)
        boneBillboard_.clear();
}

void M3ModelAdapter::RebuildAnimationTables() {
    std::vector<const ::whiteout::m3::Model*> attached;
    attached.reserve(animModels_.size());
    for (const auto& m : animModels_)
        attached.push_back(m.get());
    tables_.Build(model_, attached);

    // Restate where each file's sequences landed, so a host can label them
    // without re-deriving the layout.
    std::size_t next = model_.sequences.size();
    for (std::size_t i = 0; i < attached_.size(); ++i) {
        attached_[i].firstSequence = next;
        attached_[i].sequenceCount = animModels_[i]->sequences.size();
        next += attached_[i].sequenceCount;
    }
}

bool M3ModelAdapter::AttachAnimationFile(std::string label,
                                         std::span<const ::whiteout::u8> bytes) {
    if (bytes.empty())
        return false;
    for (const auto& a : attached_) {
        if (a.label == label) {
            // Same rejection StarCraft II makes — it dedupes on the stored path
            // before adding a record — but two mods' files can share a stem, so
            // say which one was refused rather than failing mute.
            std::fprintf(stderr, "[m3a] '%s' is already attached — ignored\n", label.c_str());
            return false;
        }
    }

    // An `.m3a` is an ordinary MD34 model — same MODL versions, same chunk
    // layout — that happens to carry bones and no vertices. Measured over all
    // 1110 shipped files: every one has BONE, none has vertex data. So it goes
    // through the same parser, and only `Load`'s drawable-geometry check has to
    // be skipped.
    ::whiteout::m3::Model anim;
    ::whiteout::m3::Parser parser;
#if defined(__cpp_exceptions)
    try {
        anim = parser.parse(bytes);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[m3a] parse failed for '%s': %s\n", label.c_str(), e.what());
        return false;
    }
#else
    anim = parser.parse(bytes);
#endif
    if (anim.sequences.empty()) {
        std::fprintf(stderr, "[m3a] '%s' carries no sequences — not attached\n", label.c_str());
        return false;
    }

    animModels_.push_back(std::make_unique<::whiteout::m3::Model>(std::move(anim)));
    AttachedAnimation info;
    info.label = std::move(label);
    attached_.push_back(std::move(info));
    RebuildAnimationTables();
    return true;
}

bool M3ModelAdapter::DetachAnimationFile(std::size_t index) {
    if (index >= animModels_.size())
        return false;
    animModels_.erase(animModels_.begin() + static_cast<std::ptrdiff_t>(index));
    attached_.erase(attached_.begin() + static_cast<std::ptrdiff_t>(index));
    RebuildAnimationTables();
    return true;
}

void M3ModelAdapter::ClearAnimationFiles() {
    if (animModels_.empty())
        return;
    animModels_.clear();
    attached_.clear();
    RebuildAnimationTables();
}

void M3ModelAdapter::BuildClothGeosetMap() {
    geosetClothPiece_.assign(emittedRegions_.size(), -1);
    geosetClothProxy_.assign(emittedRegions_.size(), 0);
    // `RegionFlag::ClothSimulated` alone decides that a region is a cage, and
    // retail skips it whether or not anything simulates it. Keying the skip on
    // the built cloth instead left the cage drawing for every cloth Sc2BuildCloth
    // rejects — 20 of the corpus's 189, Greymane's coat and Jaina Modern's skirt
    // among them, both over the 256-particle ceiling. It draws the visible
    // garment's material over the proxy's own unrelated UVs, which reads as
    // patches of the wrong texture.
    for (std::size_t g = 0; g < geosetRegionFlags_.size(); ++g) {
        if ((geosetRegionFlags_[g] &
             static_cast<::whiteout::u32>(::whiteout::m3::RegionFlag::ClothSimulated)) != 0)
            geosetClothProxy_[g] = 1;
    }
#if WDX_HAS_PHYSICS
    if (!cloth_)
        return;
    for (std::size_t p = 0; p < cloth_->pieces.size(); ++p) {
        const auto& piece = cloth_->pieces[p];
        for (std::size_t g = 0; g < emittedRegions_.size(); ++g) {
            if (emittedRegions_[g] == piece.simRegion)
                geosetClothProxy_[g] = 1;
            for (std::size_t r : piece.influencedRegions) {
                if (emittedRegions_[g] == r)
                    geosetClothPiece_[g] = static_cast<i32>(p);
            }
        }
    }
#endif
}

void M3ModelAdapter::BuildEmittedRegions() {
    emittedRegions_.clear();
    emittedMaterials_.clear();
    emittedWeights_.clear();
    geosetRegionFlags_.clear();
    geosetVisibilityBone_.clear();
    if (divisionIndex_ >= model_.divisions.size())
        return;
    const auto& div = model_.divisions[divisionIndex_];
    const std::size_t vertexCount = model_.vertices.vertexCount();
    const std::size_t stride = model_.vertices.vertexSize();
    const std::size_t blobSize = model_.vertices.data.size();

    // A constant weight of one: what a geoset that is not a composite pass
    // carries, and what a section whose multiplier nothing drives evaluates to.
    ::whiteout::m3::AnimRef<f32> unitWeight;
    unitWeight.animId = 0;
    unitWeight.flags = 0;
    unitWeight.initValue = 1.0f;

    for (std::size_t r = 0; r < div.regions.size(); ++r) {
        const auto& region = div.regions[r];
        if (region.vertexCount == 0 || region.indexCount == 0)
            continue;
        const std::size_t vEnd = static_cast<std::size_t>(region.firstVertex) + region.vertexCount;
        const std::size_t iEnd = static_cast<std::size_t>(region.firstIndex) + region.indexCount;
        if (vEnd > vertexCount || iEnd > div.faces.size())
            continue;
        if (vEnd * stride > blobSize)
            continue;

        // The first batch naming the region is the one drawn, so it is also the
        // one whose bone gates the draw and whose MATM entry names the material.
        // Measured over the HotS corpus, no region carries a second batch, so
        // this pick is exact rather than a simplification there.
        ::whiteout::u16 gate = 0xFFFFu;
        ::whiteout::u32 matm = 0xFFFFFFFFu;
        for (const auto& batch : div.batches) {
            if (batch.regionIndex == r) {
                gate = batch.boneCount;
                matm = batch.materialIndex;
                break;
            }
        }

        // A `CMP_` composite is a STACK of materials over one region, each
        // section scaled by its own animated multiplier — not a choice between
        // them. It becomes one geoset per section, in section order, so the
        // later passes draw over the earlier ones at equal depth exactly as
        // retail's multi-draw does. 1270 HotS files and 2137 StarCraft II ones
        // draw through one, and 92% of them weight every section alike — so
        // picking "the heaviest section" was really picking the first, which
        // put the death ragdolls' team-coloured dissolve GLOW on the whole body
        // instead of the skin underneath it.
        const auto push = [&](::whiteout::u32 material,
                              const ::whiteout::m3::AnimRef<f32>& weight) {
            emittedRegions_.push_back(r);
            emittedMaterials_.push_back(material);
            emittedWeights_.push_back(weight);
            geosetRegionFlags_.push_back(static_cast<::whiteout::u32>(region.flags));
            geosetVisibilityBone_.push_back(gate);
        };

        const auto* composite = M3CompositeForMaterial(model_, matm);
        if (!composite) {
            push(matm, unitWeight);
            continue;
        }
        for (const auto& section : composite->sections) {
            // A section pointing at anything the surface table cannot resolve
            // would emit a geoset that never draws; skip it rather than pay for
            // a duplicate vertex range.
            if (M3StandardForMaterial(model_, section.materialIndex))
                push(section.materialIndex, section.mapMultiplier);
        }
    }
}

std::string M3CleanPath(const std::string& raw) {
    // The Ref<CHAR> keeps its terminator: size() is one past the text, and the
    // embedded NUL survives every string operation silently. c_str() is the
    // one honest exit.
    return std::string(raw.c_str());
}

bool M3LayerHasTexture(const ::whiteout::m3::TextureLayer& layer) {
    if ((static_cast<u32>(layer.flags) & static_cast<u32>(::whiteout::m3::TextureLayerFlag::Color)) != 0)
        return false;
    return !M3CleanPath(layer.texturePath).empty();
}

bool M3LayerActive(const ::whiteout::m3::TextureLayer& layer) {
    if ((static_cast<u32>(layer.flags) & static_cast<u32>(::whiteout::m3::TextureLayerFlag::Color)) != 0)
        return true;
    return M3LayerHasTexture(layer);
}

void M3ComposeUvTransform(const Vector2f& offset, const Vector3f& angle, const Vector2f& tiling,
                          f32 row0[4], f32 row1[4]) {
    // SC2 (sub_102ABBDE0) rotates and scales about the texture centre (0.5,0.5)
    // and applies the offset in source space, ahead of the rotation:
    //   M = T(+0.5) · S · R(angle.z) · T(-(0.5 + offset)).
    // Rotating about the origin or adding the offset after the rotation (as this
    // once did) sends a rotated layer's scroll onto the wrong axis and mismaps
    // the image — which is what tore the Tyrael wing ribbons. Only angle.z reaches
    // the 2D result, matching every shipped layer.
    const f32 c = std::cos(angle.z);
    const f32 s = std::sin(angle.z);
    const f32 cx = 0.5f + offset.x;
    const f32 cy = 0.5f + offset.y;
    row0[0] = tiling.x * c;
    row0[1] = -tiling.x * s;
    row0[2] = 0.0f;
    row0[3] = tiling.x * (s * cy - c * cx) + 0.5f;
    row1[0] = tiling.y * s;
    row1[1] = tiling.y * c;
    row1[2] = 0.0f;
    row1[3] = tiling.y * (-s * cx - c * cy) + 0.5f;
}

const ::whiteout::m3::TextureLayer* M3LayerForSlot(const ::whiteout::m3::StandardMaterial& mat,
                                                   M3LayerSlot slot) {
    auto get = [](const std::optional<::whiteout::m3::TextureLayer>& l)
        -> const ::whiteout::m3::TextureLayer* { return l ? &*l : nullptr; };
    switch (slot) {
    case M3LayerSlot::Diffuse:
        return get(mat.diffuseLayer);
    case M3LayerSlot::Decal:
        return get(mat.decalLayer);
    case M3LayerSlot::Specular:
        return get(mat.specularLayer);
    case M3LayerSlot::Emissive:
        return get(mat.emissiveLayer1);
    case M3LayerSlot::Emissive2:
        return get(mat.emissiveLayer2);
    case M3LayerSlot::Normal:
        return get(mat.normalLayer);
    case M3LayerSlot::AlphaMask:
        return get(mat.alphaLayer1);
    case M3LayerSlot::Gloss:
        return get(mat.glossLayer);
    case M3LayerSlot::AlphaMask2:
        return get(mat.alphaLayer2);
    case M3LayerSlot::Environment:
        return get(mat.environmentLayer);
    case M3LayerSlot::EnvironmentMask:
        return get(mat.environmentMaskLayer);
    default:
        return nullptr;
    }
}

const ::whiteout::m3::StandardMaterial*
M3StandardForMaterial(const ::whiteout::m3::Model& model, ::whiteout::u32 matmIndex) {
    if (matmIndex >= model.materialMaps.size())
        return nullptr;
    const auto& map = model.materialMaps[matmIndex];
    if (map.materialType != ::whiteout::m3::MaterialType::Standard ||
        map.materialIndex >= model.standardMaterials.size())
        return nullptr;
    return &model.standardMaterials[map.materialIndex];
}

const ::whiteout::m3::CompositeMaterial*
M3CompositeForMaterial(const ::whiteout::m3::Model& model, ::whiteout::u32 matmIndex) {
    if (matmIndex >= model.materialMaps.size())
        return nullptr;
    const auto& map = model.materialMaps[matmIndex];
    if (map.materialType != ::whiteout::m3::MaterialType::Composite ||
        map.materialIndex >= model.compositeMaterials.size())
        return nullptr;
    return &model.compositeMaterials[map.materialIndex];
}

std::vector<M3TextureRef> CollectM3Textures(const ::whiteout::m3::Model& model) {
    std::vector<M3TextureRef> out;
    std::unordered_map<std::string, std::size_t> seen; // lowercase key -> index
    for (const auto& mat : model.standardMaterials) {
        for (u32 s = 0; s < static_cast<u32>(M3LayerSlot::Count); ++s) {
            const auto* layer = M3LayerForSlot(mat, static_cast<M3LayerSlot>(s));
            if (!layer || !M3LayerHasTexture(*layer))
                continue;
            const std::string path = M3CleanPath(layer->texturePath);
            const bool cube = static_cast<M3LayerSlot>(s) == M3LayerSlot::Environment;
            // Only the Normal slot is declared linear. The mask and gloss
            // slots are data too and still sample through an sRGB view — 56%
            // of them read alpha, which that view leaves alone, so it is a
            // separate question rather than this one.
            const bool linear = static_cast<M3LayerSlot>(s) == M3LayerSlot::Normal;
            // Cube-ness and linearity join the key: the same file wanted two
            // ways is two GPU textures, and one view cannot answer both
            // bindings.
            std::string key;
            if (cube)
                key += "cube:";
            if (linear)
                key += "lin:";
            key += path;
            std::transform(key.begin(), key.end(), key.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (seen.contains(key))
                continue;
            seen.emplace(std::move(key), out.size());
            using ::whiteout::m3::TextureLayerFlag;
            const u32 f = static_cast<u32>(layer->flags);
            M3TextureRef ref;
            ref.path = path;
            ref.wrapFlags = ((f & static_cast<u32>(TextureLayerFlag::UVWrapX)) ? 0x1u : 0u) |
                            ((f & static_cast<u32>(TextureLayerFlag::UVWrapY)) ? 0x2u : 0u);
            ref.cube = cube;
            ref.linear = linear;
            out.push_back(std::move(ref));
        }
    }
    return out;
}

namespace {

using MaddMaterial = ::whiteout::m3::StandardMaterial;
using MaddLayerSlot = std::optional<::whiteout::m3::TextureLayer> MaddMaterial::*;

/// Every layer of a StandardMaterial, wider than `M3LayerForSlot`, which stops
/// at the eleven the renderer binds.
const std::array<MaddLayerSlot, 14>& MaddLayerSlots() {
    static const std::array<MaddLayerSlot, 14> slots = {
        &MaddMaterial::diffuseLayer,        &MaddMaterial::decalLayer,
        &MaddMaterial::specularLayer,       &MaddMaterial::glossLayer,
        &MaddMaterial::emissiveLayer1,      &MaddMaterial::emissiveLayer2,
        &MaddMaterial::environmentLayer,    &MaddMaterial::environmentMaskLayer,
        &MaddMaterial::alphaLayer1,         &MaddMaterial::alphaLayer2,
        &MaddMaterial::normalLayer,         &MaddMaterial::heightLayer,
        &MaddMaterial::lightMapLayer,       &MaddMaterial::ambientOcclusionLayer};
    return slots;
}

/// The address mode, which the record does not reliably carry: it only names one
/// inside a per-layer UV transform, and most layers have none, so 5778 of the
/// 7015 restored layers (82%) come back with no wrap bit at all. Clamp is not
/// what that means — shipped MAT_ content wraps on 14464 of 14534 StarCraft II
/// layers and 27664 of 28267 Heroes ones — and a clamped layer whose UVs leave
/// [0,1] samples one edge column across the whole surface: half of Tracer's body
/// flat-shaded, and a specular map reduced to a constant.
void MaddWrapLayers(MaddMaterial& mat) {
    using ::whiteout::m3::TextureLayerFlag;
    for (const auto slot : MaddLayerSlots())
        if (auto& layer = mat.*slot; layer)
            layer->flags |= TextureLayerFlag::UVWrapX | TextureLayerFlag::UVWrapY;
}

/// Blend ops and masks a shader graph never names, whose defaults are wrong
/// twice over: Mod darkens an emissive layer instead of lighting with it, and an
/// unmasked cubemap washes the surface out to the reflection's own colour. These
/// are the conventions the fixed-function records hold — §2.7 for the counts.
void MaddApproximateLayerOps(::whiteout::m3::StandardMaterial& mat) {
    using ::whiteout::m3::LayerBlendOp;
    mat.emissiveBlendMode1 = LayerBlendOp::Add;
    mat.emissiveBlendMode2 = LayerBlendOp::Add;
    if (mat.environmentLayer && !mat.environmentMaskLayer && mat.specularLayer)
        mat.environmentMaskLayer = mat.specularLayer;
}

/// The `LayerBlendOp` an `Envio` layer is applied with, or -1 when the record
/// names none.
i32 MaddEnvioBlendOp(const ::whiteout::m3::DataDrivenMaterial& madd) {
    for (const auto& group : madd.decodeProperties().groups) {
        for (const auto& prop : group.properties) {
            if (prop.name != "EnvioControl" || prop.data.size() < sizeof(u32))
                continue;
            u32 op = 0;
            std::memcpy(&op, prop.data.data(), sizeof(op));
            return static_cast<i32>(op);
        }
    }
    return -1;
}

} // namespace

M3DataDrivenResult M3RestoreDataDrivenMaterials(::whiteout::m3::Model& model) {
    using ::whiteout::m3::BlendMode;
    using ::whiteout::m3::LayerBlendOp;
    using ::whiteout::m3::MaterialType;

    M3DataDrivenResult out;
    if (model.dataDrivenMaterials.empty())
        return out;
    // The version the restored records are stamped with. A StandardMaterial
    // built by the conversion carries none, and a writer takes a chunk's
    // version from its first element -- so on a Heroes model with no MAT_ of
    // its own (2581 of 2661 records across the corpus) leaving it unset writes
    // MAT_ v0xFFFFFFFF, which no engine and not even this parser will read.
    // Match what the model already has; failing that the cap Heroes enforces,
    // which StarCraft II also accepts.
    const i32 standardVersion =
        model.standardMaterials.empty()
            ? static_cast<i32>(::whiteout::m3::HOTS_MAX_STANDARD_MATERIAL_VERSION)
            : model.standardMaterials.front().getVersion();

    // MADD -> StandardMaterial index, or -1 for a record with no standard form.
    // Keyed because several MATM entries can name the same record and the
    // conversion is the expensive part.
    std::unordered_map<u32, i64> rebuilt;
    for (auto& map : model.materialMaps) {
        if (map.materialType != MaterialType::DataDriven ||
            map.materialIndex >= model.dataDrivenMaterials.size())
            continue;
        const auto [it, fresh] = rebuilt.try_emplace(map.materialIndex, i64{-1});
        if (fresh) {
            const auto& madd = model.dataDrivenMaterials[map.materialIndex];
            auto conv = madd.toStandardMaterial();
            const bool exact = conv.converted;
            if (!exact)
                conv = madd.approximateStandardMaterial();
            if (conv.converted) {
                MaddWrapLayers(conv.material);
                if (!exact)
                    MaddApproximateLayerOps(conv.material);
                // MAT_.blendMode survives the forward conversion in the field
                // WhiteoutLib still calls `unknown124` — measured across all
                // 2582 shipped records, SC2_MATERIAL_RENDERING_DESIGN.md §2.7.
                // The 15 values past the enum blend rather than paint an FX
                // reticle as an opaque quad.
                conv.material.blendMode = madd.unknown124 <= static_cast<u32>(BlendMode::Mod2x)
                                              ? static_cast<BlendMode>(madd.unknown124)
                                              : BlendMode::AlphaBlend;
                // The op the reflection is applied with, which the
                // conversion misses because the record files it under `Envio`
                // and the layer under `EnvironmentMap`. Left at the default
                // 0 = Mod it multiplies a hero's skin to black; a graph names
                // no op at all, and Add is the likeness that does not (§2.7).
                if (conv.material.environmentLayer) {
                    const i32 op = MaddEnvioBlendOp(madd);
                    conv.material.layerBlendMode =
                        op >= 0 && op <= static_cast<i32>(LayerBlendOp::AddNoAlpha)
                            ? static_cast<LayerBlendOp>(op)
                            : LayerBlendOp::Add;
                }
                conv.material.forceVersion(standardVersion);
                it->second = static_cast<i64>(model.standardMaterials.size());
                model.standardMaterials.push_back(std::move(conv.material));
                ++(exact ? out.restored : out.approximated);
            } else {
                ++out.refused;
                std::fprintf(stderr, "[m3] '%s': material '%s' has no standard form — %s\n",
                             model.name.c_str(), madd.materialName.c_str(), conv.blocker.c_str());
            }
        }
        if (it->second >= 0) {
            map.materialType = MaterialType::Standard;
            map.materialIndex = static_cast<u32>(it->second);
        }
    }
    return out;
}

std::vector<renderer::model::TextureData> M3ModelAdapter::GetTextures() {
    const std::vector<M3TextureRef> refs = CollectM3Textures(model_);
    std::vector<renderer::model::TextureData> out;
    out.reserve(refs.size());
    for (std::size_t i = 0; i < refs.size(); ++i) {
        renderer::model::TextureData td;
        td.textureId = static_cast<i32>(i);
        td.replaceableId = 0;
        td.width = 0;
        td.height = 0;
        td.wrapFlags = refs[i].wrapFlags;
        td.cubeMap = refs[i].cube;
        td.linearData = refs[i].linear;
        td.sharedKey = refs[i].path;
        out.push_back(std::move(td));
    }
    return out;
}

std::vector<MeshData> M3ModelAdapter::GetMeshes() {
    std::vector<MeshData> out;
    if (divisionIndex_ >= model_.divisions.size())
        return out;
    const auto& div = model_.divisions[divisionIndex_];

    // getPositions() decodes the whole blob, so it is called once rather than
    // per region — the stride walk is the expensive part and it is identical
    // for every region. Still needed with the baked path: `positions` is the
    // CPU-side copy GetBounds and the sort centroid read.
    const std::vector<::whiteout::Vector3f> positions = model_.vertices.getPositions();

    // The blob is model-global and a region is a contiguous vertex range, so
    // each region's buffer is a stride-aligned slice of it. No decode, no
    // repack — the bytes reaching the GPU are the bytes that were in the file.
    const std::vector<VertexAttribute> attrs = DescribeM3Vertex(model_.vertices);
    const std::size_t stride = model_.vertices.vertexSize();
    const std::vector<u8>& blob = model_.vertices.data;

    // Regions, in file order — see the header for why regions rather than
    // batches, and for the REGN v2 gap this counts. Which regions survive is
    // decided once in BuildEmittedRegions so every per-geoset accessor agrees
    // on what `geosetId` means.
    std::size_t noFaceRange = 0;
    for (const auto& region : div.regions)
        if (region.vertexCount != 0 && region.indexCount == 0)
            ++noFaceRange;

    out.reserve(emittedRegions_.size());
    for (std::size_t g = 0; g < emittedRegions_.size(); ++g) {
        const auto& region = div.regions[emittedRegions_[g]];

        const std::size_t vBegin = region.firstVertex;
        const std::size_t vEnd = vBegin + region.vertexCount;
        const std::size_t iBegin = region.firstIndex;
        const std::size_t iEnd = iBegin + region.indexCount;

        MeshData mesh;
        mesh.geosetId = static_cast<i32>(g);
        // The MATM index BuildEmittedRegions settled on — the region's first
        // batch, or one composite section of it.
        const ::whiteout::u32 matm = emittedMaterials_[g];
        mesh.materialId = matm < model_.materialMaps.size() ? static_cast<i32>(matm) : -1;
        mesh.lod = 0;
        mesh.positions.assign(positions.begin() + static_cast<std::ptrdiff_t>(vBegin),
                              positions.begin() + static_cast<std::ptrdiff_t>(vEnd));

        const std::size_t byteBegin = vBegin * stride;
        const std::size_t byteEnd = vEnd * stride;
        if (byteEnd > blob.size())
            continue; // vertexCount disagrees with the blob; skip the region
        mesh.baked.stride = static_cast<u32>(stride);
        mesh.baked.attributes = attrs;
        mesh.baked.data.assign(blob.begin() + static_cast<std::ptrdiff_t>(byteBegin),
                               blob.begin() + static_cast<std::ptrdiff_t>(byteEnd));

        mesh.indices.reserve(region.indexCount);
        for (std::size_t i = iBegin; i < iEnd; ++i)
            mesh.indices.push_back(static_cast<u32>(div.faces[i]));
        RewriteClothSkin(g, mesh);
        out.push_back(std::move(mesh));
    }
    if (noFaceRange > 0) {
        // One line, not one per region: a portrait model has six of these and
        // the cause is the same for all of them.
        std::fprintf(stderr,
                     "[m3] '%s': %zu of %zu regions carry no face range (REGN v%d — the parser "
                     "reads firstIndex/indexCount only from v3)\n",
                     model_.name.c_str(), noFaceRange, div.regions.size(),
                     div.regions.empty() ? -1 : div.regions[0].getVersion());
    }
    return out;
}

void M3ModelAdapter::RewriteClothSkin([[maybe_unused]] std::size_t geoset,
                                      [[maybe_unused]] MeshData& mesh) const {
#if WDX_HAS_PHYSICS
    if (!cloth_ || geoset >= geosetClothPiece_.size() || geosetClothPiece_[geoset] < 0)
        return;
    const auto& piece = cloth_->pieces[static_cast<std::size_t>(geosetClothPiece_[geoset])];
    const auto& c = model_.clothPhysics[piece.chunkIndex];
    const std::size_t region = emittedRegions_[geoset];

    const ::whiteout::m3::ClothProxy* proxy = nullptr;
    for (const auto& p : c.proxies) {
        if (p.proxyIndex == region && p.clothIndex == piece.simRegion)
            proxy = &p;
    }
    const std::size_t stride = mesh.baked.stride;
    const std::size_t count = mesh.baked.data.size() / (stride != 0 ? stride : 1);
    if (proxy == nullptr || stride < 20 || proxy->proxyVertices.size() != count ||
        proxy->proxyWeights.size() != count) {
        return;
    }

    // Offsets 12 and 16 are `BoneWeights` and `BoneIndices` — see
    // `DescribeM3Vertex`. Overwriting them in place is the whole rewrite: the
    // geoset's palette becomes the cloth's particles (`GetSkinWeights`), and a
    // vertex that named a bone now names the particle that carries it.
    for (std::size_t v = 0; v < count; ++v) {
        const ::whiteout::u64 slots = proxy->proxyVertices[v];
        const ::whiteout::u32 weights = proxy->proxyWeights[v];
        ::whiteout::u8* rec = mesh.baked.data.data() + v * stride;
        for (int k = 0; k < 4; ++k) {
            const auto src = static_cast<std::size_t>((slots >> (16 * k)) & 0xFFFFu);
            ::whiteout::u8 w = static_cast<::whiteout::u8>((weights >> (8 * k)) & 0xFFu);
            // 0xFFFF is the format's "no influence"; so is a source vertex the
            // cloth build dropped. Both become slot 0 at weight 0, which is what
            // the solver's own absent-anchor lanes do.
            const ::whiteout::i16 particle =
                (src < piece.oldToNew.size()) ? piece.oldToNew[src] : ::whiteout::i16{-1};
            const bool live = particle >= 0 &&
                              static_cast<std::size_t>(particle) < piece.particleCount;
            if (!live)
                w = 0;
            rec[16 + k] = live ? static_cast<::whiteout::u8>(particle) : 0u;
            rec[12 + k] = w;
        }
    }
#endif
}

::whiteout::flakes::ModelBounds M3ModelAdapter::GetBounds() {
    ::whiteout::flakes::ModelBounds b;
    const auto& e = model_.bounds;
    const bool degenerate = e.max.x <= e.min.x && e.max.y <= e.min.y && e.max.z <= e.min.z;
    if (!degenerate) {
        b.min = {e.min.x, e.min.y, e.min.z};
        b.max = {e.max.x, e.max.y, e.max.z};
        b.valid = true;
        return b;
    }
    // Fall through to the interface's union-over-positions default when the
    // model carries no usable box of its own. Common in practice: MODL.bounds
    // is the *animated* extent and a model with no sequences leaves it zeroed.
    return IModelSource::GetBounds();
}

namespace {

/// @brief What an `SDEV` key's name means.
///
/// Decided by **name**, not by `Event::eventType`. The census over 3607 corpus
/// models says the type is not a discriminator: `Evt_Sound` ships as type 2 and
/// as type 65283, `Evt_Simulate` as 2, 15619 and 65283. Only `Evt_SeqEnd` is
/// consistently type 4, and it is the one that needs no decoding.
///
/// The whole shipped vocabulary is those three names over 7092 keys. That is
/// not a sample — it is every event StarCraft II and Heroes author into a
/// model.
enum class M3EventKind { Sound, SequenceEnd, Simulate, Unknown };

M3EventKind M3DecodeEventKind(const std::string& rawName) {
    // The `Ref<CHAR>` keeps the terminator, so `name.size()` is one past the
    // text and a plain `==` against a literal never matches. Silently: the
    // strings print identically. Every comparison against an M3 name has to
    // go through the c_str() round-trip.
    const std::string name(rawName.c_str());
    if (name == "Evt_Sound")
        return M3EventKind::Sound;
    if (name == "Evt_SeqEnd")
        return M3EventKind::SequenceEnd;
    if (name == "Evt_Simulate")
        return M3EventKind::Simulate;
    return M3EventKind::Unknown;
}

} // namespace

std::vector<EventObjectConfig> M3ModelAdapter::GetEventObjects() {
    std::vector<EventObjectConfig> out;
    if (tables_.SequenceCount() == 0)
        return out;

    // One config per (sequence, kind, id, bone). `EventObjectConfig` carries a
    // single payload and a list of times, so distinct payloads have to become
    // distinct configs — a container firing two different sounds is two.
    struct Key {
        i32 sequence;
        i32 bone;
        std::string id;
        bool operator==(const Key&) const = default;
    };
    std::vector<std::pair<Key, std::vector<::whiteout::u32>>> groups;

    bool warnedUnknown = false;
    for (std::size_t s = 0; s < tables_.SequenceCount(); ++s) {
        // The model's own sequences only. An event key carries a bone *index*,
        // and an attached `.m3a`'s indices are in its own bone array — a
        // differently-ordered subset that can name bones the model does not
        // have — so anchoring one against this skeleton would fire the cue on
        // whatever bone happened to land at that index. Tracks bind by animId
        // and survive the merge; bone indices do not, so these are dropped
        // rather than mis-anchored. (Moot today: the M3 spawn path never asks
        // for event configs, and it reads them once at spawn either way.)
        if (tables_.AssetOfSequence(static_cast<i32>(s)) != 0)
            continue;
        for (const auto& def : tables_.LayersFor(static_cast<i32>(s))) {
            const ::whiteout::m3::SubTrackContainer* stcPtr = tables_.StcAt(def.stc);
            if (!stcPtr)
                continue;
            const auto& stc = *stcPtr;
            for (const auto& blk : stc.sdev) {
                for (std::size_t ki = 0; ki < blk.keys.size(); ++ki) {
                    const auto& ev = blk.keys[ki];
                    const M3EventKind kind = M3DecodeEventKind(ev.name);
                    if (kind == M3EventKind::Unknown) {
                        if (!warnedUnknown) {
                            std::fprintf(stderr,
                                         "[m3 events] unhandled event name '%s' in %s — ignored\n",
                                         std::string(ev.name.c_str()).c_str(),
                                         std::string(model_.name.c_str()).c_str());
                            warnedUnknown = true;
                        }
                        continue;
                    }
                    // Recognised and deliberately not dispatched:
                    //   Evt_SeqEnd  — a playback marker on bone 0xFFFF at the
                    //                 track's last frame. The playlist already
                    //                 knows where a sequence ends.
                    //   Evt_Simulate — the ragdoll hand-off (it appears in
                    //                 *DeathRagdoll / *PhysicsDeath models on a
                    //                 real bone). Nothing consumes it until
                    //                 physics lands; emitting it now would only
                    //                 add configs the pool skips.
                    if (kind != M3EventKind::Sound)
                        continue;
                    if (ki >= blk.timestamps.size())
                        continue;

                    const Key k{static_cast<i32>(s),
                                ev.boneIndex == 0xFFFFu ? -1 : static_cast<i32>(ev.boneIndex),
                                std::string(ev.optionString.c_str())};
                    auto it = std::find_if(groups.begin(), groups.end(),
                                           [&](const auto& g) { return g.first == k; });
                    if (it == groups.end()) {
                        groups.push_back({k, {}});
                        it = groups.end() - 1;
                    }
                    it->second.push_back(static_cast<::whiteout::u32>(blk.timestamps[ki]));
                }
            }
        }
    }

    out.reserve(groups.size());
    for (auto& [k, times] : groups) {
        std::sort(times.begin(), times.end());
        times.erase(std::unique(times.begin(), times.end()), times.end());
        EventObjectConfig c;
        c.name = "Evt_Sound";
        c.kind = EventObjectConfig::Kind::SND;
        // The option string is the cue name ("Terran_ExplosionLarge"), which
        // resolves through StarCraft II's own sound tables rather than through
        // anything in the model. Carried verbatim so a host that has those
        // tables can dispatch it; without them the pool's SND path finds
        // nothing and says so once.
        c.id = k.id;
        c.nodeIndex = k.bone;
        c.sequenceIndex = k.sequence;
        c.eventTrackTimes = std::move(times);
        out.push_back(std::move(c));
    }
    return out;
}

void M3ModelAdapter::CreatePoseStages(renderer::animation::PoseStageList& out) const {
    // ---- IKJT ------------------------------------------------------------
    for (const auto& jt : model_.ikJoints) {
        // The chunk names the two ends; the chain between them is the parent
        // walk from the effector up to the root, which is exactly what
        // `CJTIKSolver_Init` does before marking each bone.
        const std::size_t root = jt.boneIndex1;
        const std::size_t tip = jt.boneIndex2;
        if (root >= model_.bones.size() || tip >= model_.bones.size())
            continue;

        std::vector<i32> chain;
        std::size_t cur = tip;
        // Bounded by the bone count: a malformed parent link that cycles would
        // otherwise walk forever, and a damaged chunk should cost a skipped
        // solver rather than a hang.
        for (std::size_t guard = 0; guard <= model_.bones.size(); ++guard) {
            chain.push_back(static_cast<i32>(cur));
            if (cur == root)
                break;
            const ::whiteout::u16 p = model_.bones[cur].parentIndex;
            if (p == 0xFFFFu || p >= model_.bones.size())
                break;
            cur = p;
        }
        // Only a walk that actually reached the named root is a chain.
        if (chain.empty() || static_cast<std::size_t>(chain.back()) != root)
            continue;
        std::reverse(chain.begin(), chain.end()); // root-first
        if (chain.size() < 2)
            continue;

        out.push_back(std::make_unique<M3JtIkStage>(std::move(chain), jt.raycastUp,
                                                    jt.raycastDown, jt.maxSpeed,
                                                    jt.goalThreshold));
    }

    // ---- PATU ------------------------------------------------------------
    for (const auto& tb : model_.turretBehaviors) {
        if (tb.boneIndex >= model_.bones.size())
            continue;
        // The permitted axis, from the descriptor's third basis row.
        //
        // Every PATU descriptor in the corpus is an identity transform with
        // zero weights and zero limits (checked, not assumed — see
        // m3_solver_test's corpus sweep), so this reduces to +Z, which is the
        // yaw axis in StarCraft II's Z-up space and the right answer for a
        // turret. The chunk names the bone and essentially nothing else; the
        // axis, limits and turn rate a game would use live in SC2's Actor data,
        // which is not in the model and not something we have.
        const Vector3f axis{tb.transform.data[2][0], tb.transform.data[2][1],
                            tb.transform.data[2][2]};
        out.push_back(std::make_unique<M3TurretStage>(
            static_cast<i32>(tb.boneIndex), axis,
            // No turn rate is parsed; `yawWeight` is the closest authored
            // quantity and behaves as one (larger = swings faster). A zero
            // weight would freeze the turret, so it falls back to a rate that
            // crosses 180 degrees in about a second.
            tb.yawWeight > 0.0f ? tb.yawWeight : 3.0f, tb.yawLimited != 0, tb.yawMin,
            tb.yawMax));
    }

#if WDX_HAS_PHYSICS
    // ---- PHRB/PHYJ -------------------------------------------------------
    // Physics is last in the list by contract: solvers correct the animated
    // pose and physics consumes the corrected one (`pose_stage.h`). Nothing
    // for the overwhelming majority of models, whose rigid bodies are
    // kinematic hit proxies that never become dynamic.
    if (auto stage = renderer::profiles::sc2_heroes::CreateSc2PhysicsStage(model_)) {
        out.push_back(std::move(stage));
    }

    // ---- PHCL ------------------------------------------------------------
    // After the bodies, which is the same "consumes the corrected pose" rule
    // one step further along: a cloth anchored to a ragdoll's bone has to read
    // where the ragdoll put it, not where the animation did.
    if (auto stage = renderer::profiles::sc2_heroes::CreateSc2ClothStage(
            model_, cloth_, static_cast<i32>(model_.bones.size()))) {
        out.push_back(std::move(stage));
    }
#endif
}

SkeletonData M3ModelAdapter::GetSkeleton() {
    SkeletonData sk;
    const auto& bones = model_.bones;
    sk.nodeCount = static_cast<i32>(bones.size());
    if (bones.empty())
        return sk;

    sk.nodeParents.resize(bones.size());
    sk.billboardFlags.assign(bones.size(), 0u);
    for (std::size_t i = 0; i < bones.size(); ++i) {
        const ::whiteout::u16 p = bones[i].parentIndex;
        // 0xFFFF is the documented root marker; anything else out of range is a
        // damaged chunk and is treated as a root rather than followed.
        sk.nodeParents[i] =
            (p == 0xFFFFu || static_cast<std::size_t>(p) >= bones.size()) ? -1 : static_cast<i32>(p);

        // Raw BONE flags, masked to the billboard bits, and always zero: no
        // bone in 51469 corpus files sets either. `.m3` billboards come from
        // the BBSC chunk instead, and `Evaluate` applies them itself (see
        // m3_billboard.h) rather than exporting them here — the vocabulary
        // this field speaks is WC3's four modes, and StarCraft II's seven do
        // not fit it.
        const auto f = static_cast<::whiteout::u32>(bones[i].flags);
        sk.billboardFlags[i] =
            f & (static_cast<::whiteout::u32>(::whiteout::m3::BoneFlag::Billboard1) |
                 static_cast<::whiteout::u32>(::whiteout::m3::BoneFlag::Billboard2));
    }

    sk.inverseBindMatrices.assign(bones.size(), Matrix44f::identity());
    const std::size_t n = (std::min)(bones.size(), model_.initialReference.size());
    for (std::size_t i = 0; i < n; ++i)
        sk.inverseBindMatrices[i] = model_.initialReference[i].matrix;
    if (model_.initialReference.size() < bones.size()) {
        std::fprintf(stderr, "[m3] '%s': IREF has %zu matrices for %zu bones; the rest bind as identity\n",
                     model_.name.c_str(), model_.initialReference.size(), bones.size());
    }

#if WDX_HAS_PHYSICS
    // One node per cloth particle, appended after the skeleton. Their inverse
    // bind is a pure translation because the rotation half already lives in the
    // output frame the solver writes — see `sc2_cloth.h`. Parentless, and not
    // billboards: nothing samples or composes them, the cloth stage writes each
    // one outright.
    if (cloth_) {
        for (const auto& piece : cloth_->pieces) {
            for (const Vector3f& rest : piece.restPositions) {
                sk.inverseBindMatrices.push_back(Matrix44f::translation({-rest.x, -rest.y, -rest.z}));
                sk.nodeParents.push_back(-1);
                sk.billboardFlags.push_back(0u);
            }
        }
        sk.nodeCount = static_cast<i32>(sk.inverseBindMatrices.size());
    }
#endif
    return sk;
}

std::vector<SkinWeightData> M3ModelAdapter::GetSkinWeights() {
    std::vector<SkinWeightData> out;
    if (divisionIndex_ >= model_.divisions.size() || model_.bones.empty())
        return out;
    const auto& div = model_.divisions[divisionIndex_];
    const auto& lookup = model_.boneLookup;
    const i32 boneCount = static_cast<i32>(model_.bones.size());

    out.reserve(emittedRegions_.size());
    for (std::size_t g = 0; g < emittedRegions_.size(); ++g) {
        const auto& region = div.regions[emittedRegions_[g]];

        SkinWeightData sw;
        sw.geosetId = static_cast<i32>(g);
#if WDX_HAS_PHYSICS
        // A cloth-influenced region's palette is the cloth, not the skeleton:
        // `GetMeshes` has already repointed its per-vertex indices at particle
        // slots, so the window it reads from has to be the particle nodes in
        // particle order.
        if (cloth_ != nullptr && g < geosetClothPiece_.size() && geosetClothPiece_[g] >= 0) {
            const auto& piece = cloth_->pieces[static_cast<std::size_t>(geosetClothPiece_[g])];
            sw.paletteLocalVertexIndices = true;
            const i32 base = boneCount + static_cast<i32>(piece.firstParticle);
            sw.subsetNodeIndices.reserve(piece.particleCount);
            for (std::size_t k = 0; k < piece.particleCount; ++k)
                sw.subsetNodeIndices.push_back(base + static_cast<i32>(k));
            out.push_back(std::move(sw));
            continue;
        }
#endif
        // `influences` stays empty, and that is the entire point: the weights
        // and indices are already in the baked vertex blob, described at
        // offsets 12 and 16, and go to the GPU untouched. Filling this in
        // would decode four bytes per vertex only to have the loader repack
        // them into a second stream holding the same numbers.
        sw.paletteLocalVertexIndices = true;

        // REGN's bone-lookup window *is* the region's palette. A vertex stores
        // its bone as an index into that window, so slot i of the palette must
        // hold the bone the window's entry i names — no remap, and nothing to
        // rewrite.
        const std::size_t first = region.firstBoneLookup;
        const std::size_t count = region.boneLookupCount;
        sw.subsetNodeIndices.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            const std::size_t slot = first + i;
            i32 gb = (slot < lookup.size()) ? static_cast<i32>(lookup[slot]) : 0;
            if (gb < 0 || gb >= boneCount)
                gb = 0;
            sw.subsetNodeIndices.push_back(gb);
        }
        // A region with no window still needs one slot for its vertices to
        // point at; its own root bone is the honest choice.
        if (sw.subsetNodeIndices.empty()) {
            const i32 rootBone =
                (region.rootBone < model_.bones.size()) ? static_cast<i32>(region.rootBone) : 0;
            sw.subsetNodeIndices.push_back(rootBone);
        }
        out.push_back(std::move(sw));
    }
    return out;
}

namespace {

/// @brief A shipped `.m3` string carries its terminator inside the
///        `std::string` — the `Reference` count includes it — so anything
///        comparing or concatenating one has to drop it first.
std::string_view TrimNuls(std::string_view s) {
    while (!s.empty() && s.back() == '\0')
        s.remove_suffix(1);
    return s;
}

} // namespace

bool M3ModelAdapter::IsGlobalLoop(i32 sequence) const {
    const ::whiteout::m3::Sequence* s = tables_.SequenceAt(sequence);
    if (!s)
        return false;

    // The engine's rule, and only this one: `M3AnimState::Init` (SC2 4.8
    // `sub_10288B3C0`, repeated by `sub_10288BAB0` after every `.m3a` merge)
    // walks every sequence in the global table and starts the ones flagged
    // `AlwaysGlobal`, at play flags `(seqFlags & 1) | 0x12` — persistent, on
    // the world clock, and not counted as "playing". Nothing in the binary
    // looks at the name; the string "GLstand" does not appear in it at all.
    if ((s->flags & ::whiteout::m3::SequenceFlag::AlwaysGlobal) !=
        ::whiteout::m3::SequenceFlag::None)
        return true;

    // ...and then the convention, because the flag alone leaves most of the
    // shipped content dark. Measured over 119396 sequences in 4 corpora:
    // `AlwaysGlobal` is set on 934, of which 893 are named `GL*` — but 8023
    // sequences are named `GL*`, so 89% of them do NOT carry it. In the game
    // those are started by the unit's actor data (`AnimBracketStart` against a
    // catalog entry), which a model viewer has none of, so the name is the
    // only signal left that a radar dish is supposed to keep turning.
    //
    // Restricted to sequences whose containers ALL run concurrent, which is
    // what makes an overlay safe: such a container abstains on every property
    // it does not key, so it cannot fight the sequence the host asked for.
    // That covers 5692 of the 7130 unflagged `GL*` sequences; the 1438 it
    // declines are doodads (`Aiur_..._Waterfall`) whose *only* sequence is
    // `GLstand`, so the host plays it as the main sequence anyway and starting
    // it twice would just double the weight.
    const std::string_view name = TrimNuls(s->name);
    if (name.size() < 2 || (name[0] != 'G' && name[0] != 'g') ||
        (name[1] != 'L' && name[1] != 'l'))
        return false;
    const auto defs = tables_.LayersFor(sequence);
    if (defs.empty())
        return false;
    for (const auto& d : defs)
        if (!d.transparent)
            return false;
    return true;
}

std::vector<M3ModelAdapter::SubtrackInfo> M3ModelAdapter::SubtracksOf(i32 sequence) const {
    std::vector<SubtrackInfo> out;
    const ::whiteout::m3::Sequence* seq = tables_.SequenceAt(sequence);
    if (!seq)
        return out;
    const std::string_view seqName = TrimNuls(seq->name);
    for (const auto& d : tables_.LayersFor(sequence)) {
        SubtrackInfo info;
        info.priority = d.priority;
        info.concurrent = d.transparent;
        if (const auto* stc = tables_.StcAt(d.stc)) {
            info.trackCount = stc->animIds.size();
            std::string_view n = TrimNuls(stc->name);
            // `<sequence>_<part>`. Authored that way throughout the corpus, but
            // an `.m3a`'s containers can carry the *file's* naming instead, so
            // a miss keeps the whole name rather than mangling it.
            if (n.size() > seqName.size() + 1 && n.substr(0, seqName.size()) == seqName &&
                n[seqName.size()] == '_')
                n.remove_prefix(seqName.size() + 1);
            info.name = n.empty() ? "full" : std::string(n);
        } else {
            info.name = "full";
        }
        out.push_back(std::move(info));
    }
    return out;
}

std::vector<SequenceInfo> M3ModelAdapter::GetSequences() const {
    std::vector<SequenceInfo> out;
    // The model's own sequences first, then each attached `.m3a`'s — the same
    // global index space `BuildLayers` decodes, so a host can hand an index
    // straight back without knowing which file it came from. Names are left
    // exactly as authored: a `.m3a` may well redefine one of the model's (we
    // measured `Stand` in both halves of a shipped pair), and StarCraft II only
    // hides the earlier one when the file is loaded with `EAnimLoadFlag`
    // `Override`, which is not the default and which nothing here sets.
    out.reserve(tables_.SequenceCount());
    for (std::size_t q = 0; q < tables_.SequenceCount(); ++q) {
        const ::whiteout::m3::Sequence& s = *tables_.SequenceAt(static_cast<i32>(q));
        SequenceInfo info;
        info.name = s.name.empty() ? "Sequence" : s.name;
        // Kept as authored rather than rebased to zero: the sub-track keys are
        // stamped in this same frame space, so shifting the window here would
        // desynchronise every track that starts at a non-zero frame.
        info.startMs = static_cast<i32>(s.startFrame);
        info.endMs = static_cast<i32>(s.endFrame);
        info.moveSpeed = s.moveSpeed;
        info.nonLooping = (s.flags & ::whiteout::m3::SequenceFlag::NotLooping) !=
                          ::whiteout::m3::SequenceFlag::None;
        info.alwaysPlays = IsGlobalLoop(static_cast<i32>(q));
        const auto defs = tables_.LayersFor(static_cast<i32>(q));
        info.concurrent = !defs.empty();
        for (const auto& d : defs)
            info.concurrent = info.concurrent && d.transparent;
        out.push_back(std::move(info));
    }
    if (out.empty()) {
        // Only for a model with no SEQS at all — the actor still needs a clock.
        SequenceInfo s;
        s.name = "Stand";
        s.startMs = 0;
        s.endMs = 1000;
        out.push_back(std::move(s));
    }
    return out;
}

void M3ModelAdapter::BuildLayers(const PoseRequest& req, std::vector<M3Layer>& out) const {
    out.clear();
    const auto clips = req.clips;
    for (std::size_t c = 0; c < clips.size() && c < 64; ++c) {
        const ClipRef& clip = clips[c];
        if (tables_.SequenceCount() == 0)
            break;
        // Wrapped rather than dropped. Hosts pass a raw, unbounded index and
        // rely on the wrap — it is what `ClipPlaylist::Advance` does — and a
        // dropped layer here is invisible: the model renders in bind pose and
        // nothing reports a miss.
        const i32 n = static_cast<i32>(tables_.SequenceCount());
        const i32 seqIdx = ((clip.sequence % n) + n) % n;
        const auto& seq = *tables_.SequenceAt(seqIdx);
        // Absolute frame time, as the keys are stamped. The unwrapped elapsed
        // is what arrives, because a looping track wraps on its own duration
        // and the sequence-windowed time has already lost that.
        const i32 t = static_cast<i32>(seq.startFrame) + clip.elapsedMs;

        const auto defs = tables_.LayersFor(seqIdx);
        for (std::size_t k = 0; k < defs.size(); ++k) {
            // One container out of the group, when the clip named one. Out of
            // range drops the clip rather than falling back to all of them: a
            // stale index (the host kept one across an `.m3a` detach) showing
            // up as the whole sequence would look like the layer works.
            if (clip.subtrack >= 0 && static_cast<std::size_t>(clip.subtrack) != k)
                continue;
            const auto& d = defs[k];
            M3Layer l;
            l.play = static_cast<::whiteout::u16>(c);
            l.stc = d.stc;
            l.priority = d.priority;
            l.transparent = d.transparent;
            l.timeMs = t;
            l.weight = clip.weight;
            l.loop = clip.loop;
            l.sequence = static_cast<::whiteout::u16>(seqIdx);
            l.global = clip.global;
            l.blendingOut = clip.blendingOut;
            out.push_back(l);
        }
    }
    // Priority first, and stable so that layers of equal priority keep clip
    // order — newest clip wins a tie, matching the insertion-ordered player
    // list StarCraft II walks.
    std::stable_sort(out.begin(), out.end(),
                     [](const M3Layer& a, const M3Layer& b) { return a.priority > b.priority; });
}

renderer::model::FrameState M3ModelAdapter::Evaluate(const PoseRequest& req) const {
    renderer::model::FrameState fs;
    const std::size_t boneCount = model_.bones.size();
    if (boneCount == 0)
        return fs;

    std::vector<M3Layer> layers;
    BuildLayers(req, layers);

    // Bind pose when nothing is playing: every channel falls back to its
    // AnimRef default, which is exactly what an empty layer list produces.
    fs.boneWorldMatrices.resize(boneCount);
#if WDX_HAS_PHYSICS
    // The cloth particles' nodes, seeded so an actor whose stage has not run
    // yet — or a build with physics compiled out of the *stage* but not the
    // palette — draws the cloth region in bind pose rather than collapsed on
    // the origin. `translate(+rest)` is the exact inverse of the particle's
    // inverse bind.
    if (cloth_) {
        fs.boneWorldMatrices.reserve(boneCount + cloth_->particleCount);
        for (const auto& piece : cloth_->pieces)
            for (const Vector3f& rest : piece.restPositions)
                fs.boneWorldMatrices.push_back(Matrix44f::translation(rest));
    }
#endif
    std::vector<::whiteout::u8> visible(boneCount, 1);

    // Billboards resolve against the camera, so they cost a matrix inverse per
    // evaluate and are skipped outright on the nine models in ten that have no
    // `BBSC` chunk. StarCraft II runs them later than this — in the draw-prep
    // pass, after the solvers — but it runs them by rewriting the bone's local
    // rotation and letting the dirty-bit walk re-resolve the subtree, and doing
    // it inside this walk is the same thing with the subtree already ordered.
    const bool billboarding = !boneBillboard_.empty();
    M3CameraFrame cam;
    if (billboarding)
        cam = M3BuildCameraFrame(req.world, req.view, req.cameraPos);

    // A `replace` override is the host saying "this bone's model-space matrix
    // is mine: do not sample it and do not compose the parent chain into it".
    // That is how a pose stage's claims come back — the ragdoll's bit-0 flag in
    // StarCraft II's own runtime, which is read by the animation system for
    // exactly this purpose (`DOMINO_GLUE.md` §6.6). Without it the sampler
    // re-poses a simulated bone every frame and the stage overwrites it again,
    // which lands in the same place but means a claim buys nothing.
    //
    // A driven bone is still a parent: bones below it compose onto the driven
    // matrix exactly as they would onto a sampled one, which is what carries a
    // ragdoll's hands and attachment points with its arms. Same rule as
    // `M2ModelAdapter::EvaluateBones`.
    std::vector<const ::whiteout::flakes::NodeOverride*> over;
    if (!req.overrides.empty()) {
        over.assign(boneCount, nullptr);
        for (const auto& o : req.overrides) {
            if (o.node >= 0 && static_cast<std::size_t>(o.node) < boneCount)
                over[static_cast<std::size_t>(o.node)] = &o;
        }
    }

    for (std::size_t i = 0; i < boneCount; ++i) {
        const auto& b = model_.bones[i];
        const ::whiteout::flakes::NodeOverride* ov = over.empty() ? nullptr : over[i];

        const ::whiteout::u16 p = b.parentIndex;
        // Bones are stored parents-first, so one linear pass resolves the
        // hierarchy; a forward reference would read an unwritten matrix, so it
        // is treated as a root instead.
        const bool hasParent = p != 0xFFFFu && p < i;

        Quaternion sampledRotation{0.0f, 0.0f, 0.0f, 1.0f};
        Vector3f sampledScale{1.0f, 1.0f, 1.0f};
        bool sampled = false;
        if (ov != nullptr && ov->replace) {
            fs.boneWorldMatrices[i] = ov->m;
        } else {
            const Vector3f t = SampleRef(b.position, layers);
            const Quaternion r = SampleRef(b.rotation, layers);
            const Vector3f s = SampleRef(b.scale, layers);
            sampledRotation = r;
            sampledScale = s;
            sampled = true;

            Matrix44f local = M3ComposeLocal(t, r, s);
            // A non-replace override composes *after* the local TRS and before
            // the parent multiply, which is where a turret or look-at correction
            // belongs (`pose_request.h`). Nothing in-tree writes one — the
            // solvers are pose stages instead — but a silent no-op here is the
            // same failure the claims had.
            if (ov != nullptr)
                local = local * ov->m;

            if (hasParent)
                fs.boneWorldMatrices[i] = local * fs.boneWorldMatrices[p];
            else
                fs.boneWorldMatrices[i] = local;
        }

        if (billboarding && boneBillboard_[i] >= 0) {
            // A bone hanging off the model root keeps its own animated rotation
            // composed onto the billboard — that is what lets a root-level
            // sprite spin in the screen plane. A child bone's is discarded, and
            // its correction quaternion takes its place. The engine draws the
            // line at "is my parent a bone", which here is `hasParent`.
            const Quaternion* spin = (!hasParent && sampled) ? &sampledRotation : nullptr;

            // A host-replaced bone has no sampled local transform to divide
            // through, so it is billboarded as a rigid frame of its own — the
            // row lengths stand in for the scale and there is no parent to
            // re-multiply. The engine has no equivalent of a `replace`
            // override at all, so there is nothing to be faithful to here.
            const Matrix44f* parentWorld =
                (sampled && hasParent) ? &fs.boneWorldMatrices[p] : nullptr;
            Vector3f scale = sampledScale;
            if (!sampled) {
                const Matrix44f& m = fs.boneWorldMatrices[i];
                scale = {Vector3f{m.data[0][0], m.data[0][1], m.data[0][2]}.length(),
                         Vector3f{m.data[1][0], m.data[1][1], m.data[1][2]}.length(),
                         Vector3f{m.data[2][0], m.data[2][1], m.data[2][2]}.length()};
            }
            M3ApplyBillboard(model_.billboardBehaviors[static_cast<std::size_t>(boneBillboard_[i])],
                             cam, spin, scale, parentWorld, fs.boneWorldMatrices[i]);
        }

        // Visibility is hierarchical: a bone under an invisible parent is
        // invisible whatever its own track says, and the engine never even
        // samples it in that case (M3Anim_EvaluateBoneVisibility tests the
        // parent's visible bit before doing any work).
        if (hasParent && !visible[p]) {
            visible[i] = 0;
        } else {
            // Discrete channel — sampled in override mode, so the first play
            // past the filters wins outright rather than blending.
            visible[i] = SampleRefOverride(b.visibility, layers) != 0 ? 1 : 0;
        }
    }

    EvaluateGeosetVisibility(layers, visible, fs);
    EvaluateMaterialUvTransforms(layers, fs);
    EvaluateMaterialMapAlphas(layers, fs);
    EvaluateLights(layers, visible, req.world, fs);
    EvaluateRibbons(layers, visible, req.world, fs);
    EvaluateParticles(layers, visible, req.world, fs);
    EvaluatePhysics(layers, fs);
    return fs;
}

/// The tiling a layer means, with an unauthored axis read as 1.
///
/// A `.m3` layer names its UV transform in one optional property, and a layer
/// that names none leaves the pair at the struct's zero. Composed literally
/// that is an ALL-ZERO matrix — every pixel of the layer samples texel (0,0),
/// so the surface comes out one flat colour, and for most character skins that
/// colour is the dark corner of the atlas. Silence, not a request to collapse
/// the surface onto a point: 48032 of the 1039144 shipped `MAT_` layers in the
/// Heroes corpus read (0,0), and every layer a shader-graph MADD restores does.
///
/// A DRIVEN track is taken at face value; only the silence is filled in.
Vector2f M3NeutralTiling(Vector2f tiling, bool driven) {
    if (driven)
        return tiling;
    return {tiling.x != 0.0f ? tiling.x : 1.0f, tiling.y != 0.0f ? tiling.y : 1.0f};
}

void M3ModelAdapter::EvaluateMaterialUvTransforms(std::span<const M3Layer> layers,
                                                  renderer::model::FrameState& fs) const {
    for (std::size_t m = 0; m < model_.standardMaterials.size(); ++m) {
        const auto& mat = model_.standardMaterials[m];
        for (u32 slot = 0; slot < static_cast<u32>(M3LayerSlot::Count); ++slot) {
            const ::whiteout::m3::TextureLayer* layer =
                M3LayerForSlot(mat, static_cast<M3LayerSlot>(slot));
            if (!layer)
                continue;
            // A driven track, or a bind pose that is not the identity. Every
            // shipped AnimRef carries a non-zero animId whether or not anything
            // drives it, so "is it animated" has to ask the tables — reading
            // the id alone answers yes for all 2862196 layers in the corpus.
            const bool tilingDriven = tables_.RowOf(layer->uvTiling.animId) >= 0;
            const bool driven = tables_.RowOf(layer->uvOffset.animId) >= 0 ||
                                tables_.RowOf(layer->uvAngle.animId) >= 0 || tilingDriven;
            const Vector2f& o = layer->uvOffset.initValue;
            const Vector3f& a = layer->uvAngle.initValue;
            const Vector2f t = M3NeutralTiling(layer->uvTiling.initValue, tilingDriven);
            const bool moved = o.x != 0.0f || o.y != 0.0f || a.x != 0.0f || a.y != 0.0f ||
                               a.z != 0.0f || t.x != 1.0f || t.y != 1.0f;
            if (!driven && !moved)
                continue;

            renderer::model::FrameState::TexAnimMatrix out{};
            out.textureAnimId = M3UvTransformId(static_cast<u32>(m), static_cast<M3LayerSlot>(slot));
            M3ComposeUvTransform(SampleRef(layer->uvOffset, layers),
                                 SampleRef(layer->uvAngle, layers),
                                 M3NeutralTiling(SampleRef(layer->uvTiling, layers), tilingDriven),
                                 out.row0, out.row1);
            fs.texAnimMatrices.push_back(out);
        }
    }
}

void M3ModelAdapter::EvaluateMaterialMapAlphas(std::span<const M3Layer> layers,
                                               renderer::model::FrameState& fs) const {
    for (std::size_t m = 0; m < model_.standardMaterials.size(); ++m) {
        const auto& mat = model_.standardMaterials[m];
        for (u32 slot = 0; slot < static_cast<u32>(M3LayerSlot::Count); ++slot) {
            const ::whiteout::m3::TextureLayer* layer =
                M3LayerForSlot(mat, static_cast<M3LayerSlot>(slot));
            // Driven means a container names the id -- every shipped AnimRef
            // carries one, driven or not. A geoset fade converted from
            // Warcraft III is the case that needs it: alpha 0 through every
            // Stand on a carrier whose rest is 1.
            if (!layer || tables_.RowOf(layer->mapAlpha.animId) < 0)
                continue;
            fs.layerMapAlphas.push_back(
                {M3UvTransformId(static_cast<u32>(m), static_cast<M3LayerSlot>(slot)),
                 std::clamp(SampleRef(layer->mapAlpha, layers), 0.0f, 1.0f)});
        }
    }
}

void M3ModelAdapter::EvaluatePhysics(std::span<const M3Layer> layers,
                                     renderer::model::FrameState& fs) const {
    // `PHCL.active` gates the cloth's whole contribution — StarCraft II skips
    // the write-back outright when it reads zero, so the mesh keeps the pose it
    // last had rather than simulating unseen. Sampled here, and on the same flag
    // bit as `dynamicState`: 69 of the corpus's 392 cloth records ask for it.
    if (!model_.clothPhysics.empty()) {
        fs.clothActive.resize(model_.clothPhysics.size());
        for (std::size_t i = 0; i < model_.clothPhysics.size(); ++i) {
            const auto& c = model_.clothPhysics[i];
            ::whiteout::u32 v = c.active.initValue;
            if ((c.active.flags & 0x2u) != 0u)
                v = SampleRefOverride(c.active, layers);
            fs.clothActive[i] = v != 0 ? 1 : 0;
        }
    }

    if (model_.rigidBodies.empty())
        return;

    fs.physicsBodyDynamic.resize(model_.rigidBodies.size());
    for (std::size_t i = 0; i < model_.rigidBodies.size(); ++i) {
        const auto& rb = model_.rigidBodies[i];
        // `initValue` is the *base*, not a fallback for the unbound case:
        // `M3Physics_UpdateBodyDrivenState` loads PHRB+40 into its scratch and
        // only lets the sampler overwrite it. And the gate on that sampler is
        // the AnimRef's flag bit 1 rather than its `animId` — every shipped
        // `dynamicState` has a non-zero id, so reading the id as the gate sends
        // nine bodies in ten looking for keys that are not theirs.
        ::whiteout::u32 v = rb.dynamicState.initValue;
        if ((rb.dynamicState.flags & 0x2u) != 0u)
            v = SampleRefOverride(rb.dynamicState, layers);
        fs.physicsBodyDynamic[i] = v != 0 ? 1 : 0;
    }

#if WDX_HAS_PHYSICS
    // One frame behind for a simulated bone: this runs before the pose stages,
    // so a claimed bone still holds the animated pose here. The physics stage
    // overwrites these with the poses it actually produced — this fill is what
    // covers the models that have no stage at all, whose bodies are kinematic
    // proxies that never simulate.
    renderer::profiles::sc2_heroes::Sc2PlaceCollisionShapes(physicsShapeBones_,
                                                            physicsShapeLocals_,
                                                            physicsShapeAniso_,
                                                            fs.boneWorldMatrices,
                                                            fs.collisionTransforms);
#endif
}

std::vector<renderer::model::CollisionShapeData> M3ModelAdapter::GetCollisionShapes() {
    physicsShapeBones_.clear();
    physicsShapeLocals_.clear();
    physicsShapeAniso_.clear();
#if WDX_HAS_PHYSICS
    auto built = renderer::profiles::sc2_heroes::Sc2BuildCollisionShapes(model_);
    physicsShapeBones_ = std::move(built.bones);
    physicsShapeLocals_ = std::move(built.locals);
    physicsShapeAniso_ = std::move(built.anisotropic);
    return std::move(built.shapes);
#else
    return {};
#endif
}

std::vector<renderer::model::ClothOverlayData> M3ModelAdapter::GetClothOverlays() {
#if WDX_HAS_PHYSICS
    if (!cloth_) {
        return {};
    }
    namespace sc2 = renderer::profiles::sc2_heroes;
    std::vector<renderer::model::ClothOverlayData> out;
    out.reserve(cloth_->pieces.size());
    const auto boneCount = static_cast<i32>(model_.bones.size());
    for (const auto& piece : cloth_->pieces) {
        renderer::model::ClothOverlayData d;
        // The palette layout `sc2_cloth.h` promises: particle `i` of this piece
        // is one node, appended after the real skeleton, and its live position
        // is that node's origin.
        d.particleNodes.reserve(piece.particleCount);
        for (std::size_t i = 0; i < piece.particleCount; ++i) {
            d.particleNodes.push_back(boneCount + static_cast<i32>(piece.firstParticle + i));
        }
        d.pinnedCount = piece.def.pinnedCount;
        d.links.reserve(piece.def.edges.size() * 2);
        for (const auto& e : piece.def.edges) {
            if (e.a >= piece.particleCount || e.b >= piece.particleCount) {
                continue;
            }
            d.links.push_back(e.a);
            d.links.push_back(e.b);
        }
        d.colliders.reserve(piece.def.capsules.size());
        for (const auto& c : piece.def.capsules) {
            renderer::model::ClothColliderData cd;
            cd.node = c.anchor >= 0 && c.anchor < boneCount ? c.anchor : -1;
            cd.local = sc2::Sc2ComposeBone(
                Quaternion{c.localRotation.x, c.localRotation.y, c.localRotation.z,
                           c.localRotation.w},
                Vector3f{c.localPosition.x, c.localPosition.y, c.localPosition.z});
            cd.radius0 = c.radius0;
            cd.radius1 = c.radius1;
            cd.length = c.fullLength;
            d.colliders.push_back(cd);
        }
        d.activeIndex = static_cast<i32>(piece.chunkIndex);
        out.push_back(std::move(d));
    }
    return out;
#else
    return {};
#endif
}

void M3ModelAdapter::EvaluateGeosetVisibility(std::span<const M3Layer> layers,
                                              std::span<const ::whiteout::u8> visible,
                                              renderer::model::FrameState& fs) const {
    if (emittedRegions_.empty() || divisionIndex_ >= model_.divisions.size())
        return;
    fs.geosetAlphas.assign(emittedRegions_.size(), 1.0f);
    fs.geosetHidden.assign(emittedRegions_.size(), 0);
    for (std::size_t g = 0; g < emittedRegions_.size(); ++g) {
        // A composite section's multiplier is how much of that pass survives —
        // one constantly, for every geoset that is not one. Zero means the
        // section is off outright, which 142 of the corpus's 4006 ship.
        if (g < emittedWeights_.size()) {
            const f32 w = std::clamp(SampleRef(emittedWeights_[g], layers), 0.0f, 1.0f);
            fs.geosetAlphas[g] = w;
            if (w <= 0.0f)
                fs.geosetHidden[g] = 1;
        }
        // A geoset is gated by its BATCH's bone — `geosetVisibilityBone_`
        // explains why that is not the region's root bone. The chain walk is
        // already folded into `visible`, so this is a single lookup. Retail
        // skips the batch's submission outright rather than drawing it at
        // zero alpha, which is what `geosetHidden` means; the alpha goes too
        // so anything reading only the fade agrees.
        const ::whiteout::u16 gate =
            g < geosetVisibilityBone_.size() ? geosetVisibilityBone_[g] : ::whiteout::u16{0xFFFFu};
        if (gate != 0xFFFFu && gate < visible.size() && !visible[gate]) {
            fs.geosetAlphas[g] = 0.0f;
            fs.geosetHidden[g] = 1;
        }
        // A cloth's simulated region is a coarse invisible proxy — the visible
        // surface is the region bound to it. Taken out of the draw list rather
        // than faded, because it is not part of the model at all (the same
        // distinction `geosetHidden` exists for).
        if (g < geosetClothProxy_.size() && geosetClothProxy_[g])
            fs.geosetHidden[g] = 1;
    }
}

void M3ModelAdapter::EvaluateLights(std::span<const M3Layer> layers,
                                    std::span<const ::whiteout::u8> visible,
                                    const Matrix44f& world,
                                    renderer::model::FrameState& fs) const {
    if (model_.lights.empty())
        return;
    fs.lights.reserve(model_.lights.size());

    for (const auto& l : model_.lights) {
        renderer::model::FrameState::LightState st;
        switch (l.lightType) {
        case ::whiteout::m3::LightType::Directional:
            st.kind = renderer::model::FrameState::LightKind::Directional;
            break;
        default:
            // Spot lights carry a cone the shared LightState cannot express;
            // treated as omni until a shading model needs the cone, which is
            // strictly better than dropping the light.
            st.kind = renderer::model::FrameState::LightKind::Omni;
            break;
        }

        const std::size_t bone = l.boneIndex;
        Matrix44f m = Matrix44f::identity();
        if (bone < fs.boneWorldMatrices.size())
            m = fs.boneWorldMatrices[bone] * world;
        else
            m = world;

        st.worldPos = {m.data[3][0], m.data[3][1], m.data[3][2]};
        // Row 2 is the bone's forward axis in this row-vector convention.
        st.worldDir = {m.data[2][0], m.data[2][1], m.data[2][2]};

        const Vector3f colour = SampleRef(l.diffuseColor, layers);
        const f32 intensity = SampleRef(l.intensityMultiplier, layers);
        // Premultiplied, matching what LightState documents as shader-ready.
        st.diffuse = {colour.x * intensity, colour.y * intensity, colour.z * intensity};
        st.dirIntensity = intensity;
        // The highlight is its own colour, not a scaling of the diffuse —
        // deferredlight.fx:220 multiplies the specular term by the light's own
        // constant, and LightFlag::Specular is the axis that enables it.
        const Vector3f spec = SampleRef(l.specularColor, layers);
        const f32 specMul = SampleRef(l.specularMultiplier, layers);
        st.specular = {spec.x * specMul, spec.y * specMul, spec.z * specMul};
        st.useSpecular = (static_cast<u32>(l.flags) &
                          static_cast<u32>(::whiteout::m3::LightFlag::Specular)) != 0;
        st.attenStart = SampleRef(l.attenuationStart, layers);
        st.attenEnd = l.attenuationEnd;
        st.enabled = bone >= visible.size() || visible[bone] != 0;
        fs.lights.push_back(st);
    }
}

void M3ModelAdapter::EvaluateRibbons(std::span<const M3Layer> layers,
                                     std::span<const ::whiteout::u8> visible,
                                     const Matrix44f& world,
                                     renderer::model::FrameState& fs) const {
    if (model_.ribbonEmitters.empty())
        return;
    // Renderer units per model unit, the M2 route's own derivation: the
    // world matrix's uniform scale (row 0's length).
    const f32 scale = std::sqrt(world.data[0][0] * world.data[0][0] +
                                world.data[0][1] * world.data[0][1] +
                                world.data[0][2] * world.data[0][2]);
    fs.ribbonStates.reserve(model_.ribbonEmitters.size());

    const auto boneWorld = [&](std::size_t bone) {
        if (bone < fs.boneWorldMatrices.size())
            return fs.boneWorldMatrices[bone] * world;
        return world;
    };
    const auto colorOf = [](const ::whiteout::m3::ColorBGRA& c) -> Vector4f {
        return {c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, c.a / 255.0f};
    };

    for (std::size_t i = 0; i < model_.ribbonEmitters.size(); ++i) {
        const auto& rib = model_.ribbonEmitters[i];
        renderer::model::FrameState::RibbonFrameState st;
        st.emitterId = static_cast<i32>(i);
        st.transform = boneWorld(rib.boneIndex);
        st.unitScale = (scale > 0.0f) ? scale : 1.0f;
        // The WC3-family scalars stay at their defaults — the SC2 stages read
        // the block below instead. `visibility` carries the bone-visibility
        // gate the way retail's node-active bit does (pTransformNode+150 & 2).
        st.above = 0;
        st.below = 0;
        st.alpha = 1.0f;
        st.color = {1, 1, 1};
        st.visibility = (rib.boneIndex >= visible.size() || visible[rib.boneIndex]) ? 1.0f : 0.0f;
        st.slot = 0;

        auto& s2 = st.sc2;
        s2.speed = SampleRef(rib.initialSpeed, layers);
        // Yaw/pitch are authored in DEGREES; the emitter converts at use — the
        // only deg→rad in the pipeline (SC2_RIBBON_RE.md §4.7).
        s2.yawDeg = SampleRef(rib.initialYaw, layers);
        s2.pitchDeg = SampleRef(rib.initialPitch, layers);
        s2.lifetime = SampleRef(rib.lifetime, layers);
        s2.maxLength = SampleRef(rib.maxLength, layers);
        s2.size3 = SampleRef(rib.sizeAnimation, layers);
        // Twist keys are RADIANS end-to-end — no conversion exists anywhere.
        s2.rotation3 = SampleRef(rib.rotationAnimation, layers);
        s2.color3[0] = colorOf(SampleRef(rib.colorStart, layers));
        s2.color3[1] = colorOf(SampleRef(rib.colorMid, layers));
        s2.color3[2] = colorOf(SampleRef(rib.colorEnd, layers));
        // Bool32, sampled in override mode like every other discrete channel;
        // gates EMISSION only — the live trail finishes its life (RE §3.1).
        s2.active = SampleRefOverride(rib.active, layers) != 0;
        s2.parentVelocityScale =
            (static_cast<u32>(rib.flags) & 0x10u) ? SampleRef(rib.particleVelocity, layers)
                                                  : 0.0f;
        // Overlay-wave amplitude/frequency pairs (yaw/pitch/speed/size/alpha) and
        // the shared overlay phase (RE §4.7). The head applies them per channel
        // whose static type is nonzero; sampling them unconditionally is cheap
        // and inert when the type is 0.
        s2.waveAmp[0] = SampleRef(rib.yawAmplitude, layers);
        s2.waveFreq[0] = SampleRef(rib.yawFrequency, layers);
        s2.waveAmp[1] = SampleRef(rib.pitchAmplitude, layers);
        s2.waveFreq[1] = SampleRef(rib.pitchFrequency, layers);
        s2.waveAmp[2] = SampleRef(rib.speedAmplitude, layers);
        s2.waveFreq[2] = SampleRef(rib.speedFrequency, layers);
        s2.waveAmp[3] = SampleRef(rib.sizeAmplitude, layers);
        s2.waveFreq[3] = SampleRef(rib.sizeFrequency, layers);
        s2.waveAmp[4] = SampleRef(rib.alphaAmplitude, layers);
        s2.waveFreq[4] = SampleRef(rib.alphaFrequency, layers);
        s2.overlayPhase = SampleRef(rib.overlay, layers);
        if (!rib.splineRibbons.empty()) {
            // Only record 0 — the runtime reads splineRibbons.ptr[0] alone.
            const auto& sr = rib.splineRibbons.front();
            s2.splineYawDeg = SampleRef(sr.yaw, layers);
            s2.splinePitchDeg = SampleRef(sr.pitch, layers);
            s2.velocityBaseFactor = SampleRef(sr.velocityBaseFactor, layers);
            s2.velocityEndFactor = SampleRef(sr.velocityEndFactor, layers);
            s2.splineNodeTransform = boneWorld(sr.boneIndex);
            // Spline overlay waves: yaw/pitch scale the SRIB rotation tangents,
            // velocity scales the end factor (RE §3.4).
            s2.splineWaveAmp[0] = SampleRef(sr.yawAmplitude, layers);
            s2.splineWaveFreq[0] = SampleRef(sr.yawFrequency, layers);
            s2.splineWaveAmp[1] = SampleRef(sr.pitchAmplitude, layers);
            s2.splineWaveFreq[1] = SampleRef(sr.pitchFrequency, layers);
            s2.splineWaveAmp[2] = SampleRef(sr.velocityAmplitude, layers);
            s2.splineWaveFreq[2] = SampleRef(sr.velocityFrequency, layers);
        }
        fs.ribbonStates.push_back(st);
    }
}

std::vector<renderer::effects::Sc2RibbonEmitterConfig> M3ModelAdapter::GetSc2RibbonConfigs() {
    using ::whiteout::m3::RibbonEmitter;
    std::vector<renderer::effects::Sc2RibbonEmitterConfig> out;
    out.reserve(model_.ribbonEmitters.size());
    for (const RibbonEmitter& rib : model_.ribbonEmitters) {
        renderer::effects::Sc2RibbonEmitterConfig c;
        c.boneIndex = rib.boneIndex;
        c.materialIndex = static_cast<i32>(rib.materialIndex);
        c.flags = static_cast<u32>(rib.flags);
        c.additionalFlags = static_cast<u32>(rib.additionalFlags);
        // The dword Ribbon_SelectSimTechnique tests is the FALLBACK force
        // pair at RIB_+0x174, not the primary pair (RE §3, oracle O1).
        c.forcesFallback = static_cast<u32>(rib.localForcesFallback) |
                           (static_cast<u32>(rib.worldForcesFallback) << 16);
        // WhiteoutLib carries the pre-RE labels for the 0x190/0x194 pair
        // (RE §1.1): its `emitterShape` is the cross-section and its
        // `ribbonType` is the cull method.
        c.ribbonType = static_cast<u8>(rib.emitterShape);
        c.cullMethod = static_cast<u8>(rib.ribbonType);
        c.divisions = rib.divisions;
        c.edges = static_cast<i32>(rib.edges);
        c.innerRadius = rib.innerRadius;
        c.midTime[0] = rib.sizeMidTime;
        c.midTime[1] = rib.colorMidTime;
        c.midTime[2] = rib.alphaMidTime;
        c.midTime[3] = rib.rotationMidTime;
        c.midHold[0] = rib.sizeMidHoldTime;
        c.midHold[1] = rib.colorMidHoldTime;
        c.midHold[2] = rib.alphaMidHoldTime;
        c.midHold[3] = rib.rotationMidHoldTime;
        c.sizeSmoothing = static_cast<u8>(rib.sizeSmoothing);
        c.colorSmoothing = static_cast<u8>(rib.colorSmoothing);
        c.drag = rib.drag;
        c.mass = rib.mass;
        c.gravity3 = {rib.gravityX, rib.gravityY, rib.gravity};
        c.friction = rib.friction;
        c.bounce = rib.bounce;
        c.noiseAmplitude = rib.noiseAmplitude;
        c.noiseFrequency = rib.noiseFrequency;
        c.noiseCoherence = rib.noiseCoherence;
        c.noiseEdge = rib.noiseEdge;
        c.lodReduce = static_cast<u8>(rib.lodReduce);
        c.lodCut = static_cast<u8>(rib.lodCut);
        c.waveTypes[0] = rib.yawType;
        c.waveTypes[1] = rib.pitchType;
        c.waveTypes[2] = rib.speedType;
        c.waveTypes[3] = rib.sizeType;
        c.waveTypes[4] = rib.alphaType;
        c.lifetimeInit = rib.lifetime.initValue;
        c.maxLengthInit = rib.maxLength.initValue;
        c.initialSpeedInit = rib.initialSpeed.initValue;

        c.splines.reserve(rib.splineRibbons.size());
        for (const auto& sr : rib.splineRibbons) {
            renderer::effects::Sc2SplineRibbonConfig sc;
            sc.emissionOffset = sr.emissionOffset;
            sc.emissionVector = sr.emissionVector;
            // SRIB 0x18..0x2F are FOUR raw floats x2 (endTangent/endOffset)
            // that WhiteoutLib still parses under the pre-RE labels — a
            // 20-byte AnimRef plus `reserved` (RE §1.2). The parse is a
            // byte-faithful field copy, so the vectors reassemble exactly
            // from the mislabeled members until the WhiteoutLib struct is
            // relabeled (planned with the spline phase).
            sc.endTangent = {
                std::bit_cast<f32>(static_cast<u32>(sr.velocity.interpType) |
                                   (static_cast<u32>(sr.velocity.flags) << 16)),
                std::bit_cast<f32>(sr.velocity.animId), sr.velocity.initValue};
            sc.endOffset = {sr.velocity.nullValue,
                            std::bit_cast<f32>(sr.velocity.unused),
                            std::bit_cast<f32>(sr.reserved)};
            sc.boneIndex = static_cast<i32>(sr.boneIndex);
            sc.emissionVectorNormFactor = sr.emissionVectorNormFactor;
            sc.velocityNormFactor = sr.velocityNormFactor;
            sc.waveTypes[0] = sr.yawType;
            sc.waveTypes[1] = sr.pitchType;
            sc.waveTypes[2] = sr.velocityType;
            c.splines.push_back(sc);
        }
        out.push_back(std::move(c));
    }
    return out;
}

void M3ModelAdapter::EvaluateParticles(std::span<const M3Layer> layers,
                                       std::span<const ::whiteout::u8> visible,
                                       const Matrix44f& world,
                                       renderer::model::FrameState& fs) const {
    if (model_.particleEmitters.empty())
        return;
    // Renderer units per model unit: the world matrix's uniform scale, the
    // same derivation the ribbon and M2 routes take.
    const f32 scale = std::sqrt(world.data[0][0] * world.data[0][0] +
                                world.data[0][1] * world.data[0][1] +
                                world.data[0][2] * world.data[0][2]);
    fs.particleStates.reserve(model_.particleEmitters.size());

    const auto boneWorld = [&](std::size_t bone) {
        if (bone < fs.boneWorldMatrices.size())
            return fs.boneWorldMatrices[bone] * world;
        return world;
    };
    // Colours stay PACKED. The lifetime interpolation between the three stops
    // is integer in the shipped code (RE §5.8), so unpacking to floats here
    // would just move the rounding somewhere it cannot be checked.
    const auto packed = [](const ::whiteout::m3::ColorBGRA& c) -> u32 {
        return (static_cast<u32>(c.a) << 24) | (static_cast<u32>(c.r) << 16) |
               (static_cast<u32>(c.g) << 8) | static_cast<u32>(c.b);
    };

    // The players the squirt keys are read against: every layer, once per
    // model. Retail's reader walks each live player and resolves the track
    // through that player's own container (RE §16.7); taking the top layer
    // alone let a global loop above the sequence hide every one of its keys.
    // The times stay unwrapped — the reader wraps each on its track's end.
    // They travel to the ACTOR layer, which owns burst detection; the emitter
    // never sees an animation clock.
    fs.sc2AnimPlayers.clear();
    fs.sc2AnimPlayers.reserve(layers.size());
    for (const M3Layer& l : layers)
        fs.sc2AnimPlayers.push_back(
            {l.stc, l.timeMs, l.loop, l.sequence, l.priority, l.global, l.blendingOut});

    // `rotationFlags & 0x10 / 0x20`: an emitter pushes its matrix's row lengths
    // onto its collision / trail child's BONE, replacing that bone's own local
    // scale (RE §16.34). Composed here, where both bones are — in model space,
    // so without the actor's own scale, and always, where retail's push onto an
    // animated bone races the update order (design §8). A later `PAR_` pushing
    // the same child wins, as the element order would have it.
    const std::size_t emitterCount = model_.particleEmitters.size();
    const auto modelBone = [&](std::size_t bone) {
        return bone < fs.boneWorldMatrices.size() ? fs.boneWorldMatrices[bone]
                                                  : Matrix44f::identity();
    };
    std::vector<Vector3f> pushedScale(emitterCount, Vector3f{0.0f, 0.0f, 0.0f});
    std::vector<::whiteout::u8> pushed(emitterCount, 0);
    for (std::size_t j = 0; j < emitterCount; ++j) {
        const auto& parent = model_.particleEmitters[j];
        const u32 rf = static_cast<u32>(parent.rotationFlags);
        if ((rf & 0x30u) == 0u)
            continue;
        const Matrix44f m = modelBone(parent.boneIndex);
        std::array<f32, 16> rows{};
        for (std::size_t r = 0; r < 4; ++r)
            for (std::size_t c = 0; c < 4; ++c)
                rows[r * 4 + c] = m.data[r][c];
        const Vector3f lengths = renderer::particle::Sc2ChildScale(rows);
        const auto onto = [&](i32 child) {
            if (child >= 0 && static_cast<std::size_t>(child) < emitterCount) {
                pushedScale[static_cast<std::size_t>(child)] = lengths;
                pushed[static_cast<std::size_t>(child)] = 1;
            }
        };
        if ((rf & 0x10u) != 0u)
            onto(parent.collisionSpawnIndex);
        if ((rf & 0x20u) != 0u)
            onto(parent.trailLinkIndex);
    }

    for (std::size_t i = 0; i < model_.particleEmitters.size(); ++i) {
        const auto& par = model_.particleEmitters[i];
        renderer::model::FrameState::ParticleFrameState st{};
        st.emitterId = static_cast<i32>(i);
        st.transform = boneWorld(par.boneIndex);
        if (pushed[i] != 0) {
            // The bone's own local scale: the row lengths of its local matrix,
            // its model-space matrix over its parent's.
            const std::size_t bone = par.boneIndex;
            Matrix44f local = modelBone(bone);
            if (bone < model_.bones.size()) {
                const u16 up = model_.bones[bone].parentIndex;
                if (up != 0xFFFFu && up < fs.boneWorldMatrices.size())
                    local = local * Matrix44f::inverse(fs.boneWorldMatrices[up]);
            }
            const auto rowLength = [&local](std::size_t r) {
                return std::sqrt(local.data[r][0] * local.data[r][0] +
                                 local.data[r][1] * local.data[r][1] +
                                 local.data[r][2] * local.data[r][2]);
            };
            const Vector3f localScale{rowLength(0), rowLength(1), rowLength(2)};
            st.transform = renderer::particle::Sc2PushChildScale(modelBone(bone), localScale,
                                                                  pushedScale[i]) *
                           world;
        }
        // Where the emitter is: the spawn sweep starts a world-space particle
        // there, and without it every one is born at the world origin.
        st.worldPosition = {st.transform.data[3][0], st.transform.data[3][1],
                            st.transform.data[3][2]};
        st.unitScale = (scale > 0.0f) ? scale : 1.0f;
        // The WC3-family scalars stay at their defaults — the SC2 stages read
        // the block below instead. `visibility` carries the bone-visibility
        // gate the way retail's node-active bit does.
        st.visibility = (par.boneIndex >= visible.size() || visible[par.boneIndex]) ? 1.0f : 0.0f;
        st.modelParticle = !par.modelPaths.empty();

        auto& s2 = st.sc2;
        s2.emissionRate = SampleRef(par.emissionRate, layers);
        s2.speed = SampleRef(par.initialSpeed, layers);
        s2.speedRandom = SampleRef(par.initialSpeedRandom, layers);
        // Yaw/pitch are DEGREES and convert at use — the emitter's only
        // deg→rad. Rotation keys below are RADIANS end to end and convert
        // nowhere, which is the correction that matters here: reading them as
        // degrees spins a particle 57× too fast.
        s2.yawDeg = SampleRef(par.initialYaw, layers);
        s2.pitchDeg = SampleRef(par.initialPitch, layers);
        s2.horizontal = SampleRef(par.initialHorizontal, layers);
        s2.vertical = SampleRef(par.initialVertical, layers);
        s2.lifetime = SampleRef(par.lifetime, layers);
        s2.lifetimeRandom = SampleRef(par.lifetimeRandom, layers);
        s2.size3 = SampleRef(par.sizeAnimation, layers);
        s2.sizeRandom3 = SampleRef(par.sizeRandomAnimation, layers);
        s2.rotation3 = SampleRef(par.rotationAnimation, layers);
        s2.rotationRandom3 = SampleRef(par.rotationRandomAnimation, layers);
        s2.colorBGRA[0] = packed(SampleRef(par.colorStart, layers));
        s2.colorBGRA[1] = packed(SampleRef(par.colorMid, layers));
        s2.colorBGRA[2] = packed(SampleRef(par.colorEnd, layers));
        s2.colorRandomBGRA[0] = packed(SampleRef(par.colorStartRandom, layers));
        s2.colorRandomBGRA[1] = packed(SampleRef(par.colorMidRandom, layers));
        s2.colorRandomBGRA[2] = packed(SampleRef(par.colorEndRandom, layers));
        s2.shapeOuter = SampleRef(par.shapeOuter, layers);
        s2.shapeInner = SampleRef(par.shapeInner, layers);
        s2.outerRadius = SampleRef(par.outerRadius, layers);
        s2.innerRadius = SampleRef(par.innerRadius, layers);
        // Only read when inheriting (stateFlags bit 3); sampling it otherwise
        // would put a live track's value where the runtime keeps a zero.
        s2.parentVelocityScale = (static_cast<u32>(par.flags) & 0x40u)
                                     ? SampleRef(par.particleVelocity, layers)
                                     : 0.0f;
        s2.trailEmissionRate = SampleRef(par.trailEmissionRate, layers);
        s2.splineLower = SampleRef(par.lowerBound, layers);
        s2.splineUpper = SampleRef(par.upperBound, layers);

        // Overlay waves, in the runtime's channel order. Their static types
        // live in the desc; sampling the pairs unconditionally is cheap and
        // inert wherever the type is 0.
        const ::whiteout::m3::AnimRef<f32>* amp[9] = {
            &par.yawAmplitude,      &par.pitchAmplitude,      &par.speedAmplitude,
            &par.sizeAmplitude,     &par.alphaAmplitude,      &par.colorAmplitude,
            &par.rotationAmplitude, &par.horizontalAmplitude, &par.verticalAmplitude};
        const ::whiteout::m3::AnimRef<f32>* freq[9] = {
            &par.yawFrequency,      &par.pitchFrequency,      &par.speedFrequency,
            &par.sizeFrequency,     &par.alphaFrequency,      &par.colorFrequency,
            &par.rotationFrequency, &par.horizontalFrequency, &par.verticalFrequency};
        for (int k = 0; k < 9; ++k) {
            s2.overlayAmp[k] = SampleRef(*amp[k], layers);
            s2.overlayFreq[k] = SampleRef(*freq[k], layers);
        }
        s2.overlayPhase = SampleRef(par.phaseShift, layers);

        // Shape 6 only: the control points are an AnimRef each, so the whole
        // spline moves per frame rather than being a load-time constant.
        if (par.emitterShape == ::whiteout::m3::EmitterShape::Spline) {
            s2.splinePoints.reserve(par.splineLineData.size());
            for (const auto& pt : par.splineLineData)
                s2.splinePoints.push_back(SampleRef(pt, layers));
        }

        // `PARC` copies are emission SLOTS of this emitter, not emitters: each
        // overrides the rate and the bone, and nothing else.
        s2.slots.reserve(par.copyIndices.size());
        for (const u32 ci : par.copyIndices) {
            if (ci >= model_.particleEmitterCopies.size())
                continue;
            const auto& cp = model_.particleEmitterCopies[ci];
            renderer::model::FrameState::ParticleFrameState::Sc2ParticleFrame::Slot slot;
            slot.emissionRate = SampleRef(cp.emissionRate, layers);
            slot.boneWorld = boneWorld(cp.boneIndex);
            s2.slots.push_back(slot);
        }

        s2.active = st.visibility > 0.0f;
        fs.particleStates.push_back(std::move(st));
    }
}

std::vector<renderer::effects::Sc2ParticleEmitterConfig> M3ModelAdapter::GetSc2ParticleConfigs() {
    using ::whiteout::m3::EmitterShape;
    using ::whiteout::m3::ParticleEmitter;

    // Every container this animId is keyed in, so a squirt table survives load
    // with its key TIMES intact. Sampling cannot recover them: a squirt fires
    // when the clock steps over a key (RE §7b), which is a property of the
    // key list and not of any one frame's value.
    //
    // The blocks are in SDS6, the SIGNED 16-bit array — not SDU6, which the
    // `AnimRef<u16>` declaration implies and which is EMPTY in every corpus
    // model. Measured, not assumed: 3131 of 3131 bound `squirtAmount` refs
    // across both corpora resolve to slot 7. The declared C++ type does not
    // name the slot; only the container's own `animRefs` word does. Both are
    // read anyway, because covering the declared one costs three lines and a
    // burst count is non-negative either way.
    const auto squirtTable = [this](const ::whiteout::m3::AnimRef<::whiteout::u16>& ref) {
        renderer::effects::Sc2SquirtKeys keys;
        const i32 row = tables_.RowOf(ref.animId);
        if (ref.animId == 0 || ref.animId == 0xFFFFFFFFu || row < 0)
            return keys;
        // `UpdateSlotEmission` reads nothing unless the ref animates (`flags &
        // 2`, RE §5.2), so a bound track without the bit owes no burst.
        if ((ref.flags & 0x2u) == 0u)
            return keys;
        // Carried as the u16 the runtime's track table holds (OP7b's `values`
        // are u16). `ComputeEmitCount` then reads each crossed key back SIGNED
        // and floors a negative one at zero (RE §16.31), so a key with the high
        // bit set owes no burst — `Sc2SquirtBurst` applies that at the sum.
        const auto take = [&keys](u16 stc, const auto& blk) {
            const std::size_t n = (std::min)(blk.timestamps.size(), blk.keys.size());
            for (std::size_t k = 0; k < n; ++k) {
                keys.push_back({stc, static_cast<f32>(blk.timestamps[k]),
                                static_cast<f32>(static_cast<u16>(blk.keys[k])),
                                static_cast<i32>(blk.endFrame)});
            }
        };
        for (u16 stc = 0; stc < tables_.StcCount(); ++stc) {
            const ::whiteout::m3::SubTrackContainer* coll = tables_.StcAt(stc);
            const M3TrackHandle h = tables_.At(row, stc);
            if (!coll || !h.Valid())
                continue;
            if (const auto* b = BlockOf(*coll, h, static_cast<const ::whiteout::i16*>(nullptr)))
                take(stc, *b);
            else if (const auto* u = BlockOf(*coll, h, static_cast<const ::whiteout::u16*>(nullptr)))
                take(stc, *u);
        }
        return keys;
    };
    // The pre-roll's peaks, one per container. `EmitBurst` reads the lifetime
    // track in the column the ACTIVE SEQUENCE's number names (RE §16.33), which
    // only the frame knows, so the desc holds every column's answer — each
    // through the gated `Sc2PreRollPeak`: the curve seeded at zero, or the raw
    // init value where the container has no track. An unbound track has no
    // columns and budgets from its init value alone.
    const auto preRollPeaks = [this](const ::whiteout::m3::AnimRef<f32>& ref) {
        std::vector<f32> peaks;
        const i32 row = tables_.RowOf(ref.animId);
        if (ref.animId == 0 || ref.animId == 0xFFFFFFFFu || row < 0)
            return peaks;
        peaks.assign(tables_.StcCount(), ref.initValue);
        for (u16 stc = 0; stc < tables_.StcCount(); ++stc) {
            const ::whiteout::m3::SubTrackContainer* coll = tables_.StcAt(stc);
            const M3TrackHandle h = tables_.At(row, stc);
            if (!coll || !h.Valid())
                continue;
            if (const auto* blk = BlockOf(*coll, h, static_cast<const f32*>(nullptr)))
                peaks[stc] = renderer::particle::Sc2PreRollPeak(blk->keys, true, ref.initValue);
        }
        return peaks;
    };

    std::vector<renderer::effects::Sc2ParticleEmitterConfig> out;
    out.reserve(model_.particleEmitters.size());
    for (const ParticleEmitter& par : model_.particleEmitters) {
        renderer::effects::Sc2ParticleEmitterConfig c;
        c.materialIndex = static_cast<i32>(par.materialIndex);
        c.flags = static_cast<u32>(par.flags);
        c.additionalFlags = static_cast<u32>(par.additionalFlags);
        c.rotationFlags = static_cast<u32>(par.rotationFlags);
        c.forces = static_cast<u32>(par.localForces) | (static_cast<u32>(par.worldForces) << 16);
        c.forcesFallback = static_cast<u32>(par.localForcesFallback) |
                           (static_cast<u32>(par.worldForcesFallback) << 16);

        // WhiteoutLib's `EmitterShape` already carries the RE's numbering —
        // 6 Spline, 7 Mesh — so this is a cast, not the swap the design
        // budgeted for. The spec that had them the other way round is what the
        // enum was corrected against.
        c.emitShape = static_cast<u8>(par.emitterShape);
        c.velocityType = par.velocityType;
        c.maxParticles = par.maxParticles;
        c.lodReduce = static_cast<u8>(par.lodReduce);
        c.lodCut = static_cast<u8>(par.lodCut);
        c.shapeRegions.reserve(par.shapeRegions.size());
        for (const u32 r : par.shapeRegions)
            c.shapeRegions.push_back(static_cast<i32>(r));

        // Slot 0 is the emitter itself; 1..n are its copies, in `copyIndices`
        // order. The squirt tables are index-parallel, so a copy that
        // overrides nothing still occupies its slot.
        c.slotBones.push_back(static_cast<i32>(par.boneIndex));
        c.squirt.push_back(squirtTable(par.squirtAmount));
        for (const u32 ci : par.copyIndices) {
            if (ci >= model_.particleEmitterCopies.size())
                continue;
            const auto& cp = model_.particleEmitterCopies[ci];
            c.slotBones.push_back(static_cast<i32>(cp.boneIndex));
            c.squirt.push_back(squirtTable(cp.squirtAmount));
        }

        c.sizeRandom = par.sizeRandomEnable != 0;
        c.rotationRandom = par.rotationRandomEnable != 0;
        c.colorRandom = par.colorRandomEnable != 0;
        // `alphaRandomEnable` (+0x2BC) is not carried: `SampleParticleColor`
        // (4.8 `0x102920A90`) tests `colorRandomEnable` at +0x27C and never
        // reads the record's +0x2BC.
        const u32 types[9] = {par.yawType,      par.pitchType,      par.speedType,
                              par.sizeType,     par.alphaType,      par.colorType,
                              par.rotationType, par.horizontalType, par.verticalType};
        for (int k = 0; k < 9; ++k)
            c.overlayType[k] = types[k];

        c.drag = par.drag;
        c.mass = par.mass;
        c.massRandom = par.massRandom;
        // gravityX/Y are u32 in the struct and floats in the file — they are
        // documented "expected 0" and every corpus carrier has them so, but a
        // bit pattern is what they hold, not a count.
        c.gravity3 = {std::bit_cast<f32>(par.gravityX), std::bit_cast<f32>(par.gravityY),
                      par.gravity};
        c.bounce = par.bounce;
        c.friction = par.friction;
        c.collisionDieBounce = par.collisionDieBounce;
        c.killRadius = par.killRadius;
        c.windMultiplier = par.windMultiplier;
        c.noiseAmplitude = par.noiseAmplitude;
        c.noiseFrequency = par.noiseFrequency;
        c.noiseCoherence = par.noiseCoherence;
        c.noiseEdge = par.noiseEdge;

        c.instanceType = static_cast<u8>(par.instanceType);
        c.midTime[0] = par.sizeMidTime;
        c.midTime[1] = par.colorMidTime;
        c.midTime[2] = par.alphaMidTime;
        c.midTime[3] = par.rotationMidTime;
        c.midHold[0] = par.sizeMidHoldTime;
        c.midHold[1] = par.colorMidHoldTime;
        c.midHold[2] = par.alphaMidHoldTime;
        c.midHold[3] = par.rotationMidHoldTime;
        c.sizeSmoothing = static_cast<u8>(par.sizeSmoothing);
        c.colorSmoothing = static_cast<u8>(par.colorSmoothing);
        c.rotationSmoothing = static_cast<u8>(par.rotationSmoothing);
        c.flipbookMidTime = par.flipbookMidTime;
        c.flipbookColumns = par.flipbookColumns;
        c.flipbookRows = par.flipbookRows;
        c.flipbookColumnFraction = par.flipbookColumnFraction;
        c.flipbookRowFraction = par.flipbookRowFraction;
        c.flipbookStartInit = par.flipbookStartInitIndex;
        c.flipbookStartStop = par.flipbookStartStopIndex;
        c.flipbookEndInit = par.flipbookEndInitIndex;
        c.tailLength = par.tailLength;
        c.instanceAngle = par.instanceAngle;
        c.instanceDistance = par.instanceDistance;

        c.collisionSpawnIndex = par.collisionSpawnIndex;
        c.collisionSpawnMin = par.collisionSpawnMin;
        c.collisionSpawnMax = par.collisionSpawnMax;
        c.collisionSpawnChance = par.collisionSpawnChance;
        c.collisionSpawnEnergy = par.collisionSpawnEnergy;
        c.trailLinkIndex = par.trailLinkIndex;
        c.trailChance = par.trailChance;
        c.splatProjectorIndex = par.splatProjectionIndex;
        c.splatChance = par.splatChance;
        // The ribbon campaign named this pair; OP14 settled what reads it.
        c.modelOrientPreset = par.spawnRibbonOnBounceChance;
        c.modelOrientVariant = par.ribbonLinkIndex;
        // Without the terminator the reference counts: a path that keeps it
        // never ends in `.m3`, and never reads.
        c.modelPaths.clear();
        c.modelPaths.reserve(par.modelPaths.size());
        for (const auto& path : par.modelPaths)
            c.modelPaths.emplace_back(TrimNuls(path));

        // `lifetimeRandom` is the track EmitBurst peaks when the randomise bit
        // is set, `lifetime` otherwise (RE §15.4) — not the larger of the two.
        const bool randomiseLifespan =
            (static_cast<u32>(par.additionalFlags) &
             static_cast<u32>(::whiteout::m3::ParticleAdditionalFlag::LifespanRandomize)) != 0;
        const auto& lifetimeRef = randomiseLifespan ? par.lifetimeRandom : par.lifetime;
        c.preRollPeaks = preRollPeaks(lifetimeRef);
        c.preRollInit = lifetimeRef.initValue;

        out.push_back(std::move(c));
    }
    return out;
}

std::shared_ptr<const renderer::particle::EmitMesh>
BuildM3EmitMesh(const ::whiteout::m3::Model& model, std::size_t divisionIndex,
                std::span<const u8> wanted) {
    namespace pp = renderer::particle;
    if (divisionIndex >= model.divisions.size())
        return nullptr;
    const auto& div = model.divisions[divisionIndex];
    if (div.regions.empty() || wanted.empty())
        return nullptr;

    const std::vector<Vector3f> positions = model.vertices.getPositions();
    // Shape 7 rejects positions against the R byte (OP13). A format without
    // vertex colour leaves the array empty, which the sampler reads as 255.
    const std::vector<::whiteout::m3::ColorBGRA> colours =
        model.vertices.hasVertexColors() ? model.vertices.getColors()
                                         : std::vector<::whiteout::m3::ColorBGRA>{};
    const std::size_t stride = model.vertices.vertexSize();
    const std::vector<u8>& blob = model.vertices.data;
    const auto& lookup = model.boneLookup;
    const i32 boneCount = static_cast<i32>(model.bones.size());
    const bool skinned = stride >= 20 && boneCount > 0;

    auto mesh = std::make_shared<pp::EmitMesh>();
    // One entry per REGION of the division, so an emitter's `shapeRegions` is
    // an index into `subs` with no remap. The regions nothing emits from get an
    // empty entry rather than being skipped — a sparse list would make the
    // indices lie, and an empty entry costs three words.
    mesh->subs.resize(div.regions.size());

    for (std::size_t r = 0; r < div.regions.size(); ++r) {
        if (r >= wanted.size() || !wanted[r])
            continue;
        const auto& region = div.regions[r];
        if (region.indexCount < 3 || region.vertexCount == 0)
            continue;
        const std::size_t vBegin = region.firstVertex;
        const std::size_t vEnd = vBegin + region.vertexCount;
        if (vEnd > positions.size() || vEnd * stride > blob.size())
            continue;
        const std::size_t iEnd = region.firstIndex + region.indexCount;
        if (iEnd > div.faces.size())
            continue;

        // Where this region's vertices land in the shared arrays. `tris` is
        // global by construction, so the sampler never needs the region back.
        const u32 base = static_cast<u32>(mesh->rest.size());
        for (std::size_t v = vBegin; v < vEnd; ++v)
            mesh->rest.push_back(positions[v]);
        if (colours.size() >= vEnd) {
            for (std::size_t v = vBegin; v < vEnd; ++v)
                mesh->colorR.push_back(colours[v].r);
        }

        if (skinned) {
            for (std::size_t v = vBegin; v < vEnd; ++v) {
                const u8* rec = blob.data() + v * stride;
                std::array<i32, pp::kEmitMeshBones> b{};
                std::array<f32, pp::kEmitMeshBones> w{};
                for (usize k = 0; k < pp::kEmitMeshBones; ++k) {
                    // A vertex names a slot of its REGION's bone-lookup window,
                    // which is that region's whole palette — resolve it to a
                    // global bone here, because EmitMesh is posed against the
                    // actor's skeleton and knows nothing about regions.
                    const std::size_t slot = region.firstBoneLookup + rec[16 + k];
                    i32 gb = (slot < lookup.size()) ? static_cast<i32>(lookup[slot]) : 0;
                    if (gb < 0 || gb >= boneCount)
                        gb = 0;
                    b[k] = gb;
                    w[k] = rec[12 + k] / 255.0f;
                }
                mesh->bones.push_back(b);
                mesh->weights.push_back(w);
            }
        }

        pp::EmitMesh::SubMesh sm;
        sm.firstTri = static_cast<u32>(mesh->tris.size() / 3);
        f32 area = 0.0f;
        for (std::size_t i = region.firstIndex; i + 2 < iEnd; i += 3) {
            const u32 i0 = base + div.faces[i];
            const u32 i1 = base + div.faces[i + 1];
            const u32 i2 = base + div.faces[i + 2];
            if (i0 >= mesh->rest.size() || i1 >= mesh->rest.size() || i2 >= mesh->rest.size())
                continue;
            mesh->tris.push_back(i0);
            mesh->tris.push_back(i1);
            mesh->tris.push_back(i2);
            // The running sum is BIND-pose area, like the engine's — it caches
            // one table per mesh and never rebuilds it, so a posed model still
            // picks triangles in their rest-pose proportion. SC2's own shape 7
            // picks uniformly over the triangle INDEX and ignores this, but the
            // table costs one float per triangle and D3 shape 10 needs it.
            const Vector3f& p0 = mesh->rest[i0];
            const Vector3f& p1 = mesh->rest[i1];
            const Vector3f& p2 = mesh->rest[i2];
            const Vector3f e1{p1.x - p0.x, p1.y - p0.y, p1.z - p0.z};
            const Vector3f e2{p2.x - p0.x, p2.y - p0.y, p2.z - p0.z};
            const Vector3f n{e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z,
                             e1.x * e2.y - e1.y * e2.x};
            area += 0.5f * std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
            mesh->areaCdf.push_back(area);
        }
        sm.triCount = static_cast<u32>(mesh->tris.size() / 3) - sm.firstTri;
        mesh->subs[r] = sm;
    }

    if (mesh->tris.empty())
        return nullptr;
    // Every region's bytes or none: a partial array would shift the index of
    // every vertex after the first region that had no colour.
    if (mesh->colorR.size() != mesh->rest.size())
        mesh->colorR.clear();
    // A model with no usable weights samples the rest pose, and saying so with
    // empty arrays is what SkinEmitMeshVertex's early-out reads.
    if (!skinned) {
        mesh->bones.clear();
        mesh->weights.clear();
    }
    return std::shared_ptr<const pp::EmitMesh>(std::move(mesh));
}

} // namespace whiteout::flakes::io

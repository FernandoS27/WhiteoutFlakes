#include "io/d3/d3_model_adapter.h"

#include "renderer/animation/anim_math.h"
#include "renderer/profiles/diablo3/d3_collision.h"
#if WDX_HAS_PHYSICS
#include "renderer/profiles/diablo3/d3_cloth.h"
#include "renderer/profiles/diablo3/d3_physics.h"
#endif

#include <whiteout/sno/d3/native/geometry.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_set>

namespace whiteout::flakes::io {

using renderer::model::MeshData;
using renderer::model::SequenceInfo;
using renderer::model::SkeletonData;
using renderer::model::SkinWeightData;
using renderer::model::TextureData;
using renderer::model::FrameState;
using renderer::model::VertexAttribute;
using renderer::model::VertexSemantic;

namespace {

bool EqualCi(std::string_view a, std::string_view b) {
    if (a.size() != b.size())
        return false;
    for (usize i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

Quaternion QuatOf(const Vector4f& v) {
    return {v.x, v.y, v.z, v.w};
}

Quaternion NormalizeQ(Quaternion q) {
    const f32 len = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (len <= 1e-8f)
        return Quaternion::identity();
    const f32 inv = 1.0f / len;
    return {q.x * inv, q.y * inv, q.z * inv, q.w * inv};
}

// PRSTransform -> Matrix44f through the shared kernel, with a zero pivot: a D3
// bone's transform is a complete local frame, so there is nothing to compose
// around. Never transcribed from the guide's row-vector formula — that layout
// matters only when comparing a palette dump pulled out of the game.
Matrix44f MatrixOf(const d3n::PRSTransform& t) {
    const Vector3f s{t.flScale, t.flScale, t.flScale};
    return renderer::animation::ComposePivotSRT(t.vTranslation, NormalizeQ(QuatOf(t.qRotation)), s,
                                                {0.0f, 0.0f, 0.0f});
}

// The signed decode. native::Quaternion16 declares u16 and the encoding is
// i16 / 32767 — proven over 11,556,940 of 11,556,940 corpus rotation keys (mean
// sum-of-squares 0.999960, worst deviation 5.96e-05, exactly the quantisation
// step). Read as unsigned, every rotation lands in the wrong hemisphere and the
// model folds. `w` is LAST.
Quaternion DecodeQ16(const d3n::Quaternion16& q) {
    auto s = [](u16 v) { return static_cast<f32>(static_cast<i16>(v)) * (1.0f / 32767.0f); };
    return NormalizeQ({s(q.nX), s(q.nY), s(q.nZ), s(q.nW)});
}

// NLERP with a dot-sign flip. Quat_Nlerp (0x710097A9D0) is flip, lerp, 1/sqrt;
// there is no acos anywhere in the subsystem.
Quaternion Nlerp(const Quaternion& a, Quaternion b, f32 t) {
    const f32 dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (dot < 0.0f) {
        b.x = -b.x;
        b.y = -b.y;
        b.z = -b.z;
        b.w = -b.w;
    }
    return NormalizeQ({a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t,
                       a.w + (b.w - a.w) * t});
}

Vector3f Lerp3(const Vector3f& a, const Vector3f& b, f32 t) {
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
}

// Which of a curve's keys brackets `frame`, and how far between them. Keys are
// frame-stamped and monotonic; a curve with one key holds it.
template <typename KeyT>
void Bracket(const std::vector<KeyT>& keys, f32 frame, usize& i0, usize& i1, f32& t) {
    i0 = i1 = 0;
    t = 0.0f;
    if (keys.size() < 2)
        return;
    const auto it = std::upper_bound(keys.begin(), keys.end(), frame,
                                     [](f32 f, const KeyT& k) {
                                         return f < static_cast<f32>(k.nFrame);
                                     });
    if (it == keys.begin()) {
        i0 = i1 = 0;
        return;
    }
    if (it == keys.end()) {
        i0 = i1 = keys.size() - 1;
        return;
    }
    i1 = static_cast<usize>(it - keys.begin());
    i0 = i1 - 1;
    const f32 f0 = static_cast<f32>(keys[i0].nFrame);
    const f32 f1 = static_cast<f32>(keys[i1].nFrame);
    t = (f1 > f0) ? (frame - f0) / (f1 - f0) : 0.0f;
}

// Both vertex colours in one 8-byte attribute, two 8-bit channels per UNORM16
// lane: `lane = c0 | (c1 << 8)`.
//
// One element rather than two because a *second* COLOR would need semantic
// index 1, and this toolchain cannot round-trip that: Slang appends its own '0'
// to every semantic it emits, so `COLOR1` becomes `COLOR10` — index ten — and
// D3D11/D3D12 then reject the input layout outright (m2_combiners.slang carries
// the same note for TEXCOORD1). A u16 survives the UNORM round trip exactly —
// f32 has a 24-bit mantissa — so the shader recovers both bytes with
// `uint(round(lane * 65535.0))`.
//
// The swizzle happens exactly here, at the point of packing: in the shader it
// would be invisible to the geometry test.
void PackColorPair(const d3n::VertexColor& c0, const d3n::VertexColor& c1, u16 (&out)[4]) {
    out[0] = static_cast<u16>(c0.r | (c1.r << 8));
    out[1] = static_cast<u16>(c0.g | (c1.g << 8));
    out[2] = static_cast<u16>(c0.b | (c1.b << 8));
    out[3] = static_cast<u16>(c0.a | (c1.a << 8));
}

// The record DescribeD3Vertex describes. `.m2` and `.m3` bake their on-disk
// blobs verbatim; D3's is not a valid vertex buffer — only the position is
// plain float data — so this is a *repack*, not a passthrough, and it exists
// because the interleaved WC3 `Vertex` the parallel-array path uploads has room
// for neither the second UV set nor a vertex colour a source supplied.
struct D3Vertex {
    Vector3f position;
    Vector3f normal;
    Vector2f uv0;
    Vector2f uv1;
    Vector4f tangent; // .xyz tangent, .w bitangent sign
    u16 colors[4];    // see PackColorPair
};
static_assert(sizeof(D3Vertex) == 64, "DescribeD3Vertex offsets assume a 64-byte record");

Vector3f Cross(const Vector3f& a, const Vector3f& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
f32 Dot(const Vector3f& a, const Vector3f& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

// Actor_PickWeightedLookIndex (0x710060A010), with a fixed seed. The engine
// hashes a per-instance seed; a viewer that re-randomised on every reload would
// be unreproducible, and the census says the pick almost never has a choice
// anyway (weight 0 x 140,044 against 100 x 13,071).
u32 PickWeightedLook(const d3n::Actor& actor, const d3n::Appearances& app) {
    const d3n::WeightedLook* looks[8] = {&actor.tLook0, &actor.tLook1, &actor.tLook2,
                                         &actor.tLook3, &actor.tLook4, &actor.tLook5,
                                         &actor.tLook6, &actor.tLook7};
    i32 total = 0;
    for (const auto* l : looks)
        total += (std::max)(0, l->nWeight);
    if (total <= 0)
        return 0;

    // Fixed seed: the appearance's own id, so two spawns of one actor agree and
    // two different actors sharing an appearance do too.
    const u32 r = static_cast<u32>(app.dwSnoId) % static_cast<u32>(total + 1);
    i32 acc = 0;
    for (const auto* l : looks) {
        acc += (std::max)(0, l->nWeight);
        if (static_cast<i32>(r) <= acc) {
            // Appearance_FindLookIndex: a linear scan of arLooks by name.
            for (usize i = 0; i < app.arLooks.size(); ++i) {
                if (EqualCi(app.arLooks[i].szName, l->szName))
                    return static_cast<u32>(i);
            }
            return 0;
        }
    }
    return 0;
}

} // namespace

// ---------------------------------------------------------------------------
// Material resolution
// ---------------------------------------------------------------------------

const d3n::SubObjectAppearance* D3VariantFor(const d3n::Appearances& app,
                                             const d3n::SubObject& sub, u32 lookIndex) {
    for (const auto& mat : app.arMaterials) {
        // **szName, not szMaterialName.** WhiteoutLib's two string fields are
        // named the wrong way round for what they hold: `szName` is the
        // material name and `szMaterialName` is a per-instance mesh id
        // ("HC_x02_y01_<material>_001"). Measured over 300 corpus `.app`:
        // AppearanceMaterial.szName matches SubObject.szName **2413 of 2413**
        // times and SubObject.szMaterialName **1** time. Joining on the
        // plausibly-named field leaves every sub-object without a material —
        // which draws as unlit grey rather than failing, so nothing says so.
        if (!EqualCi(mat.szName, sub.szName))
            continue;
        if (lookIndex < mat.arVariants.size())
            return &mat.arVariants[lookIndex];
        // arVariants.size() == dwLookCount holds on 64,899/64,899 slots, so a
        // short list is a parse bug rather than content variation. Fall back to
        // look 0 rather than dropping the sub-object, and let the surface-table
        // test be the thing that reports it.
        return mat.arVariants.empty() ? nullptr : &mat.arVariants[0];
    }
    return nullptr;
}

const d3n::UberMaterial* D3MaterialOf(const d3n::SubObjectAppearance& variant, D3SnoCache* cache,
                                      std::shared_ptr<const d3n::Material>& keepAlive) {
    // The embedded material is the per-look override and the SNO is the shared
    // base, so embedded wins wherever both are present. "Present" is a
    // populated texture list — an all-zero UberMaterial is what a variant with
    // no override carries.
    if (!variant.tMaterial.arTextures.empty())
        return &variant.tMaterial;
    if (cache && variant.snoMaterial.valid()) {
        keepAlive = cache->Material(variant.snoMaterial.id);
        if (keepAlive)
            return &keepAlive->tMaterial;
    }
    return variant.tMaterial.arTextures.empty() ? nullptr : &variant.tMaterial;
}

// Which entries reach the canonical texture list: the ones a named slot binds,
// plus the ones a `Legacy.fx` chain stage can. Collecting every own-texture
// entry instead would acquire the model-wide 25..38 detail block — a dozen-odd
// GPU textures per model that nothing samples.
//
// The second half is a MEASUREMENT, not a guess: these are the content-stage
// types of all 855 shipped `Legacy.fx` passes that also own their texture
// (`Render_ResolveMaterialTextureStages`' default branch), with 0, 25, 40 and
// 41 dropped because those stages read a core asset and never the entry.
// Widening costs 24.2% more canonical textures over the whole 11,347-model
// corpus — mean 5.6 to 7.0 per model, worst 33 to 49 — and is what a chain
// needs to draw at all: `actor_seismicSlam_wave`'s two colour layers are types
// 11 and 13, and the slot map has no entry for either, so the list they were
// missing from is why the Death Maiden's fire swoosh was a white sheet.
bool D3LegacyChainType(i32 type) {
    switch (type) {
    case 4:
    case 10:
    case 11:
    case 13:
    case 15:
    case 16:
    case 17:
    case 42:
    case 44:
    case 46:
    case 58:
        return true;
    default:
        return false;
    }
}

bool D3SlotSamples(const d3n::MaterialTextureEntry& e) {
    const i32 type = D3TextureTypeOf(e);
    return D3TypeOwnsTexture(type) &&
           (D3SlotOfType(type) != D3SlotKind::Count || D3LegacyChainType(type));
}

std::vector<D3TextureRef> CollectD3Textures(const d3n::Appearances& app, u32 lookIndex) {
    std::vector<D3TextureRef> out;
    auto add = [&out](i32 sno) {
        if (sno <= 0)
            return;
        for (const auto& e : out) {
            if (e.snoId == sno)
                return;
        }
        out.push_back(D3TextureRef{sno, false});
    };

    // Walks the *materials*, not the sub-objects, so the order is stable
    // regardless of which sub-objects a later skip filter drops. Only the
    // embedded material is reachable without a cache; a SNO-only variant
    // contributes its textures when the surface table resolves it, which it
    // does through this same function with a cache in hand.
    for (const auto& mat : app.arMaterials) {
        const usize v = (lookIndex < mat.arVariants.size()) ? lookIndex : 0;
        if (mat.arVariants.empty())
            continue;
        for (const auto& tex : mat.arVariants[v].tMaterial.arTextures) {
            if (D3SlotSamples(tex))
                add(tex.snoTexture.id);
        }
    }
    return out;
}

std::vector<D3TextureRef> CollectD3Textures(const d3n::Appearances& app, u32 lookIndex,
                                            std::span<const D3SubObjectRef> emitted,
                                            std::span<const u32> lookByGeoset) {
    // The uniform pass first, unchanged and in the same order, so an undressed
    // model produces the identical list it always did — the canonical order is
    // an index space two other places hold ids into, and quietly permuting it
    // is how `.m3` broke twice.
    std::vector<D3TextureRef> out = CollectD3Textures(app, lookIndex);
    auto add = [&out](i32 sno) {
        if (sno <= 0)
            return;
        for (const auto& e : out) {
            if (e.snoId == sno)
                return;
        }
        out.push_back(D3TextureRef{sno, false});
    };

    // Then whatever the overrides reach that the uniform pass did not. Walking
    // sub-objects rather than materials is the point: one material serves a
    // whole weight class, so two pieces of it can sit at two look indices and
    // the material walk can only ever see one.
    const d3n::GeoSet* sets[2] = {&app.tGeoSet0, &app.tGeoSet1};
    for (usize g = 0; g < emitted.size() && g < lookByGeoset.size(); ++g) {
        const u32 look = lookByGeoset[g];
        if (look == lookIndex)
            continue;
        const auto& r = emitted[g];
        const auto& subs = sets[r.geoSet & 1]->arSubObjects;
        if (r.index >= subs.size())
            continue;
        const d3n::SubObjectAppearance* variant = D3VariantFor(app, subs[r.index], look);
        if (!variant)
            continue;
        for (const auto& tex : variant->tMaterial.arTextures) {
            if (D3SlotSamples(tex))
                add(tex.snoTexture.id);
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

std::shared_ptr<D3ModelAdapter> D3ModelAdapter::LoadActor(const ContentRef& ref,
                                                          std::span<const u8> bytes,
                                                          D3SnoCache& cache, bool lazyClips) {
    // The caller already read the file to sniff its magic; adopting those bytes
    // is what keeps the very first load from reading it twice.
    auto actor = cache.AdoptActor(D3SnoIdOfBytes(bytes), bytes);
    if (!actor) {
        std::fprintf(stderr, "[d3] '%s' is not a parsable Actor\n", ref.Describe().c_str());
        return nullptr;
    }
    return FromActor(std::move(actor), cache, lazyClips, ref.Describe());
}

std::shared_ptr<D3ModelAdapter> D3ModelAdapter::LoadActorBySno(i32 snoActor, D3SnoCache& cache,
                                                               bool lazyClips) {
    auto actor = cache.Actor(snoActor);
    if (!actor)
        return nullptr;
    return FromActor(std::move(actor), cache, lazyClips, "#" + std::to_string(snoActor));
}

std::shared_ptr<D3ModelAdapter> D3ModelAdapter::FromActor(std::shared_ptr<const d3n::Actor> actor,
                                                          D3SnoCache& cache, bool lazyClips,
                                                          const std::string& what) {
    if (!actor->snoAppearance.valid()) {
        std::fprintf(stderr, "[d3] actor '%s' names no appearance\n", what.c_str());
        return nullptr;
    }
    auto app = cache.Appearance(actor->snoAppearance.id);
    if (!app) {
        std::fprintf(stderr, "[d3] actor '%s': appearance #%d did not resolve\n", what.c_str(),
                     actor->snoAppearance.id);
        return nullptr;
    }

    const u32 look = PickWeightedLook(*actor, *app);
    auto self = std::make_shared<D3ModelAdapter>(std::move(app), look);
    self->cache_ = &cache;
    self->actor_ = actor;
    // One `.phy` per ACTOR, not per body. An appearance opened on its own has
    // none and takes the registered defaults, which the corpus says are also
    // the shipped modes.
    if (actor->snoPhysics.valid())
        self->physicsSno_ = actor->snoPhysics.id;
    if (actor->snoAnimSet.valid())
        self->BindAnimations(cache, cache.AnimSet(actor->snoAnimSet.id), lazyClips);
    return self;
}

std::shared_ptr<D3ModelAdapter> D3ModelAdapter::LoadAppearance(const ContentRef& ref,
                                                               std::span<const u8> bytes,
                                                               D3SnoCache& cache) {
    auto app = cache.AdoptAppearance(D3SnoIdOfBytes(bytes), bytes);
    if (!app) {
        std::fprintf(stderr, "[d3] '%s' is not a parsable Appearance\n", ref.Describe().c_str());
        return nullptr;
    }
    auto self = std::make_shared<D3ModelAdapter>(std::move(app), 0);
    self->cache_ = &cache;
    return self;
}

D3ModelAdapter::D3ModelAdapter(std::shared_ptr<const d3n::Appearances> app, u32 lookIndex)
    : app_(std::move(app)), lookIndex_(lookIndex) {
    if (!app_)
        return;
    lookNames_.reserve(app_->arLooks.size());
    for (const auto& l : app_->arLooks)
        lookNames_.push_back(l.szName);
    if (lookIndex_ >= lookNames_.size())
        lookIndex_ = 0;
    BuildEmittedSubObjects();
    RefreshLookVisibility();
    BuildSkeletonCache();
}

void D3ModelAdapter::SetLookIndex(u32 index) {
    if (index < lookNames_.size()) {
        lookIndex_ = index;
        RefreshLookVisibility();
    }
}

void D3ModelAdapter::PublishUvAnimation(const PoseRequest& req, FrameState& fs) const {
    if (!app_)
        return;
    // World-clocked, not clip-clocked. The original seeds each entry's phase
    // from a global frame time (`sub_71000F6E70` reads g_GameContext's clock and
    // subtracts a per-instance random offset), so a scroll keeps running while
    // an animation is paused or looping — and a clip time that wraps would snap
    // every scrolling layer back to its start once a second.
    const i32 ms = (req.globalTimeMs >= 0) ? req.globalTimeMs : req.PrimaryClip().timeMs;
    const f32 seconds = static_cast<f32>(ms) * 0.001f;

    const d3n::GeoSet* sets[2] = {&app_->tGeoSet0, &app_->tGeoSet1};
    for (usize g = 0; g < emitted_.size(); ++g) {
        const auto& subs = sets[emitted_[g].geoSet & 1]->arSubObjects;
        if (emitted_[g].index >= subs.size())
            continue;
        const d3n::SubObjectAppearance* v =
            D3VariantFor(*app_, subs[emitted_[g].index], LookForGeoset(g));
        if (!v)
            continue;
        // The `Legacy.fx` chain's stage order, when this sub-object binds one.
        // Its scrolling layers are keyed by POSITION and not by slot, because
        // the types that scroll on a chain routinely have no slot at all —
        // `actor_seismicSlam_wave` animates types 11 and 13, and D3SlotOfType
        // maps neither. Empty for every other family, and then this costs two
        // cache hits per geoset.
        std::array<i32, kD3MaxChainStages> chainTypes{};
        const u32 chainCount = D3ChainStageTypes(v->tMaterial, cache_, chainTypes);
        // ... and again for the distortion pass, which is a second chain over
        // the same material and routinely the only one that scrolls.
        std::array<i32, kD3MaxChainStages> distTypes{};
        const u32 distCount = D3ChainStageTypes(v->tMaterial, cache_, distTypes, true);

        auto emit = [&](const D3UvXform& uv, i32 id) {
            f32 a[6];
            D3UvAffine(uv, seconds, a);
            FrameState::TexAnimMatrix m{};
            m.textureAnimId = id;
            m.row0[0] = a[0];
            m.row0[1] = a[1];
            m.row0[3] = a[2];
            m.row1[0] = a[3];
            m.row1[1] = a[4];
            m.row1[3] = a[5];
            fs.texAnimMatrices.push_back(m);
        };

        // The embedded material only. A SNO-only variant needs the cache the
        // surface table holds, and this runs on the pose path where there is
        // none — the same split D3MaterialOf already draws.
        bool seen[kD3SlotCount] = {};
        for (const auto& e : v->tMaterial.arTextures) {
            const i32 type = D3TextureTypeOf(e);
            const auto uv = D3ReadUvXform(e);
            const bool scrolls = uv.mode == D3UvMode::ScaleRotateScroll && uv.animated;
            const D3SlotKind kind = D3SlotOfType(type);
            if (kind != D3SlotKind::Count && !seen[static_cast<u32>(kind)]) {
                seen[static_cast<u32>(kind)] = true;
                if (scrolls)
                    emit(uv, D3UvTransformId(g, kind));
            }
            if (!scrolls)
                continue;
            for (u32 i = 0; i < chainCount; ++i) {
                if (chainTypes[i] == type)
                    emit(uv, D3UvTransformIdForStage(g, i));
            }
            for (u32 i = 0; i < distCount; ++i) {
                if (distTypes[i] == type)
                    emit(uv, D3UvTransformIdForDistortionStage(g, i));
            }
        }
    }
}

void D3ModelAdapter::RefreshLookVisibility() {
    lookHidden_.assign(emitted_.size(), 0);
    if (!app_)
        return;
    const d3n::GeoSet* sets[2] = {&app_->tGeoSet0, &app_->tGeoSet1};
    for (usize g = 0; g < emitted_.size(); ++g) {
        const auto& subs = sets[emitted_[g].geoSet & 1]->arSubObjects;
        if (emitted_[g].index >= subs.size())
            continue;
        const d3n::SubObjectAppearance* v =
            D3VariantFor(*app_, subs[emitted_[g].index], LookForGeoset(g));
        // A sub-object whose name finds no material keeps drawing: an absent
        // rule is not a rule that says "hide", and the surface table already
        // reports the unmatched count.
        if (v && (v->dwUnknown00 & kD3SubObjectVisibleBit) == 0)
            lookHidden_[g] = 1;
    }
}

void D3ModelAdapter::BuildEmittedSubObjects() {
    emitted_.clear();
    usize compressed = 0;
    const d3n::GeoSet* sets[2] = {&app_->tGeoSet0, &app_->tGeoSet1};
    for (u32 g = 0; g < 2; ++g) {
        const auto& subs = sets[g]->arSubObjects;
        for (u32 i = 0; i < subs.size(); ++i) {
            const auto& s = subs[i];
            if (s.arVertices.empty() || s.arIndices.empty())
                continue;
            // Post-v260 fixed-point positions with a global dequantisation
            // pair. Never set in shipped content; refused loudly rather than
            // read as 44-byte records out of a 28-byte stream.
            if ((static_cast<u32>(s.dwVertexFormat) & d3n::kSubObjectCompressedMask) != 0) {
                ++compressed;
                continue;
            }
            emitted_.push_back(D3SubObjectRef{g, i});
        }
    }
    if (compressed > 0) {
        std::fprintf(stderr,
                     "[d3] appearance #%d: %zu sub-objects use the compressed vertex format "
                     "(kSubObjectCompressedMask) and were skipped\n",
                     app_->dwSnoId, compressed);
    }
}

void D3ModelAdapter::BuildSkeletonCache() {
    const auto& bones = app_->arBones;
    localBind_.resize(bones.size());
    attachmentFrames_.resize(bones.size());
    localBindTrs_.resize(bones.size());
    for (usize i = 0; i < bones.size(); ++i) {
        localBind_[i] = MatrixOf(bones[i].tTransform2);
        attachmentFrames_[i] = MatrixOf(bones[i].tTransform1);
        localBindTrs_[i].translation = bones[i].tTransform2.vTranslation;
        localBindTrs_[i].rotation = NormalizeQ(QuatOf(bones[i].tTransform2.qRotation));
        localBindTrs_[i].scale = bones[i].tTransform2.flScale;
    }
}

const d3n::SubObject* D3ModelAdapter::SubObjectAt(usize g) const {
    if (!app_ || g >= emitted_.size())
        return nullptr;
    const auto& r = emitted_[g];
    const d3n::GeoSet& set = (r.geoSet == 0) ? app_->tGeoSet0 : app_->tGeoSet1;
    return (r.index < set.arSubObjects.size()) ? &set.arSubObjects[r.index] : nullptr;
}

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

std::vector<VertexAttribute> DescribeD3Vertex() {
    return {
        {VertexSemantic::Position, 0, gfx::Format::R32G32B32_FLOAT, 0},
        {VertexSemantic::Normal, 0, gfx::Format::R32G32B32_FLOAT, 12},
        {VertexSemantic::TexCoord, 0, gfx::Format::R32G32_FLOAT, 24},
        {VertexSemantic::TexCoord, 1, gfx::Format::R32G32_FLOAT, 32},
        {VertexSemantic::Tangent, 0, gfx::Format::R32G32B32A32_FLOAT, 40},
        {VertexSemantic::Color, 0, gfx::Format::R16G16B16A16_UNORM, 56},
    };
}

std::vector<MeshData> D3ModelAdapter::GetMeshes() {
    std::vector<MeshData> out;
    if (!app_)
        return out;
    out.reserve(emitted_.size());

    const auto attrs = DescribeD3Vertex();

    for (usize g = 0; g < emitted_.size(); ++g) {
        const d3n::SubObject* sub = SubObjectAt(g);
        if (!sub)
            continue;

        MeshData mesh;
        mesh.geosetId = static_cast<i32>(g);
        mesh.materialId = static_cast<i32>(g); // one SubObject is one material
        mesh.lod = 0;
        // A sub-object with a `ClothStructure` has its vertices rebuilt every
        // frame (§8.0 of the physics plan), so the renderer keeps the upload
        // bytes for it. Asked of the geometry rather than of the resolved cloth
        // pieces because this runs first — and because a block the look leaves
        // unsimulated costs one retained copy, not a wrong picture.
        mesh.deformable = !sub->arClothData.empty();

        const usize n = sub->arVertices.size();
        // `positions` stays populated alongside the baked blob and is not
        // redundant: it is the CPU-side copy GetBounds's union default and the
        // per-geoset sort centroid read, and neither touches the GPU buffer.
        mesh.positions.reserve(n);
        mesh.baked.stride = sizeof(D3Vertex);
        mesh.baked.attributes = attrs;
        mesh.baked.data.resize(n * sizeof(D3Vertex));
        auto* dst = reinterpret_cast<D3Vertex*>(mesh.baked.data.data());

        for (usize v = 0; v < n; ++v) {
            const d3n::FatVertex& src = sub->arVertices[v];
            mesh.positions.push_back(src.vPosition);

            D3Vertex& d = dst[v];
            d.position = src.vPosition;
            d.normal = d3n::vertexNormal(src);
            d.uv0 = d3n::vertexTexCoord0(src);
            d.uv1 = d3n::vertexTexCoord1(src);

            // D3 ships an explicit binormal, so the bitangent sign is computed
            // rather than guessed — and the binormal is then dropped, because
            // MeshData carries the sign and not the vector.
            const Vector3f tan = d3n::vertexTangent(src);
            const Vector3f bin = d3n::vertexBinormal(src);
            const f32 sign = (Dot(Cross(d.normal, tan), bin) < 0.0f) ? -1.0f : 1.0f;
            d.tangent = {tan.x, tan.y, tan.z, sign};

            PackColorPair(d3n::vertexColor(src), d3n::vertexAuxColor(src), d.colors);
        }

        mesh.indices.reserve(sub->arIndices.size());
        for (u16 idx : sub->arIndices)
            mesh.indices.push_back(static_cast<u32>(idx));

        out.push_back(std::move(mesh));
    }
    return out;
}

::whiteout::flakes::ModelBounds D3ModelAdapter::GetBounds() {
    ::whiteout::flakes::ModelBounds b;
    if (!app_)
        return b;
    const auto& c = app_->tBounds.vCenter;
    const auto& h = app_->tBounds.vHalfExtent;
    if (h.x > 0.0f || h.y > 0.0f || h.z > 0.0f) {
        b.min = {c.x - h.x, c.y - h.y, c.z - h.z};
        b.max = {c.x + h.x, c.y + h.y, c.z + h.z};
        b.valid = true;
        return b;
    }
    // A degenerate box is a real state (an appearance with no mesh); fall
    // through to the interface's union-over-positions default.
    return IModelSource::GetBounds();
}

// ---------------------------------------------------------------------------
// Textures
// ---------------------------------------------------------------------------

std::vector<TextureData> D3ModelAdapter::GetTextures() {
    std::vector<TextureData> out;
    if (!app_)
        return out;
    const auto refs = CollectD3Textures(*app_, lookIndex_, emitted_, geosetLooks_);
    out.reserve(refs.size());
    for (usize i = 0; i < refs.size(); ++i) {
        TextureData td;
        td.textureId = static_cast<i32>(i);
        td.replaceableId = 0;
        td.width = 0;
        td.height = 0;
        td.cubeMap = refs[i].cube;
        // "#<snoId>", the form ContentRef::Describe produces and the staging
        // path parses. A scheme-prefixed key ("d3:tex:1234") would be read as a
        // *path*, resolve against nothing, and leave every texture white with
        // no error anywhere.
        td.sharedKey = "#" + std::to_string(refs[i].snoId);
        out.push_back(std::move(td));
    }
    return out;
}

// ---------------------------------------------------------------------------
// Skeleton and skinning
// ---------------------------------------------------------------------------

SkeletonData D3ModelAdapter::GetSkeleton() {
    SkeletonData sk;
    if (!app_)
        return sk;
    const auto& bones = app_->arBones;
    sk.nodeCount = static_cast<i32>(bones.size());
    if (bones.empty())
        return sk;

    sk.nodeParents.resize(bones.size());
    sk.billboardFlags.assign(bones.size(), 0u);
    sk.inverseBindMatrices.resize(bones.size());
    for (usize i = 0; i < bones.size(); ++i) {
        const i32 p = bones[i].nParentIndex;
        sk.nodeParents[i] =
            (p < 0 || static_cast<usize>(p) >= bones.size() || static_cast<usize>(p) == i) ? -1 : p;
        // tTransform4 — the inverse of bind pose B, which is what
        // Skeleton_BuildSkinningPaletteAndBounds reads (bone+180 = disk 0xEC).
        // NOT tTransform1: that is the inverse of pose A, the frame hardpoints
        // and attachments use, and substituting it is correct for 92% of bones
        // and catastrophic for the rest.
        sk.inverseBindMatrices[i] = MatrixOf(bones[i].tTransform4);
    }
    // nodePivots stays empty: a PRSTransform is a complete local transform.
    return sk;
}

std::vector<SkinWeightData> D3ModelAdapter::GetSkinWeights() {
    std::vector<SkinWeightData> out;
    if (!app_)
        return out;
    const i32 boneCount = static_cast<i32>(app_->arBones.size());
    if (boneCount == 0)
        return out;
    out.reserve(emitted_.size());

    for (usize g = 0; g < emitted_.size(); ++g) {
        const d3n::SubObject* sub = SubObjectAt(g);
        if (!sub)
            continue;
        SkinWeightData sw;
        sw.geosetId = static_cast<i32>(g);
        sw.influences.resize(sub->arVertices.size());
        // Both branches below write global node indices, rigid included.
        sw.globalVertexIndices = true;

        // Rigid first: 3,994 of 4,372 sampled sub-objects have no influences at
        // all and are positioned by nBoneIndex. Treating that as the fallback
        // would be backwards.
        if (sub->arVertexInfluences.empty()) {
            const i32 bone =
                (sub->nBoneIndex >= 0 && sub->nBoneIndex < boneCount) ? sub->nBoneIndex : 0;
            for (auto& inf : sw.influences) {
                inf.boneIdx[0] = bone;
                inf.weight[0] = 1.0f;
            }
            out.push_back(std::move(sw));
            continue;
        }

        const usize n = (std::min)(sub->arVertices.size(), sub->arVertexInfluences.size());
        for (usize v = 0; v < n; ++v) {
            const d3n::VertInfluences& src = sub->arVertexInfluences[v];
            const d3n::Influence* three[3] = {&src.tInfluence0, &src.tInfluence1,
                                              &src.tInfluence2};
            auto& dst = sw.influences[v];
            for (int k = 0; k < 3; ++k) {
                // Bone indices are GLOBAL skeleton indices — 21,701 of 21,701
                // sampled below boneCount — so there is no palette to remap and
                // no SkinningInfo batching to reconstruct.
                const i32 b = three[k]->nBoneIndex;
                const bool live = b >= 0 && b < boneCount && three[k]->flWeight > 0.0f;
                dst.boneIdx[k] = live ? b : 0;
                dst.weight[k] = live ? three[k]->flWeight : 0.0f;
            }
            dst.boneIdx[3] = 0;
            dst.weight[3] = 0.0f; // lane 3 is always empty: D3 ships three
        }
        // Vertices past the influence array (a truncated record) stay at the
        // default all-zero influence, which the loader's normalisation turns
        // into bone 0 rather than a NaN.
        out.push_back(std::move(sw));
    }
    return out;
}

// ---------------------------------------------------------------------------
// Animation
// ---------------------------------------------------------------------------

void D3ModelAdapter::BindAnimations(D3SnoCache& cache, std::shared_ptr<const d3n::AnimSet> animSet,
                                     bool lazy) {
    cache_ = &cache;
    animSet_ = std::move(animSet);
    lazyClips_ = lazy;
    clips_.clear();
    sequences_.clear();
    if (!animSet_)
        return;

    // The core tag map only. Without a gameplay ACD there is no weapon class,
    // so the 28 weapon-class maps are not resolved here; they are additional
    // named groups a host can select and they land with the UI half of the tag
    // table.
    clips_.reserve(animSet_->tCoreTagMap.size());
    std::unordered_set<std::string> taken;
    for (const auto& entry : animSet_->tCoreTagMap) {
        if (!entry.snoAnim.valid())
            continue;
        Clip c;
        c.tagId = entry.dwTagId;
        c.animSno = entry.snoAnim.id;
        // The clip is named after the `.ani` it plays, because the tag cannot
        // name itself and the animation can. The runtime *does* have a tag name
        // table — AnimTagName, 0x71006A09A0, 453 entries of 64 bytes — but 2.6.2
        // ships it with every name pointer aimed at the same empty string, so
        // even the client's own tag-to-text call returns "" for every tag it
        // knows. (The power-tag table immediately after it in the same array
        // kept its names, which is how you can tell the blanks are deliberate
        // and not a misread record layout.) CoreTOC, meanwhile, names every
        // SNO — and the storage root has already read it.
        c.name = cache.NameOf(entry.snoAnim.id);
        char buf[32];
        std::snprintf(buf, sizeof(buf), "Tag_%05X", static_cast<unsigned>(entry.dwTagId));
        if (c.name.empty())
            c.name = buf;
        else if (!taken.insert(c.name).second)
            // Two tags sharing one `.ani` is ordinary — a hand-off and its
            // idle, an attack and its variant. Only the second one onward pays
            // for it, and it pays in the id that actually distinguishes them.
            c.name += std::string(" (") + buf + ")";
        clips_.push_back(std::move(c));
    }

    // 259 clips and 5.8 MB per character AnimSet, to play one idle. The `.ans`
    // itself is small (27 KB) and holds the whole tag map, so the map is parsed
    // eagerly and each `.ani` is fetched on first play — the opposite default
    // to M2LazyAnimations, deliberately: that one is off because every
    // byte-identical gate was recorded against the eager parse, and D3 has no
    // such gate to protect.
    if (!lazyClips_) {
        for (const Clip& c : clips_)
            EnsureClip(c);
    }

    sequences_.reserve(clips_.size());
    for (const Clip& c : clips_) {
        // A lazily-loaded clip has no duration until it lands. Reporting zero
        // would make the playlist treat it as instantaneous, so an unresolved
        // clip advertises a nominal one-second window and is corrected on the
        // first EnsureClip.
        const f32 dur = (c.durationSec > 0.0f) ? c.durationSec : 1.0f;
        SequenceInfo s;
        s.name = c.name;
        s.startMs = 0;
        s.endMs = static_cast<i32>(dur * 1000.0f);
        s.nonLooping = false;
        sequences_.push_back(std::move(s));
    }
}

bool D3ModelAdapter::EnsureClip(const Clip& clip) const {
    if (clip.resolved)
        return clip.anim != nullptr;
    clip.resolved = true;
    if (!cache_ || clip.animSno <= 0)
        return false;
    clip.anim = cache_->Anim(clip.animSno);
    if (!clip.anim || clip.anim->arPermutations.empty()) {
        clip.anim = nullptr;
        return false;
    }
    if (clip.permutation >= clip.anim->arPermutations.size())
        clip.permutation = 0;
    const auto& perm = clip.anim->arPermutations[clip.permutation];

    // fps = flFramesPerTick * 60; duration = (frames - 1) / fps
    // (Anim_InitPlaybackState, 0x710033B9D0).
    const f32 fps = perm.flFramesPerTick * 60.0f;
    const_cast<Clip&>(clip).durationSec =
        (fps > 0.0f && perm.dwFrameCount > 1)
            ? static_cast<f32>(perm.dwFrameCount - 1) / fps
            : 0.0f;
    // The two blend fields are ticks at 1/60 s, clamped to the clip's duration.
    const f32 durMs = clip.durationSec * 1000.0f;
    auto ticksToMs = [durMs](i32 ticks) {
        const f32 ms = static_cast<f32>((std::max)(0, ticks)) * (1000.0f / 60.0f);
        return static_cast<i32>((durMs > 0.0f) ? (std::min)(ms, durMs) : ms);
    };
    const_cast<Clip&>(clip).blendInMs = ticksToMs(perm.nBlendTicksFromOtherAnim);
    const_cast<Clip&>(clip).blendOutMs = ticksToMs(perm.nBlendTicksSamePermSwap);

    // Bones bind BY NAME. arBoneNames are 4-byte hashes at runtime and 64-byte
    // strings on disk, so the match is against BoneStructure.szName; -1 marks a
    // bone this appearance does not have, and one with no curve is seeded from
    // tTransform2.
    clip.boneMap.assign(perm.arBoneNames.size(), -1);
    for (usize b = 0; b < perm.arBoneNames.size(); ++b) {
        for (usize n = 0; n < app_->arBones.size(); ++n) {
            if (EqualCi(perm.arBoneNames[b].szBoneName, app_->arBones[n].szName)) {
                clip.boneMap[b] = static_cast<i32>(n);
                break;
            }
        }
    }
    // Keyframed attachments: `{flFrame, TriggerEvent}` at the permutation's
    // own frame rate, which is the same fps the duration above came from. The
    // engine plays them through TriggerEvent_Execute exactly as it plays an
    // actor's message events; only the key differs. Sorted because file order
    // is authoring order and the driver walks a time window.
    clip.attachments.clear();
    clip.attachments.reserve(perm.arAttachments.size());
    for (const auto& att : perm.arAttachments) {
        ClipAttachment ca;
        ca.timeMs = (fps > 0.0f) ? static_cast<i32>(att.flFrame / fps * 1000.0f + 0.5f) : 0;
        ca.event = &att.tEvent;
        clip.attachments.push_back(ca);
    }
    std::stable_sort(clip.attachments.begin(), clip.attachments.end(),
                     [](const ClipAttachment& a, const ClipAttachment& b) {
                         return a.timeMs < b.timeMs;
                     });

    // Two ceilings from the binary: 512 bones for a native pose, 255 for a
    // retargeted clip (the private remap table is 255 x i16). Reported rather
    // than enforced — a model past either is data we have not seen.
    const bool retargeted =
        clip.anim->snoAppearance.valid() && clip.anim->snoAppearance.id != app_->dwSnoId;
    const usize ceiling = retargeted ? 255u : 512u;
    if (perm.arBoneNames.size() > ceiling) {
        std::fprintf(stderr, "[d3] clip '%s': %zu bones exceeds the %s ceiling of %zu\n",
                     clip.name.c_str(), perm.arBoneNames.size(),
                     retargeted ? "retargeted" : "native", ceiling);
    }
    return true;
}

std::span<const D3ModelAdapter::ClipAttachment> D3ModelAdapter::ClipAttachments(
    i32 sequence) const {
    if (sequence < 0 || static_cast<usize>(sequence) >= clips_.size())
        return {};
    const Clip& c = clips_[static_cast<usize>(sequence)];
    if (!EnsureClip(c))
        return {};
    return c.attachments;
}

std::vector<SequenceInfo> D3ModelAdapter::GetSequences() const {
    if (!sequences_.empty())
        return sequences_;
    // No AnimSet is the normal state for a browsed `.app`. One synthetic
    // sequence still gives the actor a clock.
    SequenceInfo s;
    s.name = "Bind";
    s.startMs = 0;
    s.endMs = 1000;
    return {std::move(s)};
}

TransitionPolicy D3ModelAdapter::DefaultTransition() const {
    TransitionPolicy p;
    p.crossFade = true;
    // Per clip, not a constant — but DefaultTransition has no clip in hand, so
    // this is the first resolved clip's ramp and falls back to M3's 150 ms.
    for (const Clip& c : clips_) {
        if (c.resolved && c.anim) {
            p.blendInMs = (c.blendInMs > 0) ? c.blendInMs : 150;
            p.blendOutMs = (c.blendOutMs > 0) ? c.blendOutMs : 150;
            return p;
        }
    }
    p.blendInMs = 150;
    p.blendOutMs = 150;
    return p;
}

FrameState D3ModelAdapter::Evaluate(const PoseRequest& req) const {
    FrameState fs;
    if (!app_)
        return fs;
    const auto& bones = app_->arBones;
    const usize boneCount = bones.size();

    // Start from the local bind pose. A bone the clip does not name keeps it —
    // the guide's "rest pose used to seed slot buffers" (bone+116 = tTransform2).
    std::vector<Vector3f> t(boneCount);
    std::vector<Quaternion> r(boneCount);
    std::vector<f32> s(boneCount, 1.0f);
    for (usize i = 0; i < boneCount; ++i) {
        t[i] = localBindTrs_[i].translation;
        r[i] = localBindTrs_[i].rotation;
        s[i] = localBindTrs_[i].scale;
    }

    const ClipRef clip = req.PrimaryClip();
    const Clip* c = (clip.sequence >= 0 && static_cast<usize>(clip.sequence) < clips_.size())
                        ? &clips_[static_cast<usize>(clip.sequence)]
                        : nullptr;
    if (c && EnsureClip(*c)) {
        const auto& perm = c->anim->arPermutations[c->permutation];
        const f32 fps = perm.flFramesPerTick * 60.0f;
        const f32 lastFrame = static_cast<f32>((std::max)(1, perm.dwFrameCount) - 1);
        f32 frame = (fps > 0.0f) ? (static_cast<f32>(clip.timeMs) * 0.001f) * fps : 0.0f;
        if (clip.loop && lastFrame > 0.0f) {
            frame = std::fmod(frame, lastFrame);
            if (frame < 0.0f)
                frame += lastFrame;
        } else {
            frame = std::clamp(frame, 0.0f, lastFrame);
        }

        const usize tracks = c->boneMap.size();
        for (usize b = 0; b < tracks; ++b) {
            const i32 node = c->boneMap[b];
            if (node < 0 || static_cast<usize>(node) >= boneCount)
                continue;
            usize i0 = 0, i1 = 0;
            f32 f = 0.0f;
            if (b < perm.arTranslationCurves.size()) {
                const auto& keys = perm.arTranslationCurves[b].arKeys;
                if (!keys.empty()) {
                    Bracket(keys, frame, i0, i1, f);
                    t[node] = Lerp3(keys[i0].vPosition, keys[i1].vPosition, f);
                }
            }
            if (b < perm.arRotationCurves.size()) {
                const auto& keys = perm.arRotationCurves[b].arKeys;
                if (!keys.empty()) {
                    Bracket(keys, frame, i0, i1, f);
                    r[node] = Nlerp(DecodeQ16(keys[i0].tRotation), DecodeQ16(keys[i1].tRotation), f);
                }
            }
            if (b < perm.arScaleCurves.size()) {
                const auto& keys = perm.arScaleCurves[b].arKeys;
                if (!keys.empty()) {
                    Bracket(keys, frame, i0, i1, f);
                    s[node] = keys[i0].flScale + (keys[i1].flScale - keys[i0].flScale) * f;
                }
            }
        }
    }

    // Compose down the hierarchy. arBones is parent-before-child in every
    // shipped file; a forward walk is what Skeleton_ComposeWorldPose does, and a
    // parent index past its own child would be a broken file rather than a
    // different ordering.
    fs.boneWorldMatrices.assign(boneCount, Matrix44f::identity());
    for (usize i = 0; i < boneCount; ++i) {
        const Matrix44f local = renderer::animation::ComposePivotSRT(
            t[i], r[i], {s[i], s[i], s[i]}, {0.0f, 0.0f, 0.0f});
        const i32 p = bones[i].nParentIndex;
        fs.boneWorldMatrices[i] = (p >= 0 && static_cast<usize>(p) < i)
                                      ? (local * fs.boneWorldMatrices[static_cast<usize>(p)])
                                      : local;
    }
    // Not animated: D3 states visibility per equipped item and per look, not
    // per keyframe. It rides FrameState because that is the one channel
    // RenderModel reads a per-geoset draw bit from, and a restyle takes effect
    // on the next Evaluate rather than needing the mesh re-uploaded.
    //
    // Two independent rules, unioned rather than either winning: the file's own
    // per-look bit (`lookHidden_`) and whatever a host has dressed
    // (`geosetHidden_`). Without the first, Tyrael draws the Stranger, the
    // Restored angel AND the skeleton he is never both of.
    if (!geosetHidden_.empty() || !lookHidden_.empty()) {
        fs.geosetHidden.assign(emitted_.size(), 0);
        for (usize g = 0; g < emitted_.size(); ++g) {
            const bool hidden = (g < geosetHidden_.size() && geosetHidden_[g] != 0) ||
                                (g < lookHidden_.size() && lookHidden_[g] != 0);
            fs.geosetHidden[g] = hidden ? 1 : 0;
        }
    }
    if (!collisionBones_.empty()) {
        renderer::profiles::diablo3::D3PlaceCollisionShapes(collisionBones_, fs.boneWorldMatrices,
                                                            fs.collisionTransforms);
    }
    PublishUvAnimation(req, fs);
    return fs;
}

std::vector<renderer::model::CollisionShapeData> D3ModelAdapter::GetCollisionShapes() {
    collisionBones_.clear();
    if (!app_)
        return {};
    // The cooked polytopes — 70% of every rig — are payload references, so the
    // file has to come back. One read per model load; see `ReadBytes`. Without
    // a provider it comes back empty and the builder drops kind 2 rather than
    // inventing a box for it.
    const std::vector<u8> bytes = cache_ ? cache_->ReadBytes(app_->dwSnoId) : std::vector<u8>{};
    auto built = renderer::profiles::diablo3::D3BuildCollisionShapes(*app_, bytes);
    collisionBones_ = std::move(built.bones);
    return std::move(built.shapes);
}

void D3ModelAdapter::CreatePoseStages(renderer::animation::PoseStageList& out) const {
#if WDX_HAS_PHYSICS
    namespace d3p = renderer::profiles::diablo3;
    if (!app_)
        return;
    // Made once and reused, so a rebind mid-collapse comes back collapsed.
    if (!ragdoll_)
        ragdoll_ = std::make_shared<d3p::D3PhysicsControl>();
    // Two thirds of every rig is a cooked polytope held as a payload reference,
    // so the file has to come back or most bodies would get no fixture and be
    // dropped. One read per stage build; see `ReadBytes`.
    const std::vector<u8> bytes = cache_ ? cache_->ReadBytes(app_->dwSnoId) : std::vector<u8>{};
    std::shared_ptr<const d3n::Physics> phy;
    if (cache_ && physicsSno_ >= 0)
        phy = cache_->Physics(physicsSno_);
    // Which builder. The client picks by call site, not by data: the anchored
    // rig comes from `ActorAnim_InitAnimTree` and so exists from load, and the
    // bone-body collapse replaces it on a gameplay event. An appearance that
    // authors anchors is one the client would have built the anchored rig for,
    // so that is the rule here — and it is the disjunction `HasPhysicsRig`
    // reports, so the host's button and the stage always agree.
    const auto mode = d3p::D3HasRagdollAnchor(*app_) ? d3p::D3RigMode::Ragdoll
                                                     : d3p::D3RigMode::BoneBodies;
    if (auto stage = d3p::CreateD3PhysicsStage(*app_, bytes, phy.get(), ragdoll_, mode))
        out.push_back(std::move(stage));

    // Cloth goes **after** the rigid stage, which is the ordering `pose_stage.h`
    // states and `m3_model_adapter.cpp` already uses: a cape stapled to a bone a
    // ragdoll drives has to read where the ragdoll put it, not where the
    // animation did.
    std::vector<d3p::D3ClothPiece> pieces;
    std::vector<std::shared_ptr<const d3n::Cloth>> cloths;
    if (ResolveClothPieces(pieces, cloths)) {
        // A fresh buffer per call, never one the adapter keeps. `D3Drawable`
        // hands the *same* adapter to every actor with the same (appearance,
        // look), and what a cloth stage publishes is per-actor: the spans in
        // `FrameState::geosetDeforms` point straight into this, so two actors
        // sharing one would each draw whichever of them stepped last. Open the
        // Storage Explorer on the model already in the viewer and that second
        // actor exists.
        auto cloth = std::make_shared<d3p::D3ClothOutput>();
        if (auto stage = d3p::CreateD3ClothStage(*app_, pieces, cloths, std::move(cloth)))
            out.push_back(std::move(stage));
    }
#else
    (void)out;
#endif
}

void D3ModelAdapter::SetRagdoll(bool on) {
#if WDX_HAS_PHYSICS
    if (!ragdoll_)
        ragdoll_ = std::make_shared<renderer::profiles::diablo3::D3PhysicsControl>();
    ragdoll_->simulating = on;
#else
    (void)on;
#endif
}

bool D3ModelAdapter::ResolveClothPieces(
    std::vector<renderer::profiles::diablo3::D3ClothPiece>& pieces,
    std::vector<std::shared_ptr<const d3n::Cloth>>& cloths) const {
#if WDX_HAS_PHYSICS
    pieces.clear();
    cloths.clear();
    if (!app_ || !cache_)
        return false;
    pieces = renderer::profiles::diablo3::D3FindClothPieces(*app_, lookIndex_);
    if (pieces.empty())
        return false;
    // Fill in the emitted-geoset index and re-resolve the `.clt` under the look
    // that geoset actually draws: the cloth is a property of the variant, and
    // looks are per geoset here, not per model.
    cloths.reserve(pieces.size());
    for (auto& piece : pieces) {
        for (usize g = 0; g < emitted_.size(); ++g) {
            if (emitted_[g].geoSet != 0 || emitted_[g].index != static_cast<u32>(piece.subObject))
                continue;
            piece.geoset = static_cast<i32>(g);
            const d3n::SubObject& sub = app_->tGeoSet0.arSubObjects[emitted_[g].index];
            if (const auto* v = D3VariantFor(*app_, sub, LookForGeoset(g)))
                piece.clothSno = v->snoCloth.id;
            break;
        }
        cloths.push_back(piece.clothSno >= 0 ? cache_->Cloth(piece.clothSno) : nullptr);
    }
    return true;
#else
    (void)pieces;
    (void)cloths;
    return false;
#endif
}

std::vector<renderer::model::ClothOverlayData> D3ModelAdapter::GetClothOverlays() {
#if WDX_HAS_PHYSICS
    std::vector<renderer::profiles::diablo3::D3ClothPiece> pieces;
    std::vector<std::shared_ptr<const d3n::Cloth>> cloths;
    if (!ResolveClothPieces(pieces, cloths))
        return {};
    return renderer::profiles::diablo3::D3BuildClothOverlays(*app_, pieces, cloths);
#else
    // Without a solver nothing would ever move them, and a wireframe frozen in
    // the authored rest shape is the one picture this overlay must not draw.
    return {};
#endif
}

bool D3ModelAdapter::HasPhysicsRig() const {
    namespace d3p = renderer::profiles::diablo3;
    if (!app_)
        return false;
    // Either builder counts: an anchored rig (`sub_71003E07B0`) or a bone-body
    // collapse at the lod the client uses for one. The same disjunction the
    // adapter picks its mode with, so the button and the stage cannot disagree.
    return d3p::D3HasRagdollAnchor(*app_) || d3p::D3HasDynamicBody(*app_, d3p::kD3BoneBodyLod);
}

bool D3ModelAdapter::IsRagdoll() const {
#if WDX_HAS_PHYSICS
    return ragdoll_ && ragdoll_->simulating;
#else
    return false;
#endif
}

} // namespace whiteout::flakes::io

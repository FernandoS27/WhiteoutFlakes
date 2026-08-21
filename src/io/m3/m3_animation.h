#pragma once

/// @file m3_animation.h
/// @brief `.m3` track resolution and sampling.
///
/// Sits beside the adapter for the same reason `mdx_animation` and
/// `m2_animation` do: the *arithmetic* of animation is shareable, but the
/// *policy* is the format. StarCraft II's policy is unusual enough to be worth
/// naming up front, because each point below is a place a "reasonable"
/// implementation silently disagrees with the shipped engine:
///
///  - A property does not name a track. It names an `animId`, which each
///    sub-track container resolves independently — so the same property is
///    driven by different keys depending on which container is playing.
///  - One play of a sequence is several *layers*, one per container in the
///    sequence's group, each at its own priority. That is how split-body
///    animation works: an upper-body container and a lower-body one run
///    concurrently out of a single logical play.
///  - A layer with no track for a property either abstains (leaving lower
///    layers visible) or forces the property's default, decided by the
///    container's `runsConcurrent` flag. The asymmetry *is* the feature.
///  - Blending spends a weight budget from 1.0, highest priority first, and
///    combines with a smoothstep rather than a plain weighted mean.
///  - A looping track wraps on **its own** duration, not the sequence's.
///  - Track-level quaternion interpolation is a raw componentwise lerp: no
///    normalisation, no shortest-arc sign fix. Verified against
///    `M3Anim_EvalTrackQuat`; see SC2_ANIM_RE.md §6.
///
/// Everything here is a pure function of parsed data plus a layer list, so it
/// is testable without a device, a model file, or a scene.

#include "whiteout/flakes/pose_request.h"
#include "whiteout/flakes/types.h"

#include <whiteout/models/m3/structures.h>

#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace whiteout::flakes::io {

/// @brief Which of a sub-track container's 13 typed arrays a block lives in.
enum class M3SdSlot : ::whiteout::u8 {
    Event = 0,
    Vec2 = 1,
    Vec3 = 2,
    Quat = 3,
    Color = 4,
    Float = 5,
    U8 = 6,
    S16 = 7,
    U16 = 8,
    S32 = 9,
    U32 = 10,
    Flag = 11,
    Bounds = 12,
    None = 0xFF,
};

/// @brief One resolved (container, animId) → keyframe block.
struct M3TrackHandle {
    M3SdSlot slot = M3SdSlot::None;
    ::whiteout::u32 block = 0;

    bool Valid() const {
        return slot != M3SdSlot::None;
    }
};

/// @brief One sampler layer — a container playing at a priority.
struct M3Layer {
    /// @brief Which logical play this layer came from. Every layer of one play
    ///        shares it, and the sampler takes at most one contribution per
    ///        distinct value — that is the whole of the de-duplication rule.
    ::whiteout::u16 play = 0;
    ::whiteout::u16 stc = 0;
    ::whiteout::u16 priority = 0;
    /// @brief `STC.runsConcurrent`. A transparent layer with no track for a
    ///        property abstains; an opaque one forces the property's default.
    bool transparent = false;
    /// @brief Unwrapped ms since the play began — the value a looping track
    ///        takes modulo its own duration.
    ::whiteout::i32 timeMs = 0;
    ::whiteout::f32 weight = 1.0f;
    bool loop = true;
};

/// @brief Load-time resolution of the STC / animId indirection.
///
/// The file stores, per container, two parallel arrays: the animIds the
/// container drives, and a packed word naming the typed block holding the
/// keys. Resolving that per sample would mean a linear scan of `animIds` for
/// every property of every bone every frame, so it is flattened once into a
/// dense (row × container) grid, mirroring what StarCraft II's loader builds.
///
/// The grid spans the model **and every attached `.m3a`**. StarCraft II keeps
/// one record per loaded asset and hands each a contiguous slice of a single
/// global sequence and container index space (`sub_1028819E0`); this is the
/// same layout, so a sequence index means "the n-th sequence across all loaded
/// files" and needs no per-asset decoding at the call site. Attached files bind
/// to the model purely through `animId` — never through bone index, because an
/// `.m3a`'s bone list is a differently-ordered subset that can even name bones
/// the model does not have.
class M3AnimTables {
public:
    /// @param attached external animation files, in attach order. Their
    ///        containers and sequences append to the base model's.
    void Build(const ::whiteout::m3::Model& model,
               std::span<const ::whiteout::m3::Model* const> attached = {});

    /// @brief Dense row for @p animId, or `-1` when nothing animates it.
    ::whiteout::i32 RowOf(::whiteout::u32 animId) const {
        const auto it = rowOf_.find(animId);
        return it == rowOf_.end() ? -1 : it->second;
    }

    M3TrackHandle At(::whiteout::i32 row, ::whiteout::u16 stc) const {
        if (row < 0 || stc >= stcCount_)
            return {};
        return table_[static_cast<std::size_t>(row) * stcCount_ + stc];
    }

    /// @brief The containers driving @p sequence, already sorted by descending
    ///        priority.
    struct LayerDef {
        ::whiteout::u16 stc = 0;
        ::whiteout::u16 priority = 0;
        bool transparent = false;
    };
    std::span<const LayerDef> LayersFor(::whiteout::i32 sequence) const {
        if (sequence < 0 || static_cast<std::size_t>(sequence) >= seqLayers_.size())
            return {};
        return seqLayers_[static_cast<std::size_t>(sequence)];
    }

    ::whiteout::u16 StcCount() const {
        return stcCount_;
    }
    /// @brief Number of distinct animIds the model animates.
    std::size_t RowCount() const {
        return rowOf_.size();
    }

    /// @brief The container at a *global* index, or null when out of range.
    ///
    /// Callers hold global indices (an `M3Layer::stc`), which may name a
    /// container in the model or in any attached file, so nothing outside this
    /// class should be indexing `Model::subTrackCollections` directly.
    const ::whiteout::m3::SubTrackContainer* StcAt(::whiteout::u16 stc) const {
        return stc < stcs_.size() ? stcs_[stc] : nullptr;
    }

    /// @brief The sequence at a *global* index, or null when out of range.
    const ::whiteout::m3::Sequence* SequenceAt(::whiteout::i32 sequence) const {
        if (sequence < 0 || static_cast<std::size_t>(sequence) >= seqs_.size())
            return nullptr;
        return seqs_[static_cast<std::size_t>(sequence)];
    }
    /// @brief Sequences across the model and every attached file.
    std::size_t SequenceCount() const {
        return seqs_.size();
    }
    /// @brief Which loaded file owns a global sequence — 0 is the model itself,
    ///        1.. are attached files in attach order. `-1` when out of range.
    ::whiteout::i32 AssetOfSequence(::whiteout::i32 sequence) const;

    /// @brief Largest timestamp in a block, the modulus a looping track wraps
    ///        on. Zero for an empty or single-key block.
    /// @param stc a *global* container index.
    ::whiteout::i32 DurationOf(::whiteout::u16 stc, M3TrackHandle h) const;

private:
    /// @brief One loaded file's slice of the two global index spaces.
    struct Asset {
        const ::whiteout::m3::Model* model = nullptr;
        ::whiteout::u32 stcBase = 0;
        ::whiteout::u32 seqBase = 0;
        ::whiteout::u32 seqEnd = 0;
    };

    std::unordered_map<::whiteout::u32, ::whiteout::i32> rowOf_;
    std::vector<M3TrackHandle> table_;
    ::whiteout::u16 stcCount_ = 0;
    std::vector<std::vector<LayerDef>> seqLayers_;
    std::vector<Asset> assets_;
    // Flattened views over the assets, in global index order. The pointees live
    // in models the adapter owns for as long as the tables do.
    std::vector<const ::whiteout::m3::SubTrackContainer*> stcs_;
    std::vector<const ::whiteout::m3::Sequence*> seqs_;
};

/// @brief Where a time lands in a keyframe block.
struct M3KeySpan {
    /// @brief Indices of the bracketing keys; equal when the time is pinned to
    ///        a single key.
    std::size_t i0 = 0;
    std::size_t i1 = 0;
    ::whiteout::f32 frac = 0.0f;
    bool valid = false;
};

/// @brief Bracket @p timeMs inside @p times.
///
/// @param loop wrap on the block's own last timestamp, per the note above.
/// @param interpolate false ⇒ hold the left key (a step track).
M3KeySpan M3LocateKey(std::span<const ::whiteout::i32> times, ::whiteout::i32 timeMs, bool loop,
                      bool interpolate);

/// @brief Componentwise quaternion lerp — deliberately not a slerp.
///
/// Reproduces `M3Anim_EvalTrackQuat`, which does a plain SSE lerp with no
/// normalisation and no hemisphere correction. Real slerping happens one level
/// up, when several layers are combined.
::whiteout::Quaternion M3LerpQuatRaw(const ::whiteout::Quaternion& a,
                                     const ::whiteout::Quaternion& b, ::whiteout::f32 t);

/// @brief Slerp used to combine two layers' rotations, with the shortest-arc
///        sign fix the track-level lerp omits.
::whiteout::Quaternion M3SlerpQuat(const ::whiteout::Quaternion& a, const ::whiteout::Quaternion& b,
                                   ::whiteout::f32 t);

/// @brief The blend factor one contribution applies, given the weight already
///        accumulated below it.
///
/// `t = w / (acc + w)`, shaped by `t²(3 − 2t)`. Constants verified in the
/// binary (`-2.0f`, `3.0f`); a plain normalised mean is visibly different on
/// any three-way blend.
inline ::whiteout::f32 M3SmoothstepFactor(::whiteout::f32 accumulated, ::whiteout::f32 w) {
    const ::whiteout::f32 denom = accumulated + w;
    if (denom <= 0.0f)
        return 1.0f;
    const ::whiteout::f32 t = w / denom;
    return t * t * (3.0f - 2.0f * t);
}

/// @brief Budget the weight walk starts with, and the epsilon it stops at.
inline constexpr ::whiteout::f32 kM3StartBudget = 1.0f;
inline constexpr ::whiteout::f32 kM3BudgetEpsilon = 1e-5f;
/// @brief Above this remaining budget the first contribution is returned as-is.
inline constexpr ::whiteout::f32 kM3SettledBudget = 0.99999f;

} // namespace whiteout::flakes::io

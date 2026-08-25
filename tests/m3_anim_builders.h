#pragma once

// ============================================================================
// Synthetic `whiteout::m3::Model` fixtures for the animation tests.
//
// The M3 animation structures are plain vectors once parsed, so a test can
// build one directly instead of shipping binary files. That matters more here
// than it looks: the behaviours worth testing — split body, the transparent /
// opaque asymmetry, one contribution per play, the weight budget — need
// several sub-track containers keyed against each other in specific ways, and
// no single shipped model exercises them in isolation.
//
// The one thing these builders encode that is *not* independently verified is
// the animRefs word layout (`(sdSlot << 16) | blockIndex`). That is O8 in the
// design doc; `m3_animation_test`'s corpus sweep asserts real files agree with
// it, so if the assumption is wrong both the fixtures and the assert fail
// together rather than the fixtures quietly agreeing with a broken reader.
// ============================================================================

#include <whiteout/models/m3/structures.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace m3fix {

using namespace ::whiteout;

/// @brief Which of a sub-track container's 13 typed arrays a value type lives
///        in. Mirrors the slot order in `SubTrackContainer`.
enum class SdSlot : u32 {
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
};

/// @brief The `animRefs` entry pointing at block @p index of @p slot.
inline u32 AnimRefWord(SdSlot slot, u32 index) {
    return (static_cast<u32>(slot) << 16) | (index & 0xFFFFu);
}
inline SdSlot SlotOf(u32 word) {
    return static_cast<SdSlot>(word >> 16);
}
inline u32 IndexOf(u32 word) {
    return word & 0xFFFFu;
}

template <typename T>
inline m3::AnimBlock<T> Block(std::vector<i32> times, std::vector<T> keys) {
    m3::AnimBlock<T> b;
    b.endFrame = times.empty() ? 0u : static_cast<u32>(times.back());
    b.flags = 0;
    b.timestamps = std::move(times);
    b.keys = std::move(keys);
    return b;
}

/// @brief An animated reference. Bit 4 of @p flags steps; nothing else does —
///        @p interpType is dead at sample time (the loader overwrites it with a
///        track-table row), so it is here only to keep fixtures shaped like the
///        file.
template <typename T>
inline m3::AnimRef<T> Ref(u32 animId, T init, u16 interpType = 1, u16 flags = 1) {
    m3::AnimRef<T> r;
    r.animId = animId;
    r.initValue = init;
    r.nullValue = T{};
    r.interpType = interpType;
    r.flags = flags;
    return r;
}

/// @brief A reference with no animation at all — always its init value.
template <typename T>
inline m3::AnimRef<T> ConstRef(T init) {
    m3::AnimRef<T> r;
    r.animId = 0;
    r.initValue = init;
    r.nullValue = T{};
    r.interpType = 0;
    r.flags = 0;
    return r;
}

/// @brief Builds one `SubTrackContainer`, keeping `animIds` and `animRefs`
///        in step as tracks are added.
class StcBuilder {
public:
    explicit StcBuilder(std::string name, u16 priority = 0, bool runsConcurrent = false) {
        stc_.name = std::move(name);
        stc_.animPriority = priority;
        stc_.runsConcurrent = runsConcurrent ? 1u : 0u;
        stc_.animationStateIndex = 0;
    }

    StcBuilder& Vec3(u32 animId, m3::AnimBlock<Vector3f> b) {
        return Add(animId, SdSlot::Vec3, stc_.sd3v, std::move(b));
    }
    StcBuilder& Quat(u32 animId, m3::AnimBlock<Quaternion> b) {
        return Add(animId, SdSlot::Quat, stc_.sd4q, std::move(b));
    }
    StcBuilder& Float(u32 animId, m3::AnimBlock<f32> b) {
        return Add(animId, SdSlot::Float, stc_.sdr3, std::move(b));
    }
    StcBuilder& Vec2(u32 animId, m3::AnimBlock<Vector2f> b) {
        return Add(animId, SdSlot::Vec2, stc_.sd2v, std::move(b));
    }
    StcBuilder& Color(u32 animId, m3::AnimBlock<m3::ColorBGRA> b) {
        return Add(animId, SdSlot::Color, stc_.sdcc, std::move(b));
    }
    StcBuilder& U32(u32 animId, m3::AnimBlock<u32> b) {
        return Add(animId, SdSlot::U32, stc_.sdu3, std::move(b));
    }
    /// @brief The slot shipped content actually uses for a discrete u32 channel.
    ///
    /// `SDFG` and `SDU3` both carry four-byte keys and a file may name either;
    /// every keyed visibility and `dynamicState` in the corpus names this one,
    /// so a fixture built only through @ref U32 exercises a path no model takes.
    StcBuilder& Flags(u32 animId, const std::vector<i32>& stamps,
                      const std::vector<u32>& values) {
        m3::AnimBlock<m3::Flag> b;
        b.timestamps = stamps;
        b.flags = 0;
        b.endFrame = stamps.empty() ? 0u : static_cast<u32>(stamps.back());
        for (u32 v : values)
            b.keys.push_back(m3::Flag{v});
        return Add(animId, SdSlot::Flag, stc_.sdfg, std::move(b));
    }
    StcBuilder& U16(u32 animId, m3::AnimBlock<u16> b) {
        return Add(animId, SdSlot::U16, stc_.sdu6, std::move(b));
    }
    StcBuilder& Event(u32 animId, m3::AnimBlock<m3::Event> b) {
        return Add(animId, SdSlot::Event, stc_.sdev, std::move(b));
    }

    m3::SubTrackContainer Build() {
        return stc_;
    }

private:
    template <typename T>
    StcBuilder& Add(u32 animId, SdSlot slot, std::vector<m3::AnimBlock<T>>& arr,
                    m3::AnimBlock<T> b) {
        const u32 index = static_cast<u32>(arr.size());
        arr.push_back(std::move(b));
        stc_.animIds.push_back(animId);
        stc_.animRefs.push_back(AnimRefWord(slot, index));
        return *this;
    }

    m3::SubTrackContainer stc_;
};

/// @brief Assembles a model: bones, sub-track containers, and the
///        sequence → container-group pairing that makes split body work.
class ModelBuilder {
public:
    /// @brief Adds a bone. @p parent is -1 for a root.
    ModelBuilder& Bone(std::string name, i32 parent, m3::AnimRef<Vector3f> pos,
                       m3::AnimRef<Quaternion> rot, m3::AnimRef<Vector3f> scale,
                       m3::BoneFlag flags = m3::BoneFlag::Animated) {
        m3::Bone b;
        b.name = std::move(name);
        b.parentIndex = parent < 0 ? 0xFFFFu : static_cast<u16>(parent);
        b.flags = flags;
        b.position = pos;
        b.rotation = rot;
        b.scale = scale;
        b.visibility = ConstRef<u32>(1);
        m_.bones.push_back(std::move(b));
        return *this;
    }

    /// @brief A bone with no animated channels — holds its init values.
    ModelBuilder& StaticBone(std::string name, i32 parent, Vector3f pos = {0, 0, 0}) {
        return Bone(std::move(name), parent, ConstRef(pos),
                    ConstRef(Quaternion{0, 0, 0, 1}), ConstRef(Vector3f{1, 1, 1}),
                    m3::BoneFlag::None);
    }

    u32 AddStc(m3::SubTrackContainer stc) {
        m_.subTrackCollections.push_back(std::move(stc));
        return static_cast<u32>(m_.subTrackCollections.size() - 1);
    }

    /// @brief Adds a sequence together with the containers that drive it.
    ///
    /// The group is what makes one play become several layers: StarCraft II
    /// spawns one player per container in the sequence's group, each at that
    /// container's own priority.
    ModelBuilder& Sequence(std::string name, u32 startFrame, u32 endFrame,
                           std::vector<u32> stcIndices,
                           m3::SequenceFlag flags = m3::SequenceFlag::None) {
        m3::Sequence s;
        s.id = static_cast<i32>(m_.sequences.size());
        s.index = s.id;
        s.name = std::move(name);
        s.startFrame = startFrame;
        s.endFrame = endFrame;
        s.moveSpeed = 0.0f;
        s.flags = flags;
        m_.sequences.push_back(std::move(s));

        m3::AnimationGroup g;
        g.name = m_.sequences.back().name;
        g.subtrackIndices = std::move(stcIndices);
        m_.animationGroups.push_back(std::move(g));
        return *this;
    }

    /// @brief Identity inverse-bind matrices, one per bone.
    ModelBuilder& IdentityIref() {
        m_.initialReference.assign(m_.bones.size(), m3::InitialReference{});
        for (auto& r : m_.initialReference)
            r.matrix = Matrix44f::identity();
        return *this;
    }

    ModelBuilder& Iref(std::vector<Matrix44f> mats) {
        m_.initialReference.clear();
        for (auto& mm : mats) {
            m3::InitialReference r;
            r.matrix = mm;
            m_.initialReference.push_back(r);
        }
        return *this;
    }

    m3::Model Build() {
        if (m_.initialReference.empty())
            IdentityIref();
        return m_;
    }

private:
    m3::Model m_;
};

// ---------------------------------------------------------------------------
// Canned fixtures
// ---------------------------------------------------------------------------

/// @brief Bone index the split-body fixture animates from both layers.
inline constexpr int kSplitSharedBone = 1;
inline constexpr u32 kUpperPosAnimId = 100;
inline constexpr u32 kLowerPosAnimId = 101;

/// @brief Two bones, one sequence, two containers.
///
///  - container 0 "lower", priority 1, opaque      — keys bone 0 and bone 1
///  - container 1 "upper", priority 2, concurrent  — keys bone 1 only
///
/// Bone 1 is keyed by both, which is what the one-contribution-per-play rule
/// resolves; bone 0 is keyed only by the lower layer, which is what the
/// transparent-skip rule leaves alone.
inline m3::Model SplitBodyFixture() {
    ModelBuilder mb;
    mb.Bone("root", -1, Ref<Vector3f>(kLowerPosAnimId, {0, 0, 0}),
            ConstRef(Quaternion{0, 0, 0, 1}), ConstRef(Vector3f{1, 1, 1}));
    mb.Bone("chest", 0, Ref<Vector3f>(kUpperPosAnimId, {0, 0, 0}),
            ConstRef(Quaternion{0, 0, 0, 1}), ConstRef(Vector3f{1, 1, 1}));

    StcBuilder lower("lower", /*priority*/ 1, /*runsConcurrent*/ false);
    lower.Vec3(kLowerPosAnimId, Block<Vector3f>({0, 1000}, {{0, 0, 0}, {10, 0, 0}}));
    lower.Vec3(kUpperPosAnimId, Block<Vector3f>({0, 1000}, {{0, 0, 0}, {0, 20, 0}}));

    StcBuilder upper("upper", /*priority*/ 2, /*runsConcurrent*/ true);
    upper.Vec3(kUpperPosAnimId, Block<Vector3f>({0, 1000}, {{0, 0, 0}, {0, 0, 30}}));

    const u32 lowerIdx = mb.AddStc(lower.Build());
    const u32 upperIdx = mb.AddStc(upper.Build());
    mb.Sequence("Attack", 0, 1000, {lowerIdx, upperIdx});
    return mb.Build();
}

} // namespace m3fix

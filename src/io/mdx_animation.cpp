#include "io/mdx_animation.h"
#include "common_types.h"
#include <algorithm>

using namespace whiteout;
using namespace whiteout::mdx;

namespace WhiteoutDex {

namespace {

Quaternion Wc3Slerp(const Quaternion& a, const Quaternion& b, f32 t) {
    f32 d = a.dot(b);
    d = std::clamp(d, -1.0f, 1.0f);

    Quaternion end = b;
    if (d < 0.0f) {
        d = -d;
        end.x = -end.x;
        end.y = -end.y;
        end.z = -end.z;
        end.w = -end.w;
    }

    const f32 DOT_THRESHOLD = 0.9f;
    if (d > DOT_THRESHOLD) {

        Quaternion result = Quaternion(
            a.x + t * (end.x - a.x),
            a.y + t * (end.y - a.y),
            a.z + t * (end.z - a.z),
            a.w + t * (end.w - a.w)
        );
        return result.normalized();
    }

    f32 theta_0 = std::acos(d);
    f32 theta = theta_0 * t;

    f32 sin_theta = std::sin(theta);
    f32 sin_theta_0 = std::sin(theta_0);

    if (sin_theta_0 < 1e-6f)
        return a;

    f32 s0 = std::sin(theta_0 - theta) / sin_theta_0;
    f32 s1 = sin_theta / sin_theta_0;

    return Quaternion(
        a.x * s0 + end.x * s1,
        a.y * s0 + end.y * s1,
        a.z * s0 + end.z * s1,
        a.w * s0 + end.w * s1
    );
}

Quaternion Wc3Squad(const Quaternion& start, const Quaternion& outtan,
                             const Quaternion& inttan, const Quaternion& end, f32 t) {
    Quaternion slerp1 = Wc3Slerp(start, end, t);
    Quaternion slerp2 = Wc3Slerp(outtan, inttan, t);
    return Wc3Slerp(slerp1, slerp2, 2 * t * (1 - t));
}

}

struct KeyBracket {
    i32 lo = -1;
    i32 hi = -1;
    f32 t = 0.0f;
};

template<typename KeyType>
static KeyBracket FindBracket(const KeyType* keys, i32 count, i32 timeMs,
                              i32 seqStart, i32 seqEnd) {
    KeyBracket b;
    if (count == 0) return b;

    {
        i32 lo = 0, hi = count;
        while (lo < hi) { i32 m = (lo + hi) >> 1; if ((i32)keys[m].frame < seqStart) lo = m + 1; else hi = m; }
        b.lo = lo;
    }
    i32 rangeLo = b.lo;
    if (rangeLo >= count || (i32)keys[rangeLo].frame > seqEnd) { b.lo = -1; return b; }

    {
        i32 lo = rangeLo, hi = count;
        while (lo < hi) { i32 m = (lo + hi) >> 1; if ((i32)keys[m].frame <= seqEnd) lo = m + 1; else hi = m; }
        b.hi = lo - 1;
    }
    i32 rangeHi = b.hi;

    if (rangeLo == rangeHi) { b.lo = b.hi = rangeLo; return b; }

    i32 firstFrame = (i32)keys[rangeLo].frame;
    i32 lastFrame  = (i32)keys[rangeHi].frame;

    if (timeMs < firstFrame || timeMs >= lastFrame) {
        i32 loopLen = seqEnd - seqStart;
        i32 segLen  = (firstFrame - lastFrame) + loopLen;
        b.lo = rangeHi;
        b.hi = rangeLo;
        if (segLen <= 0) return b;
        i32 pos = (timeMs >= lastFrame)
            ? timeMs - lastFrame
            : (timeMs - seqStart) + (seqEnd - lastFrame);
        b.t = std::clamp((f32)pos / (f32)segLen, 0.0f, 1.0f);
        return b;
    }

    {
        i32 lo = rangeLo, hi = rangeHi;
        while (lo < hi) { i32 m = (lo + hi + 1) >> 1; if ((i32)keys[m].frame <= timeMs) lo = m; else hi = m - 1; }
        b.lo = lo;
        b.hi = lo + 1;
        i32 denom = (i32)keys[b.hi].frame - (i32)keys[b.lo].frame;
        b.t = denom > 0 ? (f32)(timeMs - (i32)keys[b.lo].frame) / (f32)denom : 0.0f;
    }
    return b;
}

static f32 HermiteInterp(f32 a, f32 outTanA, f32 inTanB, f32 b, f32 t) {
    f32 t2 = t * t, t3 = t2 * t;
    f32 h1 =  2*t3 - 3*t2 + 1;
    f32 h2 = -2*t3 + 3*t2;
    f32 h3 =    t3 - 2*t2 + t;
    f32 h4 =    t3 -   t2;
    return h1*a + h2*b + h3*outTanA + h4*inTanB;
}

static f32 BezierInterp(f32 a, f32 outTanA, f32 inTanB, f32 b, f32 t) {
    f32 it = 1.0f - t;
    return it*it*it*a + 3*it*it*t*outTanA + 3*it*t*t*inTanB + t*t*t*b;
}

template<typename ScalarFn>
static Vector3f ApplyV3(ScalarFn fn, const Vector3f& a, const Vector3f& ota,
                        const Vector3f& itb, const Vector3f& b, f32 t) {
    return {fn(a.x, ota.x, itb.x, b.x, t),
            fn(a.y, ota.y, itb.y, b.y, t),
            fn(a.z, ota.z, itb.z, b.z, t)};
}

template<typename T, typename LerpFn, typename CurveFn>
static T EvaluateTrackImpl(const Track<T>& track, i32 timeMs, i32 seqStart, i32 seqEnd,
                           const T& defaultVal, LerpFn lerp, CurveFn curve,
                           bool forceNoInterp = false) {
    if (!track.isUsed || track.keyCount == 0) return defaultVal;

    const InterpolationType interp = track.interpolationType;
    const bool useCurve = (interp == InterpolationType::Hermite ||
                           interp == InterpolationType::Bezier);

    auto& mut = const_cast<Track<T>&>(track);
    if (useCurve) {
        auto keys = mut.tangentKeys();
        auto br = FindBracket(keys.data(), (i32)keys.size(), timeMs, seqStart, seqEnd);
        if (br.lo < 0) return defaultVal;
        if (br.lo == br.hi || forceNoInterp) return keys[br.lo].value;
        return curve(keys[br.lo].value, keys[br.lo].outTan,
                     keys[br.hi].inTan, keys[br.hi].value, br.t, interp);
    }
    auto keys = mut.keys();
    auto br = FindBracket(keys.data(), (i32)keys.size(), timeMs, seqStart, seqEnd);
    if (br.lo < 0) return defaultVal;
    if (br.lo == br.hi || forceNoInterp || interp == InterpolationType::None)
        return keys[br.lo].value;
    return lerp(keys[br.lo].value, keys[br.hi].value, br.t);
}

f32 EvaluateTrackF32(const Track<f32>& track, i32 timeMs, i32 seqStart, i32 seqEnd,
                     f32 defaultVal, bool forceNoInterp) {
    return EvaluateTrackImpl<f32>(track, timeMs, seqStart, seqEnd, defaultVal,
        [](f32 a, f32 b, f32 t) { return a + (b - a) * t; },
        [](f32 a, f32 ota, f32 itb, f32 b, f32 t, InterpolationType it) {
            return it == InterpolationType::Hermite
                ? HermiteInterp(a, ota, itb, b, t)
                : BezierInterp(a, ota, itb, b, t);
        },
        forceNoInterp);
}

Vector3f EvaluateTrackVec3(const Track<Vector3f>& track, i32 timeMs, i32 seqStart, i32 seqEnd,
                           Vector3f defaultVal) {
    return EvaluateTrackImpl<Vector3f>(track, timeMs, seqStart, seqEnd, defaultVal,
        [](const Vector3f& a, const Vector3f& b, f32 t) { return Vector3f::lerp(a, b, t); },
        [](const Vector3f& a, const Vector3f& ota, const Vector3f& itb,
           const Vector3f& b, f32 t, InterpolationType it) {
            return it == InterpolationType::Hermite
                ? ApplyV3(HermiteInterp, a, ota, itb, b, t)
                : ApplyV3(BezierInterp, a, ota, itb, b, t);
        });
}

Quaternion EvaluateTrackQuat(const Track<Quaternion>& track, i32 timeMs, i32 seqStart, i32 seqEnd,
                             Quaternion defaultVal) {
    return EvaluateTrackImpl<Quaternion>(track, timeMs, seqStart, seqEnd, defaultVal,
        [](const Quaternion& a, const Quaternion& b, f32 t) { return Wc3Slerp(a, b, t); },
        [](const Quaternion& a, const Quaternion& ota, const Quaternion& itb,
           const Quaternion& b, f32 t, InterpolationType) {
            return Wc3Squad(a, ota, itb, b, t);
        });
}

u32 EvaluateTrackU32(const Track<u32>& track, i32 timeMs, i32 seqStart, i32 seqEnd, u32 defaultVal) {
    if (!track.isUsed || track.keyCount == 0) return defaultVal;
    auto keys = const_cast<Track<u32>&>(track).keys();
    i32 count = (i32)keys.size();
    if (count == 0) return defaultVal;

    i32 rangeLo = -1, rangeHi = -1;
    for (i32 i = 0; i < count; i++) {
        i32 f = (i32)keys[i].frame;
        if (f > seqEnd) break;
        if (f >= seqStart) { if (rangeLo < 0) rangeLo = i; rangeHi = i; }
    }
    if (rangeLo < 0) return defaultVal;

    u32 val = keys[rangeLo].value;
    for (i32 i = rangeLo; i <= rangeHi; i++) {
        if ((i32)keys[i].frame <= timeMs) val = keys[i].value;
        else break;
    }
    return val;
}

Matrix44f BindPose3x4ToMatrix44f(const std::array<f32, 12>& bp) {
    Matrix44f m{};
    m.data[0] = {bp[0], bp[1], bp[2],  0.0f};
    m.data[1] = {bp[3], bp[4], bp[5],  0.0f};
    m.data[2] = {bp[6], bp[7], bp[8],  0.0f};
    m.data[3] = {bp[9], bp[10],bp[11], 1.0f};
    return m;
}

Matrix44f Vec3QuatScaleToMatrix44f(const Vector3f& t, const Quaternion& r,
                                    const Vector3f& s, const Vector3f& pivot) {
    Matrix44f mS = Matrix44f::scaling(s);
    Matrix44f mR = Matrix44f::rotation(r).transpose();
    Matrix44f mNegPiv  = Matrix44f::translation({-pivot.x, -pivot.y, -pivot.z});
    Matrix44f mPosPivT = Matrix44f::translation({pivot.x + t.x, pivot.y + t.y, pivot.z + t.z});
    return mNegPiv * mS * mR * mPosPivT;
}

void MdxHierarchy::Build(const whiteout::mdx::Model& model) {
    nodes_.clear();
    objectIdToIdx_.clear();
    boneCount_ = (i32)model.bones.size();

    std::vector<u32> parentIds;

    auto addNode = [&](const Node& node, HierarchyNode::Source src, i32 srcIdx) {
        HierarchyNode hn;
        hn.objectId    = (i32)node.objectId;
        hn.flags       = (u32)node.flags;
        hn.translation = &node.translationTracks;
        hn.rotation    = &node.rotationTracks;
        hn.scaling     = &node.scalingTracks;
        hn.source      = src;
        hn.sourceIndex = srcIdx;
        if (hn.objectId < (i32)model.pivotPoints.size())
            hn.pivot = model.pivotPoints[hn.objectId];
        nodes_.push_back(hn);
        parentIds.push_back(node.parentId);
    };

    auto addAll = [&](const auto& arr, HierarchyNode::Source src) {
        for (usize i = 0; i < arr.size(); i++)
            addNode(arr[i].node, src, (i32)i);
    };

    addAll(model.bones,             HierarchyNode::Source::Bone);
    addAll(model.helpers,           HierarchyNode::Source::Helper);
    addAll(model.particleEmitters,  HierarchyNode::Source::ParticleEmitter);
    addAll(model.particleEmitters2, HierarchyNode::Source::ParticleEmitter2);
    addAll(model.ribbonEmitters,    HierarchyNode::Source::RibbonEmitter);
    addAll(model.collisionShapes,   HierarchyNode::Source::CollisionShape);
    addAll(model.attachments,       HierarchyNode::Source::Attachment);
    addAll(model.lights,            HierarchyNode::Source::Light);

    addAll(model.eventObjects,      HierarchyNode::Source::EventObject);

    for (i32 i = 0; i < (i32)nodes_.size(); i++)
        objectIdToIdx_[nodes_[i].objectId] = i;

    for (usize i = 0; i < nodes_.size(); i++) {
        if (parentIds[i] == Node::NO_PARENT) continue;
        auto it = objectIdToIdx_.find((i32)parentIds[i]);
        if (it != objectIdToIdx_.end()) nodes_[i].parentIdx = it->second;
    }

    std::vector<i32> sorted;
    sorted.reserve(nodes_.size());
    for (i32 i = 0; i < (i32)nodes_.size(); i++)
        if (nodes_[i].parentIdx < 0) sorted.push_back(i);

    for (i32 head = 0; head < (i32)sorted.size(); head++) {
        i32 cur = sorted[head];
        for (i32 i = 0; i < (i32)nodes_.size(); i++)
            if (nodes_[i].parentIdx == cur) sorted.push_back(i);
    }

    if (sorted.size() == nodes_.size()) {
        std::vector<i32> newIdx(nodes_.size());
        for (i32 i = 0; i < (i32)sorted.size(); i++)
            newIdx[sorted[i]] = i;

        std::vector<HierarchyNode> sortedNodes(nodes_.size());
        for (i32 i = 0; i < (i32)sorted.size(); i++) {
            sortedNodes[i] = nodes_[sorted[i]];
            if (sortedNodes[i].parentIdx >= 0)
                sortedNodes[i].parentIdx = newIdx[sortedNodes[i].parentIdx];
        }
        nodes_ = std::move(sortedNodes);

        objectIdToIdx_.clear();
        for (i32 i = 0; i < (i32)nodes_.size(); i++)
            objectIdToIdx_[nodes_[i].objectId] = i;
    }

    boneIdxToNodeIdx_.assign(boneCount_, -1);
    for (i32 i = 0; i < (i32)nodes_.size(); i++) {
        const auto& n = nodes_[i];
        if (n.source == HierarchyNode::Source::Bone &&
            n.sourceIndex >= 0 && n.sourceIndex < boneCount_) {
            boneIdxToNodeIdx_[n.sourceIndex] = i;
        }
    }
}

void MdxHierarchy::Evaluate(i32 timeMs, i32 seqStart, i32 seqEnd,
                             const std::vector<u32>& globalSequences,
                             std::vector<Matrix44f>& boneWorldMatrices,
                             std::vector<Matrix44f>& allNodeMatrices,
                             const Vector3f* cameraPos,
                             i32 globalTimeMs) const {
    i32 nc = (i32)nodes_.size();
    allNodeMatrices.resize(nc);

    static const Vector3f   defaultT = {0, 0, 0};
    static const Quaternion defaultR = {0, 0, 0, 1};
    static const Vector3f   defaultS = {1, 1, 1};

    std::vector<Matrix44f> stackM(nc, Matrix44f::identity());

    struct EffTime { i32 time; i32 start; i32 end; };
    auto effTime = [&](const auto* track) -> EffTime {
        EffTime e{timeMs, seqStart, seqEnd};
        if (!track || !track->isUsed) return e;
        if (track->globalSequenceId == Track<f32>::kNoGlobalSequence) return e;
        u32 gsId = track->globalSequenceId;
        if (gsId >= (u32)globalSequences.size()) return e;
        u32 duration = globalSequences[gsId];
        if (duration == 0) {
            e.start = 0; e.end = 0x3FFFFFFF; e.time = 0;
            return e;
        }
        i32 gsTime = (globalTimeMs >= 0) ? globalTimeMs : timeMs;
        e.start = 0;
        e.end   = (i32)duration;
        e.time  = (i32)std::fmod((f32)gsTime, (f32)duration);
        return e;
    };

    auto evalVec3 = [&](const Track<Vector3f>* tr, const Vector3f& def) {
        if (!tr) return def;
        auto e = effTime(tr);
        return EvaluateTrackVec3(*tr, e.time, e.start, e.end, def);
    };
    auto evalQuat = [&](const Track<Quaternion>* tr, const Quaternion& def) {
        if (!tr) return def;
        auto e = effTime(tr);
        return EvaluateTrackQuat(*tr, e.time, e.start, e.end, def);
    };

    for (i32 i = 0; i < nc; i++) {
        const auto& n = nodes_[i];

        const Vector3f   localT = evalVec3(n.translation, defaultT);
        const Quaternion localR = evalQuat(n.rotation,    defaultR);
        const Vector3f   localS = evalVec3(n.scaling,     defaultS);

        const u32 flags = n.flags;
        using NF = Node::NodeFlag;
        const bool rmT = flags & (u32)NF::DontInheritTranslation;
        const bool rmR = flags & (u32)NF::DontInheritRotation;
        const bool rmS = flags & (u32)NF::DontInheritScaling;

        const Vector3f& currPivot = n.pivot;
        Vector3f parentPivot = {0, 0, 0};
        Matrix44f M = Matrix44f::identity();
        if (n.parentIdx >= 0 && n.parentIdx < nc) {
            M = stackM[n.parentIdx];
            parentPivot = nodes_[n.parentIdx].pivot;
        }

        auto applyTranslate = [&](f32 tx, f32 ty, f32 tz) {
            M.data[3][0] += tx * M.data[0][0] + ty * M.data[1][0] + tz * M.data[2][0];
            M.data[3][1] += tx * M.data[0][1] + ty * M.data[1][1] + tz * M.data[2][1];
            M.data[3][2] += tx * M.data[0][2] + ty * M.data[1][2] + tz * M.data[2][2];
        };

        auto stripTranslation = [&]() {
            M.data[3][0] = 0; M.data[3][1] = 0; M.data[3][2] = 0;
        };
        auto stripRotationKeepScale = [&]() {

            for (i32 r = 0; r < 3; ++r) {
                f32 mx = M.data[r][0], my = M.data[r][1], mz = M.data[r][2];
                f32 mag = std::sqrt(mx*mx + my*my + mz*mz);
                M.data[r][0] = 0; M.data[r][1] = 0; M.data[r][2] = 0;
                M.data[r][r] = mag;
            }
        };
        auto stripScaleKeepRotation = [&]() {

            for (i32 r = 0; r < 3; ++r) {
                f32 mx = M.data[r][0], my = M.data[r][1], mz = M.data[r][2];
                f32 mag = std::sqrt(mx*mx + my*my + mz*mz);
                if (mag > 1e-8f) {
                    f32 inv = 1.0f / mag;
                    M.data[r][0] = mx * inv;
                    M.data[r][1] = my * inv;
                    M.data[r][2] = mz * inv;
                }
            }
        };
        auto stripRotationAndScale = [&]() {

            for (i32 r = 0; r < 3; ++r) {
                M.data[r][0] = 0; M.data[r][1] = 0; M.data[r][2] = 0;
                M.data[r][r] = 1.0f;
            }
        };

        Vector3f tTrans = localT;
        if (rmT) {
            stripTranslation();
            if (rmR && rmS)       stripRotationAndScale();
            else if (rmR)         stripRotationKeepScale();
            else if (rmS)         stripScaleKeepRotation();
            tTrans.x += parentPivot.x;
            tTrans.y += parentPivot.y;
            tTrans.z += parentPivot.z;
        }
        tTrans.x += currPivot.x - parentPivot.x;
        tTrans.y += currPivot.y - parentPivot.y;
        tTrans.z += currPivot.z - parentPivot.z;
        applyTranslate(tTrans.x, tTrans.y, tTrans.z);

        if (rmR && !rmT && !rmS) {
            stripRotationKeepScale();
        }
        if (localR.x != 0.0f || localR.y != 0.0f || localR.z != 0.0f || localR.w != 1.0f) {

            M = Matrix44f::rotation(localR).transpose() * M;
        }

        if (rmS && !rmT) {
            if (rmR) stripRotationAndScale();
            else     stripScaleKeepRotation();
        }
        if (localS.x != 1.0f || localS.y != 1.0f || localS.z != 1.0f) {
            for (i32 c = 0; c < 4; ++c) M.data[0][c] *= localS.x;
            for (i32 c = 0; c < 4; ++c) M.data[1][c] *= localS.y;
            for (i32 c = 0; c < 4; ++c) M.data[2][c] *= localS.z;
        }

        stackM[i] = M;

        Matrix44f worldM = M;
        worldM.data[3][0] -= currPivot.x * worldM.data[0][0] + currPivot.y * worldM.data[1][0] + currPivot.z * worldM.data[2][0];
        worldM.data[3][1] -= currPivot.x * worldM.data[0][1] + currPivot.y * worldM.data[1][1] + currPivot.z * worldM.data[2][1];
        worldM.data[3][2] -= currPivot.x * worldM.data[0][2] + currPivot.y * worldM.data[1][2] + currPivot.z * worldM.data[2][2];

        allNodeMatrices[i] = worldM;
    }

    boneWorldMatrices.resize(boneCount_);
    for (i32 i = 0; i < nc; i++) {
        if (nodes_[i].source == HierarchyNode::Source::Bone &&
            nodes_[i].sourceIndex < boneCount_) {
            boneWorldMatrices[nodes_[i].sourceIndex] = allNodeMatrices[i];
        }
    }
}

i32 MdxHierarchy::ObjectIdToNodeIndex(i32 objectId) const {
    auto it = objectIdToIdx_.find(objectId);
    return it != objectIdToIdx_.end() ? it->second : -1;
}

i32 MdxHierarchy::BoneIndexToNodeIndex(i32 boneIdx) const {
    if (boneIdx < 0 || boneIdx >= (i32)boneIdxToNodeIdx_.size()) return -1;
    return boneIdxToNodeIdx_[boneIdx];
}

}

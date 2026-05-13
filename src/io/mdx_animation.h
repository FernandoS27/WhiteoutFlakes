#pragma once

#include <cmath>
#include <vector>
#include <whiteout/models/mdx/structures.h>
#include <whiteout/models/mdx/types.h>
#include <whiteout/vector_types.h>
#include "whiteout/flakes/model_types.h"
#include "whiteout/flakes/types.h"

namespace whiteout::flakes::io {

f32 EvaluateTrackF32(const whiteout::mdx::Track<whiteout::f32>& track, i32 timeMs, i32 seqStart,
                     i32 seqEnd, f32 defaultVal, bool forceNoInterp = false);

whiteout::u32 EvaluateTrackU32(const whiteout::mdx::Track<whiteout::u32>& track, i32 timeMs,
                               i32 seqStart, i32 seqEnd, whiteout::u32 defaultVal);

whiteout::Vector3f EvaluateTrackVec3(const whiteout::mdx::Track<whiteout::Vector3f>& track,
                                     i32 timeMs, i32 seqStart, i32 seqEnd,
                                     whiteout::Vector3f defaultVal);

whiteout::Vector4f EvaluateTrackVec4(const whiteout::mdx::Track<whiteout::Vector4f>& track,
                                     i32 timeMs, i32 seqStart, i32 seqEnd,
                                     whiteout::Vector4f defaultVal);

whiteout::Quaternion EvaluateTrackQuat(const whiteout::mdx::Track<whiteout::Quaternion>& track,
                                       i32 timeMs, i32 seqStart, i32 seqEnd,
                                       whiteout::Quaternion defaultVal);

struct HierarchyNode {
    i32 objectId = 0;
    i32 parentIdx = -1;
    whiteout::Vector3f pivot = {0, 0, 0};
    u32 flags = 0;

    const whiteout::mdx::Track<whiteout::Vector3f>* translation = nullptr;
    const whiteout::mdx::Track<whiteout::Quaternion>* rotation = nullptr;
    const whiteout::mdx::Track<whiteout::Vector3f>* scaling = nullptr;

    enum class Source {
        Bone,
        Helper,
        ParticleEmitter,
        ParticleEmitter2,
        RibbonEmitter,
        CollisionShape,
        Attachment,
        Light,
        EventObject,
        CornEmitter,
        Other
    };
    Source source = Source::Other;
    i32 sourceIndex = 0;
};

class MdxHierarchy {
public:
    void Build(const whiteout::mdx::Model& model);

    void Evaluate(i32 timeMs, i32 seqStart, i32 seqEnd,
                  const std::vector<whiteout::u32>& globalSequences,
                  std::vector<Matrix44f>& boneWorldMatrices,
                  std::vector<Matrix44f>& allNodeMatrices, const Vector3f* cameraPos = nullptr,
                  i32 globalTimeMs = -1) const;

    i32 BoneCount() const {
        return boneCount_;
    }
    i32 NodeCount() const {
        return (i32)nodes_.size();
    }
    const std::vector<HierarchyNode>& Nodes() const {
        return nodes_;
    }

    i32 ObjectIdToNodeIndex(i32 objectId) const;

    i32 BoneIndexToNodeIndex(i32 boneIdx) const;

private:
    std::vector<HierarchyNode> nodes_;
    i32 boneCount_ = 0;
    std::unordered_map<i32, i32> objectIdToIdx_;
    std::vector<i32> boneIdxToNodeIdx_;
};

Matrix44f BindPose3x4ToMatrix44f(const std::array<whiteout::f32, 12>& bp);
Matrix44f Vec3QuatScaleToMatrix44f(const whiteout::Vector3f& t, const whiteout::Quaternion& r,
                                   const whiteout::Vector3f& s, const whiteout::Vector3f& pivot);

} // namespace whiteout::flakes::io

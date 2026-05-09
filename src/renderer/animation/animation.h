#pragma once

#include "common_types.h"
#include "model/model_types.h"
#include "types.h"
#include <memory>
#include <unordered_map>

namespace whiteout::flakes::renderer::animation {

struct GeosetSkinInfo {
    std::vector<model::VertexInfluence> vertices;
};

struct GeosetPaletteLayout {
    std::vector<i32>                       subsetNodeIndices;
    std::vector<model::GroupAverageRecord> groupAverages;
};

struct SkinningData {
    i32                                          nodeCount = 0;
    std::vector<Matrix44f>                       inverseBindMatrices;
    std::unordered_map<i32, GeosetSkinInfo>      geosetWeights;
    std::unordered_map<i32, GeosetPaletteLayout> geosetLayouts;
};

class SkinningSystem {
public:
    void Clear() {
        data_.reset();
        currentMatrices_.clear();
        offsetMatrices_.clear();
        matricesDirty_ = false;
        nodesReady_ = false;
    }

    void SetSharedData(std::shared_ptr<SkinningData> data) {
        data_ = std::move(data);
        const i32 n = data_ ? data_->nodeCount : 0;
        currentMatrices_.assign(n, Matrix44f::identity());
        offsetMatrices_.assign(n, Matrix44f::identity());
        matricesDirty_ = false;
        nodesReady_ = false;
    }

    void SetSkeleton(i32 nodeCount, const f32* inverseBindData) {
        SkinningData* d = ensureOwnedData();
        d->nodeCount = nodeCount;
        d->inverseBindMatrices.resize(nodeCount);
        for (i32 i = 0; i < nodeCount; i++) {
            const f32* m = inverseBindData + i * 16;
            Matrix44f& mat = d->inverseBindMatrices[i];
            mat.data[0] = {m[0], m[1], m[2],  m[3]};
            mat.data[1] = {m[4], m[5], m[6],  m[7]};
            mat.data[2] = {m[8], m[9], m[10], m[11]};
            mat.data[3] = {m[12],m[13],m[14], m[15]};
        }
        currentMatrices_.assign(nodeCount, Matrix44f::identity());
        offsetMatrices_.assign(nodeCount, Matrix44f::identity());
        nodesReady_ = false;
    }

    void SetGeosetLayout(i32 geosetId, GeosetPaletteLayout layout) {
        ensureOwnedData()->geosetLayouts[geosetId] = std::move(layout);
    }

    void SetGeosetWeights(i32 geosetId, i32 vertCount,
                          const i32* nodeIndices, const f32* weights) {
        GeosetSkinInfo& info = ensureOwnedData()->geosetWeights[geosetId];
        info.vertices.resize(vertCount);
        for (i32 v = 0; v < vertCount; v++) {
            for (i32 j = 0; j < 4; j++) {
                info.vertices[v].boneIdx[j] = nodeIndices[v * 4 + j];
                info.vertices[v].weight[j]  = weights[v * 4 + j];
            }
        }
    }

    std::shared_ptr<SkinningData> SharedData() const { return data_; }

    void UpdateNodeMatrices(i32 nodeCount, const f32* worldData) {
        if (!data_ || nodeCount != data_->nodeCount) return;
        for (i32 i = 0; i < nodeCount; i++) {
            const f32* m = worldData + i * 16;
            Matrix44f& mat = currentMatrices_[i];
            mat.data[0] = {m[0], m[1], m[2],  m[3]};
            mat.data[1] = {m[4], m[5], m[6],  m[7]};
            mat.data[2] = {m[8], m[9], m[10], m[11]};
            mat.data[3] = {m[12],m[13],m[14], m[15]};
        }
        matricesDirty_ = true;
        nodesReady_ = true;
    }

    bool HasSkeleton()              const { return data_ && data_->nodeCount > 0; }
    bool IsReady()                  const { return nodesReady_; }
    i32  NodeCount()                const { return data_ ? data_->nodeCount : 0; }
    bool HasWeights(i32 geosetId)   const {
        return data_ && data_->geosetWeights.count(geosetId) > 0;
    }
    bool NeedsUpdate()              const { return matricesDirty_; }

    const GeosetSkinInfo* GetGeosetWeights(i32 geosetId) const {
        if (!data_) return nullptr;
        auto it = data_->geosetWeights.find(geosetId);
        return (it != data_->geosetWeights.end()) ? &it->second : nullptr;
    }

    const GeosetPaletteLayout* GetGeosetLayout(i32 geosetId) const {
        if (!data_) return nullptr;
        auto it = data_->geosetLayouts.find(geosetId);
        return (it != data_->geosetLayouts.end()) ? &it->second : nullptr;
    }

    i32 GeosetPaletteSize(i32 geosetId) const {
        auto* layout = GetGeosetLayout(geosetId);
        if (!layout) return 0;
        return (i32)layout->subsetNodeIndices.size() + (i32)layout->groupAverages.size();
    }

    const Matrix44f* OffsetMatrices() const { return offsetMatrices_.data(); }

    i32 LocalSlotToNodeIndex(i32 geosetId, i32 localSlot) const {
        auto* layout = GetGeosetLayout(geosetId);
        if (!layout) return -1;
        if (localSlot < 0 || localSlot >= (i32)layout->subsetNodeIndices.size())
            return -1;
        return layout->subsetNodeIndices[localSlot];
    }

    void ComputeOffsetMatrices() {
        if (!data_ || !matricesDirty_) return;
        const i32 n = data_->nodeCount;
        const auto& inv = data_->inverseBindMatrices;
        for (i32 i = 0; i < n; i++) {

            offsetMatrices_[i] = inv[i] * currentMatrices_[i];
        }
        matricesDirty_ = false;
    }

    i32 ComputeGeosetPalette(i32 geosetId, Matrix44f* out, i32 capacity) const {
        auto* layout = GetGeosetLayout(geosetId);
        if (!layout || !out || capacity <= 0 || !data_) {
            if (out && capacity > 0) {
                for (i32 i = 0; i < capacity; ++i) out[i] = Matrix44f::identity();
            }
            return 0;
        }
        const i32 subsetN  = (i32)layout->subsetNodeIndices.size();
        const i32 groupN   = (i32)layout->groupAverages.size();
        const i32 total    = subsetN + groupN;
        const i32 n        = total < capacity ? total : capacity;
        const i32 nodeCnt  = data_->nodeCount;

        for (i32 i = 0; i < subsetN && i < capacity; ++i) {
            i32 g = layout->subsetNodeIndices[i];
            if (g >= 0 && g < nodeCnt) out[i] = offsetMatrices_[g];
            else                       out[i] = Matrix44f::identity();
        }

        for (i32 g = 0; g < groupN; ++g) {
            const i32 slot = layout->groupAverages[g].pseudoSlot;
            if (slot < 0 || slot >= capacity) continue;
            const auto& rec = layout->groupAverages[g];
            if (rec.nodeIndices.empty()) {
                out[slot] = Matrix44f::identity();
                continue;
            }
            Matrix44f sum = Matrix44f::zero();
            i32 cnt = 0;
            for (i32 nodeIdx : rec.nodeIndices) {
                if (nodeIdx < 0 || nodeIdx >= nodeCnt) continue;
                const Matrix44f& m = offsetMatrices_[nodeIdx];
                for (i32 r = 0; r < 4; r++)
                    for (i32 c = 0; c < 4; c++)
                        sum.data[r][c] += m.data[r][c];
                ++cnt;
            }
            if (cnt > 0) {
                f32 inv = 1.0f / (f32)cnt;
                for (i32 r = 0; r < 4; r++)
                    for (i32 c = 0; c < 4; c++)
                        sum.data[r][c] *= inv;
                out[slot] = sum;
            } else {
                out[slot] = Matrix44f::identity();
            }
        }

        for (i32 i = n; i < capacity; ++i) out[i] = Matrix44f::identity();
        return n;
    }

    bool SkinVertices(i32 geosetId,
                      const std::vector<Vertex>& baseVerts,
                      std::vector<Vertex>& outVerts) const
    {
        if (!data_) return false;
        auto it = data_->geosetWeights.find(geosetId);
        if (it == data_->geosetWeights.end()) return false;
        const auto& skin = it->second;

        if (skin.vertices.size() != baseVerts.size()) return false;

        outVerts.resize(baseVerts.size());
        const i32 nodeCnt = data_->nodeCount;

        for (i32 i = 0; i < (i32)baseVerts.size(); i++) {
            const auto& inf = skin.vertices[i];

            Vector3f posSum = {0, 0, 0};
            Vector3f nrmSum = {0, 0, 0};
            f32 totalWeight = 0.0f;

            const Vector3f& basePos = baseVerts[i].position;
            const Vector3f& baseNrm = baseVerts[i].normal;

            for (i32 j = 0; j < 4; j++) {
                f32 w = inf.weight[j];
                if (w < 0.0001f) continue;

                i32 nIdx = inf.boneIdx[j];
                if (nIdx < 0 || nIdx >= nodeCnt) continue;

                const Matrix44f& offset = offsetMatrices_[nIdx];

                posSum += whiteout::transform_point(basePos, offset) * w;

                nrmSum += whiteout::transform_normal(baseNrm, offset) * w;

                totalWeight += w;
            }

            if (totalWeight < 0.0001f) {

                outVerts[i] = baseVerts[i];
                continue;
            }

            outVerts[i].position = posSum;
            outVerts[i].normal = nrmSum.normalized();
            outVerts[i].uv    = baseVerts[i].uv;
            outVerts[i].color = baseVerts[i].color;
        }

        return true;
    }

private:

    SkinningData* ensureOwnedData() {
        if (!data_) data_ = std::make_shared<SkinningData>();
        return data_.get();
    }

    std::shared_ptr<SkinningData> data_;
    std::vector<Matrix44f>        currentMatrices_;
    std::vector<Matrix44f>        offsetMatrices_;
    bool                          matricesDirty_ = false;
    bool                          nodesReady_    = false;
};

}

#pragma once

#include <memory>
#include <unordered_map>
#include <vector>
#include "gfx/gfx.h"
#include "types.h"
#include "whiteout/flakes/model_types.h"
#include "whiteout/flakes/types.h"

namespace whiteout::flakes::renderer::animation {

struct GeosetSkinInfo {
    std::vector<model::VertexInfluence> vertices;
};

struct GeosetPaletteLayout {
    std::vector<i32> subsetNodeIndices;
    std::vector<model::GroupAverageRecord> groupAverages;
};

// Like GroupAverageRecord, but `globalPseudoSlot` is the slot *in the
// per-actor palette* (i.e. the global address space that nodeIndex
// 0..nodeCount-1 also lives in), not a per-geoset slot. Populated by the
// MDX loader's post-processing pass when the actor qualifies for the
// per-actor palette path (Path A); empty otherwise.
struct GlobalGroupAverageRecord {
    i32 globalPseudoSlot;
    std::vector<i32> nodeIndices;
};

// Cap on bones per actor palette — matches bls::kMaxBones in
// bls_cb_layout.h (the BonePaletteCb shader struct is fixed at 256
// slots). Duplicated here as a literal so animation.h doesn't have to
// pull in bls_cb_layout.h. If kMaxBones ever changes you'll get a
// static_assert mismatch in the cpp that uses both.
inline constexpr i32 kActorPaletteCap = 256;

struct SkinningData {
    i32 nodeCount = 0;
    std::vector<Matrix44f> inverseBindMatrices;
    std::unordered_map<i32, GeosetSkinInfo> geosetWeights;
    std::unordered_map<i32, GeosetPaletteLayout> geosetLayouts;

    // Per-actor palette path (Path A). When `usesPerActorPalette` is
    // true, the actor holds a single bonePaletteCb sized to
    // `actorPaletteSize` (= nodeCount + globalGroupAverages.size()),
    // and every vertex's `boneIdx[k]` has been rewritten at load time
    // to reference a slot in that palette directly. The shader keeps
    // its `bones[kMaxBones]` declaration; we just write fewer slots
    // and the vertex stream points only at the filled range.
    //
    // When false, the actor stays on Path B: one bonePaletteCb per
    // geoset, vertex `boneIdx` is a local slot in that geoset's
    // GeosetPaletteLayout. Used for models whose actorPaletteSize
    // would exceed kMaxBones (256).
    //
    // Populated by the MDX adapter's post-processing pass; never
    // mutated at runtime.
    std::vector<GlobalGroupAverageRecord> globalGroupAverages;
    i32 actorPaletteSize = 0;
    bool usesPerActorPalette = false;
};

// Result of the load-time palette layout decision. Path A populates
// globalGroupAverages and mutates `skinWeights[].influences[].boneIdx[]`
// to global slot indices. Path B leaves data as-is and the caller keeps
// using the per-geoset bonePaletteCb layout.
struct PaletteLayoutDecision {
    i32 actorPaletteSize = 0;
    bool usesPerActorPalette = false;
    std::vector<GlobalGroupAverageRecord> globalGroupAverages;
};

// Decide whether the actor qualifies for Path A (single per-actor bone
// palette CB) and, if so, rewrite every vertex's boneIdx to a global
// palette slot and emit a globalGroupAverages list. Layout of slots:
//
//   [0 .. nodeCount)
//       Direct node slots. Slot i holds offsetMatrices_[i].
//   [nodeCount .. nodeCount + totalGroupAverages)
//       Pseudo slots for SD group-averaged bones. Each unique
//       (geoset, localPseudoSlot) gets one global slot here.
//
// On Path A every original local `subset[]` index maps to its global
// node index (subsetNodeIndices[idx]), and every original local
// pseudo-slot maps to the assigned global slot. Vertices whose weight
// is zero in a given lane keep whatever boneIdx the loader left there
// (we still rewrite it; it just doesn't matter because the lane has
// no contribution).
//
// Path B fires when actorPaletteSize > kActorPaletteCap. In that case
// nothing in `skinWeights` is touched; the renderer keeps the per-
// geoset palette path.
inline PaletteLayoutDecision DecidePaletteLayoutAndRewrite(
    i32 nodeCount, std::vector<model::SkinWeightData>& skinWeights) {
    PaletteLayoutDecision result;

    i32 totalGroupAverages = 0;
    for (const auto& sw : skinWeights) {
        totalGroupAverages += (i32)sw.groupAverages.size();
    }
    result.actorPaletteSize = nodeCount + totalGroupAverages;

    if (result.actorPaletteSize > kActorPaletteCap) {
        // Path B: leave skinWeights untouched.
        return result;
    }

    result.usesPerActorPalette = true;
    result.globalGroupAverages.reserve(totalGroupAverages);

    i32 nextGlobalPseudoSlot = nodeCount;
    for (auto& sw : skinWeights) {
        const i32 subsetCount = (i32)sw.subsetNodeIndices.size();
        const i32 groupCount = (i32)sw.groupAverages.size();

        // Map this geoset's local pseudo slot -> assigned global slot.
        std::unordered_map<i32, i32> localPseudoToGlobal;
        localPseudoToGlobal.reserve(groupCount);
        for (auto& rec : sw.groupAverages) {
            const i32 globalSlot = nextGlobalPseudoSlot++;
            localPseudoToGlobal.emplace(rec.pseudoSlot, globalSlot);
            GlobalGroupAverageRecord g;
            g.globalPseudoSlot = globalSlot;
            g.nodeIndices = rec.nodeIndices; // already global node indices
            result.globalGroupAverages.push_back(std::move(g));
        }

        // Rewrite every vertex lane's boneIdx in place. Lanes with
        // weight==0 still get rewritten — the value is unused but a
        // consistent rewrite keeps the data trivially traceable.
        for (auto& inf : sw.influences) {
            for (i32 k = 0; k < 4; ++k) {
                const i32 localIdx = inf.boneIdx[k];
                if (localIdx >= 0 && localIdx < subsetCount) {
                    inf.boneIdx[k] = sw.subsetNodeIndices[localIdx];
                } else {
                    auto it = localPseudoToGlobal.find(localIdx);
                    if (it != localPseudoToGlobal.end()) {
                        inf.boneIdx[k] = it->second;
                    } else {
                        // Out-of-range index (malformed MDX or weight
                        // already zero with default-init lane). Point
                        // it at node 0 so it lands on the root bone's
                        // matrix; with weight 0 it contributes nothing.
                        inf.boneIdx[k] = 0;
                    }
                }
            }
        }
    }
    return result;
}

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
            mat.data[0] = {m[0], m[1], m[2], m[3]};
            mat.data[1] = {m[4], m[5], m[6], m[7]};
            mat.data[2] = {m[8], m[9], m[10], m[11]};
            mat.data[3] = {m[12], m[13], m[14], m[15]};
        }
        currentMatrices_.assign(nodeCount, Matrix44f::identity());
        offsetMatrices_.assign(nodeCount, Matrix44f::identity());
        nodesReady_ = false;
    }

    void SetGeosetLayout(i32 geosetId, GeosetPaletteLayout layout) {
        ensureOwnedData()->geosetLayouts[geosetId] = std::move(layout);
    }

    void SetGeosetWeights(i32 geosetId, i32 vertCount, const i32* nodeIndices, const f32* weights) {
        GeosetSkinInfo& info = ensureOwnedData()->geosetWeights[geosetId];
        info.vertices.resize(vertCount);
        for (i32 v = 0; v < vertCount; v++) {
            for (i32 j = 0; j < 4; j++) {
                info.vertices[v].boneIdx[j] = nodeIndices[v * 4 + j];
                info.vertices[v].weight[j] = weights[v * 4 + j];
            }
        }
    }

    std::shared_ptr<SkinningData> SharedData() const {
        return data_;
    }

    void UpdateNodeMatrices(i32 nodeCount, const f32* worldData) {
        if (!data_ || nodeCount != data_->nodeCount)
            return;
        for (i32 i = 0; i < nodeCount; i++) {
            const f32* m = worldData + i * 16;
            Matrix44f& mat = currentMatrices_[i];
            mat.data[0] = {m[0], m[1], m[2], m[3]};
            mat.data[1] = {m[4], m[5], m[6], m[7]};
            mat.data[2] = {m[8], m[9], m[10], m[11]};
            mat.data[3] = {m[12], m[13], m[14], m[15]};
        }
        matricesDirty_ = true;
        nodesReady_ = true;
    }

    bool HasSkeleton() const {
        return data_ && data_->nodeCount > 0;
    }
    bool IsReady() const {
        return nodesReady_;
    }
    i32 NodeCount() const {
        return data_ ? data_->nodeCount : 0;
    }
    bool HasWeights(i32 geosetId) const {
        return data_ && data_->geosetWeights.count(geosetId) > 0;
    }
    bool NeedsUpdate() const {
        return matricesDirty_;
    }

    // Path A (per-actor palette) accessors. UsesPerActorPalette()
    // returns the load-time decision; the CB handle is set by the
    // model loader once the actor's gfx resources are created.
    bool UsesPerActorPalette() const {
        return data_ && data_->usesPerActorPalette;
    }
    i32 ActorPaletteSize() const {
        return data_ ? data_->actorPaletteSize : 0;
    }
    const std::vector<GlobalGroupAverageRecord>& GlobalGroupAverages() const {
        static const std::vector<GlobalGroupAverageRecord> kEmpty;
        return data_ ? data_->globalGroupAverages : kEmpty;
    }
    gfx::BufferHandle ActorPaletteCb() const {
        return bonePaletteCb_;
    }
    void SetActorPaletteCb(gfx::BufferHandle cb) {
        bonePaletteCb_ = cb;
    }

    const GeosetSkinInfo* GetGeosetWeights(i32 geosetId) const {
        if (!data_)
            return nullptr;
        auto it = data_->geosetWeights.find(geosetId);
        return (it != data_->geosetWeights.end()) ? &it->second : nullptr;
    }

    const GeosetPaletteLayout* GetGeosetLayout(i32 geosetId) const {
        if (!data_)
            return nullptr;
        auto it = data_->geosetLayouts.find(geosetId);
        return (it != data_->geosetLayouts.end()) ? &it->second : nullptr;
    }

    i32 GeosetPaletteSize(i32 geosetId) const {
        auto* layout = GetGeosetLayout(geosetId);
        if (!layout)
            return 0;
        return (i32)layout->subsetNodeIndices.size() + (i32)layout->groupAverages.size();
    }

    const Matrix44f* OffsetMatrices() const {
        return offsetMatrices_.data();
    }

    i32 LocalSlotToNodeIndex(i32 geosetId, i32 localSlot) const {
        auto* layout = GetGeosetLayout(geosetId);
        if (!layout)
            return -1;
        if (localSlot < 0 || localSlot >= (i32)layout->subsetNodeIndices.size())
            return -1;
        return layout->subsetNodeIndices[localSlot];
    }

    void ComputeOffsetMatrices() {
        if (!data_ || !matricesDirty_)
            return;
        const i32 n = data_->nodeCount;
        const auto& inv = data_->inverseBindMatrices;
        for (i32 i = 0; i < n; i++) {

            offsetMatrices_[i] = inv[i] * currentMatrices_[i];
        }
        matricesDirty_ = false;
    }

    // Build the bone-palette staging buffer for one geoset.
    //
    // The palette is laid out as two regions, back to back:
    //   1. Subset bones        — one slot per direct node reference;
    //                            the slot holds that node's offset
    //                            matrix verbatim. Filled at slot index
    //                            i = position in `subsetNodeIndices`.
    //   2. Group-average bones — one slot per "pseudo bone" that
    //                            averages several nodes' offset
    //                            matrices into a single mean. Used by
    //                            geosets where multiple bones share a
    //                            single weight slot. Filled at slot
    //                            index = the record's pseudoSlot,
    //                            which is expected to land in
    //                            [subsetBoneCount, subsetBoneCount +
    //                            groupAverageCount).
    //
    // Returns the number of slots populated. The caller (BuildBonePalette)
    // reads only [0, returned). Trailing slots are left untouched, so
    // the shader must never index past the returned count.
    i32 ComputeGeosetPalette(i32 geosetId, Matrix44f* out, i32 capacity) const {
        const auto* layout = GetGeosetLayout(geosetId);
        if (!layout || !out || capacity <= 0 || !data_) {
            FillPaletteWithIdentity(out, capacity);
            return 0;
        }

        const i32 subsetBoneCount = static_cast<i32>(layout->subsetNodeIndices.size());
        const i32 groupAverageCount = static_cast<i32>(layout->groupAverages.size());
        const i32 populatedSlots = std::min(subsetBoneCount + groupAverageCount, capacity);

        FillSubsetBoneSlots(*layout, out, capacity);
        FillGroupAverageSlots(*layout, out, capacity);
        return populatedSlots;
    }

private:
    static void FillPaletteWithIdentity(Matrix44f* out, i32 capacity) {
        if (!out || capacity <= 0)
            return;
        for (i32 i = 0; i < capacity; ++i)
            out[i] = Matrix44f::identity();
    }

    // Each subset bone maps directly to one model node. An out-of-
    // range node index falls back to identity rather than crashing —
    // typically indicates a malformed MDX, but rendering an identity
    // bone is the least-broken behavior we can produce on the fly.
    void FillSubsetBoneSlots(const GeosetPaletteLayout& layout, Matrix44f* out,
                             i32 capacity) const {
        const i32 nodeCount = data_->nodeCount;
        const i32 subsetBoneCount = static_cast<i32>(layout.subsetNodeIndices.size());
        const i32 lastSlot = std::min(subsetBoneCount, capacity);
        for (i32 slot = 0; slot < lastSlot; ++slot) {
            const i32 nodeIndex = layout.subsetNodeIndices[slot];
            out[slot] =
                IsValidNodeIndex(nodeIndex) ? offsetMatrices_[nodeIndex] : Matrix44f::identity();
        }
    }

    void FillGroupAverageSlots(const GeosetPaletteLayout& layout, Matrix44f* out,
                               i32 capacity) const {
        for (const auto& group : layout.groupAverages) {
            const i32 slot = group.pseudoSlot;
            if (slot < 0 || slot >= capacity)
                continue;
            out[slot] = AverageOffsetMatrices(group.nodeIndices);
        }
    }

    // Arithmetic mean of N nodes' offset matrices — falls back to
    // identity for empty inputs or when every node index is out of
    // range (defensive; group records are authored upstream and
    // should reference live nodes).
    Matrix44f AverageOffsetMatrices(const std::vector<i32>& nodeIndices) const {
        Matrix44f sum = Matrix44f::zero();
        i32 contributorCount = 0;
        for (i32 nodeIndex : nodeIndices) {
            if (!IsValidNodeIndex(nodeIndex))
                continue;
            sum += offsetMatrices_[nodeIndex];
            ++contributorCount;
        }
        if (contributorCount == 0)
            return Matrix44f::identity();
        sum *= 1.0f / static_cast<f32>(contributorCount);
        return sum;
    }

    bool IsValidNodeIndex(i32 nodeIndex) const {
        return nodeIndex >= 0 && nodeIndex < data_->nodeCount;
    }

public:
    bool SkinVertices(i32 geosetId, const std::vector<Vertex>& baseVerts,
                      std::vector<Vertex>& outVerts) const {
        if (!data_)
            return false;
        auto it = data_->geosetWeights.find(geosetId);
        if (it == data_->geosetWeights.end())
            return false;
        const auto& skin = it->second;

        if (skin.vertices.size() != baseVerts.size())
            return false;

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
                if (w < 0.0001f)
                    continue;

                i32 nIdx = inf.boneIdx[j];
                if (nIdx < 0 || nIdx >= nodeCnt)
                    continue;

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
            outVerts[i].uv = baseVerts[i].uv;
            outVerts[i].color = baseVerts[i].color;
        }

        return true;
    }

private:
    SkinningData* ensureOwnedData() {
        if (!data_)
            data_ = std::make_shared<SkinningData>();
        return data_.get();
    }

    std::shared_ptr<SkinningData> data_;
    std::vector<Matrix44f> currentMatrices_;
    std::vector<Matrix44f> offsetMatrices_;
    bool matricesDirty_ = false;
    bool nodesReady_ = false;

    // Per-actor bone palette CB (Path A only). Allocated by the model
    // loader once per actor instance after SetSharedData; freed by the
    // owner (Actor) on destruction. Invalid on Path B actors.
    gfx::BufferHandle bonePaletteCb_ = gfx::BufferHandle::Invalid;
};

} // namespace whiteout::flakes::renderer::animation

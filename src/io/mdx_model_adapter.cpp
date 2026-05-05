#include "mdx_model_adapter.h"
#include "common_types.h"
#include "io/content_provider.h"
#include "team_glow_data.h"
#include "texture_image_usage.h"
#include "renderer/model_source_utils.h"
#include <cmath>
#include <cstdio>

namespace WhiteoutDex {

inline gfx::Format WhiteoutFormatToGfx(whiteout::textures::PixelFormat pf, bool srgb) {
    using PF = whiteout::textures::PixelFormat;
    switch (pf) {
        case PF::R8:      return gfx::Format::R8_UNORM;
        case PF::R16:     return gfx::Format::R16_UNORM;
        case PF::R32F:    return gfx::Format::R32_FLOAT;
        case PF::RG8:     return gfx::Format::R8G8_UNORM;
        case PF::RG16:    return gfx::Format::R16G16_UNORM;
        case PF::RG32F:   return gfx::Format::R32G32_FLOAT;
        case PF::RGBA8:   return srgb ? gfx::Format::R8G8B8A8_UNORM_SRGB
                                      : gfx::Format::R8G8B8A8_UNORM;
        case PF::RGBA16:  return gfx::Format::R16G16B16A16_UNORM;
        case PF::RGBA32F: return gfx::Format::R32G32B32A32_FLOAT;
        case PF::BC1:     return srgb ? gfx::Format::BC1_UNORM_SRGB : gfx::Format::BC1_UNORM;
        case PF::BC2:     return srgb ? gfx::Format::BC2_UNORM_SRGB : gfx::Format::BC2_UNORM;
        case PF::BC3:     return srgb ? gfx::Format::BC3_UNORM_SRGB : gfx::Format::BC3_UNORM;
        case PF::BC4:     return gfx::Format::BC4_UNORM;
        case PF::BC5:     return gfx::Format::BC5_UNORM;
        case PF::BC6H:    return gfx::Format::BC6H_UF16;
        case PF::BC7:     return srgb ? gfx::Format::BC7_UNORM_SRGB : gfx::Format::BC7_UNORM;
    }
    return gfx::Format::Unknown;
}

using namespace whiteout;
using namespace whiteout::mdx;
namespace fs = std::filesystem;

namespace {

inline void swizPos(Vector3f& v)   { v = {v.y, -v.x, v.z}; }
inline void swizScale(Vector3f& v) { v = {v.y,  v.x, v.z}; }
inline void swizQuat(Quaternion& q){ q = {q.y, -q.x, q.z, q.w}; }
inline void swizTangent(Vector4f& t){ t = {t.y, -t.x, t.z, t.w}; }

template <typename T, typename Fn>
void transformTrack(Track<T>& track, Fn fn) {
    if (!track.isUsed || track.keys_data.empty()) return;
    if (isSmoothInterpolation(track.interpolationType)) {
        for (auto& k : track.tangentKeys()) {
            fn(k.value);
            fn(k.inTan);
            fn(k.outTan);
        }
    } else {
        for (auto& k : track.keys()) {
            fn(k.value);
        }
    }
}

void transformNodeTracks(Node& n) {
    transformTrack(n.translationTracks, [](Vector3f& v){ swizPos(v); });
    transformTrack(n.rotationTracks,    [](Quaternion& q){ swizQuat(q); });
    transformTrack(n.scalingTracks,     [](Vector3f& v){ swizScale(v); });
}

void transformBindPose(std::array<f32, 12>& bp) {
    Vector3f r0{bp[0], bp[1], bp[2]};
    Vector3f r1{bp[3], bp[4], bp[5]};
    Vector3f r2{bp[6], bp[7], bp[8]};
    Vector3f t {bp[9], bp[10], bp[11]};

    bp[0] =  r1.y; bp[1] = -r1.x; bp[2] =  r1.z;
    bp[3] = -r0.y; bp[4] =  r0.x; bp[5] = -r0.z;
    bp[6] =  r2.y; bp[7] = -r2.x; bp[8] =  r2.z;
    swizPos(t);
    bp[9] = t.x; bp[10] = t.y; bp[11] = t.z;
}

void TransformMdxModelToMaxCoords(whiteout::mdx::Model& m) {

    for (auto& gs : m.geosets) {
        for (auto& v : gs.vertexPositions) swizPos(v);
        for (auto& v : gs.vertexNormals)   swizPos(v);
        for (auto& t : gs.tangents)        swizTangent(t);
        swizPos(gs.extent.minimum);
        swizPos(gs.extent.maximum);
        for (auto& ext : gs.sequenceExtents) {
            swizPos(ext.minimum);
            swizPos(ext.maximum);
        }
    }

    for (auto& p : m.pivotPoints) swizPos(p);

    for (auto& s : m.sequences) {
        swizPos(s.extent.minimum);
        swizPos(s.extent.maximum);
    }
    swizPos(m.modelExtent.minimum);
    swizPos(m.modelExtent.maximum);

    auto transformAll = [](auto& arr) {
        for (auto& x : arr) transformNodeTracks(x.node);
    };
    transformAll(m.bones);
    transformAll(m.helpers);
    transformAll(m.attachments);
    transformAll(m.lights);
    transformAll(m.particleEmitters);
    transformAll(m.particleEmitters2);
    transformAll(m.ribbonEmitters);
    transformAll(m.eventObjects);
    transformAll(m.cornEmitters);
    for (auto& cs : m.collisionShapes) {
        transformNodeTracks(cs.node);
        for (auto& v : cs.vertices) swizPos(v);
    }

    for (auto& c : m.cameras) {
        swizPos(c.position);
        swizPos(c.targetPosition);
        transformTrack(c.positionTracks,       [](Vector3f& v){ swizPos(v); });
        transformTrack(c.targetPositionTracks, [](Vector3f& v){ swizPos(v); });
    }

    for (auto& bp : m.bindPoses) transformBindPose(bp);

}

inline const Layer::SubTexture* FindDiffuseSubTexture(
    const std::vector<Layer::SubTexture>& subs) {
    if (subs.empty()) return nullptr;
    for (const auto& s : subs)
        if (s.slot == Layer::SlotType::DiffuseMap) return &s;
    return &subs[0];
}

}

MdxModelAdapter::MdxModelAdapter(whiteout::mdx::Model model, fs::path basePath,
                                 IContentProvider* contentProvider)
    : model_(std::move(model))
    , basePath_(std::move(basePath))
    , resolver_(basePath_)
    , contentProvider_(contentProvider) {

    if constexpr (kDefaultCoordSpace == CoordSpace::Max) {
        TransformMdxModelToMaxCoords(model_);
    }

    hierarchy_.Build(model_);

    const auto& nodes = hierarchy_.Nodes();
    boneGateGeoset_.assign(nodes.size(), -1);
    for (usize i = 0; i < nodes.size(); ++i) {
        if (nodes[i].source != HierarchyNode::Source::Bone) continue;
        const auto& bone = model_.bones[nodes[i].sourceIndex];
        if (bone.geosetId == whiteout::mdx::Bone::MULTIPLE_GEOSETS) continue;
        const whiteout::u32 gaId = bone.geosetAnimationId;
        if (gaId == whiteout::mdx::Bone::MULTIPLE_GEOSETS) continue;
        if (gaId >= model_.geosetAnimations.size()) continue;
        const whiteout::u32 targetGeoset = model_.geosetAnimations[gaId].geosetId;
        if (targetGeoset >= model_.geosets.size()) continue;
        boneGateGeoset_[i] = (i32)targetGeoset;
    }
}

std::vector<MeshData> MdxModelAdapter::GetMeshes() {
    std::vector<MeshData> result;
    result.reserve(model_.geosets.size());

    for (i32 i = 0; i < (i32)model_.geosets.size(); i++) {
        const auto& gs = model_.geosets[i];
        MeshData mesh;
        mesh.geosetId   = i;
        mesh.materialId = (i32)gs.materialId;
        mesh.lod        = gs.lod;

        i32 vc = (i32)gs.vertexPositions.size();
        mesh.positions.resize(vc);
        mesh.normals.resize(vc);
        mesh.uvs.resize(vc);

        const bool hasTangents = (i32)gs.tangents.size() == vc;
        if (hasTangents) mesh.tangents.resize(vc);

        const bool hasUv1 = gs.textureCoordinateSets.size() >= 2
                         && (i32)gs.textureCoordinateSets[1].size() == vc;
        if (hasUv1) mesh.uvs1.resize(vc);

        for (i32 v = 0; v < vc; v++) {
            mesh.positions[v] = gs.vertexPositions[v];
            if (v < (i32)gs.vertexNormals.size())
                mesh.normals[v] = gs.vertexNormals[v];
            if (!gs.textureCoordinateSets.empty() &&
                v < (i32)gs.textureCoordinateSets[0].size()) {
                mesh.uvs[v] = {gs.textureCoordinateSets[0][v].x,
                               gs.textureCoordinateSets[0][v].y};
            }
            if (hasUv1) {
                mesh.uvs1[v] = {gs.textureCoordinateSets[1][v].x,
                                gs.textureCoordinateSets[1][v].y};
            }
            if (hasTangents) mesh.tangents[v] = gs.tangents[v];
        }

        mesh.indices.assign(gs.faces.begin(), gs.faces.end());

        result.push_back(std::move(mesh));
    }
    return result;
}

TextureData MdxModelAdapter::LoadTextureFile(const std::string& path,
                                              i32 textureId,
                                              i32 replaceableId) const {
    TextureData td;
    td.textureId     = textureId;
    td.replaceableId = replaceableId;
    td.width = td.height = 0;

    td.sharedKey     = NormalizeTextureKey(path);

    auto applyResult = [&](whiteout::textures::Texture& tex) {
        gfx::Format gfxFmt = WhiteoutFormatToGfx(tex.format(), tex.isSrgb());
        if (gfxFmt == gfx::Format::Unknown) {
            tex.format(whiteout::textures::PixelFormat::RGBA8);
            gfxFmt = tex.isSrgb() ? gfx::Format::R8G8B8A8_UNORM_SRGB
                                  : gfx::Format::R8G8B8A8_UNORM;
        }

        gfxFmt = ApplyTextureSrgbPolicy(gfxFmt, path);
        td.width     = (i32)tex.width();
        td.height    = (i32)tex.height();
        td.format    = gfxFmt;
        td.mipLevels = (i32)tex.mipCount();

        usize total = 0;
        for (u32 m = 0; m < tex.mipCount(); ++m)
            total += tex.mipData(m).size();
        td.pixels.resize(total);
        u8* cursor = td.pixels.data();
        for (u32 m = 0; m < tex.mipCount(); ++m) {
            auto src = tex.mipData(m);
            std::memcpy(cursor, src.data(), src.size());
            cursor += src.size();
        }
    };

    auto tryParsePath = [&](const fs::path& p) -> bool {

        auto u8 = p.u8string();
        std::string pathStr(reinterpret_cast<const char*>(u8.data()), u8.size());
        auto result = DispatchTextureParser(ExtensionLower(p),
            [&](auto& parser) { return parser.parse(pathStr); });
        if (result) { applyResult(*result); return true; }
        return false;
    };

    auto tryParseBuffer = [&](std::span<const u8> buf, const std::string& ext) -> bool {
        auto result = DispatchTextureParser(ext,
            [&](auto& parser) { return parser.parse(buf); });
        if (result) { applyResult(*result); return true; }
        return false;
    };

    fs::path resolved = resolver_.ResolveTexture(path);
    if (!resolved.empty() && tryParsePath(resolved)) {
        return td;
    }

    if (contentProvider_) {
        std::string foundExt;
        auto data = contentProvider_->ReadFile(path, &foundExt);
        if (data) {
            if (foundExt.empty()) foundExt = ExtensionLower(fs::path(path));
            if (tryParseBuffer(*data, foundExt)) {
                return td;
            }
        }
    }

    auto u8base = resolver_.BasePath().u8string();
    std::fprintf(stderr,
                 "[textures] ERR: MDX texture[%d] NOT FOUND: '%s' (base: %s)\n",
                 textureId, path.c_str(),
                 reinterpret_cast<const char*>(u8base.data()));

    return td;
}

std::vector<TextureData> MdxModelAdapter::GetTextures() {
    std::vector<TextureData> result;
    result.reserve(model_.textures.size());

    for (i32 i = 0; i < (i32)model_.textures.size(); i++) {
        const auto& tex = model_.textures[i];
        TextureData td;
        if (tex.replaceableId != 0) {

            td.textureId     = i;
            td.replaceableId = (i32)tex.replaceableId;
            td.width = td.height = 0;
        } else if (!tex.fileName.empty()) {

            std::string sharedKey = NormalizeTextureKey(tex.fileName);
            if (IsTextureCached(sharedKey)) {
                td.textureId     = i;
                td.replaceableId = (i32)tex.replaceableId;
                td.width = td.height = 0;
                td.sharedKey = std::move(sharedKey);
            } else {
                td = LoadTextureFile(tex.fileName, i, (i32)tex.replaceableId);
            }
        } else {

            td.textureId     = i;
            td.replaceableId = (i32)tex.replaceableId;
            td.width = td.height = 4;
            td.pixels.assign(4 * 4 * 4, 255);
        }

        td.wrapFlags = static_cast<u32>(tex.flags) & 0x3;
        result.push_back(std::move(td));
    }
    return result;
}

i32 MdxModelAdapter::MapShadingFlags(Layer::ShadingFlag sf) const {
    using SF = Layer::ShadingFlag;
    const u32 s = (u32)sf;
    i32 flags = 0;
    if (hasFlag(s, SF::TwoSided))    flags |= MAT_TWO_SIDED;
    if (hasFlag(s, SF::Unshaded))    flags |= MAT_UNSHADED;
    if (hasFlag(s, SF::Unfogged))    flags |= MAT_UNFOGGED;
    if (hasFlag(s, SF::NoDepthTest)) flags |= MAT_NO_DEPTH_TEST;
    if (hasFlag(s, SF::NoDepthSet))  flags |= MAT_NO_DEPTH_SET;
    return flags;
}

std::vector<MaterialData> MdxModelAdapter::GetMaterials() {
    std::vector<MaterialData> result;
    result.reserve(model_.materials.size());

    for (i32 i = 0; i < (i32)model_.materials.size(); i++) {
        const auto& mat = model_.materials[i];
        MaterialData md;
        md.materialId    = i;
        md.priorityPlane = (i32)mat.priorityPlane;
        md.sortOrder     = 0;

        for (i32 li = 0; li < (i32)mat.layers.size(); ++li) {
            const auto& layer = mat.layers[li];
            MaterialLayerData ld;
            ld.filterMode = MapFilterMode((i32)layer.filterMode);
            ld.alpha      = layer.alpha;
            ld.flags      = MapShadingFlags(layer.shadingFlags);
            ld.textureAnimationId = ((i32)layer.textureAnimationId < 0
                                     || (i32)layer.textureAnimationId >= (i32)model_.textureAnimations.size())
                                    ? -1 : (i32)layer.textureAnimationId;

            if (hasFlag((u32)layer.shadingFlags, Layer::ShadingFlag::SphereEnvMap)) {
                ld.coordId = -1;
            } else {
                ld.coordId = static_cast<i32>(layer.coordId);
            }

            ld.shaderId   = static_cast<i32>(static_cast<whiteout::u32>(layer.shader));

            ld.emissiveGain    = layer.emissiveGain;
            ld.fresnelOpacity  = layer.fresnelOpacity;
            ld.fresnelTeamColor = layer.fresnelTeamColor;
            ld.fresnelColor    = { layer.fresnelColor.x,
                                   layer.fresnelColor.y,
                                   layer.fresnelColor.z };

            if (!layer.subTextures.empty()) {

                auto subAt = [&](usize pos) -> i32 {
                    return pos < layer.subTextures.size()
                               ? (i32)layer.subTextures[pos].textureId : -1;
                };
                ld.textureId      = subAt(0);
                ld.normalMapId    = subAt(1);
                ld.ormMapId       = subAt(2);
                ld.emissiveMapId  = subAt(3);

                if (layer.subTextures.size() > 4) {
                    const i32 tex = (i32)layer.subTextures[4].textureId;
                    if (tex >= 0 && tex < (i32)model_.textures.size()
                        && model_.textures[tex].replaceableId == 1) {
                        ld.teamColorMapId = kHdTeamColorActive;
                    } else {
                        ld.teamColorMapId = tex;
                    }
                }

            } else {
                ld.textureId = (i32)layer.textureId;
            }

            md.layers.push_back(ld);
        }
        result.push_back(std::move(md));
    }
    return result;
}

SkeletonData MdxModelAdapter::GetSkeleton() {
    SkeletonData sk;

    sk.nodeCount = hierarchy_.NodeCount();

    sk.inverseBindMatrices.assign(sk.nodeCount, Matrix44f::identity());

    sk.billboardFlags.assign(sk.nodeCount, 0);
    sk.nodePivots.assign(sk.nodeCount, Vector3f{0, 0, 0});
    sk.nodeParents.assign(sk.nodeCount, -1);
    const auto& nodes = hierarchy_.Nodes();
    using NF = whiteout::mdx::Node::NodeFlag;
    for (i32 i = 0; i < (i32)nodes.size(); i++) {
        const u32 nf = nodes[i].flags;
        sk.billboardFlags[i] = PackBillboardFlags(
            hasFlag(nf, NF::Billboarded),
            hasFlag(nf, NF::BillboardedLockX),
            hasFlag(nf, NF::BillboardedLockY),
            hasFlag(nf, NF::BillboardedLockZ),
            hasFlag(nf, NF::CameraAnchored));
        sk.nodePivots[i]  = nodes[i].pivot;
        sk.nodeParents[i] = nodes[i].parentIdx;
    }

    return sk;
}

std::vector<SkinWeightData> MdxModelAdapter::GetSkinWeights() {
    std::vector<SkinWeightData> result;
    result.reserve(model_.geosets.size());

    auto resolveBoneIdx = [&](i32 matsValue) -> i32 {
        i32 nodeIdx = hierarchy_.BoneIndexToNodeIndex(matsValue);
        if (nodeIdx < 0) nodeIdx = hierarchy_.ObjectIdToNodeIndex(matsValue);
        return (nodeIdx >= 0) ? nodeIdx : 0;
    };

    constexpr i32 kMaxPaletteSlots = 256;

    for (i32 gi = 0; gi < (i32)model_.geosets.size(); gi++) {
        const auto& gs = model_.geosets[gi];
        i32 vc = (i32)gs.vertexPositions.size();
        SkinWeightData sw;
        sw.geosetId = gi;
        sw.influences.resize(vc);

        std::vector<i32> subset;
        std::unordered_map<i32, i32> globalToLocal;
        auto addToSubset = [&](i32 globalIdx) -> i32 {
            auto it = globalToLocal.find(globalIdx);
            if (it != globalToLocal.end()) return it->second;
            i32 local = (i32)subset.size();
            subset.push_back(globalIdx);
            globalToLocal.emplace(globalIdx, local);
            return local;
        };

        if (!gs.skinData.empty()) {

            for (i32 v = 0; v < vc; v++) {
                i32 base = v * 8;
                if (base + 7 < (i32)gs.skinData.size()) {
                    for (i32 k = 0; k < 4; k++) {
                        u8 raw = gs.skinData[base + k];
                        i32 matsValue;
                        if (!gs.matrixIndices.empty() && raw < gs.matrixIndices.size())
                            matsValue = (i32)gs.matrixIndices[raw];
                        else
                            matsValue = (i32)raw;
                        i32 globalNode = resolveBoneIdx(matsValue);
                        i32 localSlot  = addToSubset(globalNode);
                        sw.influences[v].boneIdx[k] = localSlot;
                        sw.influences[v].weight[k]  = gs.skinData[base + 4 + k] / 255.0f;
                    }
                }
            }
        } else if (!gs.vertexGroups.empty() && !gs.matrixGroups.empty()) {

            std::vector<u32> groupStart(gs.matrixGroups.size() + 1, 0);
            for (i32 g = 0; g < (i32)gs.matrixGroups.size(); g++)
                groupStart[g + 1] = groupStart[g] + gs.matrixGroups[g];

            std::vector<i32> groupPseudoSlotGlobal(gs.matrixGroups.size(), -1);
            for (i32 g = 0; g < (i32)gs.matrixGroups.size(); g++) {
                u32 count = gs.matrixGroups[g];
                if (count > 4) continue;
                u32 start = groupStart[g];
                u32 clamp = count > 4 ? 4 : count;
                for (u32 k = 0; k < clamp && (start + k) < gs.matrixIndices.size(); k++) {
                    i32 globalNode = resolveBoneIdx((i32)gs.matrixIndices[start + k]);
                    addToSubset(globalNode);
                }
            }

            for (i32 g = 0; g < (i32)gs.matrixGroups.size(); g++) {
                u32 count = gs.matrixGroups[g];
                if (count <= 4) continue;
                i32 pseudoLocal = (i32)subset.size() + (i32)sw.groupAverages.size();
                if (pseudoLocal >= kMaxPaletteSlots) {
                    std::fprintf(stderr,
                        "[GetSkinWeights] geoset %d group %d (N=%u) exceeds per-geoset"
                        " palette cap (%d); falling back to 4-bone clamp\n",
                        gi, g, count, kMaxPaletteSlots);
                    continue;
                }
                GroupAverageRecord rec;
                rec.pseudoSlot = pseudoLocal;
                rec.nodeIndices.reserve(count);
                u32 start = groupStart[g];
                for (u32 k = 0; k < count && (start + k) < gs.matrixIndices.size(); k++) {
                    i32 nodeIdx = resolveBoneIdx((i32)gs.matrixIndices[start + k]);
                    rec.nodeIndices.push_back(nodeIdx);
                }
                groupPseudoSlotGlobal[g] = pseudoLocal;
                sw.groupAverages.push_back(std::move(rec));
            }

            for (i32 v = 0; v < vc && v < (i32)gs.vertexGroups.size(); v++) {
                i32 groupId = gs.vertexGroups[v];
                if (groupId >= (i32)gs.matrixGroups.size()) continue;

                i32 pseudoLocal = groupPseudoSlotGlobal[groupId];
                if (pseudoLocal >= 0) {
                    sw.influences[v].boneIdx[0] = pseudoLocal;
                    sw.influences[v].weight[0]  = 1.0f;
                } else {
                    u32 start = groupStart[groupId];
                    u32 count = gs.matrixGroups[groupId];
                    if (count > 4) count = 4;
                    f32 w = (count > 0) ? 1.0f / (f32)count : 0.0f;
                    for (u32 k = 0; k < count && (start + k) < gs.matrixIndices.size(); k++) {
                        i32 globalNode = resolveBoneIdx((i32)gs.matrixIndices[start + k]);
                        auto it = globalToLocal.find(globalNode);
                        i32 localSlot = (it != globalToLocal.end()) ? it->second
                                                                    : addToSubset(globalNode);
                        sw.influences[v].boneIdx[k] = localSlot;
                        sw.influences[v].weight[k]  = w;
                    }
                }
            }
        }

        const i32 paletteSlotsUsed = (i32)subset.size() + (i32)sw.groupAverages.size();
        if (paletteSlotsUsed > kMaxPaletteSlots) {
            std::fprintf(stderr,
                "[GetSkinWeights] geoset %d needs %d palette slots (>%d)\n",
                gi, paletteSlotsUsed, kMaxPaletteSlots);
        }

        sw.subsetNodeIndices = std::move(subset);
        result.push_back(std::move(sw));
    }
    return result;
}

i32 MdxModelAdapter::MapPE2FilterMode(u32 mdxMode) const {

    switch (mdxMode) {
        case 0: return FILTER_BLEND;
        case 1: return FILTER_ADDITIVE;
        case 2: return FILTER_MODULATE;
        case 3: return FILTER_MODULATE_2X;
        case 4: return FILTER_TRANSPARENT;
        default: return FILTER_BLEND;
    }
}

std::vector<ParticleEmitterConfig> MdxModelAdapter::GetParticleConfigs() {
    std::vector<ParticleEmitterConfig> result;
    result.reserve(model_.particleEmitters2.size());

    for (i32 i = 0; i < (i32)model_.particleEmitters2.size(); i++) {
        const auto& pe = model_.particleEmitters2[i];
        ParticleEmitterConfig cfg;
        cfg.textureId  = (i32)pe.textureId;
        cfg.filterMode = MapPE2FilterMode(pe.filterMode);
        cfg.rows       = (i32)pe.rows;
        cfg.cols       = (i32)pe.columns;
        cfg.lifeSpan   = pe.lifespan;
        cfg.squirt     = (pe.squirt != 0);

        cfg.startColor = {pe.segmentColor[0].x, pe.segmentColor[0].y, pe.segmentColor[0].z};
        cfg.midColor   = {pe.segmentColor[1].x, pe.segmentColor[1].y, pe.segmentColor[1].z};
        cfg.endColor   = {pe.segmentColor[2].x, pe.segmentColor[2].y, pe.segmentColor[2].z};

        cfg.startAlpha = (f32)pe.segmentAlpha[0];
        cfg.midAlpha   = (f32)pe.segmentAlpha[1];
        cfg.endAlpha   = (f32)pe.segmentAlpha[2];

        cfg.startScale = pe.segmentScaling[0];
        cfg.midScale   = pe.segmentScaling[1];
        cfg.endScale   = pe.segmentScaling[2];
        cfg.midTime    = pe.time;

        cfg.particleType = (i32)pe.headOrTail + 1;
        cfg.tailLength   = pe.tailLength;

        cfg.headLifeStart   = (i32)pe.headInterval[0];
        cfg.headLifeEnd     = (i32)pe.headInterval[1];
        cfg.headLifeRepeat  = (i32)pe.headInterval[2];
        cfg.headDecayStart  = (i32)pe.headDecayInterval[0];
        cfg.headDecayEnd    = (i32)pe.headDecayInterval[1];
        cfg.headDecayRepeat = (i32)pe.headDecayInterval[2];
        cfg.tailLifeStart   = (i32)pe.tailInterval[0];
        cfg.tailLifeEnd     = (i32)pe.tailInterval[1];
        cfg.tailLifeRepeat  = (i32)pe.tailInterval[2];
        cfg.tailDecayStart  = (i32)pe.tailDecayInterval[0];
        cfg.tailDecayEnd    = (i32)pe.tailDecayInterval[1];
        cfg.tailDecayRepeat = (i32)pe.tailDecayInterval[2];

        const u32 nf = (u32)pe.node.flags;
        using NF = Node::NodeFlag;
        cfg.modelSpace  = hasFlag(nf, NF::ModelSpace);
        cfg.xyQuad      = hasFlag(nf, NF::XYQuad);
        cfg.sortZ       = hasFlag(nf, NF::SortPrimitives);
        cfg.unshaded    = hasFlag(nf, NF::Unshaded);
        cfg.lineEmitter = hasFlag(nf, NF::LineEmitter);
        cfg.unfogged    = hasFlag(nf, NF::Unfogged);

        cfg.priorityPlane  = (i32)pe.priorityPlane;
        cfg.replaceableId  = (i32)pe.replaceableId;

        result.push_back(cfg);
    }
    return result;
}

namespace {

particle::FilterMode MapToServiceFilterMode(whiteout::u32 mdxMode) {

    switch (mdxMode) {
        case 0: return particle::FilterMode::Blend;
        case 1: return particle::FilterMode::Additive;
        case 2: return particle::FilterMode::Modulate;
        case 3: return particle::FilterMode::Modulate2X;
        case 4: return particle::FilterMode::AlphaKey;
        default: return particle::FilterMode::Blend;
    }
}

particle::ImVector MdxColorToImVector(const whiteout::Vector3f& rgb, whiteout::u8 alpha) {

    auto clamp8 = [](f32 v) -> u8 {
        if (v <= 0.0f) return 0;
        if (v >= 1.0f) return 255;
        return static_cast<u8>(v * 255.0f);
    };
    return { alpha, clamp8(rgb.x), clamp8(rgb.y), clamp8(rgb.z) };
}

}

std::vector<particle::PlaneEmitterInit> MdxModelAdapter::GetPlaneEmitterInits() const {
    std::vector<particle::PlaneEmitterInit> result;
    result.reserve(model_.particleEmitters2.size());

    using namespace whiteout::mdx;

    for (const auto& pe : model_.particleEmitters2) {
        particle::PlaneEmitterInit init;

        init.textureRows    = pe.rows;
        init.textureCols    = pe.columns;
        init.lifeSpan       = pe.lifespan;
        init.tailLength     = pe.tailLength;
        init.angularVelocity = 0.0f;
        init.priorityPlane  = static_cast<i32>(pe.priorityPlane);
        init.replaceableId  = static_cast<i32>(pe.replaceableId);

        init.hasHead = (pe.headOrTail != 1);
        init.hasTail = (pe.headOrTail != 0);

        const whiteout::u32 nf = static_cast<whiteout::u32>(pe.node.flags);
        using NF = Node::NodeFlag;
        init.modelSpace = hasFlag(nf, NF::ModelSpace);
        init.xyQuads    = hasFlag(nf, NF::XYQuad);
        init.sortZ      = hasFlag(nf, NF::SortPrimitives);
        init.longitude  = hasFlag(nf, NF::LineEmitter) ? 0.0f : 6.2831853071795864769f;

        init.material.textureId     = static_cast<i32>(pe.textureId);
        init.material.filterMode    = MapToServiceFilterMode(pe.filterMode);
        init.material.unshaded      = hasFlag(nf, NF::Unshaded);
        init.material.unfogged      = hasFlag(nf, NF::Unfogged);
        init.material.replaceableId = static_cast<i32>(pe.replaceableId);

        init.squirtAtStart = (pe.squirt != 0);

        const f32 midTime = pe.time * pe.lifespan;

        particle::ImVector startColor = MdxColorToImVector(pe.segmentColor[0], pe.segmentAlpha[0]);
        particle::ImVector midColor   = MdxColorToImVector(pe.segmentColor[1], pe.segmentAlpha[1]);
        particle::ImVector endColor   = MdxColorToImVector(pe.segmentColor[2], pe.segmentAlpha[2]);

        auto& k0 = init.keys[0];
        k0.endTime        = midTime;
        k0.startColor     = startColor;
        k0.endColor       = midColor;
        k0.startScale     = pe.segmentScaling[0];
        k0.endScale       = pe.segmentScaling[1];
        k0.headCellStart  = static_cast<i32>(pe.headInterval[0]);
        k0.headCellEnd    = static_cast<i32>(pe.headInterval[1]);
        k0.headCellRepeat = static_cast<i32>(pe.headInterval[2]);
        k0.tailCellStart  = static_cast<i32>(pe.tailInterval[0]);
        k0.tailCellEnd    = static_cast<i32>(pe.tailInterval[1]);
        k0.tailCellRepeat = static_cast<i32>(pe.tailInterval[2]);

        auto& k1 = init.keys[1];
        k1.endTime        = pe.lifespan;
        k1.startColor     = midColor;
        k1.endColor       = endColor;
        k1.startScale     = pe.segmentScaling[1];
        k1.endScale       = pe.segmentScaling[2];
        k1.headCellStart  = static_cast<i32>(pe.headDecayInterval[0]);
        k1.headCellEnd    = static_cast<i32>(pe.headDecayInterval[1]);
        k1.headCellRepeat = static_cast<i32>(pe.headDecayInterval[2]);
        k1.tailCellStart  = static_cast<i32>(pe.tailDecayInterval[0]);
        k1.tailCellEnd    = static_cast<i32>(pe.tailDecayInterval[1]);
        k1.tailCellRepeat = static_cast<i32>(pe.tailDecayInterval[2]);

        init.coordSpace = kDefaultCoordSpace;

        result.push_back(std::move(init));
    }

    return result;
}

std::vector<RibbonEmitterConfig> MdxModelAdapter::GetRibbonConfigs() {
    std::vector<RibbonEmitterConfig> result;
    result.reserve(model_.ribbonEmitters.size());

    for (i32 i = 0; i < (i32)model_.ribbonEmitters.size(); i++) {
        const auto& rb = model_.ribbonEmitters[i];
        RibbonEmitterConfig cfg;

        if (rb.materialId < (u32)model_.materials.size() &&
            !model_.materials[rb.materialId].layers.empty()) {
            const auto& mat   = model_.materials[rb.materialId];
            const auto& layer = mat.layers[0];

            if (const auto* diffuse = FindDiffuseSubTexture(layer.subTextures))
                cfg.textureId = (i32)diffuse->textureId;
            else
                cfg.textureId = (i32)layer.textureId;

            cfg.filterMode = MapFilterMode((i32)layer.filterMode);

            const u32 sf = (u32)layer.shadingFlags;
            cfg.unshaded = hasFlag(sf, Layer::ShadingFlag::Unshaded);
            cfg.twoSided = hasFlag(sf, Layer::ShadingFlag::TwoSided);

            cfg.priorityPlane = (i32)mat.priorityPlane;
        }

        cfg.twoSided = true;

        cfg.rows     = (i32)rb.rows;
        cfg.cols     = (i32)rb.columns;
        cfg.emission = (f32)rb.emissionRate;
        cfg.life     = rb.lifespan;
        cfg.gravity  = rb.gravity;

        result.push_back(cfg);
    }
    return result;
}

std::vector<CollisionShapeData> MdxModelAdapter::GetCollisionShapes() {
    std::vector<CollisionShapeData> result;
    result.reserve(model_.collisionShapes.size());

    for (const auto& cs : model_.collisionShapes) {
        CollisionShapeData cd;
        cd.type   = (i32)cs.type;
        cd.radius = cs.radius;
        if (cs.vertices.size() >= 1) cd.vertices[0] = cs.vertices[0];
        if (cs.vertices.size() >= 2) cd.vertices[1] = cs.vertices[1];

        if (cs.node.objectId < model_.pivotPoints.size())
            cd.pivot = model_.pivotPoints[cs.node.objectId];
        result.push_back(cd);
    }
    return result;
}

FrameState MdxModelAdapter::Evaluate(i32 sequenceIdx, i32 timeMs, i32 globalTimeMs,
                                     const Matrix44f& worldTransform,
                                     const Vector3f& cameraPos) const {

    i32 seqStart = 0, seqEnd = 0;
    if (sequenceIdx >= 0 && sequenceIdx < (i32)model_.sequences.size()) {
        seqStart = (i32)model_.sequences[sequenceIdx].intervalStart;
        seqEnd   = (i32)model_.sequences[sequenceIdx].intervalEnd;
    }

    FrameState fs;

    std::vector<Matrix44f> boneWorld, allNodes;
    hierarchy_.Evaluate(timeMs, seqStart, seqEnd,
                        model_.globalSequences, boneWorld, allNodes,
                        &cameraPos, globalTimeMs);

    fs.boneWorldMatrices = std::move(allNodes);

    auto effectiveTime = [&](u32 gsId) -> std::tuple<i32, i32, i32> {
        if (gsId != whiteout::mdx::Track<whiteout::f32>::kNoGlobalSequence && gsId < (u32)model_.globalSequences.size()) {
            u32 duration = model_.globalSequences[gsId];
            if (duration > 0) {
                i32 gsTime = (globalTimeMs >= 0) ? globalTimeMs : timeMs;
                i32 t = (i32)std::fmod((f32)gsTime, (f32)duration);
                return {t, 0, (i32)duration};
            } else {

                return {0, 0, 0x3FFFFFFF};
            }
        }
        return {timeMs, seqStart, seqEnd};
    };

    auto evalF32 = [&](const Track<f32>& tr, f32 def, bool forceNoInterp = false) {
        auto [t, s, e] = effectiveTime(tr.globalSequenceId);
        return EvaluateTrackF32(tr, t, s, e, def, forceNoInterp);
    };
    auto evalVec3 = [&](const Track<Vector3f>& tr, const Vector3f& def) {
        auto [t, s, e] = effectiveTime(tr.globalSequenceId);
        return EvaluateTrackVec3(tr, t, s, e, def);
    };
    auto evalQuat = [&](const Track<Quaternion>& tr, const Quaternion& def) {
        auto [t, s, e] = effectiveTime(tr.globalSequenceId);
        return EvaluateTrackQuat(tr, t, s, e, def);
    };
    auto evalU32 = [&](const Track<u32>& tr, u32 def) {
        auto [t, s, e] = effectiveTime(tr.globalSequenceId);
        return EvaluateTrackU32(tr, t, s, e, def);
    };

    auto nodeOf = [&](const Node& n) {
        return hierarchy_.ObjectIdToNodeIndex((i32)n.objectId);
    };

    i32 geosetCount = (i32)model_.geosets.size();
    fs.geosetAlphas.assign(geosetCount, 1.0f);
    fs.geosetColors.assign(geosetCount, Vector3f(1, 1, 1));

    for (const auto& ga : model_.geosetAnimations) {
        i32 gid = (i32)ga.geosetId;
        if (gid < 0 || gid >= geosetCount) continue;

        const f32 identity = ga.alphaTracks.isUsed ? 1.0f : ga.alpha;
        fs.geosetAlphas[gid] = evalF32(ga.alphaTracks, identity);

        if (ga.colorTracks.isUsed) {
            Vector3f c = evalVec3(ga.colorTracks, ga.color);
            fs.geosetColors[gid] = {c.z, c.y, c.x};
        } else {
            fs.geosetColors[gid] = ga.color;
        }
    }

    const auto& hierNodes = hierarchy_.Nodes();
    std::vector<u8> nodeVisible(hierNodes.size(), 1);
    for (usize ni = 0; ni < hierNodes.size(); ++ni) {
        u8 vis = (hierNodes[ni].parentIdx < 0)
                    ? u8{1}
                    : nodeVisible[hierNodes[ni].parentIdx];
        const i32 gate = boneGateGeoset_[ni];
        if (vis && gate >= 0 && fs.geosetAlphas[gate] <= 0.0f) vis = 0;
        nodeVisible[ni] = vis;
    }
    auto gateByBoneAncestors = [&](i32 nodeIdx) -> f32 {
        return (nodeIdx >= 0 && nodeIdx < (i32)nodeVisible.size() &&
                !nodeVisible[nodeIdx])
                   ? 0.0f
                   : 1.0f;
    };

    for (i32 mi = 0; mi < (i32)model_.materials.size(); mi++) {
        const auto& mat = model_.materials[mi];
        for (i32 li = 0; li < (i32)mat.layers.size(); li++) {
            const auto& layer = mat.layers[li];
            if (!layer.alphaTracks.isUsed) continue;
            FrameState::LayerAlphaState las;
            las.materialId = mi;
            las.layerIndex = li;
            las.alpha      = evalF32(layer.alphaTracks, layer.alpha);
            fs.layerAlphas.push_back(las);
        }
    }

    for (i32 mi = 0; mi < (i32)model_.materials.size(); mi++) {
        const auto& mat = model_.materials[mi];
        for (i32 li = 0; li < (i32)mat.layers.size(); li++) {
            const auto& layer = mat.layers[li];
            const bool anyAnim =
                layer.fresnelColorTracks.isUsed    ||
                layer.fresnelAlphaTracks.isUsed    ||
                layer.fresnelTeamColorTracks.isUsed||
                layer.emissiveGainTracks.isUsed;
            if (!anyAnim) continue;

            FrameState::LayerFresnelState lfs;
            lfs.materialId       = mi;
            lfs.layerIndex       = li;
            lfs.fresnelColor     = evalVec3(layer.fresnelColorTracks,     layer.fresnelColor);
            lfs.fresnelOpacity   = evalF32 (layer.fresnelAlphaTracks,     layer.fresnelOpacity);
            lfs.fresnelTeamColor = evalF32 (layer.fresnelTeamColorTracks, layer.fresnelTeamColor);
            lfs.emissiveGain     = evalF32 (layer.emissiveGainTracks,     layer.emissiveGain);
            fs.layerFresnels.push_back(lfs);
        }
    }

    for (i32 mi = 0; mi < (i32)model_.materials.size(); mi++) {
        const auto& mat = model_.materials[mi];
        for (i32 li = 0; li < (i32)mat.layers.size(); li++) {
            const auto& layer = mat.layers[li];

            if (layer.textureIdTracks.isUsed) {
                FrameState::LayerTextureIdState lts;
                lts.materialId = mi;
                lts.layerIndex = li;
                lts.slot       = FrameState::LayerTexSlot::Diffuse;
                lts.textureId  = (i32)evalU32(layer.textureIdTracks, layer.textureId);
                fs.layerTextureIds.push_back(lts);
                continue;
            }

            for (usize k = 0; k < layer.subTextures.size(); ++k) {
                const auto& sub = layer.subTextures[k];
                if (!sub.tracks.isUsed) continue;
                FrameState::LayerTexSlot slot;
                switch (k) {
                    case 0: slot = FrameState::LayerTexSlot::Diffuse;   break;
                    case 1: slot = FrameState::LayerTexSlot::Normal;    break;
                    case 2: slot = FrameState::LayerTexSlot::ORM;       break;
                    case 3: slot = FrameState::LayerTexSlot::Emissive;  break;
                    case 4: slot = FrameState::LayerTexSlot::TeamColor; break;
                    default: continue;
                }
                FrameState::LayerTextureIdState lts;
                lts.materialId = mi;
                lts.layerIndex = li;
                lts.slot       = slot;
                lts.textureId  = (i32)evalU32(sub.tracks, sub.textureId);
                fs.layerTextureIds.push_back(lts);
            }
        }
    }

    fs.particleStates.resize(model_.particleEmitters2.size());
    const auto& nodes = hierarchy_.Nodes();

    auto worldOf = [&](i32 nodeIdx) -> Matrix44f {
        if (nodeIdx < 0 || nodeIdx >= (i32)fs.boneWorldMatrices.size()) return Matrix44f::identity();
        const auto& piv = nodes[nodeIdx].pivot;
        Matrix44f pivotT = Matrix44f::translation({piv.x, piv.y, piv.z});
        return pivotT * fs.boneWorldMatrices[nodeIdx];
    };

    const Matrix44f kPE2SpawnFrameRotation = Matrix44f::rotation_z(
        1.5707963267948966f);

    for (i32 i = 0; i < (i32)model_.particleEmitters2.size(); i++) {
        const auto& pe = model_.particleEmitters2[i];
        auto& ps = fs.particleStates[i];
        const i32 nodeIdx = nodeOf(pe.node);

        ps.emitterId = i;

        ps.transform = kPE2SpawnFrameRotation * worldOf(nodeIdx) * worldTransform;

        const bool squirting = (pe.squirt != 0);
        ps.squirting    = squirting;
        ps.emissionRate = evalF32(pe.emissionRateTracks, pe.emissionRate, squirting);
        ps.speed        = evalF32(pe.speedTracks,     pe.speed);
        ps.variation    = evalF32(pe.variationTracks, pe.variation);

        ps.coneAngle    = evalF32(pe.latitudeTracks, pe.latitude) * (3.14159265358979323846f / 180.0f);
        ps.gravity      = evalF32(pe.gravityTracks, pe.gravity);
        ps.width        = evalF32(pe.widthTracks,   pe.width);
        ps.length       = evalF32(pe.lengthTracks,  pe.length);
        ps.visibility   = evalF32(pe.visibilityTracks, 1.0f) * gateByBoneAncestors(nodeIdx);
    }

    for (i32 i = 0; i < (i32)model_.attachments.size(); i++) {
        const auto& att = model_.attachments[i];
        const i32 nodeIdx = nodeOf(att.node);
        const f32 vis = evalF32(att.visibilityTracks, 1.0f) * gateByBoneAncestors(nodeIdx);
        fs.attachmentStates.push_back({i, worldOf(nodeIdx) * worldTransform, vis});
    }

    for (i32 i = 0; i < (i32)model_.particleEmitters.size(); i++) {
        const auto& pe = model_.particleEmitters[i];
        const i32 nodeIdx = nodeOf(pe.node);

        FrameState::PE1FrameState ps;
        ps.emitterId    = i;
        ps.transform    = worldOf(nodeIdx) * worldTransform;
        ps.emissionRate = evalF32(pe.emissionRateTracks, pe.emissionRate);
        ps.speed        = evalF32(pe.speedTracks,        pe.initialVelocity);
        ps.latitude     = evalF32(pe.latitudeTracks,     pe.latitude);
        ps.longitude    = evalF32(pe.longitudeTracks,    pe.longitude);
        ps.gravity      = evalF32(pe.gravityTracks,      pe.gravity);
        ps.visibility   = evalF32(pe.visibilityTracks,   1.0f) * gateByBoneAncestors(nodeIdx);
        fs.pe1States.push_back(ps);
    }

    fs.ribbonStates.resize(model_.ribbonEmitters.size());

    for (i32 i = 0; i < (i32)model_.ribbonEmitters.size(); i++) {
        const auto& rb = model_.ribbonEmitters[i];
        auto& rs = fs.ribbonStates[i];
        const i32 nodeIdx = nodeOf(rb.node);

        rs.emitterId  = i;
        rs.transform  = worldOf(nodeIdx) * worldTransform;
        rs.above      = evalF32(rb.heightAboveTracks, rb.heightAbove);
        rs.below      = evalF32(rb.heightBelowTracks, rb.heightBelow);
        rs.alpha      = evalF32(rb.alphaTracks,       rb.alpha);
        rs.visibility = evalF32(rb.visibilityTracks,  1.0f) * gateByBoneAncestors(nodeIdx);
        rs.slot       = (i32)evalU32(rb.textureSlotTracks, rb.textureSlot);

        if (rb.colorTracks.isUsed) {
            Vector3f c = evalVec3(rb.colorTracks, rb.color);
            rs.color = {c.z, c.y, c.x};
        } else {
            rs.color = rb.color;
        }
    }

    fs.lights.reserve(model_.lights.size());
    for (i32 i = 0; i < (i32)model_.lights.size(); ++i) {
        const auto& L = model_.lights[i];
        FrameState::LightState ls;
        ls.kind = (L.type == Light::LightType::Omni)        ? FrameState::LightKind::Omni :
                  (L.type == Light::LightType::Directional) ? FrameState::LightKind::Directional :
                                                              FrameState::LightKind::Ambient;

        const i32 lightNodeIdx = nodeOf(L.node);
        const f32 visibility = evalF32(L.visibilityTracks, 1.0f) *
                               gateByBoneAncestors(lightNodeIdx);

        ls.enabled = visibility > 0.0f;
        if (!ls.enabled) { fs.lights.push_back(ls); continue; }

        Vector3f color = L.color;
        if (L.colorTracks.isUsed) {
            Vector3f animated = evalVec3(L.colorTracks, L.color);
            color = {animated.z, animated.y, animated.x};
        }
        f32 inten = std::max(0.0f, evalF32(L.intensityTracks,        L.intensity));
        f32 ambI  = std::max(0.0f, evalF32(L.ambientIntensityTracks, L.ambientIntensity));
        ls.diffuse = { color.x * inten, color.y * inten, color.z * inten };
        ls.ambient = { ambI, ambI, ambI };

        Matrix44f world = worldOf(lightNodeIdx) * worldTransform;
        if (ls.kind == FrameState::LightKind::Directional) {
            ls.worldDir = whiteout::transform_normal(Vector3f{0, 0, -1}, world);
        } else {
            ls.worldPos = whiteout::transform_point(Vector3f{0, 0, 0}, world);
        }
        ls.attenStart = L.attenuationStart;
        ls.attenEnd   = L.attenuationEnd;
        fs.lights.push_back(ls);
    }

    fs.collisionTransforms.resize(model_.collisionShapes.size());
    for (i32 i = 0; i < (i32)model_.collisionShapes.size(); i++) {
        const i32 nodeIdx = nodeOf(model_.collisionShapes[i].node);
        fs.collisionTransforms[i] = (nodeIdx >= 0 && nodeIdx < (i32)fs.boneWorldMatrices.size())
                                     ? fs.boneWorldMatrices[nodeIdx] : Matrix44f::identity();
    }

    for (i32 i = 0; i < (i32)model_.textureAnimations.size(); i++) {
        const auto& ta = model_.textureAnimations[i];

        Vector3f   trans = evalVec3(ta.translationTracks, {0, 0, 0});
        Vector3f   scale = evalVec3(ta.scalingTracks,     {1, 1, 1});
        Quaternion rot   = evalQuat(ta.rotationTracks,    Quaternion(0, 0, 0, 1));

        const f32 ang = 2.0f * std::atan2(rot.z, rot.w);
        const f32 c = std::cos(ang), si = std::sin(ang);
        const f32 a =  scale.x * c;
        const f32 b = -scale.x * si;
        const f32 d =  scale.y * si;
        const f32 e =  scale.y * c;
        const f32 cc = 0.5f - (a * 0.5f + b * 0.5f) + trans.x;
        const f32 ff = 0.5f - (d * 0.5f + e * 0.5f) + trans.y;

        FrameState::TexAnimMatrix tam{};
        tam.textureAnimId = i;
        tam.row0[0] = a; tam.row0[1] = b; tam.row0[2] = 0.0f; tam.row0[3] = cc;
        tam.row1[0] = d; tam.row1[1] = e; tam.row1[2] = 0.0f; tam.row1[3] = ff;
        fs.texAnimMatrices.push_back(tam);

        for (i32 mi = 0; mi < (i32)model_.materials.size(); mi++) {
            for (i32 li = 0; li < (i32)model_.materials[mi].layers.size(); li++) {
                const auto& layer = model_.materials[mi].layers[li];
                if ((i32)layer.textureAnimationId == i) {
                    FrameState::TexAnimState tas;
                    tas.materialId = mi;
                    tas.layerIndex = li;
                    tas.uOff  = trans.x;
                    tas.vOff  = trans.y;
                    tas.uTile = scale.x;
                    tas.vTile = scale.y;
                    tas.rotation = ang;
                    fs.texAnims.push_back(tas);
                    break;
                }
            }
        }
    }

    return fs;
}

std::vector<AttachmentConfig> MdxModelAdapter::GetAttachmentConfigs() {
    std::vector<AttachmentConfig> result;
    for (const auto& att : model_.attachments) {
        AttachmentConfig cfg;
        cfg.attachmentId = (i32)att.attachmentId;
        cfg.modelPath = att.path;
        result.push_back(cfg);
    }
    return result;
}

std::vector<PE1EmitterConfig> MdxModelAdapter::GetPE1Configs() {
    std::vector<PE1EmitterConfig> result;
    for (const auto& pe : model_.particleEmitters) {
        if (pe.spawnModelFileName.empty()) continue;
        PE1EmitterConfig cfg;
        cfg.modelPath = pe.spawnModelFileName;
        cfg.lifespan  = pe.lifespan;
        cfg.scale     = 1.0f;
        result.push_back(cfg);
    }
    return result;
}

static EventObjectConfig::Kind DecodeEventKind(std::string_view name) {
    if (name.size() < 3) return EventObjectConfig::Kind::Unknown;
    auto eq = [&](const char* p) {
        return name[0] == p[0] && name[1] == p[1] && name[2] == p[2];
    };
    if (eq("SPN")) return EventObjectConfig::Kind::SPN;
    if (eq("SPL")) return EventObjectConfig::Kind::SPL;
    if (eq("UBR")) return EventObjectConfig::Kind::UBR;
    if (eq("FPT")) return EventObjectConfig::Kind::FPT;
    if (eq("SND")) return EventObjectConfig::Kind::SND;
    return EventObjectConfig::Kind::Unknown;
}

std::vector<EventObjectConfig> MdxModelAdapter::GetEventObjects() {
    std::vector<EventObjectConfig> result;
    result.reserve(model_.eventObjects.size());
    for (const auto& ev : model_.eventObjects) {
        EventObjectConfig cfg;
        cfg.name = ev.node.name;
        cfg.kind = DecodeEventKind(cfg.name);

        if (cfg.name.size() >= 4) {
            std::string_view tail{cfg.name.data() + 4, cfg.name.size() - 4};

            while (!tail.empty() && (tail.back() == '\0' || tail.back() == ' ')) tail.remove_suffix(1);
            cfg.id.assign(tail);
        }
        cfg.nodeIndex        = hierarchy_.ObjectIdToNodeIndex((i32)ev.node.objectId);
        if (ev.node.objectId < model_.pivotPoints.size())
            cfg.pivot = model_.pivotPoints[ev.node.objectId];
        cfg.globalSequenceId = ev.globalSequenceId;
        cfg.eventTrackTimes  = ev.eventTrackTimes;
        result.push_back(std::move(cfg));
    }
    return result;
}

std::vector<u32> MdxModelAdapter::GetGlobalSequences() {
    return std::vector<u32>(model_.globalSequences.begin(),
                            model_.globalSequences.end());
}

std::vector<SequenceInfo> MdxModelAdapter::GetSequences() const {
    std::vector<SequenceInfo> result;
    result.reserve(model_.sequences.size());
    for (const auto& seq : model_.sequences) {
        const bool nonLoop =
            (seq.flags & whiteout::mdx::Sequence::Flag::NonLooping)
                != whiteout::mdx::Sequence::Flag::None;
        result.push_back({seq.name, (i32)seq.intervalStart, (i32)seq.intervalEnd,
                          seq.moveSpeed, nonLoop});
    }
    return result;
}

std::vector<CameraPreset> MdxModelAdapter::GetCameraPresets() const {
    std::vector<CameraPreset> presets;
    for (const auto& cam : model_.cameras) {
        const auto& pos = cam.position;
        const auto& tgt = cam.targetPosition;

        f32 dx = pos.x - tgt.x, dy = pos.y - tgt.y, dz = pos.z - tgt.z;
        f32 dist = std::sqrt(dx*dx + dy*dy + dz*dz);
        if (dist < 0.01f) dist = 100.0f;
        f32 invD = 1.0f / dist;
        f32 pitch = std::asin(std::clamp(dz * invD, -1.0f, 1.0f));
        f32 yaw   = std::atan2(dy * invD, dx * invD);

        CameraPreset cp;
        cp.name        = std::wstring(cam.name.begin(), cam.name.end());
        cp.position    = pos;
        cp.target      = tgt;
        cp.fovDiagonal = cam.fieldOfView;
        cp.zNear       = cam.nearClippingPlane;
        cp.zFar        = cam.farClippingPlane;
        cp.staticRoll  = 0.0f;
        cp.pitch       = pitch;
        cp.yaw         = yaw;
        cp.distance    = dist;
        cp.isLive      = false;

        const bool animated = cam.positionTracks.isUsed
                           || cam.targetPositionTracks.isUsed
                           || cam.targetRotationTracks.isUsed;
        if (animated) {
            auto posTracks  = cam.positionTracks;
            auto tgtTracks  = cam.targetPositionTracks;
            auto rollTracks = cam.targetRotationTracks;
            Vector3f pivot       = pos;
            Vector3f targetPivot = tgt;
            cp.animator = [posTracks  = std::move(posTracks),
                           tgtTracks  = std::move(tgtTracks),
                           rollTracks = std::move(rollTracks),
                           pivot, targetPivot]
                (Vector3f& outPos, Vector3f& outTgt,
                 f32& outRoll, i32 timeMs,
                 i32 seqStart, i32 seqEnd) {
                const Vector3f zero{0.0f, 0.0f, 0.0f};
                Vector3f posDelta = EvaluateTrackVec3(posTracks, timeMs, seqStart, seqEnd, zero);
                Vector3f tgtDelta = EvaluateTrackVec3(tgtTracks, timeMs, seqStart, seqEnd, zero);
                outPos  = { pivot.x       + posDelta.x, pivot.y       + posDelta.y, pivot.z       + posDelta.z };
                outTgt  = { targetPivot.x + tgtDelta.x, targetPivot.y + tgtDelta.y, targetPivot.z + tgtDelta.z };
                outRoll = EvaluateTrackF32(rollTracks, timeMs, seqStart, seqEnd, 0.0f);
            };
        }

        presets.push_back(std::move(cp));
    }
    return presets;
}

}

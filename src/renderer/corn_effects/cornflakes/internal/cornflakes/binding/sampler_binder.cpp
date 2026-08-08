#include "binding_internal.hpp"

#include <cornflakes/diagnostics/issue_codes.hpp>
#include <cornflakes/sampler/curve_cdf.hpp>
#include <cornflakes/sampler/mesh.hpp>
#include <cornflakes/sampler/texture.hpp>
#include <cornflakes/sampler/turbulence.hpp>
#include <cornflakes/interface/asset/mesh_asset.hpp>
#include <cornflakes/interface/asset/mesh_provider.hpp>
#include <cornflakes/interface/asset/texture_provider.hpp>
#include <cornflakes/interface/asset/vector_field_asset.hpp>
#include <cornflakes/interface/asset/vector_field_provider.hpp>
#include <cornflakes/interface/binding/effect_binder.hpp>
#include <cornflakes/interface/binding/sampler_resource.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace whiteout::cornflakes {

namespace {

void readField3F(const AssetObject& obj, const char* name, std::array<f32, 3>& out) noexcept {
    const auto* f = findField(obj, name);
    if (f != nullptr && f->bytes.size() >= sizeof(f32) * 3U) {
        std::memcpy(out.data(), f->bytes.data(), sizeof(f32) * 3U);
    }
}

void samplerIssue(const BindResources& res, Severity severity, u32 code, std::string_view message) {
    if (res.issues == nullptr) {
        return;
    }
    Issue issue;
    issue.severity = severity;
    issue.category = Category::Sampler;
    issue.code = code;
    issue.message = message;
    res.issues->push(issue);
}

void buildCurveSampler(SamplerResource& res, const AssetObject& data, IArena& arena) {
    res.kind = SamplerKind::Curve;
    res.curve.times = fieldFloatArray(data, "Times");
    res.curve.values = fieldFloatArray(data, "FloatValues");
    res.curve.tangents = fieldFloatArray(data, "FloatTangents");

    const u32 valueType = fieldUint(data, "ValueType").value_or(0U);
    u8 components = 0;
    if (valueType >= 1U && valueType <= 4U) {
        components = static_cast<u8>(valueType);
    } else if (!res.curve.times.empty()) {
        const std::size_t derived = res.curve.values.size() / res.curve.times.size();
        if (derived >= 1U && derived <= 4U) {
            components = static_cast<u8>(derived);
        }
    }
    res.curve.components = components > 0 ? components : 1U;
    res.curve.interpolator = fieldUint(data, "Interpolator").value_or(0U);
    res.curve.looped = fieldBool(data, "IsLoopedCurve").value_or(false);
    res.curve.isProbabilityCurve = fieldBool(data, "IsProbabilityCurve").value_or(false);
    buildSamplerCurveCdf(res.curve, arena);
}

f32 halfToFloat(u16 h) noexcept {
    const u32 sign = static_cast<u32>(h & 0x8000U) << 16U;
    u32 exp = (h >> 10U) & 0x1FU;
    u32 mant = h & 0x3FFU;
    u32 bits = 0U;
    if (exp == 0U) {
        if (mant != 0U) {
            exp = 1U;
            while ((mant & 0x400U) == 0U) {
                mant <<= 1U;
                --exp;
            }
            mant &= 0x3FFU;
            bits = ((exp + 112U) << 23U) | (mant << 13U);
        }
    } else if (exp == 0x1FU) {
        bits = 0x7F800000U | (mant << 13U);
    } else {
        bits = ((exp + 112U) << 23U) | (mant << 13U);
    }
    bits |= sign;
    f32 out = 0.0F;
    std::memcpy(&out, &bits, sizeof(f32));
    return out;
}

f32 decodeScalar(const std::byte* p, MeshScalarType t, bool normalized) noexcept {
    switch (t) {
    case MeshScalarType::U8: {
        const auto v = static_cast<u8>(*p);
        return normalized ? static_cast<f32>(v) / 255.0F : static_cast<f32>(v);
    }
    case MeshScalarType::I8: {
        const auto v = static_cast<i8>(*p);
        return normalized ? std::max(-1.0F, static_cast<f32>(v) / 127.0F) : static_cast<f32>(v);
    }
    case MeshScalarType::U16: {
        u16 v = 0U;
        std::memcpy(&v, p, sizeof(u16));
        return normalized ? static_cast<f32>(v) / 65535.0F : static_cast<f32>(v);
    }
    case MeshScalarType::I16: {
        i16 v = 0;
        std::memcpy(&v, p, sizeof(i16));
        return normalized ? std::max(-1.0F, static_cast<f32>(v) / 32767.0F) : static_cast<f32>(v);
    }
    case MeshScalarType::U32: {
        u32 v = 0U;
        std::memcpy(&v, p, sizeof(u32));
        return static_cast<f32>(v);
    }
    case MeshScalarType::I32: {
        i32 v = 0;
        std::memcpy(&v, p, sizeof(i32));
        return static_cast<f32>(v);
    }
    case MeshScalarType::F16: {
        u16 v = 0U;
        std::memcpy(&v, p, sizeof(u16));
        return halfToFloat(v);
    }
    default: {
        f32 v = 0.0F;
        std::memcpy(&v, p, sizeof(f32));
        return v;
    }
    }
}

void decodeStream(const MeshVertexStream& s, u32 vertexCount, u32 comps, std::vector<f32>& out) {
    out.assign(static_cast<std::size_t>(vertexCount) * comps, 0.0F);
    const u32 fp = s.footprint();
    const u32 scalarSize = meshScalarSize(s.scalar);
    const u32 n = std::min<u32>(comps, s.components);
    for (u32 v = 0U; v < vertexCount; ++v) {
        for (u32 c = 0U; c < n; ++c) {
            const std::size_t off =
                static_cast<std::size_t>(v) * fp + static_cast<std::size_t>(c) * scalarSize;
            if (off + scalarSize > s.data.size()) {
                return;
            }
            out[static_cast<std::size_t>(v) * comps + c] =
                decodeScalar(s.data.data() + off, s.scalar, s.normalized);
        }
    }
}

const MeshVertexStream* channelStream(const MeshGeometry& g, u32 n, std::string_view base,
                                      std::string_view prefix) noexcept {
    if (n == 0U) {
        if (const auto* s = g.stream(base)) {
            return s;
        }
    }
    char suffix[12];
    const int len = std::snprintf(suffix, sizeof(suffix), "%u", n);
    if (len <= 0) {
        return nullptr;
    }
    for (const auto& s : g.streams) {
        if (s.semantic.size() == prefix.size() + static_cast<std::size_t>(len) &&
            s.semantic.compare(0, prefix.size(), prefix) == 0 &&
            s.semantic.compare(prefix.size(), static_cast<std::size_t>(len), suffix) == 0) {
            return &s;
        }
    }
    return nullptr;
}

u32 channelStreamCount(const MeshGeometry& g, std::string_view base,
                       std::string_view prefix) noexcept {
    u32 n = 0U;
    while (channelStream(g, n, base, prefix) != nullptr) {
        ++n;
    }
    return n;
}

void deltaDecode(std::span<f32> values, u32 vertexCount, u32 comps,
                 MeshScalarType scalar) noexcept {
    if (vertexCount < 2U || scalar == MeshScalarType::F16) {
        return;
    }
    const auto wrap = [scalar](f32 v) -> f32 {
        switch (scalar) {
        case MeshScalarType::U8:
            return static_cast<f32>(static_cast<u8>(static_cast<i64>(v)));
        case MeshScalarType::I8:
            return static_cast<f32>(static_cast<i8>(static_cast<i64>(v)));
        case MeshScalarType::U16:
            return static_cast<f32>(static_cast<u16>(static_cast<i64>(v)));
        case MeshScalarType::I16:
            return static_cast<f32>(static_cast<i16>(static_cast<i64>(v)));
        case MeshScalarType::U32:
            return static_cast<f32>(static_cast<u32>(static_cast<i64>(v)));
        case MeshScalarType::I32:
            return static_cast<f32>(static_cast<i32>(static_cast<i64>(v)));
        default:
            return v;
        }
    };
    for (u32 v = 1U; v < vertexCount; ++v) {
        for (u32 c = 0U; c < comps; ++c) {
            const std::size_t cur = static_cast<std::size_t>(v) * comps + c;
            const std::size_t prev = cur - comps;
            if (cur < values.size()) {
                values[cur] = wrap(values[cur] + values[prev]);
            }
        }
    }
}

void decodeMeshStream(const MeshVertexStream& s, const MeshGeometry& g, u32 comps,
                      std::vector<f32>& out) {
    decodeStream(s, g.vertexCount, comps, out);
    if (g.deltaEncoded) {
        deltaDecode(out, g.vertexCount, comps, s.scalar);
    }
}

std::span<const f32> publishFloats(const std::vector<f32>& src, IArena& arena) {
    if (src.empty()) {
        return {};
    }
    const auto dst = arenaArray<f32>(arena, src.size());
    std::copy(src.begin(), src.end(), dst.begin());
    return std::span<const f32>{dst.data(), dst.size()};
}

const MeshShapeData* prepareMeshShape(SamplerShape& shape, IArena& arena,
                                      const BindResources& res) {
    if (shape.meshResource.empty()) {
        samplerIssue(res, Severity::Warning, issues::sampler::kMeshResourceEmpty,
                     "shape sampler: ShapeType = Mesh but MeshResource is empty");
        return nullptr;
    }
    if (res.meshes == nullptr) {
        samplerIssue(res, Severity::Warning, issues::sampler::kMeshNoProvider,
                     "shape sampler: ShapeType = Mesh but no IMeshResourceProvider was installed");
        return nullptr;
    }
    const auto bytes = res.meshes->readMesh(shape.meshResource);
    if (bytes.empty()) {
        samplerIssue(res, Severity::Warning, issues::sampler::kMeshNotFound,
                     "shape sampler: MeshResource could not be resolved");
        return nullptr;
    }
    IssueBag sink;
    const auto asset = readPkmm(bytes, arena, res.issues != nullptr ? *res.issues : sink);
    if (!asset) {
        samplerIssue(res, Severity::Warning, issues::sampler::kMeshUnparsable,
                     "shape sampler: MeshResource is not a loadable .pkmm");
        return nullptr;
    }
    if (shape.subMeshIndex >= asset->geometries.size()) {
        samplerIssue(res, Severity::Warning, issues::sampler::kMeshSubMeshOob,
                     "shape sampler: SubMeshIndex is out of range for the referenced mesh");
        return nullptr;
    }
    const MeshGeometry& g = asset->geometries[shape.subMeshIndex];
    const MeshVertexStream* posStream = g.stream("Position");
    if (posStream == nullptr || g.vertexCount == 0U) {
        samplerIssue(res, Severity::Warning, issues::sampler::kMeshNoPositions,
                     "shape sampler: mesh sub-mesh has no Position stream, nothing to sample");
        return nullptr;
    }
    std::vector<f32> positions;
    decodeMeshStream(*posStream, g, 3U, positions);
    if (posStream->scalar == MeshScalarType::U16 && posStream->normalized && g.hasBBox) {
        for (u32 v = 0U; v < g.vertexCount; ++v) {
            for (u32 c = 0U; c < 3U; ++c) {
                f32& p = positions[static_cast<std::size_t>(v) * 3U + c];
                p = g.bboxMin[c] + p * (g.bboxMax[c] - g.bboxMin[c]);
            }
        }
    }

    std::vector<f32> normals;
    if (const auto* s = g.stream("Normal")) {
        decodeMeshStream(*s, g, 3U, normals);
    }
    std::vector<f32> tangents;
    if (const auto* s = g.stream("Tangent")) {
        decodeMeshStream(*s, g, 4U, tangents);
    }
    std::vector<std::vector<f32>> texcoords(channelStreamCount(g, "Texcoord", "UvStream"));
    for (u32 n = 0U; n < texcoords.size(); ++n) {
        decodeMeshStream(*channelStream(g, n, "Texcoord", "UvStream"), g, 2U, texcoords[n]);
    }
    std::vector<std::vector<f32>> colors(channelStreamCount(g, "Color", "ColorStream"));
    for (u32 n = 0U; n < colors.size(); ++n) {
        decodeMeshStream(*channelStream(g, n, "Color", "ColorStream"), g, 4U, colors[n]);
    }

    const auto map = meshTransposeMap(asset->frameAxes(), meshFrameAxes(kEngineCoordinateFrame));
    const bool mirrors = meshTransposeMirrors(map);
    const auto rotateInPlace = [&](std::vector<f32>& stream) {
        for (std::size_t i = 0; i + 3U <= stream.size(); i += 3U) {
            const auto v = meshTranspose(map, {stream[i], stream[i + 1U], stream[i + 2U]});
            stream[i] = v[0];
            stream[i + 1U] = v[1];
            stream[i + 2U] = v[2];
        }
    };
    const std::vector<f32> positionsFile = positions;
    rotateInPlace(positions);
    rotateInPlace(normals);

    std::vector<u32> indices;
    if (g.primitive == MeshPrimitive::Triangles) {
        indices.reserve(g.indexCount);
        for (u32 i = 0U; i + 3U <= g.indexCount; i += 3U) {
            indices.push_back(g.index(i));
            indices.push_back(g.index(i + 1U));
            indices.push_back(g.index(i + 2U));
        }
    } else if (g.primitive == MeshPrimitive::TriangleStrips && g.indexCount >= 3U) {
        indices.reserve(static_cast<std::size_t>(g.indexCount - 2U) * 3U);
        for (u32 i = 0U; i + 3U <= g.indexCount; ++i) {
            indices.push_back(g.index(i));
            indices.push_back(g.index(i + 1U));
            indices.push_back(g.index(i + 2U));
        }
    } else {
        samplerIssue(res, Severity::Warning, issues::sampler::kMeshUnsupportedFormat,
                     "shape sampler: mesh sub-mesh is a line list, which has no surface to sample");
        return nullptr;
    }
    const std::vector<u32> indicesFile = indices;
    if (mirrors) {
        for (std::size_t t = 0; t + 3U <= indices.size(); t += 3U) {
            std::swap(indices[t + 1U], indices[t + 2U]);
        }
    }
    if (indices.empty()) {
        samplerIssue(res, Severity::Warning, issues::sampler::kMeshNoPositions,
                     "shape sampler: mesh sub-mesh has no triangles");
        return nullptr;
    }

    shape.meshHasTetrahedra = g.tetrahedra.valid() && !g.volumeSamplingPdf.empty();

    auto* out = arenaNew<MeshShapeData>(arena);
    out->vertexCount = g.vertexCount;
    out->triangleCount = static_cast<u32>(indices.size() / 3U);
    out->positions = publishFloats(positions, arena);
    out->normals = publishFloats(normals, arena);
    out->tangents = publishFloats(tangents, arena);
    out->defaultUvStream = shape.defaultUvStream;
    out->defaultColorStream = shape.defaultColorStream;
    const auto publishStreamSet = [&](const std::vector<std::vector<f32>>& src) {
        if (src.empty()) {
            return std::span<const std::span<const f32>>{};
        }
        const auto arr = arenaArray<std::span<const f32>>(arena, src.size());
        for (std::size_t i = 0; i < src.size(); ++i) {
            arr[i] = publishFloats(src[i], arena);
        }
        return std::span<const std::span<const f32>>{arr.data(), arr.size()};
    };
    out->texcoordStreams = publishStreamSet(texcoords);
    out->colorStreams = publishStreamSet(colors);
    {
        const auto idxArr = arenaArray<u32>(arena, indices.size());
        std::copy(indices.begin(), indices.end(), idxArr.begin());
        out->indices = std::span<const u32>{idxArr.data(), idxArr.size()};
    }

    {
        f64 total = 0.0;
        for (u32 t = 0U; t < out->triangleCount; ++t) {
            total += 0.5 * static_cast<f64>(meshTriangleAreaCoeff(*out, t));
        }
        out->surfaceArea = static_cast<f32>(total);
    }

    if (g.kdTree.valid()) {
        const auto cfg = meshProjConfigFor(g.kdTree.bboxMin, g.kdTree.bboxMax);
        if (cfg.maxIter != 0U) {
            const auto tris = arenaArray<MeshProjTriangle>(arena, out->triangleCount);
            buildMeshProjTriangles(std::span<const f32>{positionsFile.data(), positionsFile.size()},
                                   std::span<const u32>{indicesFile.data(), indicesFile.size()},
                                   std::span<MeshProjTriangle>{tris.data(), tris.size()});
            out->projTriangles = std::span<const MeshProjTriangle>{tris.data(), tris.size()};
            out->kdNodes = g.kdTree.nodes;
            out->kdLeaves = g.kdTree.leaves;
            out->kdBboxMin = g.kdTree.bboxMin;
            out->kdBboxMax = g.kdTree.bboxMax;
            out->projConfig = cfg;
            out->projWindingFlipped = mirrors;
            for (std::size_t d = 0; d < 3U; ++d) {
                const auto& m = map[d];
                if (m.source < 3U) {
                    out->engineToFile[m.source] = MeshAxisRemap{static_cast<u8>(d), m.sign};
                }
            }
        }
    }

    {
        const auto nodes = arenaArray<MeshBvhNode>(arena, meshBvhNodeBudget(out->triangleCount));
        const auto order = arenaArray<u32>(arena, out->triangleCount);
        const u32 nodeCount = buildMeshBvh(*out, std::span<MeshBvhNode>{nodes.data(), nodes.size()},
                                           std::span<u32>{order.data(), order.size()});
        if (nodeCount > 0U) {
            out->bvh = std::span<const MeshBvhNode>{nodes.data(), nodeCount};
            out->bvhTriangles = std::span<const u32>{order.data(), order.size()};
        }
    }

    const auto mode = static_cast<MeshSamplingDistribution>(shape.meshSamplingMode);
    if (mode == MeshSamplingDistribution::Uniform && !g.surfaceSamplingPdf.empty() &&
        g.surfaceSamplingPdf.slots.size() == out->triangleCount) {
        const auto slotArr = arenaArray<MeshAliasSlot>(arena, out->triangleCount);
        for (u32 i = 0U; i < out->triangleCount; ++i) {
            slotArr[i].proba = g.surfaceSamplingPdf.slots[i].proba;
            slotArr[i].other = g.surfaceSamplingPdf.slots[i].other;
        }
        out->alias = std::span<const MeshAliasSlot>{slotArr.data(), slotArr.size()};
        out->aliasFromAsset = true;
        return out;
    }
    if (mode != MeshSamplingDistribution::Fast) {
        std::vector<f32> vertexDensity;
        if (mode == MeshSamplingDistribution::Weighted) {
            const auto* dens = channelStream(g, shape.densityColorStream, "Color", "ColorStream");
            if (dens == nullptr) {
                samplerIssue(res, Severity::Warning, issues::sampler::kMeshNoDensityStream,
                             "shape sampler: MeshSamplingMode = Weighted but the sub-mesh has no "
                             "colour stream to weight by; falling back to area-weighted sampling");
            } else {
                std::vector<f32> rgba;
                decodeMeshStream(*dens, g, 4U, rgba);
                const u32 ch = std::min<u32>(shape.densityChannel, 3U);
                vertexDensity.resize(g.vertexCount);
                for (u32 v = 0U; v < g.vertexCount; ++v) {
                    vertexDensity[v] = rgba[static_cast<std::size_t>(v) * 4U + ch];
                }
            }
        }
        std::vector<f32> densities(out->triangleCount, 0.0F);
        buildMeshTriangleDensities(*out, vertexDensity, densities);
        std::vector<MeshAliasSlot> slots(out->triangleCount);
        if (buildMeshAliasTable(densities, slots)) {
            const auto slotArr = arenaArray<MeshAliasSlot>(arena, slots.size());
            std::copy(slots.begin(), slots.end(), slotArr.begin());
            out->alias = std::span<const MeshAliasSlot>{slotArr.data(), slotArr.size()};
        }
    }
    return out;
}

void buildShapeSampler(SamplerResource& res, const AssetObject& data) {
    res.kind = SamplerKind::Shape;
    res.shape.type = static_cast<ShapeType>(fieldInt(data, "ShapeType").value_or(0));
    res.shape.dimensionality =
        static_cast<SampleDimensionality>(fieldUint(data, "SampleDimensionality").value_or(2U));
    res.shape.radius = fieldFloat(data, "Radius").value_or(1.0F);
    res.shape.innerRadius = fieldFloat(data, "InnerRadius").value_or(0.0F);
    res.shape.height = fieldFloat(data, "Height").value_or(0.0F);
    res.shape.hemisphere = fieldBool(data, "Hemisphere").value_or(false);
    res.shape.transformTranslate = fieldBool(data, "TransformTranslate").value_or(true);
    res.shape.transformRotate = fieldBool(data, "TransformRotate").value_or(true);

    readField3F(data, "BoxDimensions", res.shape.boxDimensions);
    readField3F(data, "Position", res.shape.position);
    readField3F(data, "EulerOrientation", res.shape.eulerOrientation);
    readField3F(data, "NonUniformScale", res.shape.nonUniformScale);

    res.shape.meshResource = fieldString(data, "MeshResource");
    readField3F(data, "MeshScale", res.shape.meshScale);
    res.shape.meshSamplingMode = fieldUint(data, "MeshSamplingMode").value_or(1U);
    res.shape.subMeshIndex = fieldUint(data, "SubMeshIndex").value_or(0U);
    res.shape.defaultUvStream = fieldUint(data, "DefaultUvStream").value_or(0U);
    res.shape.defaultColorStream = fieldUint(data, "DefaultColorStream").value_or(0U);
    res.shape.densityColorStream = fieldUint(data, "DensityColorStream").value_or(0U);
    res.shape.densityChannel = fieldUint(data, "DensityChannel").value_or(0U);

    constexpr f32 kDegToRad = 0.01745329252F;
    res.shape.eulerOrientation[0] *= kDegToRad;
    res.shape.eulerOrientation[1] *= kDegToRad;
    res.shape.eulerOrientation[2] *= kDegToRad;

    const f32 posY = res.shape.position[1];
    res.shape.position[1] = -res.shape.position[2];
    res.shape.position[2] = posY;
    std::swap(res.shape.nonUniformScale[1], res.shape.nonUniformScale[2]);
    std::swap(res.shape.meshScale[1], res.shape.meshScale[2]);
    std::swap(res.shape.boxDimensions[1], res.shape.boxDimensions[2]);
}

void buildEventStreamSampler(SamplerResource& res, const AssetObject& data) {
    res.kind = SamplerKind::EventStream;
    res.eventStream.times = fieldFloatArray(data, "Times");
}

void buildTurbulenceSampler(SamplerResource& res, const AssetObject& data, IArena& arena) {
    res.kind = SamplerKind::Turbulence;
    SamplerTurbulence& tb = res.turbulence;
    tb.dataSource = (fieldUint(data, "DataSource").value_or(0U) == 1U)
                        ? TurbulenceDataSource::External
                        : TurbulenceDataSource::Procedural;
    tb.vectorField.resource = fieldString(data, "VectorFieldResource");
    tb.strength = fieldFloat(data, "Strength").value_or(0.1F);
    tb.wavelength = fieldFloat(data, "Wavelength").value_or(0.5F);
    tb.globalScale = fieldFloat(data, "GlobalScale").value_or(1.0F);
    tb.lacunarity = fieldFloat(data, "Lacunarity").value_or(0.5F);
    tb.gain = fieldFloat(data, "Gain").value_or(0.5F);
    tb.gainMultiplier = fieldFloat(data, "GainMultiplier").value_or(1.0F);
    tb.octaves = fieldUint(data, "Octaves").value_or(2U);
    tb.interpolator = static_cast<u32>(fieldInt(data, "Interpolator").value_or(1));
    tb.seed = fieldUint(data, "InitialSeed").value_or(1114229502U);
    tb.timeScale = fieldFloat(data, "TimeScale").value_or(0.0F);
    tb.timeBase = fieldFloat(data, "TimeBase").value_or(0.0F);
    tb.timeRandomVariation = fieldFloat(data, "TimeRandomVariation").value_or(0.5F);

    if (tb.dataSource == TurbulenceDataSource::External) {
        return;
    }

    const auto basis = arenaArray<f32>(arena, kTurbulenceGradientCount);
    const auto spin = arenaArray<f32>(arena, kTurbulenceGradientCount);
    const auto grads = arenaArray<f32>(arena, kTurbulenceGradientCount);
    generateTurbulenceBasis(tb.seed, tb.timeRandomVariation,
                            std::span<f32>{basis.data(), basis.size()},
                            std::span<f32>{spin.data(), spin.size()});
    rotateTurbulenceBasis(std::span<const f32>{basis.data(), basis.size()},
                          std::span<const f32>{spin.data(), spin.size()}, 0.0F,
                          std::span<f32>{grads.data(), grads.size()});
    tb.rigidBasis = std::span<const f32>{basis.data(), basis.size()};
    tb.spinRate = std::span<const f32>{spin.data(), spin.size()};
    tb.gradients = std::span<const f32>{grads.data(), grads.size()};
}

void readField4F(const AssetObject& obj, const char* name, std::array<f32, 4>& out) noexcept {
    const auto* f = findField(obj, name);
    if (f != nullptr && f->bytes.size() >= sizeof(f32) * 4U) {
        std::memcpy(out.data(), f->bytes.data(), sizeof(f32) * 4U);
    }
}

bool isPowerOfTwo(u32 v) noexcept { return v != 0U && (v & (v - 1U)) == 0U; }

u8 log2u32(u32 v) noexcept {
    u8 n = 0;
    while ((1U << n) < v) {
        ++n;
    }
    return n;
}

void prepareVectorField(SamplerTurbulence& tb, const AssetObject& data, const BindResources& res) {
    SamplerVectorField& vf = tb.vectorField;

    const i32 filtering = std::clamp(fieldInt(data, "Filtering").value_or(1), 0, 2);
    vf.interpolation = static_cast<VectorFieldInterpolation>(static_cast<u32>(filtering));

    if (vf.resource.empty()) {
        samplerIssue(res, Severity::Warning, issues::sampler::kVectorFieldResourceEmpty,
                     "turbulence sampler: DataSource is External but VectorFieldResource is empty");
        return;
    }
    if (res.vectorFields == nullptr) {
        samplerIssue(res, Severity::Warning, issues::sampler::kVectorFieldNoProvider,
                     "turbulence sampler: no IVectorFieldProvider was installed");
        return;
    }
    const auto bytes = res.vectorFields->readVectorField(vf.resource);
    if (bytes.empty()) {
        samplerIssue(res, Severity::Warning, issues::sampler::kVectorFieldNotFound,
                     "turbulence sampler: VectorFieldResource could not be resolved");
        return;
    }
    const auto asset = parsePkvf(bytes);
    if (!asset) {
        samplerIssue(res, Severity::Warning, issues::sampler::kVectorFieldUnparsable,
                     "turbulence sampler: VectorFieldResource is not a valid .pkvf");
        return;
    }
    if (asset->coordinateFrame != kCoordinateFrameRightHandZUp) {
        samplerIssue(res, Severity::Warning, issues::sampler::kVectorFieldForeignFrame,
                     "turbulence sampler: .pkvf was baked in a non-Z-up frame; "
                     "coordinate conversion is not applied");
    }

    if (!isPowerOfTwo(asset->dimensions[0]) || !isPowerOfTwo(asset->dimensions[1]) ||
        !isPowerOfTwo(asset->dimensions[2])) {
        samplerIssue(res, Severity::Warning, issues::sampler::kVectorFieldUnsupportedGrid,
                     "turbulence sampler: .pkvf dimensions are not powers of two");
        return;
    }
    if (!std::isfinite(tb.globalScale) || tb.globalScale == 0.0F) {
        samplerIssue(res, Severity::Warning, issues::sampler::kVectorFieldUnsupportedGrid,
                     "turbulence sampler: GlobalScale is zero, the vector field cannot be mapped");
        return;
    }

    vf.data = asset->data;
    vf.dimensions = asset->dimensions;
    vf.dataType = asset->dataType;
    vf.strideL2[0] = log2u32(asset->dimensions[0]);
    vf.strideL2[1] = vf.strideL2[0] + log2u32(asset->dimensions[1]);
    vf.strideL2[2] = vf.strideL2[1] + log2u32(asset->dimensions[2]);

    if (asset->dimensions[3] <= 1U && vf.interpolation == VectorFieldInterpolation::Quadrilinear) {
        vf.interpolation = VectorFieldInterpolation::Trilinear;
    }

    {
        const f32 timeExtent = asset->boundsMax[3] - asset->boundsMin[3];
        vf.timeScale = (timeExtent != 0.0F) ? (1.0F / timeExtent) : 0.0F;
    }
    vf.wrapTime = fieldBool(data, "WrapTime").value_or(true);

    vf.vecScale = tb.globalScale * asset->intensityMultiplier * tb.strength;

    for (std::size_t i = 0; i < 3U; ++i) {
        const f32 extent = asset->boundsMax[i] - asset->boundsMin[i];
        const f32 scale = (extent != 0.0F) ? (1.0F / extent) : 0.0F;
        const auto dim = static_cast<f32>(asset->dimensions[i]);
        vf.gridScale[i] = dim * scale / tb.globalScale;
        vf.gridOffset[i] = -asset->boundsMin[i] * scale * dim;
    }

    const bool wrap[3] = {
        fieldBool(data, "WrapSide").value_or(true),
        fieldBool(data, "WrapDepth").value_or(true),
        fieldBool(data, "WrapVertical").value_or(true),
    };
    for (std::size_t i = 0; i < 3U; ++i) {
        vf.addrMask[i] = wrap[i] ? static_cast<i32>(asset->dimensions[i] - 1U) : -1;
    }

    vf.valid = true;
}

f32 srgbToLinear(f32 c) noexcept {
    return (c <= 0.04045F) ? (c / 12.92F) : std::pow((c + 0.055F) / 1.055F, 2.4F);
}

f32 linearToSrgb(f32 c) noexcept {
    return (c <= 0.0031308F) ? (c * 12.92F) : ((1.055F * std::pow(c, 1.0F / 2.4F)) - 0.055F);
}

const TextureImageData* prepareTextureImage(SamplerTexture& tex, IArena& arena,
                                            const BindResources& res) {
    if (tex.textureResource.empty()) {
        samplerIssue(res, Severity::Warning, issues::sampler::kTextureResourceEmpty,
                     "texture sampler: TextureResource is empty");
        return nullptr;
    }
    if (res.textures == nullptr) {
        samplerIssue(res, Severity::Warning, issues::sampler::kTextureNoProvider,
                     "texture sampler: no ITextureResourceProvider was installed");
        return nullptr;
    }
    const TextureImageDesc desc = res.textures->readTexture(tex.textureResource);
    if (desc.texels.empty() || desc.width == 0U || desc.height == 0U) {
        samplerIssue(res, Severity::Warning, issues::sampler::kTextureNotFound,
                     "texture sampler: TextureResource could not be resolved");
        return nullptr;
    }
    TextureImageData srcImg;
    srcImg.texels = desc.texels;
    srcImg.width = desc.width;
    srcImg.height = desc.height;
    switch (desc.format) {
    case TextureSourceFormat::Bgra8:
        srcImg.format = TexelFormat::Bgra8;
        break;
    case TextureSourceFormat::Rgba8:
        srcImg.format = TexelFormat::Rgba8;
        break;
    case TextureSourceFormat::Fp32Rgba:
    case TextureSourceFormat::Fp16Rgba:
        srcImg.format = TexelFormat::Fp32Rgba;
        break;
    case TextureSourceFormat::Bgra4:
        srcImg.format = TexelFormat::Bgra4;
        break;
    case TextureSourceFormat::Dxt1:
        srcImg.format = TexelFormat::Dxt1;
        break;
    }
    const std::size_t need = (desc.format == TextureSourceFormat::Fp16Rgba)
                                 ? (static_cast<std::size_t>(desc.width) * desc.height * 8U)
                                 : textureStorageBytes(srcImg);
    if (desc.texels.size() < need) {
        samplerIssue(res, Severity::Warning, issues::sampler::kTextureBadDimensions,
                     "texture sampler: texel buffer is smaller than the format requires");
        return nullptr;
    }
    if (!tex.atlasDefinition.empty()) {
        samplerIssue(res, Severity::Warning, issues::sampler::kTextureAtlasUnsupported,
                     "texture sampler: AtlasDefinition is set but atlas sampling is "
                     "not implemented; sampling the whole image instead");
    }

    const std::size_t count = static_cast<std::size_t>(desc.width) * desc.height;

    std::span<const f32> widened;
    if (desc.format == TextureSourceFormat::Fp16Rgba) {
        const auto dst = arenaArray<f32>(arena, count * 4U);
        for (std::size_t i = 0; i < count * 4U; ++i) {
            u16 h = 0;
            std::memcpy(&h, desc.texels.data() + (i * 2U), sizeof(h));
            dst[i] = halfToFloat(h);
        }
        widened = std::span<const f32>{dst.data(), dst.size()};
        srcImg.texels = std::as_bytes(widened);
    }

    auto* img = arenaNew<TextureImageData>(arena);
    if (img == nullptr) {
        return nullptr;
    }
    img->width = desc.width;
    img->height = desc.height;
    img->powerOfTwo = isPowerOfTwo(desc.width) && isPowerOfTwo(desc.height);
    img->log2Width = log2u32(desc.width);

    const auto texelAt = [&](std::size_t i) noexcept {
        const auto x = static_cast<u32>(i % desc.width);
        const auto y = static_cast<u32>(i / desc.width);
        const Float4 c = fetchTextureTexel(srcImg, x, y);
        return std::array<f32, 4>{c.x, c.y, c.z, c.w};
    };

    if (tex.scriptOutputType == 1U) {
        const auto dst = arenaArray<f32>(arena, count);
        for (std::size_t i = 0; i < count; ++i) {
            const auto t = texelAt(i);
            dst[i] = tex.sampleRawValues
                         ? t[0]
                         : ((t[0] * 0.212671F) + (t[1] * 0.71516F) + (t[2] * 0.072169F));
        }
        img->format = TexelFormat::Fp32Lum;
        img->texels = std::as_bytes(std::span<const f32>{dst.data(), dst.size()});
        return img;
    }

    const bool dstGamma =
        tex.sampleRawValues ? desc.srgb
                            : (tex.gammaSpace == TextureGammaSpace::SRGB ||
                               tex.gammaSpace == TextureGammaSpace::LinearToSRGB);
    const bool compressed =
        srcImg.format == TexelFormat::Dxt1 || srcImg.format == TexelFormat::Bgra4;
    if (dstGamma == desc.srgb || compressed) {
        img->format = srcImg.format;
        img->texels = srcImg.texels;
        return img;
    }

    if (srcImg.format == TexelFormat::Fp32Rgba) {
        const auto dst = arenaArray<f32>(arena, count * 4U);
        for (std::size_t i = 0; i < count; ++i) {
            auto t = texelAt(i);
            for (std::size_t c = 0; c < 3U; ++c) {
                t[c] = dstGamma ? linearToSrgb(t[c]) : srgbToLinear(t[c]);
            }
            std::memcpy(dst.data() + (i * 4U), t.data(), sizeof(f32) * 4U);
        }
        img->format = TexelFormat::Fp32Rgba;
        img->texels = std::as_bytes(std::span<const f32>{dst.data(), dst.size()});
        return img;
    }

    const auto dst = arenaArray<u8>(arena, count * 4U);
    for (std::size_t i = 0; i < count; ++i) {
        auto t = texelAt(i);
        for (std::size_t c = 0; c < 3U; ++c) {
            t[c] = dstGamma ? linearToSrgb(t[c]) : srgbToLinear(t[c]);
        }
        const auto q = [](f32 v) noexcept -> u8 {
            const f32 s = (v * 255.0F) + 0.5F;
            return static_cast<u8>(s < 0.0F ? 0.0F : (s > 255.0F ? 255.0F : s));
        };
        dst[(i * 4U) + 0U] = q(t[2]);
        dst[(i * 4U) + 1U] = q(t[1]);
        dst[(i * 4U) + 2U] = q(t[0]);
        dst[(i * 4U) + 3U] = q(t[3]);
    }
    img->format = TexelFormat::Bgra8;
    img->texels = std::as_bytes(std::span<const u8>{dst.data(), dst.size()});
    return img;
}

void buildTextureSampler(SamplerResource& res, const AssetObject& data) {
    res.kind = SamplerKind::Texture;
    SamplerTexture& tex = res.texture;
    tex.textureResource = fieldString(data, "TextureResource");
    tex.atlasDefinition = fieldString(data, "AtlasDefinition");
    tex.scriptOutputType = fieldUint(data, "ScriptOutputType").value_or(4U);
    tex.sampleRawValues = fieldBool(data, "SampleRawValues").value_or(true);
    tex.gammaSpace =
        static_cast<TextureGammaSpace>(fieldUint(data, "SampleGammaSpace").value_or(0U));
    tex.densityPower = fieldFloat(data, "DensityPower").value_or(1.0F);
    tex.densitySrc = static_cast<TextureDensitySrc>(fieldUint(data, "DensitySrc").value_or(4U));
    readField4F(data, "DensityRGBAWeights", tex.densityRgbaWeights);
    if (tex.scriptOutputType != 1U && tex.scriptOutputType != 4U) {
        tex.scriptOutputType = 4U;
    }
}

}

void loadSamplers(const EffectAssetModel& model, const AssetObject& layerCache, LayerProgram& lp,
                  IArena& arena, const BindResources& bindRes) {
    const auto samplerUids = fieldLinks(layerCache, "Samplers");
    if (samplerUids.empty()) {
        return;
    }
    const auto samplerArr = arenaArray<SamplerResource>(arena, samplerUids.size());
    std::size_t written = 0;

    for (const u32 sUid : samplerUids) {
        const AssetObject* sObj = findObjectByUid(model, sUid);
        if (sObj == nullptr || sObj->type != "CLayerCompileCacheSampler") {
            continue;
        }
        SamplerResource& res = samplerArr[written++];
        res.name = stableCopy(fieldString(*sObj, "SamplerName"), arena);

        const auto dataUid = fieldLink(*sObj, "Sampler");
        if (!dataUid) {
            continue;
        }
        const AssetObject* data = findObjectByUid(model, *dataUid);
        if (data == nullptr) {
            continue;
        }
        if (data->type == "CParticleNodeSamplerData_Curve" || data->type == "CSamplerCurve") {
            buildCurveSampler(res, *data, arena);
        } else if (data->type == "CParticleNodeSamplerData_Shape") {
            buildShapeSampler(res, *data);
            if (res.shape.type == ShapeType::Mesh) {
                res.shape.meshResource = stableCopy(res.shape.meshResource, arena);
                res.shape.mesh = prepareMeshShape(res.shape, arena, bindRes);
            }
        } else if (data->type == "CParticleNodeSamplerData_EventStream") {
            buildEventStreamSampler(res, *data);
        } else if (data->type == "CParticleNodeSamplerData_Turbulence") {
            buildTurbulenceSampler(res, *data, arena);
            if (res.turbulence.dataSource == TurbulenceDataSource::External) {
                res.turbulence.vectorField.resource =
                    stableCopy(res.turbulence.vectorField.resource, arena);
                prepareVectorField(res.turbulence, *data, bindRes);
            }
        } else if (data->type == "CParticleNodeSamplerData_Texture") {
            buildTextureSampler(res, *data);
            res.texture.textureResource = stableCopy(res.texture.textureResource, arena);
            res.texture.atlasDefinition = stableCopy(res.texture.atlasDefinition, arena);
            res.texture.image = prepareTextureImage(res.texture, arena, bindRes);
        }
    }
    if (written > 0) {
        lp.samplers = std::span<const SamplerResource>{samplerArr.data(), written};
        bumpSamplerBindGeneration();
    }
}

}

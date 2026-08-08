
#include <cornflakes/interface/asset/mesh_asset.hpp>

#include <cornflakes/diagnostics/issue_codes.hpp>

#include <algorithm>
#include <cstring>
#include <vector>

namespace whiteout::cornflakes {

namespace {

constexpr u8 kChunkEof = 0U;
constexpr u8 kChunkGeometry = 1U;
constexpr u8 kChunkSkeleton = 4U;
constexpr u8 kChunkExtendedInfos = 8U;
constexpr u8 kSubVStream = 2U;
constexpr u8 kSubKdTree = 6U;
constexpr u8 kSubTetra = 7U;
constexpr u8 kSubAccelSurface = 9U;
constexpr u8 kSubAccelVolume = 0xAU;

namespace axis {
constexpr u8 kRight = 1U;
constexpr u8 kUp = 3U;
constexpr u8 kBackward = 4U;
constexpr u8 kForward = 5U;
}

constexpr std::array<std::array<u8, 3>, 6> kFrameAxes{{
    {axis::kRight, axis::kUp, axis::kBackward},
    {axis::kRight, axis::kForward, axis::kUp},
    {axis::kRight, axis::kUp, axis::kForward},
    {axis::kRight, axis::kBackward, axis::kUp},
    {axis::kRight, axis::kUp, axis::kBackward},
    {axis::kRight, axis::kUp, axis::kBackward},
}};

void fail(IssueBag& issues, u32 code, std::string_view message) {
    Issue issue;
    issue.severity = Severity::Error;
    issue.category = Category::Asset;
    issue.code = code;
    issue.message = message;
    issues.push(issue);
}

class Cursor {
public:
    explicit Cursor(std::span<const std::byte> bytes) noexcept : m_bytes(bytes) {}

    bool bad() const noexcept {
        return m_bad;
    }
    std::size_t pos() const noexcept {
        return m_pos;
    }

    bool has(std::size_t n) noexcept {
        if (m_pos + n > m_bytes.size()) {
            m_bad = true;
            return false;
        }
        return true;
    }

    u8 u8v() noexcept {
        if (!has(1U)) {
            return 0U;
        }
        return static_cast<u8>(m_bytes[m_pos++]);
    }

    u32 u32v() noexcept {
        if (!has(4U)) {
            return 0U;
        }
        u32 v = 0U;
        std::memcpy(&v, m_bytes.data() + m_pos, sizeof(u32));
        m_pos += 4U;
        return v;
    }

    f32 f32v() noexcept {
        const u32 bits = u32v();
        f32 v = 0.0F;
        std::memcpy(&v, &bits, sizeof(f32));
        return v;
    }

    std::span<const std::byte> raw(std::size_t n) noexcept {
        if (!has(n)) {
            return {};
        }
        const auto out = m_bytes.subspan(m_pos, n);
        m_pos += n;
        return out;
    }

    void skip(std::size_t n) noexcept {
        if (has(n)) {
            m_pos += n;
        }
    }

    void seek(std::size_t p) noexcept {
        if (p > m_bytes.size()) {
            m_bad = true;
            return;
        }
        m_pos = p;
    }

    std::string_view pstring() noexcept {
        const u8 b0 = u8v();
        static constexpr std::array<u32, 8> kExtra{0U, 0U, 0U, 0U, 1U, 1U, 2U, 3U};
        const u32 extra = kExtra[static_cast<std::size_t>(b0 >> 5U)];
        std::array<u8, 4> buf{};
        buf[0] = static_cast<u8>(b0 & static_cast<u8>(0xFFU >> extra));
        for (u32 i = 0U; i < extra; ++i) {
            buf[i + 1U] = u8v();
        }
        const u32 be = (static_cast<u32>(buf[0]) << 24U) | (static_cast<u32>(buf[1]) << 16U) |
                       (static_cast<u32>(buf[2]) << 8U) | static_cast<u32>(buf[3]);
        const u32 len = be >> (24U - 8U * extra);
        const auto data = raw(len);
        if (data.empty()) {
            return {};
        }
        return std::string_view{reinterpret_cast<const char*>(data.data()), data.size()};
    }

    std::pair<u8, std::size_t> chunkHeader() noexcept {
        const u8 key = u8v();
        skip(3U);
        return {key, static_cast<std::size_t>(u32v())};
    }

private:
    std::span<const std::byte> m_bytes;
    std::size_t m_pos = 0U;
    bool m_bad = false;
};

std::string_view stringAt(std::span<const std::string_view> table, u32 index) noexcept {
    return index < table.size() ? table[index] : std::string_view{};
}

bool readVStream(Cursor& c, std::span<const std::string_view> strings, MeshGeometry& g,
                 std::vector<MeshVertexStream>& outStreams, IssueBag& issues) {
    const u32 declaredSize = c.u32v();
    const u32 vertexCount = c.u32v();
    const u32 streamCount = c.u32v();
    const u8 flags = c.u8v();
    c.skip(3U);

    g.simd = (flags & 0x1U) != 0U;
    g.quantized = (flags & 0x2U) != 0U;
    g.deltaEncoded = (flags & 0x4U) != 0U;
    if (g.quantized) {
        const auto bits = c.raw(3U);
        if (bits.size() == 3U) {
            for (std::size_t i = 0; i < 3U; ++i) {
                g.quantBits[i] = static_cast<u8>(bits[i]);
            }
        }
        c.skip(1U);
        for (auto& e : g.quantExtents) {
            e = c.f32v();
        }
    }

    std::vector<u32> codes;
    codes.reserve(streamCount);
    for (u32 i = 0U; i < streamCount; ++i) {
        codes.push_back(c.u32v());
    }
    if (c.bad()) {
        fail(issues, issues::asset::kPkmmTruncated, "pkmm: truncated vertex-stream declaration");
        return false;
    }

    const std::size_t body = c.pos();
    g.vertexCount = vertexCount;
    for (const u32 code : codes) {
        const u32 elem = code & 0x1FU;
        MeshVertexStream s;
        s.semantic = stringAt(strings, code >> 8U);
        s.scalar = static_cast<MeshScalarType>((elem >> 2U) & 7U);
        s.components = static_cast<u8>((elem & 3U) + 1U);
        s.normalized = (code & 0x40U) != 0U;
        s.stride16 = (code & 0x80U) != 0U;
        s.data = c.raw(static_cast<std::size_t>(vertexCount) * s.footprint());
        outStreams.push_back(s);
    }
    if (c.bad()) {
        fail(issues, issues::asset::kPkmmTruncated, "pkmm: truncated vertex-stream data");
        return false;
    }
    if (c.pos() - body != declaredSize) {
        fail(issues, issues::asset::kPkmmVStreamSize,
             "pkmm: vertex data length disagrees with the stream header");
        return false;
    }
    return true;
}

std::size_t fastlzDecompress(std::span<const std::byte> in, std::span<std::byte> out) noexcept {
    if (in.empty()) {
        return 0U;
    }
    const auto* ip = reinterpret_cast<const u8*>(in.data());
    const u8* const ipEnd = ip + in.size();
    auto* op = reinterpret_cast<u8*>(out.data());
    u8* const opBegin = op;
    u8* const opEnd = op + out.size();

    const u32 level = static_cast<u32>(*ip >> 5U) + 1U;
    if (level != 1U && level != 2U) {
        return 0U;
    }
    constexpr u32 kMaxDistance = 8191U;

    u32 ctrl = static_cast<u32>(*ip++) & 31U;
    bool loop = true;
    while (loop) {
        if (ctrl >= 32U) {
            u32 len = (ctrl >> 5U) - 1U;
            u32 ofs = (ctrl & 31U) << 8U;
            const u8* ref = op - ofs;

            if (len == 6U) {
                if (level == 1U) {
                    if (ip >= ipEnd) {
                        return 0U;
                    }
                    len += *ip++;
                } else {
                    u8 code = 255U;
                    while (code == 255U) {
                        if (ip >= ipEnd) {
                            return 0U;
                        }
                        code = *ip++;
                        len += code;
                    }
                }
            }
            if (ip >= ipEnd) {
                return 0U;
            }
            const u8 code = *ip++;
            ref -= code;
            if (level == 2U && code == 255U && ofs == (31U << 8U)) {
                if (ip + 1 >= ipEnd) {
                    return 0U;
                }
                ofs = static_cast<u32>(*ip++) << 8U;
                ofs += *ip++;
                ref = op - ofs - kMaxDistance;
            }

            if (op + len + 3U > opEnd || ref - 1 < opBegin) {
                return 0U;
            }
            if (ip < ipEnd) {
                ctrl = *ip++;
            } else {
                loop = false;
            }

            if (ref == op) {
                const u8 b = ref[-1];
                *op++ = b;
                *op++ = b;
                *op++ = b;
                for (; len != 0U; --len) {
                    *op++ = b;
                }
            } else {
                --ref;
                *op++ = *ref++;
                *op++ = *ref++;
                *op++ = *ref++;
                for (; len != 0U; --len) {
                    *op++ = *ref++;
                }
            }
        } else {
            ++ctrl;
            if (op + ctrl > opEnd || ip + ctrl > ipEnd) {
                return 0U;
            }
            for (; ctrl != 0U; --ctrl) {
                *op++ = *ip++;
            }
            loop = ip < ipEnd;
            if (loop) {
                ctrl = *ip++;
            }
        }
    }
    return static_cast<std::size_t>(op - opBegin);
}

bool decompressCompressorBuffer(std::span<const std::byte> buf, std::vector<std::byte>& out,
                                u32 expectedSize, u8& outType) noexcept {
    if (buf.size() < 5U) {
        return false;
    }
    u32 originalSize = 0U;
    std::memcpy(&originalSize, buf.data(), sizeof(u32));
    const auto type = static_cast<u8>(buf[4]);
    outType = type;
    if (originalSize != expectedSize) {
        return false;
    }
    out.assign(originalSize, std::byte{});
    const auto payload = buf.subspan(5U);
    if (type == 0U) {
        if (payload.size() != originalSize) {
            return false;
        }
        std::memcpy(out.data(), payload.data(), originalSize);
        return true;
    }
    if (type != 1U) {
        return false;
    }
    return fastlzDecompress(payload, std::span<std::byte>{out.data(), out.size()}) == originalSize;
}

bool readKdTreeChunk(Cursor& c, MeshKdTree& out, IArena& arena, IssueBag& issues) {
    for (auto& v : out.bboxMin) {
        v = c.f32v();
    }
    for (auto& v : out.bboxMax) {
        v = c.f32v();
    }
    const u32 leafCount = c.u32v();
    const u32 nodeCount = c.u32v();
    const u32 leafEntryBytes = c.u8v();
    const u32 flags = c.u8v();
    c.skip(2U);
    if (c.bad()) {
        fail(issues, issues::asset::kPkmmTruncated, "pkmm: truncated kd-tree chunk header");
        return false;
    }
    const bool compressed = (flags & 3U) != 0U;
    if (leafEntryBytes < 1U || leafEntryBytes > 4U || leafCount == 0U || nodeCount == 0U) {
        return false;
    }

    const u32 leafFileBytes = leafEntryBytes * leafCount;
    const u32 nodeFileBytes = 8U * nodeCount;
    std::vector<std::byte> leafStore;
    std::vector<std::byte> nodeStore;
    std::span<const std::byte> leafBytes;
    std::span<const std::byte> nodeBytes;

    const auto readArray = [&](u32 expected, std::vector<std::byte>& store,
                               std::span<const std::byte>& outSpan) -> bool {
        const u32 declared = c.u32v();
        if (c.bad()) {
            return false;
        }
        if (compressed) {
            const auto buf = c.raw(declared);
            if (c.bad()) {
                fail(issues, issues::asset::kPkmmTruncated, "pkmm: truncated kd-tree array");
                return false;
            }
            u8 codec = 0U;
            if (!decompressCompressorBuffer(buf, store, expected, codec)) {
                return false;
            }
            out.compressionType = codec;
            out.framedByCompressor = true;
            outSpan = std::span<const std::byte>{store.data(), store.size()};
            return true;
        }
        if (declared != expected) {
            return false;
        }
        outSpan = c.raw(expected);
        if (c.bad()) {
            fail(issues, issues::asset::kPkmmTruncated, "pkmm: truncated kd-tree array");
            return false;
        }
        return true;
    };

    if (!readArray(leafFileBytes, leafStore, leafBytes)) {
        return false;
    }
    const auto leaves = arenaArray<u32>(arena, leafCount);
    for (u32 i = 0U; i < leafCount; ++i) {
        u32 v = 0U;
        std::memcpy(&v, leafBytes.data() + static_cast<std::size_t>(i) * leafEntryBytes,
                    leafEntryBytes);
        leaves[i] = v;
    }

    if (!readArray(nodeFileBytes, nodeStore, nodeBytes)) {
        return false;
    }
    const auto nodes = arenaArray<MeshKdNode>(arena, nodeCount);
    for (u32 i = 0U; i < nodeCount; ++i) {
        std::memcpy(&nodes[i].nodeData, nodeBytes.data() + static_cast<std::size_t>(i) * 8U,
                    sizeof(u32));
        std::memcpy(&nodes[i].splitPos, nodeBytes.data() + static_cast<std::size_t>(i) * 8U + 4U,
                    sizeof(f32));
    }

    out.leaves = std::span<const u32>{leaves.data(), leaves.size()};
    out.nodes = std::span<const MeshKdNode>{nodes.data(), nodes.size()};
    return true;
}

bool readPdfChunk(Cursor& c, MeshPdf& out, IArena& arena, IssueBag& issues) {
    const u32 sampleeCount = c.u32v();
    out.totalDensity = c.f32v();
    const u32 declaredBytes = c.u32v();
    if (c.bad()) {
        fail(issues, issues::asset::kPkmmTruncated, "pkmm: truncated sampling-accel chunk header");
        return false;
    }
    if (declaredBytes != sampleeCount * 8U) {
        fail(issues, issues::asset::kPkmmPdfSize,
             "pkmm: sampling-accel slot data size disagrees with its samplee count");
        return false;
    }
    const auto bytes = c.raw(declaredBytes);
    if (c.bad()) {
        fail(issues, issues::asset::kPkmmTruncated, "pkmm: truncated sampling-accel slot data");
        return false;
    }
    if (sampleeCount == 0U) {
        return true;
    }
    const auto slots = arenaArray<MeshPdfSlot>(arena, sampleeCount);
    for (u32 i = 0U; i < sampleeCount; ++i) {
        std::memcpy(&slots[i].proba, bytes.data() + static_cast<std::size_t>(i) * 8U, sizeof(f32));
        std::memcpy(&slots[i].other, bytes.data() + static_cast<std::size_t>(i) * 8U + 4U,
                    sizeof(u32));
    }
    out.slots = std::span<const MeshPdfSlot>{slots.data(), slots.size()};
    return true;
}

bool readTetraChunk(Cursor& c, MeshTetrahedra& out, IArena& arena, IssueBag& issues) {
    const u32 indexCount = c.u32v();
    const u32 wvertCount = c.u32v();
    out.flags = c.u32v();
    if (c.bad()) {
        fail(issues, issues::asset::kPkmmTruncated, "pkmm: truncated tetrahedral chunk header");
        return false;
    }
    if (indexCount == 0U || indexCount % 4U != 0U) {
        return false;
    }
    const auto indexBytes = c.raw(static_cast<std::size_t>(indexCount) * 4U);
    const auto wvertBytes = c.raw(static_cast<std::size_t>(wvertCount) * 20U);
    if (c.bad()) {
        fail(issues, issues::asset::kPkmmTruncated, "pkmm: truncated tetrahedral data");
        return false;
    }

    const auto indices = arenaArray<u32>(arena, indexCount);
    for (u32 i = 0U; i < indexCount; ++i) {
        std::memcpy(&indices[i], indexBytes.data() + static_cast<std::size_t>(i) * 4U, sizeof(u32));
    }

    const auto wverts = arenaArray<MeshWeightedVertex>(arena, wvertCount);
    for (u32 i = 0U; i < wvertCount; ++i) {
        const auto* p = wvertBytes.data() + static_cast<std::size_t>(i) * 20U;
        std::memcpy(wverts[i].indices.data(), p, 12U);
        std::memcpy(wverts[i].weights.data(), p + 12U, 8U);
    }

    out.indices = std::span<const u32>{indices.data(), indices.size()};
    out.weighted = std::span<const MeshWeightedVertex>{wverts.data(), wverts.size()};
    return true;
}

bool readGeometry(Cursor& c, std::span<const std::string_view> strings, MeshGeometry& g,
                  std::vector<MeshVertexStream>& outStreams, std::size_t& firstStream,
                  std::size_t& streamCount, IArena& arena, IssueBag& issues) {
    const auto flagBytes = c.raw(4U);
    if (flagBytes.size() != 4U) {
        fail(issues, issues::asset::kPkmmTruncated, "pkmm: truncated geometry header");
        return false;
    }
    const u8 flags0 = static_cast<u8>(flagBytes[0]);
    const u8 flags1 = static_cast<u8>(flagBytes[1]);
    const u32 materialIndex = c.u32v();
    const u32 subChunkCount = c.u32v();
    g.indexCount = c.u32v();

    switch (flags0 & 3U) {
    case 0U:
        g.indexFormat = MeshIndexFormat::U8;
        break;
    case 1U:
        g.indexFormat = MeshIndexFormat::U16;
        break;
    case 2U:
        g.indexFormat = MeshIndexFormat::U32;
        break;
    default:
        fail(issues, issues::asset::kPkmmBadGeometry, "pkmm: corrupt index format in geometry");
        return false;
    }
    switch ((flags0 >> 2U) & 3U) {
    case 0U:
        g.primitive = MeshPrimitive::TriangleStrips;
        break;
    case 1U:
        g.primitive = MeshPrimitive::Triangles;
        break;
    case 2U:
        g.primitive = MeshPrimitive::Lines;
        break;
    default:
        fail(issues, issues::asset::kPkmmBadGeometry, "pkmm: unknown primitive type in geometry");
        return false;
    }

    g.material = stringAt(strings, materialIndex);

    const bool hasExtV1 = (flags0 & 0x80U) != 0U;
    const bool hasExtV2 = (flags1 & 0x1U) != 0U;
    u32 nameIndex = 0U;
    if (hasExtV2) {
        g.hasSurfaceVolume = true;
        g.surface = c.f32v();
        g.volume = c.f32v();
        for (auto& v : g.bboxMin) {
            v = c.f32v();
        }
        for (auto& v : g.bboxMax) {
            v = c.f32v();
        }
        g.hasBBox = true;
        nameIndex = c.u32v();
        c.skip(12U);
    } else if (hasExtV1) {
        g.hasSurfaceVolume = true;
        g.surface = c.f32v();
        g.volume = c.f32v();
    }
    if (nameIndex != 0U) {
        g.name = stringAt(strings, nameIndex - 1U);
    }

    g.indices = c.raw(static_cast<std::size_t>(meshIndexSize(g.indexFormat)) * g.indexCount);
    if (c.bad()) {
        fail(issues, issues::asset::kPkmmTruncated, "pkmm: truncated geometry indices");
        return false;
    }

    firstStream = outStreams.size();
    for (u32 i = 0U; i < subChunkCount; ++i) {
        const auto [key, size] = c.chunkHeader();
        if (c.bad()) {
            fail(issues, issues::asset::kPkmmTruncated,
                 "pkmm: truncated geometry sub-chunk header");
            return false;
        }
        const std::size_t start = c.pos();
        if (key == kSubVStream) {
            if (!readVStream(c, strings, g, outStreams, issues)) {
                return false;
            }
            if (c.pos() != start + size) {
                fail(issues, issues::asset::kPkmmVStreamSize,
                     "pkmm: vertex-stream sub-chunk over/under-ran its declared size");
                return false;
            }
        } else if (key == kSubAccelSurface || key == kSubAccelVolume) {
            MeshPdf pdf;
            if (readPdfChunk(c, pdf, arena, issues)) {
                (key == kSubAccelSurface ? g.surfaceSamplingPdf : g.volumeSamplingPdf) = pdf;
            }
            c.seek(start + size);
        } else if (key == kSubKdTree) {
            MeshKdTree tree;
            if (readKdTreeChunk(c, tree, arena, issues)) {
                g.kdTree = tree;
            }
            c.seek(start + size);
        } else if (key == kSubTetra) {
            MeshTetrahedra tets;
            if (readTetraChunk(c, tets, arena, issues)) {
                g.tetrahedra = tets;
            }
            c.seek(start + size);
        } else {
            c.seek(start + size);
        }
        if (c.bad()) {
            fail(issues, issues::asset::kPkmmTruncated, "pkmm: geometry sub-chunk runs past EOF");
            return false;
        }
    }
    streamCount = outStreams.size() - firstStream;

    if (g.tetrahedra.valid()) {
        const u64 limit =
            static_cast<u64>(g.vertexCount) + static_cast<u64>(g.tetrahedra.weighted.size());
        for (const u32 v : g.tetrahedra.indices) {
            if (v >= limit) {
                fail(issues, issues::asset::kPkmmBadGeometry,
                     "pkmm: a tetrahedral index addresses a vertex that does not exist");
                g.tetrahedra = MeshTetrahedra{};
                break;
            }
        }
    }
    return true;
}

}

u32 MeshGeometry::index(u32 i) const noexcept {
    const u32 sz = meshIndexSize(indexFormat);
    const std::size_t off = static_cast<std::size_t>(i) * sz;
    if (i >= indexCount || off + sz > indices.size()) {
        return 0U;
    }
    const auto* p = indices.data() + off;
    if (sz == 1U) {
        return static_cast<u32>(p[0]);
    }
    if (sz == 2U) {
        u16 v = 0U;
        std::memcpy(&v, p, sizeof(u16));
        return v;
    }
    u32 v = 0U;
    std::memcpy(&v, p, sizeof(u32));
    return v;
}

const MeshVertexStream* MeshGeometry::stream(std::string_view semantic) const noexcept {
    for (const auto& s : streams) {
        if (s.semantic == semantic) {
            return &s;
        }
    }
    return nullptr;
}

std::array<u8, 3> MeshAsset::frameAxes() const noexcept {
    const u8 f = coordinateFrame.value_or(0U);
    return f >= 4U ? coordinateFrameAxes : meshFrameAxes(f);
}

std::array<u8, 3> meshFrameAxes(u8 frame) noexcept {
    return kFrameAxes[std::min<std::size_t>(frame, kFrameAxes.size() - 1U)];
}

std::array<MeshAxisMap, 3> meshTransposeMap(std::array<u8, 3> from, std::array<u8, 3> to) noexcept {
    std::array<MeshAxisMap, 3> map{};
    for (u8 d = 0U; d < 3U; ++d) {
        const u8 want = to[d];
        u8 src = d;
        for (u8 s = 0U; s < 3U; ++s) {
            if (from[s] == want || from[s] == (want ^ 1U)) {
                src = s;
                break;
            }
        }
        map[d].source = src;
        map[d].sign = from[src] == want ? 1.0F : -1.0F;
    }
    return map;
}

std::array<f32, 3> meshTranspose(const std::array<MeshAxisMap, 3>& map,
                                 std::array<f32, 3> v) noexcept {
    return {v[map[0].source] * map[0].sign, v[map[1].source] * map[1].sign,
            v[map[2].source] * map[2].sign};
}

bool meshTransposeMirrors(const std::array<MeshAxisMap, 3>& map) noexcept {
    std::array<std::array<f32, 3>, 3> m{};
    for (std::size_t d = 0; d < 3U; ++d) {
        m[d][map[d].source] = map[d].sign;
    }
    const f32 det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
                    m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                    m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    return det < 0.0F;
}

std::optional<MeshAsset> readPkmm(std::span<const std::byte> bytes, IArena& arena,
                                  IssueBag& issues) {
    Cursor c{bytes};
    const u8 version = c.u8v();
    c.skip(3U);
    if (c.bad()) {
        fail(issues, issues::asset::kPkmmTruncated, "pkmm: file is too short for a header");
        return std::nullopt;
    }
    if (version != kPkmmFileVersion) {
        fail(issues, issues::asset::kPkmmBadVersion,
             "pkmm: unsupported file version (the engine only loads version 0)");
        return std::nullopt;
    }

    const u32 stringCount = c.u32v();
    std::vector<std::string_view> strings;
    strings.reserve(std::min<u32>(stringCount, 4096U));
    for (u32 i = 0U; i < stringCount; ++i) {
        strings.push_back(c.pstring());
        if (c.bad()) {
            fail(issues, issues::asset::kPkmmTruncated, "pkmm: truncated string table");
            return std::nullopt;
        }
    }

    MeshAsset mesh;
    mesh.version = version;
    mesh.coordinateFrameAxes = kFrameAxes[0];

    std::vector<MeshGeometry> geometries;
    std::vector<MeshVertexStream> streams;
    std::vector<std::pair<std::size_t, std::size_t>> streamRanges;

    for (;;) {
        const auto [key, size] = c.chunkHeader();
        if (c.bad()) {
            fail(issues, issues::asset::kPkmmTruncated, "pkmm: truncated chunk header");
            return std::nullopt;
        }
        if (key == kChunkEof) {
            break;
        }
        const std::size_t start = c.pos();
        if (key == kChunkGeometry) {
            MeshGeometry g;
            std::size_t first = 0U;
            std::size_t count = 0U;
            if (!readGeometry(c, strings, g, streams, first, count, arena, issues)) {
                return std::nullopt;
            }
            if (c.pos() != start + size) {
                fail(issues, issues::asset::kPkmmBadGeometry,
                     "pkmm: geometry chunk over/under-ran its declared size");
                return std::nullopt;
            }
            geometries.push_back(g);
            streamRanges.emplace_back(first, count);
        } else {
            if (key == kChunkSkeleton) {
                mesh.hasSkeleton = true;
            }
            if (key == kChunkExtendedInfos && size >= 4U && start + 4U <= bytes.size()) {
                mesh.coordinateFrame = static_cast<u8>(bytes[start]);
                mesh.coordinateFrameAxes = {static_cast<u8>(bytes[start + 1U]),
                                            static_cast<u8>(bytes[start + 2U]),
                                            static_cast<u8>(bytes[start + 3U])};
            }
            c.seek(start + size);
            if (c.bad()) {
                fail(issues, issues::asset::kPkmmTruncated, "pkmm: chunk runs past EOF");
                return std::nullopt;
            }
        }
    }
    if (c.pos() != bytes.size()) {
        fail(issues, issues::asset::kPkmmTrailingBytes, "pkmm: trailing bytes after the EOF chunk");
        return std::nullopt;
    }

    const auto streamArr = arenaArray<MeshVertexStream>(arena, streams.size());
    std::copy(streams.begin(), streams.end(), streamArr.begin());
    const auto geomArr = arenaArray<MeshGeometry>(arena, geometries.size());
    for (std::size_t i = 0; i < geometries.size(); ++i) {
        geomArr[i] = geometries[i];
        const auto [first, count] = streamRanges[i];
        if (count > 0U) {
            geomArr[i].streams = std::span<const MeshVertexStream>{streamArr.data() + first, count};
        }
    }
    mesh.geometries = std::span<const MeshGeometry>{geomArr.data(), geomArr.size()};
    return mesh;
}

}

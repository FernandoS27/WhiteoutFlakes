#include "bls_container.h"

#include <cstring>
#include <span>
#include <vector>

// WhiteoutLib's zlib_decompress is implemented in src/whiteout/common/deflate.cpp
// and linked into whiteout_lib. The header is internal to that library, so we
// forward-declare the symbol here rather than reaching into its src/ tree.
namespace whiteout {
std::vector<u8> zlib_decompress(std::span<const u8> data, std::string* out_error,
                                size_t expectedSize);
}

namespace whiteout::flakes::renderer::bls {

namespace {

void SetError(std::string* error, const char* msg) {
    if (error)
        *error = msg;
}

bool ReadU32(std::span<const u8> data, usize off, u32& out) {
    if (off + sizeof(u32) > data.size())
        return false;
    std::memcpy(&out, data.data() + off, sizeof(u32));
    return true;
}

} // namespace

bool BlsContainer::Load(std::span<const u8> fileBytes, std::string* error) {
    loaded_ = false;
    version_ = 0;
    platformTag_ = 0;
    permutes_.clear();
    bytes_.clear();

    if (fileBytes.size() < 8) {
        SetError(error, "BLS file too small for outer header");
        return false;
    }

    u32 magic = 0;
    std::memcpy(&magic, fileBytes.data(), sizeof(u32));
    if (magic != kHsxgMagic) {
        SetError(error, "Bad magic (expected 'HSXG')");
        return false;
    }

    u32 version = 0;
    std::memcpy(&version, fileBytes.data() + 4, sizeof(u32));
    version_ = version;

    if (version == kHsxgVersion_1_8)
        return LoadV1_8(fileBytes, error);
    if (version == kHsxgVersion_1_14)
        return LoadV1_14(fileBytes, error);

    SetError(error, "Unsupported BLS version (expected 1.8 or 1.14)");
    return false;
}

// ----------------------------------------------------------------------------
// v1.8 — uncompressed. Two inner-perm flavors share the outer container:
//   • DXBC v1.8: 80-byte PermuteHeader + DXBC blob (codeSize at +0x4C,
//     blob at +0x50, magic 'DXBC' at the blob's first 4 bytes).
//   • Metal v1.8: 44-byte PermuteHeaderMetal + MTLB blob (metallibSize
//     at +0x20, blob at +0x2C, magic 'MTLB' at the blob's first 4 bytes).
// The outer header / perm-table layout is identical; we sniff the first
// non-null perm to decide which inner shape to use and set platformTag_
// to kPlatformTag_DXBC or kPlatformTag_MTL accordingly. Caller code reads
// PermuteView::dxbc as opaque bytes, so picking the right blob span (just
// the metallib / just the DXBC, not the inner header) keeps every
// downstream consumer working unchanged. The Wc3-shipped mtlfs/mtlvs
// bundles use the Metal flavor; the shipped ps/vs bundles + the
// Wc3Shaders-rebuilt DXBC bundles use the DXBC flavor.
// ----------------------------------------------------------------------------
bool BlsContainer::LoadV1_8(std::span<const u8> fileBytes, std::string* error) {
    if (fileBytes.size() < sizeof(BlsHeader)) {
        SetError(error, "v1.8: file too small for header");
        return false;
    }

    BlsHeader h{};
    std::memcpy(&h, fileBytes.data(), sizeof(BlsHeader));

    if (h.permutationCount == 0) {
        SetError(error, "v1.8: permutation count is zero");
        return false;
    }
    const u64 permTableEnd =
        static_cast<u64>(h.permutationOffset) + u64{h.permutationCount} * sizeof(u32);
    if (permTableEnd > fileBytes.size()) {
        SetError(error, "v1.8: permutation table out of bounds");
        return false;
    }
    if (h.dataOffset > fileBytes.size()) {
        SetError(error, "v1.8: data offset out of bounds");
        return false;
    }

    bytes_.assign(fileBytes.begin(), fileBytes.end());

    const auto* permTable = reinterpret_cast<const u32*>(bytes_.data() + h.permutationOffset);
    const u8* permuteData = bytes_.data() + h.dataOffset;
    const usize permuteDataLen = bytes_.size() - h.dataOffset;

    // Per-perm size derives from the perm table: permTable[i] is the
    // start offset of perm i (permTable[0] = 0 via the pad word that
    // precedes the cum table); end = permTable[i+1] for i < N-1, or
    // permuteDataLen for i = N-1. Null perms have size == 0.
    auto permSize = [&](u32 i) -> u64 {
        const u64 start = permTable[i];
        const u64 end = (i + 1 < h.permutationCount) ? permTable[i + 1] : permuteDataLen;
        return (end >= start) ? (end - start) : 0;
    };

    // Magic sniffer. Reads the u32 at perm_payload[probeOff], compares to
    // wantMagic. Returns false if the perm is too small to probe.
    auto matchMagic = [&](const u8* perm, u64 size, u64 probeOff, u32 wantMagic) -> bool {
        if (size < probeOff + sizeof(u32))
            return false;
        u32 m = 0;
        std::memcpy(&m, perm + probeOff, sizeof(u32));
        return m == wantMagic;
    };

    // Detect inner flavor from the first non-null perm. The two probes
    // sit at distinct offsets (MTLB at +0x2C vs DXBC at +0x50) so they
    // can't both match by coincidence.
    constexpr u64 kMetalProbeOffset = 0x2C; // start of metallib blob
    constexpr u64 kDxbcProbeOffset = 0x50;  // start of DXBC blob
    u32 detectedTag = 0;
    for (u32 i = 0; i < h.permutationCount; ++i) {
        const u64 size = permSize(i);
        if (size == 0)
            continue;
        const u8* perm = permuteData + permTable[i];
        if (matchMagic(perm, size, kMetalProbeOffset, kMtlbMagic)) {
            detectedTag = kPlatformTag_MTL;
        } else if (matchMagic(perm, size, kDxbcProbeOffset, kDxbcMagic)) {
            detectedTag = kPlatformTag_DXBC;
        }
        break;
    }
    if (detectedTag == 0) {
        // Either every perm was null (degenerate but legal — treat as
        // DXBC for the empty container) or the first live perm had no
        // recognisable inner magic. Default to DXBC to preserve the
        // pre-Metal behaviour for empty bundles.
        detectedTag = kPlatformTag_DXBC;
    }
    platformTag_ = detectedTag;

    permutes_.reserve(h.permutationCount);
    for (u32 i = 0; i < h.permutationCount; ++i) {
        const u32 off = permTable[i];
        const u64 size = permSize(i);
        if (size == 0) {
            // Null perm — record an empty entry so the per-index lookup
            // the renderer does stays meaningful. The shader cache
            // forwards an empty span to CreateShader, which returns
            // Invalid; the engine's PSO builder treats Invalid handles
            // for never-bound perms as "unused".
            permutes_.push_back({PermuteHeader{}, std::span<const u8>{}});
            continue;
        }

        const u8* perm = permuteData + off;

        if (platformTag_ == kPlatformTag_MTL) {
            if (size < sizeof(PermuteHeaderMetal)) {
                SetError(error, "v1.8/Metal: perm too small for inner header");
                permutes_.clear();
                bytes_.clear();
                platformTag_ = 0;
                return false;
            }
            PermuteHeaderMetal ph{};
            std::memcpy(&ph, perm, sizeof(PermuteHeaderMetal));

            const u64 blobStart = sizeof(PermuteHeaderMetal);
            const u64 blobEnd = blobStart + ph.metallibSize;
            // Wc3's mtl perms include a single trailing 0x00 byte after
            // the metallib (see Wc3Shaders/build_bls.py pack_blob_perm),
            // so `size` is metallibSize + headerSize + 1. Validate at
            // least the metallib fits.
            if (blobEnd > size || ph.metallibSize < sizeof(u32) * 2) {
                SetError(error, "v1.8/Metal: metallib blob out of bounds");
                permutes_.clear();
                bytes_.clear();
                platformTag_ = 0;
                return false;
            }
            u32 mtlbMagic = 0;
            std::memcpy(&mtlbMagic, perm + blobStart, sizeof(u32));
            if (mtlbMagic != kMtlbMagic) {
                SetError(error, "v1.8/Metal: blob is not an MTLB container");
                permutes_.clear();
                bytes_.clear();
                platformTag_ = 0;
                return false;
            }

            // PermuteView::header carries the 80-byte DX struct shape;
            // for Metal perms the renderer doesn't read it, so we leave
            // it zero-initialised. Future Metal-side reflection (e.g.
            // per-perm stage flag) can pivot on platformTag_ and reach
            // for the Metal header separately.
            permutes_.push_back(
                {PermuteHeader{}, std::span<const u8>(perm + blobStart, ph.metallibSize)});
        } else {
            // DXBC v1.8 — the original path, unchanged.
            if (size < sizeof(PermuteHeader)) {
                SetError(error, "v1.8/DXBC: perm too small for header");
                permutes_.clear();
                bytes_.clear();
                platformTag_ = 0;
                return false;
            }
            PermuteHeader ph{};
            std::memcpy(&ph, perm, sizeof(PermuteHeader));

            const u64 blobStart = sizeof(PermuteHeader);
            const u64 blobEnd = blobStart + ph.codeSize;
            if (blobEnd > size || ph.codeSize < sizeof(u32) * 2) {
                SetError(error, "v1.8/DXBC: DXBC blob out of bounds");
                permutes_.clear();
                bytes_.clear();
                platformTag_ = 0;
                return false;
            }
            u32 dxbcMagic = 0;
            std::memcpy(&dxbcMagic, perm + blobStart, sizeof(u32));
            if (dxbcMagic != kDxbcMagic) {
                SetError(error, "v1.8/DXBC: blob is not a DXBC container");
                permutes_.clear();
                bytes_.clear();
                platformTag_ = 0;
                return false;
            }

            permutes_.push_back({ph, std::span<const u8>(perm + blobStart, ph.codeSize)});
        }
    }

    loaded_ = true;
    return true;
}

// ----------------------------------------------------------------------------
// v1.14 — zlib-compressed payload, MD5-hashed perm entries, §3.2 DX inner
// per-perm format (40-byte inner header + 48-byte resource info + 8-byte
// dxbc prefix + DXIL-in-DXBC blob).
//
// Layout (per Wc3Shaders/build_bls.py build_v14_outer + pack_v14_dx_perm):
//   bytes 0x00..0x28        BlsHeaderV14 (40 bytes)
//   bytes header.permsOffset+0x00..+0x04          padding (0)
//   bytes header.permsOffset+0x04..+(num*24)      perm entries
//   bytes header.blobsOffset+0x00..+0x04          padding (0)
//   bytes header.blobsOffset+0x04..+(num_blobs*4) cumulative blob offsets
//   bytes header.dataOffset..eof                  zlib-compressed payload
// ----------------------------------------------------------------------------
bool BlsContainer::LoadV1_14(std::span<const u8> fileBytes, std::string* error) {
    if (fileBytes.size() < sizeof(BlsHeaderV14)) {
        SetError(error, "v1.14: file too small for header");
        return false;
    }

    BlsHeaderV14 h{};
    std::memcpy(&h, fileBytes.data(), sizeof(BlsHeaderV14));
    platformTag_ = h.platformTag;

    // We accept the three backends we actually emit + load: DX SM6 (DXIL)
    // for d3d12, SPIR-V for Vulkan, and WGSL for WebGPU. GLSL is rejected.
    const bool isDx6 = (h.platformTag == kPlatformTag_DX6);
    const bool isSpirv = (h.platformTag == kPlatformTag_SPIRV);
    const bool isWgsl = (h.platformTag == kPlatformTag_WGSL);
    if (!isDx6 && !isSpirv && !isWgsl) {
        SetError(error, "v1.14: unsupported platformTag (expected '06XD', 'RIPS' or 'LSGW')");
        return false;
    }

    if (h.permCount == 0) {
        SetError(error, "v1.14: permutation count is zero");
        return false;
    }
    if (h.blobCount != 1) {
        // build_bls.py only emits single-blob bundles. WoW splits at ~64KB
        // for streaming; if we ever encounter that we'll need to concat
        // multiple decompressed blobs here.
        SetError(error, "v1.14: multi-blob bundles not yet supported");
        return false;
    }

    const u64 permTableEnd =
        static_cast<u64>(h.permsOffset) + 4 + u64{h.permCount} * sizeof(BlsV14PermEntry);
    if (permTableEnd > fileBytes.size()) {
        SetError(error, "v1.14: perm table out of bounds");
        return false;
    }
    if (h.dataOffset > fileBytes.size()) {
        SetError(error, "v1.14: data offset out of bounds");
        return false;
    }

    // Walk the perm entries to compute total decompressed size + collect
    // (size, cumulative offset) tuples. Null perms are size==0.
    std::vector<BlsV14PermEntry> perms(h.permCount);
    {
        const u8* p = fileBytes.data() + h.permsOffset + 4; // skip 4-byte padding prefix
        std::memcpy(perms.data(), p, h.permCount * sizeof(BlsV14PermEntry));
    }

    u32 totalDecompressed = 0;
    for (const auto& e : perms)
        totalDecompressed += e.size;

    // Decompress the single zlib blob into bytes_. The decompressed payload
    // is the concatenation of all live perm bytes in declaration order.
    const std::span<const u8> compressed(fileBytes.data() + h.dataOffset,
                                         fileBytes.size() - h.dataOffset);

    std::string decompErr;
    bytes_ = ::whiteout::zlib_decompress(compressed, &decompErr, totalDecompressed);
    if (bytes_.empty()) {
        SetError(error, decompErr.empty() ? "v1.14: zlib_decompress returned empty payload"
                                          : decompErr.c_str());
        return false;
    }
    if (bytes_.size() < totalDecompressed) {
        SetError(error, "v1.14: decompressed payload smaller than perm-table sum");
        bytes_.clear();
        return false;
    }

    // Slice each live perm out of the decompressed payload, parse its §3.2
    // inner DX header, and expose the inner DXBC/DXIL blob as a span.
    permutes_.reserve(h.permCount);
    u32 cursor = 0;
    for (usize i = 0; i < perms.size(); ++i) {
        const u32 sz = perms[i].size;
        if (sz == 0) {
            // null perm — keep the index aligned with the SM5 build by
            // pushing a placeholder. The cache today doesn't use null perms
            // (it iterates through PermuteCount and creates handles), so we
            // emit an empty span and let the device's CreateShader reject
            // zero-size bytecode if the caller actually tries to use it.
            permutes_.push_back({PermuteHeader{}, std::span<const u8>{}});
            continue;
        }
        if (static_cast<u64>(cursor) + sz > bytes_.size()) {
            SetError(error, "v1.14: perm slice out of bounds");
            permutes_.clear();
            bytes_.clear();
            return false;
        }

        const u8* permBytes = bytes_.data() + cursor;

        const u8* blobStart = nullptr;
        u32 blobSize = 0;

        if (isDx6) {
            // §3.2 inner: 40-byte header + 48-byte resource info + 8-byte
            // dxbc prefix + DXBC/DXIL blob. dxbc_size at 0x58, blob at 0x60.
            if (sz < sizeof(BlsV14DxInnerHeader) + 48 + 8) {
                SetError(error, "v1.14: perm too small for §3.2 DX inner header");
                permutes_.clear();
                bytes_.clear();
                return false;
            }
            BlsV14DxInnerHeader inner{};
            std::memcpy(&inner, permBytes, sizeof(BlsV14DxInnerHeader));
            if (inner.headerSize != sizeof(BlsV14DxInnerHeader)) {
                SetError(error, "v1.14: §3.2 DX inner header size mismatch");
                permutes_.clear();
                bytes_.clear();
                return false;
            }
            std::memcpy(&blobSize, permBytes + 0x58, sizeof(u32));
            const u32 dxbcOffset = 0x60;
            if (static_cast<u64>(dxbcOffset) + blobSize > sz) {
                SetError(error, "v1.14: DXBC blob out of bounds within perm");
                permutes_.clear();
                bytes_.clear();
                return false;
            }
            blobStart = permBytes + dxbcOffset;
            u32 dxbcMagic = 0;
            std::memcpy(&dxbcMagic, blobStart, sizeof(u32));
            if (dxbcMagic != kDxbcMagic) {
                SetError(error, "v1.14: inner blob is not a DXBC/DXIL container");
                permutes_.clear();
                bytes_.clear();
                return false;
            }
        } else {
            // §3.6 opaque-blob inner (used for SPIR-V/GLSL/WGSL):
            //   bytes 0x00..0x14: 20 bytes zero
            //   bytes 0x14:       u32 payload_size (= 0x14 + blob_size)
            //   bytes 0x18:       u32 stage (= 1)
            //   bytes 0x1C:       u32 entry_count (= 1)
            //   bytes 0x20:       u32 blob_size
            //   bytes 0x24:       u32 (= 8)
            //   bytes 0x28:       u32 (= 1)
            //   bytes 0x2C..:     blob, then 1-byte 0x00 trailer.
            constexpr u32 kInnerHeader = 0x2C;
            if (sz < kInnerHeader + 1) {
                SetError(error, "v1.14: perm too small for §3.6 opaque-blob header");
                permutes_.clear();
                bytes_.clear();
                return false;
            }
            std::memcpy(&blobSize, permBytes + 0x20, sizeof(u32));
            if (static_cast<u64>(kInnerHeader) + blobSize > sz) {
                SetError(error, "v1.14: §3.6 blob out of bounds within perm");
                permutes_.clear();
                bytes_.clear();
                return false;
            }
            blobStart = permBytes + kInnerHeader;
            // SPIR-V starts with the magic 0x07230203 (little-endian).
            if (isSpirv && blobSize >= 4) {
                u32 spvMagic = 0;
                std::memcpy(&spvMagic, blobStart, sizeof(u32));
                if (spvMagic != 0x07230203u) {
                    SetError(error, "v1.14: SPIR-V blob magic mismatch");
                    permutes_.clear();
                    bytes_.clear();
                    return false;
                }
            }
        }

        // The v1.14 inner header is shaped differently from the v1.8
        // PermuteHeader, but no consumer reads PermuteHeader fields today —
        // we leave it default-initialised as a placeholder.
        permutes_.push_back({PermuteHeader{}, std::span<const u8>(blobStart, blobSize)});

        cursor += sz;
    }

    loaded_ = true;
    return true;
}

} // namespace whiteout::flakes::renderer::bls

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
// v1.8 — uncompressed; PermuteHeader (80 bytes) + DXBC blob, indexed via a
// u32 offset table. Kept on the original file bytes so PermuteView spans
// remain valid for the BlsContainer's lifetime.
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

    permutes_.reserve(h.permutationCount);
    for (u32 i = 0; i < h.permutationCount; ++i) {
        const u32 off = permTable[i];
        if (static_cast<u64>(off) + sizeof(PermuteHeader) > permuteDataLen) {
            SetError(error, "v1.8: permute header out of bounds");
            permutes_.clear();
            bytes_.clear();
            return false;
        }

        PermuteHeader ph{};
        std::memcpy(&ph, permuteData + off, sizeof(PermuteHeader));

        const u64 blobStart = static_cast<u64>(off) + sizeof(PermuteHeader);
        const u64 blobEnd = blobStart + ph.codeSize;
        if (blobEnd > permuteDataLen || ph.codeSize < sizeof(u32) * 2) {
            SetError(error, "v1.8: permute DXBC blob out of bounds");
            permutes_.clear();
            bytes_.clear();
            return false;
        }

        const u8* blob = permuteData + blobStart;
        u32 dxbcMagic = 0;
        std::memcpy(&dxbcMagic, blob, sizeof(u32));
        if (dxbcMagic != kDxbcMagic) {
            SetError(error, "v1.8: permute blob is not a DXBC container");
            permutes_.clear();
            bytes_.clear();
            return false;
        }

        permutes_.push_back({ph, std::span<const u8>(blob, ph.codeSize)});
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

    // We accept the two backends we actually emit + load: DX SM6 (DXIL) for
    // d3d12 and SPIR-V for Vulkan. Others (GLSL, WGSL) get rejected.
    const bool isDx6 = (h.platformTag == kPlatformTag_DX6);
    const bool isSpirv = (h.platformTag == kPlatformTag_SPIRV);
    if (!isDx6 && !isSpirv) {
        SetError(error, "v1.14: unsupported platformTag (expected '06XD' or 'RIPS')");
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

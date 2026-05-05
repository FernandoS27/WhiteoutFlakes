#include "bls_container.h"

#include <cstring>

namespace WhiteoutDex::bls {

namespace {

void SetError(std::string* error, const char* msg) {
    if (error) *error = msg;
}

}

bool BlsContainer::Load(std::span<const u8> fileBytes, std::string* error) {
    loaded_ = false;
    permutes_.clear();
    bytes_.clear();

    if (fileBytes.size() < sizeof(BlsHeader)) {
        SetError(error, "BLS file too small for header");
        return false;
    }

    BlsHeader h{};
    std::memcpy(&h, fileBytes.data(), sizeof(BlsHeader));

    if (h.magic != kHsxgMagic) {
        SetError(error, "Bad magic (expected 'HSXG')");
        return false;
    }
    if (h.version != kHsxgVersion) {
        SetError(error, "Unsupported BLS version (expected 1.8)");
        return false;
    }
    if (h.permutationCount == 0) {
        SetError(error, "Permutation count is zero");
        return false;
    }

    const u64 permTableEnd =
        static_cast<u64>(h.permutationOffset) + u64{h.permutationCount} * sizeof(u32);
    if (permTableEnd > fileBytes.size()) {
        SetError(error, "Permutation table out of bounds");
        return false;
    }
    if (h.dataOffset > fileBytes.size()) {
        SetError(error, "Data offset out of bounds");
        return false;
    }

    bytes_.assign(fileBytes.begin(), fileBytes.end());
    header_ = h;

    const auto* permTable = reinterpret_cast<const u32*>(bytes_.data() + h.permutationOffset);
    const u8* permuteData    = bytes_.data() + h.dataOffset;
    const usize   permuteDataLen = bytes_.size() - h.dataOffset;

    permutes_.reserve(h.permutationCount);
    for (u32 i = 0; i < h.permutationCount; ++i) {
        const u32 off = permTable[i];
        if (static_cast<u64>(off) + sizeof(PermuteHeader) > permuteDataLen) {
            SetError(error, "Permute header out of bounds");
            permutes_.clear();
            bytes_.clear();
            return false;
        }

        PermuteHeader ph{};
        std::memcpy(&ph, permuteData + off, sizeof(PermuteHeader));

        const u64 blobStart = static_cast<u64>(off) + sizeof(PermuteHeader);
        const u64 blobEnd   = blobStart + ph.codeSize;
        if (blobEnd > permuteDataLen || ph.codeSize < sizeof(u32) * 2) {
            SetError(error, "Permute DXBC blob out of bounds");
            permutes_.clear();
            bytes_.clear();
            return false;
        }

        const u8* blob = permuteData + blobStart;
        u32 dxbcMagic = 0;
        std::memcpy(&dxbcMagic, blob, sizeof(u32));
        if (dxbcMagic != kDxbcMagic) {
            SetError(error, "Permute blob is not a DXBC container");
            permutes_.clear();
            bytes_.clear();
            return false;
        }

        permutes_.push_back({ ph, std::span<const u8>(blob, ph.codeSize) });
    }

    loaded_ = true;
    return true;
}

}

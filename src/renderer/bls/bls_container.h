#pragma once

#include "common_types.h"

#include <span>
#include <string>
#include <vector>

namespace whiteout::flakes::renderer::bls {

inline constexpr u32 kHsxgMagic   = 0x47585348u;
inline constexpr u32 kHsxgVersion = 0x00010008u;
inline constexpr u32 kDxbcMagic   = 0x43425844u;

#pragma pack(push, 1)
struct BlsHeader {
    u32 magic;
    u32 version;
    u32 permutationOffset;
    u32 permutationCount;
    u32 dataOffset;
};
static_assert(sizeof(BlsHeader) == 20);

struct PermuteHeader {
    u32 unk0[5];
    u32 totalSize;
    u32 shaderType;
    u32 unk1[9];
    u32 numResources;
    u32 unk2;
    u32 codeSize;
    u32 stageFlag;
};
static_assert(sizeof(PermuteHeader) == 80);
#pragma pack(pop)

struct PermuteView {
    PermuteHeader      header;
    std::span<const u8> dxbc;
};

class BlsContainer {
public:

    bool Load(std::span<const u8> fileBytes, std::string* error = nullptr);

    bool           IsLoaded()        const { return loaded_; }
    u32            Version()         const { return header_.version; }
    usize          PermuteCount()    const { return permutes_.size(); }
    PermuteView    Permute(usize i) const { return permutes_[i]; }

private:
    bool                     loaded_ = false;
    BlsHeader                header_{};
    std::vector<u8>          bytes_;
    std::vector<PermuteView> permutes_;
};

}

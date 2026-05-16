#pragma once

#include "whiteout/flakes/types.h"

#include <span>
#include <string>
#include <vector>

namespace whiteout::flakes::renderer::bls {

inline constexpr u32 kHsxgMagic = 0x47585348u;
inline constexpr u32 kHsxgVersion_1_8 = 0x00010008u;  // shipped DX (DXBC sm5)
inline constexpr u32 kHsxgVersion_1_14 = 0x0001000eu; // shipped DX (DXIL sm6)
inline constexpr u32 kDxbcMagic = 0x43425844u;        // also the DXIL outer magic

// v1.14 platform tags — FourCC at BlsHeaderV14::platformTag.
//   'DXBC' (0x43425844) — DX SM5 (we don't emit; reserved for v1.8 outer)
//   '06XD' (0x44583630) — DX SM6 (DXIL inside DXBC; §3.2 inner layout)
//   'RIPS' (0x53504952) — Vulkan SPIR-V (§3.6 opaque-blob inner layout)
//   'LSLG' (0x474C534C) — OpenGL GLSL (§3.6 opaque-blob)
//   'LSGW' (0x5753474C) — WebGPU WGSL (§3.6 opaque-blob)
inline constexpr u32 kPlatformTag_DX6 = 0x44583630u;   // '06XD'
inline constexpr u32 kPlatformTag_SPIRV = 0x53504952u; // 'RIPS'
inline constexpr u32 kPlatformTag_WGSL = 0x5753474Cu;  // 'LSGW' — WebGPU WGSL source

// v1.8 wire format ------------------------------------------------------------
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

// v1.14 wire format -----------------------------------------------------------
struct BlsHeaderV14 {
    u32 magic;       // 'HSXG'
    u32 version;     // 0x0001000e (minor=14, major=1)
    u32 platformTag; // FourCC, e.g. '06XD' for D3D12 SM6
    u32 permsOffset; // = 0x28
    u32 permCount;
    u32 blobsOffset;
    u32 blobCount;
    u32 dataOffset;
    u32 flags;
    u32 padding;
};
static_assert(sizeof(BlsHeaderV14) == 40);

// One entry in the v1.14 perm table — sized inner blob + MD5 + cumulative
// offset into the decompressed payload.
struct BlsV14PermEntry {
    u32 size;
    u8 md5[16];
    u32 cumOffset;
};
static_assert(sizeof(BlsV14PermEntry) == 24);

// §3.2 DX inner perm header — 40 bytes, followed by 48 bytes of resource
// binding info (zero-filled by build_bls.py since it has no DX12
// templates), then 8 bytes (dxbc_size + tag), then the DXBC/DXIL blob.
struct BlsV14DxInnerHeader {
    u32 stage;
    u32 payloadSize;
    u32 headerSize; // = 0x28
    u32 padding[7]; // 0x0C..0x28
};
static_assert(sizeof(BlsV14DxInnerHeader) == 40);
#pragma pack(pop)

struct PermuteView {
    PermuteHeader header;
    // Raw bytecode span. Format depends on Version() + PlatformTag():
    //   v1.8                          → DXBC (sm5)
    //   v1.14 + kPlatformTag_DX6      → DXIL-in-DXBC (sm6)
    //   v1.14 + kPlatformTag_SPIRV    → SPIR-V (vulkan)
    //   v1.14 + kPlatformTag_WGSL     → WGSL UTF-8 source (webgpu)
    std::span<const u8> dxbc;
};

class BlsContainer {
public:
    bool Load(std::span<const u8> fileBytes, std::string* error = nullptr);

    bool IsLoaded() const {
        return loaded_;
    }
    u32 Version() const {
        return version_;
    }
    u32 PlatformTag() const {
        return platformTag_;
    } // 0 for v1.8
    usize PermuteCount() const {
        return permutes_.size();
    }
    PermuteView Permute(usize i) const {
        return permutes_[i];
    }

private:
    bool LoadV1_8(std::span<const u8> fileBytes, std::string* error);
    bool LoadV1_14(std::span<const u8> fileBytes, std::string* error);

    bool loaded_ = false;
    u32 version_ = 0;
    u32 platformTag_ = 0;
    // v1.8: stores the original file. v1.14: stores the decompressed inner
    // payload so the per-perm spans we hand out remain valid for the
    // container's lifetime.
    std::vector<u8> bytes_;
    std::vector<PermuteView> permutes_;
};

} // namespace whiteout::flakes::renderer::bls

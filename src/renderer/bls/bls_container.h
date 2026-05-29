#pragma once

#include "whiteout/flakes/types.h"

#include <span>
#include <string>
#include <vector>

namespace whiteout::flakes::renderer::bls {

inline constexpr u32 kHsxgMagic = 0x47585348u;
inline constexpr u32 kHsxgVersion_1_8 = 0x00010008u;  // shipped DX (DXBC sm5) + Metal (MTLB)
inline constexpr u32 kHsxgVersion_1_14 = 0x0001000eu; // shipped DX (DXIL sm6)
inline constexpr u32 kDxbcMagic = 0x43425844u;        // also the DXIL outer magic
inline constexpr u32 kMtlbMagic = 0x424C544Du;        // Apple Metal library binary

// Platform tags — FourCC. For v1.14 read from BlsHeaderV14::platformTag;
// for v1.8 set internally by LoadV1_8 based on per-perm magic sniffing
// (MTLB at +0x2C → Metal, DXBC at +0x50 → DX SM5).
//   'DXBC' (0x43425844) — DX SM5 (v1.8 outer)
//   'MTLB' (0x424C544D) — Apple Metal Library Binary (v1.8 outer)
//   '06XD' (0x44583630) — DX SM6 (DXIL inside DXBC; v1.14 §3.2 inner)
//   'RIPS' (0x53504952) — Vulkan SPIR-V (v1.14 §3.6 opaque-blob)
//   'LSLG' (0x474C534C) — OpenGL GLSL (v1.14 §3.6 opaque-blob)
//   'LSGW' (0x5747534C) — WebGPU WGSL (v1.14 §3.6 opaque-blob)
inline constexpr u32 kPlatformTag_DXBC = kDxbcMagic;   // 'DXBC' — v1.8 DX SM5
inline constexpr u32 kPlatformTag_MTL = kMtlbMagic;    // 'MTLB' — v1.8 Apple Metal
inline constexpr u32 kPlatformTag_DX6 = 0x44583630u;   // '06XD'
inline constexpr u32 kPlatformTag_SPIRV = 0x53504952u; // 'RIPS'
inline constexpr u32 kPlatformTag_WGSL = 0x5747534Cu;  // 'LSGW' — WebGPU WGSL source

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

// v1.8 Metal per-perm header — see Wc3Shaders/build_bls.py
// METAL_PERM_INNER_HEADER_SIZE (= 0x2C). Wire layout:
//   +0x00..0x14: 20 zero bytes (pre_meta)
//   +0x14:       payload_size (u32, = 0x14 + metallib_size)
//   +0x18:       stage         (u32, always 1)
//   +0x1C:       entry_count   (u32, always 1)
//   +0x20:       metallib_size (u32)
//   +0x24:       flag          (u32, always 8)
//   +0x28:       flag          (u32, always 1)
//   +0x2C..:     MTLB blob
//   +(0x2C + metallib_size): trailing 0x00 byte
struct PermuteHeaderMetal {
    u32 preMeta[5];
    u32 payloadSize;
    u32 stage;
    u32 entryCount;
    u32 metallibSize;
    u32 flagA;
    u32 flagB;
};
static_assert(sizeof(PermuteHeaderMetal) == 44);

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
    //   v1.8  + kPlatformTag_DXBC     → DXBC (sm5)
    //   v1.8  + kPlatformTag_MTL      → Apple Metal Library Binary
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

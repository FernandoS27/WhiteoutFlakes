#include "env_probe.h"

#include "common_types.h"
#include "io/content_provider.h"

#include <whiteout/textures/dds/parser.h>
#include <whiteout/textures/dds/writer.h>
#include <whiteout/textures/texture.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

#if defined(_WIN32)
extern "C" __declspec(dllimport) void __stdcall OutputDebugStringA(const char* s);
#else
inline void OutputDebugStringA(const char*) {}
#endif

namespace WhiteoutDex::ibl {

namespace {

void DbgLogf(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    OutputDebugStringA(buf);
}

const char* TypeName(whiteout::textures::TextureType t) {
    using T = whiteout::textures::TextureType;
    switch (t) {
        case T::Texture2D:        return "Texture2D";
        case T::Texture3D:        return "Texture3D";
        case T::TextureCube:      return "TextureCube";
        case T::Texture2DArray:   return "Texture2DArray";
        case T::TextureCubeArray: return "TextureCubeArray";
    }
    return "???";
}

const char* FormatName(whiteout::textures::PixelFormat f) {
    using F = whiteout::textures::PixelFormat;
    switch (f) {
        case F::R8:      return "R8";
        case F::R16:     return "R16";
        case F::R32F:    return "R32F";
        case F::RG8:     return "RG8";
        case F::RG16:    return "RG16";
        case F::RG32F:   return "RG32F";
        case F::RGBA8:   return "RGBA8";
        case F::RGBA16:  return "RGBA16";
        case F::RGBA32F: return "RGBA32F";
        case F::BC1:     return "BC1";
        case F::BC2:     return "BC2";
        case F::BC3:     return "BC3";
        case F::BC4:     return "BC4";
        case F::BC5:     return "BC5";
        case F::BC6H:    return "BC6H";
        case F::BC7:     return "BC7";
    }
    return "???";
}

}

namespace {

LoadedEnvProbe LoadEnvProbeFromBytes(gfx::IGFXDevice& gfx,
                                      std::span<const u8> bytes,
                                      const char* sourceLabel,
                                      bool applyBlizzardFaceRemap);
}

LoadedEnvProbe LoadEnvProbe(gfx::IGFXDevice&       gfx,
                            const IContentProvider& content,
                            const std::string&      relPath) {
    LoadedEnvProbe failed{};

    auto bytes = content.ReadFile(relPath);
    if (!bytes) {
        DbgLogf("[WDEX IBL] ReadFile FAILED for %s\n", relPath.c_str());
        return failed;
    }
    DbgLogf("[WDEX IBL] ReadFile OK for %s (%zu bytes)\n",
            relPath.c_str(), bytes->size());

    return LoadEnvProbeFromBytes(
        gfx, std::span<const u8>(bytes->data(), bytes->size()),
        relPath.c_str(),  true);
}

LoadedEnvProbe LoadEnvProbeFromFile(gfx::IGFXDevice& gfx,
                                     const std::string& absPath,
                                     bool applyBlizzardFaceRemap) {
    LoadedEnvProbe failed{};
    std::ifstream f(absPath, std::ios::binary | std::ios::ate);
    if (!f) {
        DbgLogf("[WDEX IBL] LoadEnvProbeFromFile open FAILED %s\n", absPath.c_str());
        return failed;
    }
    const std::streamsize size = f.tellg();
    if (size <= 0) {
        DbgLogf("[WDEX IBL] LoadEnvProbeFromFile empty file %s\n", absPath.c_str());
        return failed;
    }
    f.seekg(0, std::ios::beg);
    std::vector<u8> buf(static_cast<usize>(size));
    if (!f.read(reinterpret_cast<char*>(buf.data()), size)) {
        DbgLogf("[WDEX IBL] LoadEnvProbeFromFile read FAILED %s\n", absPath.c_str());
        return failed;
    }
    DbgLogf("[WDEX IBL] LoadEnvProbeFromFile OK %s (%zd bytes)\n",
            absPath.c_str(), static_cast<isize>(size));
    return LoadEnvProbeFromBytes(
        gfx, std::span<const u8>(buf.data(), buf.size()), absPath.c_str(),
        applyBlizzardFaceRemap);
}

namespace {
LoadedEnvProbe LoadEnvProbeFromBytes(gfx::IGFXDevice& gfx,
                                      std::span<const u8> bytes,
                                      const char* sourceLabel,
                                      bool applyBlizzardFaceRemap) {
    LoadedEnvProbe failed{};
    whiteout::textures::dds::Parser parser(
        whiteout::textures::dds::Parser::ParseMode::Lenient);
    auto parsedOpt = parser.parse(bytes);
    if (!parsedOpt) {
        DbgLogf("[WDEX IBL] DDS parse FAILED\n");
        if (parser.hasIssues()) {
            for (const auto& issue : parser.getIssues()) {
                DbgLogf("[WDEX IBL]   issue: %s\n", issue.c_str());
            }
        }
        return failed;
    }
    whiteout::textures::Texture tex = std::move(*parsedOpt);

    DbgLogf("[WDEX IBL] parsed: type=%s fmt=%s %ux%u arraySize=%u layerCount=%u mipCount=%u srgb=%d\n",
            TypeName(tex.type()), FormatName(tex.format()),
            tex.width(), tex.height(), tex.arraySize(), tex.layerCount(),
            tex.mipCount(), tex.isSrgb() ? 1 : 0);

    const auto type = tex.type();
    if (type != whiteout::textures::TextureType::TextureCube &&
        type != whiteout::textures::TextureType::TextureCubeArray) {
        DbgLogf("[WDEX IBL] rejecting: not cube or cube-array\n");
        return failed;
    }

    auto mapFmt = [](whiteout::textures::PixelFormat pf, bool srgb) -> gfx::Format {
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
    };
    gfx::Format gpuFormat = mapFmt(tex.format(), tex.isSrgb());
    if (gpuFormat == gfx::Format::Unknown) {

        DbgLogf("[WDEX IBL] no direct gfx mapping for source fmt -- decoding to RGBA8\n");
        tex.format(whiteout::textures::PixelFormat::RGBA8);
        if (tex.format() != whiteout::textures::PixelFormat::RGBA8) {
            DbgLogf("[WDEX IBL] fallback decode FAILED\n");
            return failed;
        }
        gpuFormat = tex.isSrgb() ? gfx::Format::R8G8B8A8_UNORM_SRGB
                                 : gfx::Format::R8G8B8A8_UNORM;
    }

    const u32 mipCount  = tex.mipCount();
    const u32 cubeCount = tex.arraySize();
    const u32 layers    = tex.layerCount();
    if (mipCount == 0 || cubeCount == 0 || layers == 0) {
        DbgLogf("[WDEX IBL] rejecting: zero mip/cube/layer count\n");
        return failed;
    }
    if (layers != 6u * cubeCount) {
        DbgLogf("[WDEX IBL] rejecting: layers (%u) != 6*cubes (%u)\n",
                layers, 6u * cubeCount);
        return failed;
    }

    const u32 faceSize = tex.width();
    u64 totalBytes = 0;
    for (u32 layer = 0; layer < layers; ++layer) {
        for (u32 mip = 0; mip < mipCount; ++mip) {
            totalBytes += tex.mipData(mip, layer).size();
        }
    }

    static constexpr u32 kBlzToD3dFaceRemap[6] = { 0, 5, 2, 3, 4, 1 };
    static constexpr u32 kIdentityFaceMap[6]   = { 0, 1, 2, 3, 4, 5 };
    const u32* faceMap = applyBlizzardFaceRemap ? kBlzToD3dFaceRemap
                                                : kIdentityFaceMap;

    std::vector<u8> packed(static_cast<usize>(totalBytes));
    u8* cursor = packed.data();
    for (u32 layer = 0; layer < layers; ++layer) {
        const u32 cubeIdx = layer / 6u;
        const u32 dstFace = layer % 6u;
        const u32 srcLayer = cubeIdx * 6u + faceMap[dstFace];
        for (u32 mip = 0; mip < mipCount; ++mip) {
            auto src = tex.mipData(mip, srcLayer);
            std::memcpy(cursor, src.data(), src.size());
            cursor += src.size();
        }
    }

    gfx::TextureDesc desc;
    desc.width     = static_cast<i32>(faceSize);
    desc.height    = static_cast<i32>(faceSize);
    desc.mipLevels = static_cast<i32>(mipCount);
    desc.arraySize = static_cast<i32>(layers);
    desc.isCube    = true;
    desc.format    = gpuFormat;
    desc.usage     = gfx::TextureUsage::ShaderResource;

    DbgLogf("[WDEX IBL] uploading cube-array: %dx%d arraySize=%d mips=%d totalBytes=%llu\n",
            desc.width, desc.height, desc.arraySize, desc.mipLevels,
            static_cast<unsigned long long>(totalBytes));
    gfx::TextureHandle handle = gfx.CreateTexture(desc, packed.data());
    if (handle == gfx::TextureHandle::Invalid) {
        DbgLogf("[WDEX IBL] CreateTexture FAILED\n");
        return failed;
    }
    DbgLogf("[WDEX IBL] CreateTexture OK\n");
    return { handle, static_cast<i32>(mipCount) };
}
}

gfx::TextureHandle CreateDefaultEnvProbe(gfx::IGFXDevice& gfx) {

    usize totalBytes = 0;
    for (i32 mip = 0; mip < kEnvProbeMipLevels; ++mip) {
        i32 w = std::max(1, kEnvProbeSize >> mip);
        i32 h = std::max(1, kEnvProbeSize >> mip);
        totalBytes += static_cast<usize>(w) * h * 4;
    }
    totalBytes *= 6;

    constexpr u8 kNeutralGrey = 0x14;
    std::vector<u8> pixels(totalBytes, 0);
    u8* cursor = pixels.data();
    for (i32 face = 0; face < 6; ++face) {
        for (i32 mip = 0; mip < kEnvProbeMipLevels; ++mip) {
            i32 w = std::max(1, kEnvProbeSize >> mip);
            i32 h = std::max(1, kEnvProbeSize >> mip);
            for (i32 i = 0; i < w * h; ++i) {
                cursor[i * 4 + 0] = kNeutralGrey;
                cursor[i * 4 + 1] = kNeutralGrey;
                cursor[i * 4 + 2] = kNeutralGrey;
                cursor[i * 4 + 3] = 0xFF;
            }
            cursor += static_cast<usize>(w) * h * 4;
        }
    }

    gfx::TextureDesc desc;
    desc.width     = kEnvProbeSize;
    desc.height    = kEnvProbeSize;
    desc.mipLevels = kEnvProbeMipLevels;
    desc.arraySize = 6;
    desc.isCube    = true;
    desc.format    = gfx::Format::R8G8B8A8_UNORM;
    desc.usage     = gfx::TextureUsage::ShaderResource;
    return gfx.CreateTexture(desc, pixels.data());
}

gfx::TextureHandle CreateDebugFacesEnvProbe(gfx::IGFXDevice& gfx) {

    constexpr u8 kFaceColors[6][3] = {
        { 0xFF, 0x00, 0x00 },
        { 0x00, 0xFF, 0x00 },
        { 0x00, 0x00, 0xFF },
        { 0xFF, 0xFF, 0x00 },
        { 0x00, 0xFF, 0xFF },
        { 0xFF, 0x00, 0xFF },
    };

    usize bytesPerFace = 0;
    for (i32 mip = 0; mip < kEnvProbeMipLevels; ++mip) {
        i32 w = std::max(1, kEnvProbeSize >> mip);
        i32 h = std::max(1, kEnvProbeSize >> mip);
        bytesPerFace += static_cast<usize>(w) * h * 4;
    }
    const i32 kNumCubes = 2;
    const i32 kNumLayers = 6 * kNumCubes;
    usize totalBytes = bytesPerFace * static_cast<usize>(kNumLayers);

    std::vector<u8> pixels(totalBytes, 0);
    u8* cursor = pixels.data();
    for (i32 layer = 0; layer < kNumLayers; ++layer) {
        const i32 face = layer % 6;
        const u8 r = kFaceColors[face][0];
        const u8 g = kFaceColors[face][1];
        const u8 b = kFaceColors[face][2];
        for (i32 mip = 0; mip < kEnvProbeMipLevels; ++mip) {
            i32 w = std::max(1, kEnvProbeSize >> mip);
            i32 h = std::max(1, kEnvProbeSize >> mip);
            for (i32 i = 0; i < w * h; ++i) {
                cursor[i * 4 + 0] = r;
                cursor[i * 4 + 1] = g;
                cursor[i * 4 + 2] = b;
                cursor[i * 4 + 3] = 0xFF;
            }
            cursor += static_cast<usize>(w) * h * 4;
        }
    }

    gfx::TextureDesc desc;
    desc.width     = kEnvProbeSize;
    desc.height    = kEnvProbeSize;
    desc.mipLevels = kEnvProbeMipLevels;
    desc.arraySize = kNumLayers;
    desc.isCube    = true;
    desc.format    = gfx::Format::R8G8B8A8_UNORM;
    desc.usage     = gfx::TextureUsage::ShaderResource;
    return gfx.CreateTexture(desc, pixels.data());
}

gfx::TextureHandle CreateStudioEnvProbe(gfx::IGFXDevice& gfx) {

    constexpr u8 kDiffR = 0x3C, kDiffG = 0x3C, kDiffB = 0x40;

    constexpr u8 kSideR = 0x45, kSideG = 0x48, kSideB = 0x4C;
    constexpr u8 kTopR  = 0xA8, kTopG  = 0xB0, kTopB  = 0xC0;
    constexpr u8 kBotR  = 0x14, kBotG  = 0x12, kBotB  = 0x10;

    struct FaceRGB { u8 r, g, b; };
    const FaceRGB diffColors[6] = {
        {kDiffR, kDiffG, kDiffB}, {kDiffR, kDiffG, kDiffB},
        {kDiffR, kDiffG, kDiffB}, {kDiffR, kDiffG, kDiffB},
        {kDiffR, kDiffG, kDiffB}, {kDiffR, kDiffG, kDiffB},
    };
    const FaceRGB specColors[6] = {
        {kSideR, kSideG, kSideB},
        {kSideR, kSideG, kSideB},
        {kTopR,  kTopG,  kTopB },
        {kBotR,  kBotG,  kBotB },
        {kSideR, kSideG, kSideB},
        {kSideR, kSideG, kSideB},
    };

    usize bytesPerFace = 0;
    for (i32 mip = 0; mip < kEnvProbeMipLevels; ++mip) {
        i32 w = std::max(1, kEnvProbeSize >> mip);
        i32 h = std::max(1, kEnvProbeSize >> mip);
        bytesPerFace += static_cast<usize>(w) * h * 4;
    }
    const i32 kNumCubes = 2;
    const i32 kNumLayers = 6 * kNumCubes;
    usize totalBytes = bytesPerFace * static_cast<usize>(kNumLayers);

    std::vector<u8> pixels(totalBytes, 0);
    u8* cursor = pixels.data();
    for (i32 layer = 0; layer < kNumLayers; ++layer) {
        const i32 cube = layer / 6;
        const i32 face = layer % 6;
        const FaceRGB& c = (cube == 0) ? diffColors[face] : specColors[face];
        for (i32 mip = 0; mip < kEnvProbeMipLevels; ++mip) {
            i32 w = std::max(1, kEnvProbeSize >> mip);
            i32 h = std::max(1, kEnvProbeSize >> mip);
            for (i32 i = 0; i < w * h; ++i) {
                cursor[i * 4 + 0] = c.r;
                cursor[i * 4 + 1] = c.g;
                cursor[i * 4 + 2] = c.b;
                cursor[i * 4 + 3] = 0xFF;
            }
            cursor += static_cast<usize>(w) * h * 4;
        }
    }

    gfx::TextureDesc desc;
    desc.width     = kEnvProbeSize;
    desc.height    = kEnvProbeSize;
    desc.mipLevels = kEnvProbeMipLevels;
    desc.arraySize = kNumLayers;
    desc.isCube    = true;
    desc.format    = gfx::Format::R8G8B8A8_UNORM;
    desc.usage     = gfx::TextureUsage::ShaderResource;
    return gfx.CreateTexture(desc, pixels.data());
}

bool WriteDebugFacesDds(const std::string& outPath) {

    using whiteout::textures::Texture;
    using whiteout::textures::PixelFormat;

    constexpr u32 kSize     = 128;
    constexpr u32 kMips     = 8;
    constexpr u32 kNumCubes = 2;

    constexpr u8 kFaceCenter[6][4] = {
        { 0xFF, 0x00, 0x00, 0xFF },
        { 0x00, 0xFF, 0x00, 0xFF },
        { 0x00, 0x00, 0xFF, 0xFF },
        { 0xFF, 0xFF, 0x00, 0xFF },
        { 0x00, 0xFF, 0xFF, 0xFF },
        { 0xFF, 0x00, 0xFF, 0xFF },
    };

    enum EdgeIdx : i32 {
        E_XY_pp = 0, E_XY_pn, E_XY_np, E_XY_nn,
        E_XZ_pp, E_XZ_pn, E_XZ_np, E_XZ_nn,
        E_YZ_pp, E_YZ_pn, E_YZ_np, E_YZ_nn,
    };
    constexpr u8 kEdgeColors[12][4] = {
        { 0xFF, 0xA0, 0x40, 0xFF },
        { 0xA0, 0x40, 0xFF, 0xFF },
        { 0x40, 0xC0, 0xFF, 0xFF },
        { 0x80, 0x60, 0x40, 0xFF },
        { 0xFF, 0x80, 0xC0, 0xFF },
        { 0x80, 0xFF, 0x40, 0xFF },
        { 0xC0, 0x80, 0x40, 0xFF },
        { 0x40, 0x60, 0x80, 0xFF },
        { 0x80, 0xFF, 0xC0, 0xFF },
        { 0xC0, 0x80, 0xFF, 0xFF },
        { 0x80, 0x00, 0x20, 0xFF },
        { 0x00, 0x60, 0x20, 0xFF },
    };

    constexpr i32 kFaceEdges[6][4] = {
          { E_XY_pp, E_XY_pn, E_XZ_pp, E_XZ_pn },
          { E_XY_np, E_XY_nn, E_XZ_nn, E_XZ_np },
          { E_YZ_pn, E_YZ_pp, E_XY_np, E_XY_pp },
          { E_YZ_np, E_YZ_nn, E_XY_nn, E_XY_pn },
          { E_YZ_pp, E_YZ_np, E_XZ_np, E_XZ_pp },
          { E_YZ_pn, E_YZ_nn, E_XZ_pn, E_XZ_nn },
    };

    Texture tex = Texture::createCubeArray(PixelFormat::RGBA8, kSize,
                                           kNumCubes, kMips);

    const u32 layers = tex.layerCount();
    for (u32 layer = 0; layer < layers; ++layer) {
        const u32 face    = layer % 6u;
        const u8* center  = kFaceCenter[face];
        const u8* edgeTop = kEdgeColors[kFaceEdges[face][0]];
        const u8* edgeBot = kEdgeColors[kFaceEdges[face][1]];
        const u8* edgeLf  = kEdgeColors[kFaceEdges[face][2]];
        const u8* edgeRt  = kEdgeColors[kFaceEdges[face][3]];

        for (u32 mip = 0; mip < kMips; ++mip) {
            auto dst = tex.mipData(mip, layer);
            const u32 w = std::max(1u, kSize >> mip);
            const u32 h = std::max(1u, kSize >> mip);

            const u32 border = std::max(1u, w / 8);

            for (u32 y = 0; y < h; ++y) {
                for (u32 x = 0; x < w; ++x) {
                    const u32 distTop   = y;
                    const u32 distBot   = (h - 1) - y;
                    const u32 distLf    = x;
                    const u32 distRt    = (w - 1) - x;
                    const u32 distMin   = std::min({distTop, distBot, distLf, distRt});
                    const u8* c = center;
                    if (distMin < border) {

                        if      (distMin == distTop) c = edgeTop;
                        else if (distMin == distBot) c = edgeBot;
                        else if (distMin == distLf)  c = edgeLf;
                        else                         c = edgeRt;
                    }
                    const usize idx = (static_cast<usize>(y) * w + x) * 4;
                    dst[idx + 0] = c[0];
                    dst[idx + 1] = c[1];
                    dst[idx + 2] = c[2];
                    dst[idx + 3] = c[3];
                }
            }
        }
    }

    whiteout::textures::dds::Writer writer(
        whiteout::textures::dds::Writer::WriteMode::Lenient);
    try {
        writer.write(outPath, tex);
    } catch (...) {
        DbgLogf("[WDEX IBL] WriteDebugFacesDds FAILED to write %s\n", outPath.c_str());
        return false;
    }
    if (writer.hasIssues()) {
        DbgLogf("[WDEX IBL] WriteDebugFacesDds wrote with issues:\n");
        for (const auto& issue : writer.getIssues()) {
            DbgLogf("[WDEX IBL]   %s\n", issue.c_str());
        }
    }
    DbgLogf("[WDEX IBL] WriteDebugFacesDds OK -> %s\n", outPath.c_str());
    return true;
}

}

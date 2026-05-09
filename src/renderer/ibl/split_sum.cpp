#include "split_sum.h"

#include "whiteout/flakes/types.h"

#include <cmath>
#include <algorithm>
#include <numbers>

namespace whiteout::flakes::renderer::ibl {

namespace {

inline f32 RadicalInverseVdC(u32 bits) {
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return static_cast<f32>(bits) * 2.3283064365386963e-10f;
}

inline void Hammersley(f32& x, f32& y, u32 i, u32 count) {
    x = static_cast<f32>(i) / static_cast<f32>(count);
    y = RadicalInverseVdC(i);
}

inline void ImportanceSampleGGX(f32 Xi_x, f32 Xi_y, f32 roughness,
                                f32 N_x, f32 N_y, f32 N_z,
                                f32& H_x, f32& H_y, f32& H_z) {
    const f32 a     = roughness * roughness;
    const f32 phi   = 2.0f * std::numbers::pi_v<f32> * Xi_x;
    const f32 cosTh = std::sqrt((1.0f - Xi_y) / (1.0f + (a * a - 1.0f) * Xi_y));
    const f32 sinTh = std::sqrt(std::max(0.0f, 1.0f - cosTh * cosTh));

    const f32 Ht_x = std::cos(phi) * sinTh;
    const f32 Ht_y = std::sin(phi) * sinTh;
    const f32 Ht_z = cosTh;

    const f32 up_x = (std::fabs(N_z) >= 0.999f) ? 1.0f : 0.0f;
    const f32 up_y = 0.0f;
    const f32 up_z = (std::fabs(N_z) >= 0.999f) ? 0.0f : 1.0f;

    f32 T_x = up_y * N_z - up_z * N_y;
    f32 T_y = up_z * N_x - up_x * N_z;
    f32 T_z = up_x * N_y - up_y * N_x;
    const f32 Tlen = std::sqrt(T_x * T_x + T_y * T_y + T_z * T_z);
    if (Tlen > 1e-6f) { T_x /= Tlen; T_y /= Tlen; T_z /= Tlen; }

    const f32 B_x = N_y * T_z - N_z * T_y;
    const f32 B_y = N_z * T_x - N_x * T_z;
    const f32 B_z = N_x * T_y - N_y * T_x;

    f32 S_x = Ht_x * T_x + Ht_y * B_x + Ht_z * N_x;
    f32 S_y = Ht_x * T_y + Ht_y * B_y + Ht_z * N_y;
    f32 S_z = Ht_x * T_z + Ht_y * B_z + Ht_z * N_z;

    const f32 Slen = std::sqrt(S_x * S_x + S_y * S_y + S_z * S_z);
    if (Slen > 1e-6f) { S_x /= Slen; S_y /= Slen; S_z /= Slen; }
    H_x = S_x; H_y = S_y; H_z = S_z;
}

inline f32 GSmithGGX(f32 NoV, f32 NoL, f32 roughness) {
    const f32 k = (roughness * roughness) * 0.5f;
    const f32 GV = NoV / (NoV * (1.0f - k) + k);
    const f32 GL = NoL / (NoL * (1.0f - k) + k);
    return GV * GL;
}

inline f32 ClampRoughness(f32 r) {
    return (r * 255.0f + 1.0f) / 256.0f;
}

}

void GenerateSplitSumLut(i32 size, i32 sampleCount,
                         std::vector<u8>& outPixels) {
    outPixels.assign(static_cast<usize>(size) * size * 4, 0);

    const f32 N_x = 0.0f, N_y = 0.0f, N_z = 1.0f;

    for (i32 iy = 0; iy < size; ++iy) {
        const f32 roughness =
            ClampRoughness((static_cast<f32>(iy) + 0.5f) / static_cast<f32>(size));

        for (i32 ix = 0; ix < size; ++ix) {
            const f32 NoV = (static_cast<f32>(ix) + 0.5f) / static_cast<f32>(size);

            const f32 V_x = std::sqrt(std::max(0.0f, 1.0f - NoV * NoV));
            const f32 V_y = 0.0f;
            const f32 V_z = NoV;

            f32 A = 0.0f;
            f32 B = 0.0f;

            for (u32 i = 0; i < static_cast<u32>(sampleCount); ++i) {
                f32 Xi_x, Xi_y;
                Hammersley(Xi_x, Xi_y, i, static_cast<u32>(sampleCount));

                f32 H_x, H_y, H_z;
                ImportanceSampleGGX(Xi_x, Xi_y, roughness,
                                    N_x, N_y, N_z,
                                    H_x, H_y, H_z);

                const f32 VoH = V_x * H_x + V_y * H_y + V_z * H_z;

                f32 L_x = 2.0f * VoH * H_x - V_x;
                f32 L_y = 2.0f * VoH * H_y - V_y;
                f32 L_z = 2.0f * VoH * H_z - V_z;
                const f32 Llen = std::sqrt(L_x*L_x + L_y*L_y + L_z*L_z);
                if (Llen > 1e-6f) { L_x /= Llen; L_y /= Llen; L_z /= Llen; }

                const f32 NoL  = std::max(0.0f, L_z);
                const f32 NoH  = std::max(0.0f, H_z);
                const f32 VoHc = std::max(0.0f, VoH);

                if (NoL > 0.0f) {
                    const f32 G  = GSmithGGX(NoV, NoL, roughness);

                    f32 G_Vis = (G * VoHc) / (NoH * NoV);
                    G_Vis = std::clamp(G_Vis, 0.0f, 1.0f);
                    const f32 Fc = std::pow(1.0f - VoHc, 5.0f);

                    A += (1.0f - Fc) * G_Vis;
                    B += Fc * G_Vis;
                }
            }

            A /= static_cast<f32>(sampleCount);
            B /= static_cast<f32>(sampleCount);

            auto pack8 = [](f32 v) -> u8 {
                const f32 s = std::clamp(v, 0.0f, 1.0f);
                return static_cast<u8>(s * 255.0f);
            };

            const usize idx = (static_cast<usize>(iy) * size + ix) * 4;
            outPixels[idx + 0] = 0;
            outPixels[idx + 1] = pack8(B);
            outPixels[idx + 2] = pack8(A);
            outPixels[idx + 3] = 0;
        }
    }
}

gfx::TextureHandle CreateSplitSumLutTexture(gfx::IGFXDevice& gfx) {
    std::vector<u8> pixels;
    GenerateSplitSumLut(kSplitSumSize, kSplitSumSamples, pixels);

    gfx::TextureDesc desc;
    desc.width  = kSplitSumSize;
    desc.height = kSplitSumSize;

    desc.format = gfx::Format::B8G8R8A8_UNORM;
    desc.usage  = gfx::TextureUsage::ShaderResource;
    return gfx.CreateTexture(desc, pixels.data());
}

}

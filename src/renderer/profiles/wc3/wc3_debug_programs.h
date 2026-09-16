#pragma once

// The Warcraft III debug pixel programs (shaders/wc3_debug.slang), the
// constant buffer they read at PS b3, and the mesh overlay's vertex stages
// (shaders/wc3_overlay.slang). Created on first use, so a session that never
// opens a debug view creates nothing and draws exactly as before.

#include "bls/bls_pso_builder.h"
#include "core/debug_view.h"
#include "gfx/gfx.h"
#include "whiteout/flakes/types.h"

#include <array>
#include <map>

namespace whiteout::flakes::renderer::profiles::wc3 {

// DebugViewData::ctl.w in wc3_debug.slang.
inline constexpr u32 kWc3DebugPbr = 1u;
inline constexpr u32 kWc3DebugTeamLayer = 2u;
inline constexpr u32 kWc3DebugAoMap = 4u;
inline constexpr u32 kWc3DebugLit = 8u;
inline constexpr u32 kWc3DebugAlphaTest = 16u;

class Wc3DebugPrograms {
public:
    explicit Wc3DebugPrograms(gfx::IGFXDevice* gfx) : gfx_(gfx) {}

    /// @brief Destroy everything while the device is alive.
    void Release();

    /// @brief The HD / Crystal / SD-on-HD program. A lighting view may only
    ///        name the shadow maps this frame actually bound. `dxil` asks for
    ///        the SM 6 twin, which a retail vertex program loaded from the
    ///        d3d12 bundle needs: D3D12 will not link it with SM 5.
    gfx::ShaderHandle Hd(bool cascades, bool pointShadows, bool dxil = false);
    /// @brief The SD classic program.
    gfx::ShaderHandle Sd(bool dxil = false);
    /// @brief Whether a retail vertex program is DXIL (the d3d12 bundle).
    static bool IsDxil(const bls::BlsProgram* program);

    /// @brief The mesh overlay's vertex stage for an HD draw, skinned the way
    ///        its retail permutation skins. Reads MeshOverlayData at VS b0.
    gfx::ShaderHandle HdOverlay(bool skinned, bool boneBuffer);
    /// @brief The same for an SD draw. Reads MeshOverlayData at VS b1.
    gfx::ShaderHandle SdOverlay(bool skinned);

    /// @brief The SD normal views' pipeline. sd_highspec_vs hands the pixel
    ///        stage no normal, so these draw SD geometry through a vertex
    ///        stage of their own, built off the BLS request the real draw
    ///        resolved; its ATTRn layouts name the retail program.
    gfx::PipelineHandle SdNormalPso(const bls::PsoRequest& req, bool skinned);

    /// @brief Write one draw's constants; returns the buffer for PS b3.
    gfx::BufferHandle Write(const core::DebugViewCbData& data);

private:
    void Init();

    gfx::IGFXDevice* gfx_ = nullptr;
    bool initTried_ = false;
    // Indexed by cascades | pointShadows << 1.
    std::array<gfx::ShaderHandle, 4> hd_{gfx::ShaderHandle::Invalid, gfx::ShaderHandle::Invalid,
                                         gfx::ShaderHandle::Invalid, gfx::ShaderHandle::Invalid};
    gfx::ShaderHandle sd_ = gfx::ShaderHandle::Invalid;
    // The SM 6 twins, Invalid where no DXIL was built.
    std::array<gfx::ShaderHandle, 4> hdDxil_{gfx::ShaderHandle::Invalid, gfx::ShaderHandle::Invalid,
                                             gfx::ShaderHandle::Invalid,
                                             gfx::ShaderHandle::Invalid};
    gfx::ShaderHandle sdDxil_ = gfx::ShaderHandle::Invalid;
    gfx::ShaderHandle sdNormalVs_ = gfx::ShaderHandle::Invalid;
    gfx::ShaderHandle sdNormalVsSkinned_ = gfx::ShaderHandle::Invalid;
    gfx::ShaderHandle sdNormalPs_ = gfx::ShaderHandle::Invalid;
    // Rigid, constant palette, bone buffer.
    std::array<gfx::ShaderHandle, 3> hdOverlay_{
        gfx::ShaderHandle::Invalid, gfx::ShaderHandle::Invalid, gfx::ShaderHandle::Invalid};
    std::array<gfx::ShaderHandle, 2> sdOverlay_{gfx::ShaderHandle::Invalid,
                                                gfx::ShaderHandle::Invalid};
    gfx::BufferHandle cb_ = gfx::BufferHandle::Invalid;

    struct SdNormalKey {
        bool skinned = false;
        u32 alpha = 0;
        u32 disables = 0;
        gfx::Format rtv = gfx::Format::Unknown;
        gfx::Format dsv = gfx::Format::Unknown;
        u32 extraRtvCount = 0;
        gfx::Format extra0 = gfx::Format::Unknown;
        gfx::Format extra1 = gfx::Format::Unknown;
        gfx::Format extra2 = gfx::Format::Unknown;
        auto operator<=>(const SdNormalKey&) const = default;
    };
    std::map<SdNormalKey, gfx::PipelineHandle> sdNormalPsos_;
};

} // namespace whiteout::flakes::renderer::profiles::wc3

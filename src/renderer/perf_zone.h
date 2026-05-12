#pragma once

// =============================================================================
// Renderer-side scoped profiler zones.
//
// Two macros:
//   WDX_CPU_ZONE(name)              — CPU-side ZoneScopedN; no-op when Tracy
//                                     is disabled at build time.
//   WDX_GPU_ZONE(cmd, name)         — GPU-side scope on a gfx::IGFXCommandList.
//                                     Forwards to cmd->BeginGpuZone /
//                                     EndGpuZone via RAII so the zone closes
//                                     when the scope exits.
//
// `name` should be a string literal in normal use — Tracy copies it into its
// arena, but the renderer pays strlen on every BeginGpuZone call so prefer
// literals over std::string.
// =============================================================================

#include "gfx/gfx.h"

#if defined(TRACY_ENABLE)
#  include <tracy/Tracy.hpp>
#  define WDX_CPU_ZONE(name) ZoneScopedN(name)
#else
#  define WDX_CPU_ZONE(name) ((void)0)
#endif

namespace whiteout::flakes::renderer::perf {

class GpuZoneScope {
public:
    GpuZoneScope(gfx::IGFXCommandList* cmd, const char* name) noexcept
        : cmd_(cmd) {
        if (cmd_) cmd_->BeginGpuZone(name);
    }
    ~GpuZoneScope() {
        if (cmd_) cmd_->EndGpuZone();
    }
    GpuZoneScope(const GpuZoneScope&)            = delete;
    GpuZoneScope& operator=(const GpuZoneScope&) = delete;

private:
    gfx::IGFXCommandList* cmd_ = nullptr;
};

}

#define WDX_GPU_ZONE_CONCAT_INNER(a, b) a##b
#define WDX_GPU_ZONE_CONCAT(a, b)       WDX_GPU_ZONE_CONCAT_INNER(a, b)
#define WDX_GPU_ZONE(cmd, name) \
    ::whiteout::flakes::renderer::perf::GpuZoneScope \
        WDX_GPU_ZONE_CONCAT(wdx_gpu_zone_, __LINE__){cmd, name}

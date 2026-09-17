#pragma once

// ============================================================================
// What the process has to settle with its platform before any device exists:
// a UTF-8 argv, the MoltenVK environment, the pipeline cache and the backend a
// platform can actually start.
// ============================================================================

#include "whiteout/flakes/gfx_types.h"

#include <string>
#include <vector>

namespace whiteout::flakes::renderer {
class RenderSettings;
}

namespace whiteout::flakes::platform {

/// The arguments after the program name, as UTF-8. On Windows they are re-read
/// from the wide command line: main's argv is in the ANSI code page, so a path
/// with non-Latin characters is already mangled by the time it arrives.
std::vector<std::string> Utf8Arguments(int argc, char* argv[]);

/// macOS: point the Vulkan loader at the bundled MoltenVK ICD and opt into
/// Metal argument buffers. Before any Vulkan call. A no-op elsewhere.
void ConfigureVulkanEnvironment();

/// The backend this platform can start: Linux ships Vulkan alone, macOS
/// Vulkan, Metal and (with Dawn) WebGPU. Also coerces the stored default, so
/// saving settings does not carry an unusable choice forward.
gfx::GfxApi CoerceBackendToPlatform(gfx::GfxApi requested, renderer::RenderSettings& settings);

/// Where the Vulkan pipeline cache lives — beside the exe on Windows, a per-user
/// cache directory on Linux and macOS, where the AppImage and the .app bundle
/// are read-only — seeding `pso_trace.bin` from the shipped copy the first
/// time. Hands the path to the gfx layer.
void ConfigurePipelineCache();

} // namespace whiteout::flakes::platform

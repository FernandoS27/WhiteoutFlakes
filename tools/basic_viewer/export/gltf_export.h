#pragma once

// ============================================================================
// "Export to glTF" — writing the model on screen as glTF 2.0 / GLB.
//
// The MDX export pointed out of the Blizzard family: any model WEM reads is
// converted to a document and lowered to metallic-roughness (GLTF_DESIGN §5),
// and because it goes model → WEM → glTF it works for every format that can
// export WEM, for free.
//
// The library half (`GltfConverter::toGltf`) never sees a pixel — its images
// are suggested-URI references. This driver is the other half: it resolves
// each `TextureRef` through the scene's provider, decodes it with the in-house
// parsers, re-encodes as PNG and embeds it in the GLB (or writes it beside a
// `.gltf`). Unresolvable references keep their suggested URI and the file
// still validates. A StarCraft II or Heroes surface goes through Export to
// MDX's bake first (sc2_pbr_export.h): its specular, gloss and team colour have
// no metallic-roughness slot to move into, only pixels to become.
//
// Free of ImGui and of ViewerApp on purpose, like its MDX twin — the dialog is
// one caller and `--export-gltf` is the other.
// ============================================================================

#include "export_common.h"
#include "export_options.h"

#include <whiteout/models/wem/profile.h>

namespace whiteout::flakes {

/// @brief One export.
struct GltfExportRequest {
    ExportSubject subject;
    GltfExportOptions options;
    /// The team colour a StarCraft II or Heroes surface is baked in, packed like
    /// `Actor::teamColor` (r | g<<8 | b<<16) and snapped to the game's palette
    /// as the shading does. glTF has no team slot, so the export wears one.
    u32 teamColor = 0x000000FFu;
};

/// @brief What one export did.
struct GltfExportReport : ConversionReport {
    /// The profile whose materials crossed: Reforged wherever a `.mdx` carries
    /// it or the bake derived it, else the profile the model was authored in.
    ::whiteout::models::wem::ProfileId profile = ::whiteout::models::wem::ProfileId::Count;
    /// `exported` counts embedded images too; a failure keeps its URI.
    TextureExportCounters textures;
};

/// @brief Convert @p request's model to glTF and write it.
GltfExportReport ExportModelAsGltf(const GltfExportRequest& request);

} // namespace whiteout::flakes

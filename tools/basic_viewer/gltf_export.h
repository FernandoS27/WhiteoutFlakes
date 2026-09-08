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
// still validates.
//
// Free of ImGui and of ViewerApp on purpose, like its MDX twin — the dialog is
// one caller and `--export-gltf` is the other.
// ============================================================================

#include "io/wem/wem_export.h"
#include "whiteout/flakes/model_source.h"
#include "whiteout/flakes/types.h"

#include <whiteout/models/wem/diagnostics.h>

#include <filesystem>
#include <string>

namespace whiteout::flakes {

namespace io {
class IContentProvider;
}

namespace wem = ::whiteout::models::wem;

/// @brief One export.
struct GltfExportRequest {
    /// The model on screen. Must be one `io::CanExportModelToWem` accepts.
    renderer::model::IModelSource* source = nullptr;
    /// Where every texture reference resolves. Null exports the model alone.
    io::IContentProvider* provider = nullptr;

    std::filesystem::path outPath;
    /// Names the document, and the fallback stem for an id-addressed texture.
    std::string modelName;

    /// Which carried profile's materials cross. `Count` takes the document's
    /// own default — the profile the model was authored in.
    wem::ProfileId profile = wem::ProfileId::Count;

    /// `.glb` (one self-contained file) against `.gltf` + `.bin` + images.
    bool binary = true;

    /// Resolve, decode and PNG-encode the textures. Off leaves suggested URIs
    /// in place — the file still validates, the DCC shows placeholders.
    bool exportTextures = true;

    /// Multiplies every length on the way out. glTF says metres and WEM keeps
    /// authored game units; 1.0 exports as-authored and the knob is for users
    /// targeting a metric engine (GLTF_DESIGN §3).
    f32 scale = 1.0f;
};

/// @brief What one export did, in the shape a log line and a dialog both want.
struct GltfExportReport {
    bool ok = false;
    std::string error;

    /// The converter the model came through — "m2", "m3", "d3", "mdx".
    std::string formatId;
    /// The profile whose materials crossed.
    wem::ProfileId profile = wem::ProfileId::Count;

    int texturesExported = 0; ///< Embedded or written beside the file.
    int texturesFailed = 0;   ///< Unresolvable or undecodable; URI kept.

    /// Conversion + crossing diagnostics. Never empty on success: a lossy
    /// crossing always has something to say (§9's loss tables).
    wem::Diagnostics diagnostics;
};

/// @brief Convert @p request's model to glTF and write it.
GltfExportReport ExportModelAsGltf(const GltfExportRequest& request);

} // namespace whiteout::flakes

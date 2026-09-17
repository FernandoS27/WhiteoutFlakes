#pragma once

// ============================================================================
// What each model conversion is asked to do, filled the same way by its dialog
// and by its command-line flags. Device-free, so the command-line parser's G0
// test can hold them.
// ============================================================================

#include <whiteout/models/wem/profile.h>

namespace whiteout::flakes {

/// Through WEM into Warcraft III (`--export-mdx`).
struct MdxExportOptions {
    ::whiteout::models::wem::ProfileId profile = ::whiteout::models::wem::ProfileId::Wc3Reforged;
    /// Convert the textures to the container the profile reads and write them beside it.
    bool textures = true;
};

/// Through WEM into StarCraft II (`--export-m3`). The last five matter only for
/// a Warcraft III source (WC3_SD_MATERIAL_TO_SC2_DESIGN.md §5).
struct M3ExportOptions {
    ::whiteout::models::wem::ProfileId profile = ::whiteout::models::wem::ProfileId::Sc2;
    bool textures = true;
    /// A composite section for every approximate pass fold.
    bool exactPasses = false;
    /// Bake keyed alpha binary over a team plate.
    bool sharpenTeamKey = false;
    /// Name War3 (Mod)'s copy of a texture where it ships the same picture.
    bool war3ModTextures = false;
    /// Particle and ribbon emitters.
    bool effects = true;
    /// Ref_Origin / Ref_Overhead / Ref_Center / Ref_Target / Vol_Target.
    bool standardRefs = true;
};

/// Out to glTF 2.0 (`--export-gltf`).
struct GltfExportOptions {
    /// One `.glb`; otherwise `.gltf` + `.bin` + images. The path's extension decides.
    bool binary = true;
    /// Embed (or write beside it) the textures, re-encoded as PNG.
    bool textures = true;
};

/// A `.m3` written back in its own format (`--save-m3`). Everything off, like
/// the dialog, so the plain save writes the model as it stands.
struct M3SaveOptions {
    /// Fold the attached `.m3a` files into the model's own animation chunks.
    bool mergeAnimations = false;
    /// Retarget a Heroes model so StarCraft II loads it (MODL v29, MADD reversed).
    bool convertToSc2 = false;
    /// Copy the textures the layers reference beside it.
    bool textures = false;
};

} // namespace whiteout::flakes

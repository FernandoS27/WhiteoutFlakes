#pragma once

// ============================================================================
// Every file extension the viewer opens as a document, and what each one is.
//
// The one table File > Open, the startup picker, the loader and the game
// follow all read — adding a format is a row here (plus a loader branch when it
// is a new ModelKind). Device-free, so model_formats_test covers it.
// ============================================================================

#include "whiteout/flakes/enums.h" // ProductId

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace whiteout::flakes {

enum class ModelKind : u8 {
    /// `.mdx` / `.mdl`: probed for HD layers, written back by Save As, read
    /// through the Warcraft III art overlay.
    Wc3Model,
    /// `.pkb` / `.pkfx`: a standalone PopcornFX effect, not a model.
    Effect,
    /// `.m2` / `.m3`: another Blizzard game's model. Drawn in SD by its own
    /// profile; no MDX to write, no overlay to re-resolve.
    ForeignModel,
    /// `.acr` / `.app`: Diablo III.
    D3Actor,
    /// `.wem`: opened as whichever profile the user (or the file) picks.
    Wem,
    /// `.gltf` / `.glb`: opened through the WEM machinery, as a Generic document.
    Gltf,
};

struct ModelFormat {
    std::string_view extension; ///< Lower case, with the dot.
    ModelKind kind;
    /// The game whose storage the document's content resolves against.
    /// Neutral for the interchange formats, where the picked profile decides.
    ProductId game;
    /// The Open dialog row it is listed under; empty lists it under
    /// "All supported" only.
    std::string_view filterGroup;
    /// The CMake option that compiles its loader in, empty when always built.
    std::string_view buildOption;
    /// Whether this build can load it. A format that is not compiled in is
    /// kept out of the Open dialogs: offering a file the loader will refuse is
    /// worse than not listing it.
    bool compiled;
};

std::span<const ModelFormat> ModelFormats();

/// By extension, case-insensitively. Null for anything the viewer does not open.
const ModelFormat* FindModelFormat(const std::filesystem::path& path);

inline bool IsModelKind(const std::filesystem::path& path, ModelKind kind) {
    const ModelFormat* f = FindModelFormat(path);
    return f && f->kind == kind;
}

inline bool IsInterchangeKind(ModelKind kind) {
    return kind == ModelKind::Wem || kind == ModelKind::Gltf;
}

/// One native file-dialog filter row: a name and NFD's comma-separated
/// extension list ("mdx,mdl").
struct FileFilter {
    std::string name;
    std::string extensions;
};

/// "All supported" first, then one row per filter group in table order.
/// Compiled formats only.
std::vector<FileFilter> OpenDialogFilters();

} // namespace whiteout::flakes

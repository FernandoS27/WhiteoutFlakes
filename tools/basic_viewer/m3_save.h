// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

// ============================================================================
// "Save As" for a StarCraft II / Heroes of the Storm `.m3`.
//
// `m3_export.h`'s opposite number. That one converts a foreign model INTO an
// `.m3` through WEM, deriving a material set the source never had; this writes
// a model that is already `.m3` back out in its own format, straight through
// `m3::Writer` with no interchange hop in between. Nothing is derived, so
// nothing is lost that the options below do not lose on purpose:
//
//   * **Merge `.m3a` animations.** Nothing in a `.m3` names its external
//     animation files — StarCraft II reads them off the model's catalog entry
//     and merges each into one global sequence and container space, binding
//     tracks by `animId` alone. The host has done that naming already (the
//     Animation window's attach list), so the merge here is the same
//     concatenation `M3AnimTables::Build` lays out for playback, made
//     structural: the merged file plays the attached sequences with no catalog
//     entry and no sibling files.
//   * **Convert to StarCraft II.** Heroes-only models are Heroes-only for four
//     reasons and `m3::toStarCraft2` reverses all of them (MADD back to
//     StandardMaterial, the REF_ v3 back-reference dropped, MODL lowered to
//     v29). Version support is enforced per file rather than per chunk, so one
//     out-of-range chunk is the difference between a model StarCraft II loads
//     and one it refuses outright.
//   * **Export the textures.** A `.m3` has no texture table — layers carry
//     their own path strings, resolvable only while the model's game is the
//     one mounted. This copies each referenced file beside the saved model
//     under its authored relative path, bytes as the storage serves them; the
//     model itself is not repointed, so this one loses nothing.
// ============================================================================

#include "whiteout/flakes/types.h"

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace whiteout::flakes {

namespace io {
class IContentProvider;
class M3ModelAdapter;
}

/// @brief One save.
struct M3SaveRequest {
    /// The model on screen. Must be the `.m3` adapter itself — this writer has
    /// no conversion step to reach any other format through.
    const io::M3ModelAdapter* source = nullptr;
    /// Where the layers' texture paths resolve. Null saves the model alone.
    io::IContentProvider* provider = nullptr;

    std::filesystem::path outPath;

    /// Fold every attached `.m3a` into the written model's own animation
    /// chunks. Off writes the model's sequences alone, as the file shipped.
    bool mergeAnimations = false;

    /// Retarget for StarCraft II — MODL v29 and no chunk above what its loader
    /// accepts. A model StarCraft II already loads is written unchanged.
    bool convertToSc2 = false;

    /// Write every texture the layers reference beside the model, each under
    /// its own relative path. Bytes are copied as the provider serves them —
    /// nothing in the model is repointed, so the save stays faithful.
    bool exportTextures = false;
};

/// @brief What one save did, in the shape a log line and a dialog both want.
struct M3SaveReport {
    bool ok = false;
    std::string error;

    /// MODL version written — 29 for StarCraft II, 30 for Heroes.
    i32 version = 0;

    std::size_t mergedFiles = 0;
    std::size_t mergedSequences = 0;

    /// Whether the StarCraft II retarget actually rewrote anything, as opposed
    /// to finding a model that already loaded there.
    bool retargeted = false;

    int texturesExported = 0;
    int texturesSkipped = 0; ///< Already present at the target.
    int texturesFailed = 0;  ///< Unresolvable or unwritable.

    /// What the merge and the retarget could not carry. Populated on success
    /// too — that is when it is worth reading.
    std::vector<std::string> lossy;
};

/// @brief Write @p request's model as `.m3`.
M3SaveReport SaveModelAsM3(const M3SaveRequest& request);

} // namespace whiteout::flakes

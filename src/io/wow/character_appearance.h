#pragma once

// ============================================================================
// CharacterAppearance — choices in, geosets and composited textures out.
//
// Two steps, kept apart because only the first needs the databases and only the
// second needs pixels:
//
//   ResolveAppearance   the chosen choices → which geosets draw, and which
//                       `.blp` goes into which layer of which composite
//   ComposeCharacter    those layers → one RGBA sheet per M2 texture type
//
// The composite is what `CCharacterComponent::CreateBaseTexture` (0x100340c00)
// allocates and `PasteToSection` (0x10034b3f0) fills: one texture per slot the
// model leaves blank, built by pasting section-sized pieces into it. Retail
// moved the *source* of those pieces from CharSections to ChrCustomization but
// not the shape of the operation — a rect per section, layered in order.
//
// One detail carried over from PasteToSection rather than guessed: when the
// source is bigger than the section it targets, the client walks down mip
// levels until the widths match (`while (w > sectionWidth) { ++mip; w >>= 1; }`)
// and pastes 1:1; when it is smaller it scales up (`PasteScale`). Both come out
// as "resample the source to the section rect", which is what Blit does.
// ============================================================================

#include "io/wow/character_geosets.h"
#include "io/wow/chr_customization_table.h"
#include "whiteout/flakes/types.h"

#include <functional>
#include <span>
#include <vector>

namespace whiteout::flakes::io::wow {

/// One `.blp` going into one layer of one composite.
struct CompositePaste {
    u32 textureType = 0; ///< Which composite — the `M2Texture::type` it binds to.
    u32 layer = 0;
    u32 blendMode = 0;
    i64 sectionMask = -1;
    u32 textureFileId = 0;
};

/// A second model the chosen appearance puts on the character, posed from its
/// skeleton. See `ChoiceElement::skinnedModelFileId`.
struct SkinnedModelRef {
    u32 fileId = 0;
    i32 geoset = -1; ///< The one `skinSectionId` of that model this choice wants.
};

/// Everything a choice set produces.
struct ResolvedAppearance {
    CharacterGeosetSelection geosets;
    std::vector<CompositePaste> pastes; ///< Sorted by (textureType, layer).
    /// One entry per active choice that names one; several choices can name
    /// geosets of the *same* file, which is one model wearing two of its parts.
    std::vector<SkinnedModelRef> skinnedModels;
};

/// The first choice of every option — what the character creator opens on, and
/// what a viewer showing a model with no appearance picked should use.
std::vector<u32> DefaultChoices(const ChrModelInfo& model);

/// Turn @p choiceIds into geosets and a paste list.
///
/// An element carrying `relatedChoiceId` applies only when that other choice is
/// also selected: face textures are per skin colour, and the table says so by
/// listing one element per (face, skin) pair. Selecting a face without its skin
/// colour therefore contributes nothing, which is correct rather than a bug.
ResolvedAppearance ResolveAppearance(const ChrCustomizationTable& tables,
                                     const ChrModelInfo& model, std::span<const u32> choiceIds);

/// One finished sheet, ready to stage as a texture.
struct ComposedTexture {
    u32 textureType = 0;
    u32 width = 0, height = 0;
    std::vector<u8> rgba; ///< `width * height * 4`, top-left origin.
};

/// Reads and decodes one `.blp` by fileDataID. Returns false when the file is
/// missing or undecodable, which drops that layer rather than the sheet.
using ImageFetch =
    std::function<bool(u32 fileId, std::vector<u8>& rgba, u32& width, u32& height)>;

/// Build every composite @p model declares that @p appearance has a layer for.
///
/// A composite with no layer resolved is skipped entirely, so the model keeps
/// whatever it had rather than gaining a transparent sheet.
std::vector<ComposedTexture> ComposeCharacter(const ChrModelInfo& model,
                                              const ResolvedAppearance& appearance,
                                              const ImageFetch& fetch);

} // namespace whiteout::flakes::io::wow

#pragma once

// ============================================================================
// The dialogs that write the active document: Save As (MDX/MDL, `.m3`, an
// effect's bytes) and the conversions under File ▸ Export (WEM, MDX, M3, glTF).
// Each picks a target in the native dialog, then — all but WEM — confirms its
// options in a modal.
// ============================================================================

#include "ui/options_modal.h"
#include "whiteout/flakes/types.h"

#include <string>

namespace whiteout::flakes {

struct UiContext;

class ExportDialogs {
public:
    explicit ExportDialogs(UiContext& ctx);

    /// Routes on the source: a `.m3` saves as `.m3`, an effect is copied
    /// verbatim, and everything else that reaches here writes MDX or MDL.
    void SaveAs();
    /// One native dialog and no options: which converter runs is what the
    /// model IS, not anything the user could pick (WEM_INTEGRATION_DESIGN.md §5).
    void ExportWem();
    void ExportMdx();
    void ExportM3();
    void ExportGltf();

    /// The modals; each draws nothing until its dialog opened it.
    void Build();

    /// The `--ui-shot` harness: open @p dialog's modal on a fixed target
    /// without the native picker. False for a name this does not own.
    bool OpenForShot(std::string_view dialog);

private:
    void SaveM3();

    // Save As MDX/MDL: the MDL dialect (text only) and the texture export.
    struct SaveAsChoices {
        bool mdl = false;      // the target is .mdl: offer the dialect
        i32 dialect = 0;       // 0 = Warcraft III, 1 = Hiveworkshop
        bool textures = false; // write the used textures next to the model
        i32 textureFormat = 0; // index into kSaveAsTextureFormats (0 = keep)
    };
    // Export to MDX. The profile is Reforged and drawn disabled: the classic
    // derive is a different material vocabulary (§7.2.1) nothing has measured
    // yet. The row is drawn rather than hidden because the file IS
    // generation-specific, and a user should see which one they are getting.
    struct MdxChoices {
        i32 profile = 1; // 0 = classic, 1 = Reforged
        bool textures = true;
    };
    // Export to M3: both rows live — the two games share the container and the
    // version field is the difference (v29 imports as StarCraft II, v30 as Heroes).
    struct M3Choices {
        i32 profile = 0; // 0 = StarCraft II, 1 = Heroes of the Storm
        bool textures = true;
        bool exactPasses = false;
        bool sharpenTeamKey = false;
        bool war3ModTextures = false;
    };
    struct GltfChoices {
        bool textures = true;
    };
    // Save M3: every option changes the file's own shape — the merge folds in
    // `.m3a` files the `.m3` never named, the conversion lowers a Heroes model
    // to what StarCraft II loads, the copy adds files beside it. All off, so the
    // plain save writes the model as it stands.
    struct M3SaveChoices {
        bool mergeAnimations = false;
        bool convertToSc2 = false;
        bool textures = false;
    };

    UiContext& ctx_;
    ui::OptionsModal<SaveAsChoices> saveAs_;
    ui::OptionsModal<MdxChoices> mdx_;
    ui::OptionsModal<M3Choices> m3_;
    ui::OptionsModal<GltfChoices> gltf_;
    ui::OptionsModal<M3SaveChoices> m3Save_;
    /// Why the last Save M3 wrote nothing, so the modal can stay open and say
    /// so: a Heroes material the standard form cannot represent blocks the
    /// conversion, and that is the cue to leave the box unticked.
    std::string m3SaveError_;
};

} // namespace whiteout::flakes

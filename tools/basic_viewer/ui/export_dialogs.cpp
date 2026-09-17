#include "ui/export_dialogs.h"

#include "app/viewer_app.h"
#include "documents/model_formats.h"
#include "export/mdx_save.h"
#include "export/model_export_service.h"
#include "localization.h"
#include "export/mdx_export.h"
#include "string_util.h"
#include "ui/ui_context.h"
#include "ui/ui_metrics.h"
#include "ui/widgets.h"
#include "whiteout/flakes/util/path_utf8.h"

#include <imgui.h>
#include <nfd.hpp>

#include <algorithm>
#include <iterator>

namespace whiteout::flakes {

namespace wem = ::whiteout::models::wem;

namespace {

// A fixed name for `--ui-shot`: the dialogs show only the target's file name.
constexpr const char* kShotTarget = "ui-shot/out";

// The native save dialog; empty when cancelled.
std::filesystem::path PickSaveTarget(std::span<const nfdu8filteritem_t> filters) {
    NFD::UniquePathU8 outPath;
    if (NFD::SaveDialog(outPath, filters.data(), static_cast<nfdfiltersize_t>(filters.size())) != NFD_OKAY)
        return {};
    return io::FsPathFromUtf8(outPath.get());
}

std::string LowerExtension(const std::filesystem::path& path) {
    return tools::ToLowerAscii(io::PathToUtf8(path.extension()));
}

} // namespace

ExportDialogs::ExportDialogs(UiContext& ctx) : ctx_(ctx) {}

// ---- Save As ------------------------------------------------------------------
//
// The output format is locked to the source's: a PopcornFX effect can only be
// re-saved as the same effect, a StarCraft II model goes back out as `.m3`, and
// everything else that reaches here is Warcraft III and writes as MDX or MDL.
// Asked of the source rather than the extension, for the menu gate's reason.

void ExportDialogs::SaveAs() {
    ViewerApp& app = ctx_.app;
    if (app.Exports().Capabilities().saveM3) {
        SaveM3();
        return;
    }

    const std::filesystem::path& source = app.Documents().ActiveState().modelPath;
    if (IsModelKind(source, ModelKind::Effect)) {
        // Copied byte for byte, so the output keeps the source's extension.
        const nfdu8filteritem_t filter[] = {{"PopcornFX effect", LowerExtension(source) == ".pkfx" ? "pkfx" : "pkb"}};
        const std::filesystem::path target = PickSaveTarget(filter);
        if (!target.empty())
            app.Exports().SaveEffect(io::PathToUtf8(target));
        return;
    }

    // Two filter entries rather than "mdx,mdl", so NFD appends the extension of
    // whichever the user picks — which is then how binary vs text is decided.
    const nfdu8filteritem_t filter[] = {{"MDX model (binary)", "mdx"}, {"MDL model (text)", "mdl"}};
    const std::filesystem::path target = PickSaveTarget(filter);
    if (target.empty())
        return;
    saveAs_.Values().mdl = LowerExtension(target) == ".mdl";
    saveAs_.Open(target);
}

// Not a branch of Save As MDX: that writes Warcraft III, and this shares neither
// its writer nor one of its options. What it shares is the menu item, because
// from the user's side both are "write this model back in its own format". See
// m3_save.h.
void ExportDialogs::SaveM3() {
    const nfdu8filteritem_t filter[] = {{"StarCraft II model", "m3"}};
    const std::filesystem::path target = PickSaveTarget(filter);
    if (target.empty())
        return;
    m3Save_.Open(target);
    m3SaveError_.clear();
}

// ---- Export -------------------------------------------------------------------

void ExportDialogs::ExportWem() {
    const nfdu8filteritem_t filter[] = {{"WEM model", "wem"}};
    const std::filesystem::path target = PickSaveTarget(filter);
    if (!target.empty())
        LogOutcome("Export WEM", ctx_.app.Exports().ExportWem(target));
}

// Save As writes a Warcraft III model back in its own format. This writes a
// FOREIGN one as Warcraft III, which is the direction WEM exists for and which
// has choices Save As does not: the generation decides both the material
// vocabulary and the texture container. See mdx_export.h.
void ExportDialogs::ExportMdx() {
    const nfdu8filteritem_t filter[] = {{"Warcraft III model", "mdx"}};
    if (std::filesystem::path target = PickSaveTarget(filter); !target.empty())
        mdx_.Open(std::move(target));
}

// The MDX export one game over. See m3_export.h.
void ExportDialogs::ExportM3() {
    const nfdu8filteritem_t filter[] = {{"StarCraft II model", "m3"}};
    if (std::filesystem::path target = PickSaveTarget(filter); !target.empty())
        m3_.Open(std::move(target));
}

// Out of the Blizzard family: any model WEM reads, lowered to metallic-roughness
// for Blender and everything else. See gltf_export.h and GLTF_DESIGN.md.
void ExportDialogs::ExportGltf() {
    const nfdu8filteritem_t filter[] = {{"glTF binary", "glb"}, {"glTF text", "gltf"}};
    if (std::filesystem::path target = PickSaveTarget(filter); !target.empty())
        gltf_.Open(std::move(target));
}

bool ExportDialogs::OpenForShot(std::string_view dialog) {
    const std::filesystem::path target = io::FsPathFromUtf8(kShotTarget);
    const auto withExtension = [&](const char* ext) {
        std::filesystem::path p = target;
        p += ext;
        return p;
    };
    if (dialog == "dialog-mdx")
        mdx_.Open(withExtension(".mdx"));
    else if (dialog == "dialog-m3")
        m3_.Open(withExtension(".m3"));
    else if (dialog == "dialog-gltf")
        gltf_.Open(withExtension(".glb"));
    else if (dialog == "dialog-saveas" || dialog == "dialog-saveas-mdl") {
        saveAs_.Values().mdl = dialog == "dialog-saveas-mdl";
        saveAs_.Open(withExtension(saveAs_.Values().mdl ? ".mdl" : ".mdx"));
    } else if (dialog == "dialog-m3save")
        m3Save_.Open(withExtension(".m3"));
    else
        return false;
    return true;
}

// ---- The modals ---------------------------------------------------------------

void ExportDialogs::Build() {
    ModelExportService& exports = ctx_.app.Exports();

    saveAs_.Draw(
        "dialog.saveas.title", "app.save",
        [](SaveAsChoices& o) {
            if (o.mdl) {
                ImGui::TextUnformatted(i18n::tr("dialog.saveas.prompt"));
                ImGui::RadioButton(i18n::tr("dialog.saveas.wc3"), &o.dialect, 0);
                ImGui::SameLine();
                ImGui::RadioButton(i18n::tr("dialog.saveas.hive"), &o.dialect, 1);
                ImGui::TextDisabled("%s", i18n::tr("dialog.saveas.wc3_desc"));
                ImGui::TextDisabled("%s", i18n::tr("dialog.saveas.hive_desc"));
                ImGui::Separator();
            }
            ImGui::Checkbox(i18n::tr("dialog.saveas.export_textures"), &o.textures);
            ImGui::BeginDisabled(!o.textures);
            ImGui::TextDisabled("%s", i18n::tr("dialog.saveas.export_hint"));
            const i32 formatCount = static_cast<i32>(std::size(kSaveAsTextureFormats));
            o.textureFormat = std::clamp(o.textureFormat, 0, formatCount - 1);
            const auto formatLabel = [](i32 i) {
                return kSaveAsTextureFormats[i].ext[0] ? kSaveAsTextureFormats[i].label
                                                       : i18n::tr("dialog.saveas.keep_original");
            };
            ImGui::SetNextItemWidth(ui::kTextureFormatComboWidth);
            if (ImGui::BeginCombo(i18n::tr("dialog.saveas.convert_to"), formatLabel(o.textureFormat))) {
                for (i32 i = 0; i < formatCount; ++i) {
                    const bool selected = i == o.textureFormat;
                    if (ImGui::Selectable(formatLabel(i), selected))
                        o.textureFormat = i;
                    if (selected)
                        ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            ImGui::EndDisabled();
        },
        [&](const std::filesystem::path& target, const SaveAsChoices& o) {
            exports.SaveAsMdx(io::PathToUtf8(target), o.dialect == 1, o.textures,
                              kSaveAsTextureFormats[o.textureFormat].ext);
            return true;
        });

    mdx_.Draw(
        "dialog.mdx.title", "dialog.mdx.export",
        [](MdxChoices& o) {
            ImGui::TextUnformatted(i18n::tr("dialog.mdx.prompt"));
            // Drawn, and drawn disabled: see MdxChoices.
            ImGui::BeginDisabled(true);
            ImGui::RadioButton(i18n::tr("dialog.mdx.classic"), &o.profile, 0);
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::RadioButton(i18n::tr("dialog.mdx.reforged"), &o.profile, 1);
            ImGui::TextDisabled("%s", i18n::tr("dialog.mdx.classic_soon"));

            ImGui::Separator();
            ImGui::Checkbox(i18n::tr("dialog.mdx.export_textures"), &o.textures);
            ImGui::BeginDisabled(!o.textures);
            // Not a choice: Reforged reads `.dds` and classic reads BLP1, so
            // saying which one gets written is all there is to say.
            ImGui::TextDisabled("%s: %s", i18n::tr("dialog.mdx.convert_to"),
                                Wc3TextureExtension(o.profile == 0 ? wem::ProfileId::Wc3Classic
                                                                   : wem::ProfileId::Wc3Reforged));
            ImGui::TextDisabled("%s", i18n::tr("dialog.mdx.export_hint"));
            ImGui::EndDisabled();
        },
        [&](const std::filesystem::path& target, const MdxChoices& o) {
            MdxExportOptions options;
            options.profile = o.profile == 0 ? wem::ProfileId::Wc3Classic : wem::ProfileId::Wc3Reforged;
            options.textures = o.textures;
            LogOutcome("Export MDX", exports.ExportMdx(target, options));
            return true;
        });

    m3_.Draw(
        "dialog.m3.title", "dialog.mdx.export",
        [](M3Choices& o) {
            ImGui::TextUnformatted(i18n::tr("dialog.m3.prompt"));
            ImGui::RadioButton(i18n::tr("dialog.m3.sc2"), &o.profile, 0);
            ImGui::SameLine();
            ImGui::RadioButton(i18n::tr("dialog.m3.heroes"), &o.profile, 1);

            // The MDX popup's checkbox string; its own hint, because a `.m3`
            // names textures by mod path and they go in a folder of their own.
            // No format row: both profiles read `.dds`.
            ImGui::Separator();
            ImGui::Checkbox(i18n::tr("dialog.mdx.export_textures"), &o.textures);
            ImGui::BeginDisabled(!o.textures);
            ImGui::TextDisabled("%s: dds", i18n::tr("dialog.mdx.convert_to"));
            ImGui::TextDisabled("%s", i18n::tr("dialog.m3.export_hint"));
            ImGui::EndDisabled();

            // The Warcraft III knobs; inert for every other source. War3 (Mod) is
            // StarCraft II's, and it only saves writing textures.
            ImGui::Separator();
            ImGui::Checkbox(i18n::tr("dialog.m3.exact_passes"), &o.exactPasses);
            ImGui::Checkbox(i18n::tr("dialog.m3.sharpen_team_key"), &o.sharpenTeamKey);
            ImGui::BeginDisabled(!o.textures || o.profile != 0);
            ImGui::Checkbox(i18n::tr("dialog.m3.war3_mod_textures"), &o.war3ModTextures);
            ImGui::TextDisabled("%s", i18n::tr("dialog.m3.war3_mod_hint"));
            ImGui::EndDisabled();
        },
        [&](const std::filesystem::path& target, const M3Choices& o) {
            M3ExportOptions options;
            options.profile = o.profile == 0 ? wem::ProfileId::Sc2 : wem::ProfileId::Heroes;
            options.textures = o.textures;
            options.exactPasses = o.exactPasses;
            options.sharpenTeamKey = o.sharpenTeamKey;
            options.war3ModTextures = o.war3ModTextures && o.profile == 0;
            LogOutcome("Export M3", exports.ExportM3(target, options));
            return true;
        });

    // The container is the filename's own choice — `.glb` embeds everything,
    // `.gltf` writes JSON + `.bin` + images — so the dialog states it.
    const bool binary = LowerExtension(gltf_.Target()) != ".gltf";
    gltf_.Draw(
        "dialog.gltf.title", "dialog.mdx.export",
        [&](GltfChoices& o) {
            ImGui::TextUnformatted(i18n::tr("dialog.gltf.prompt"));
            ImGui::TextDisabled("%s", i18n::tr(binary ? "dialog.gltf.glb" : "dialog.gltf.gltf"));
            ImGui::Separator();
            ImGui::Checkbox(i18n::tr("dialog.mdx.export_textures"), &o.textures);
            ImGui::BeginDisabled(!o.textures);
            ImGui::TextDisabled("%s: png", i18n::tr("dialog.mdx.convert_to"));
            ImGui::TextDisabled("%s", i18n::tr("dialog.mdx.export_hint"));
            ImGui::EndDisabled();
        },
        [&](const std::filesystem::path& target, const GltfChoices& o) {
            GltfExportOptions options;
            options.binary = binary;
            options.textures = o.textures;
            LogOutcome("Export glTF", exports.ExportGltf(target, options));
            return true;
        });

    // The merge has nothing to fold in until a file is attached, and the
    // Animation window is where that happens — so the row says how many wait.
    const auto attachedFiles = [this] {
        const Sc2AnimationFiles* sc2 = ctx_.app.Features().Sc2();
        return sc2 ? sc2->AttachedFiles().size() : usize{0};
    };
    m3Save_.Draw(
        "dialog.m3save.title", "app.save",
        [&](M3SaveChoices& o) {
            const usize attached = attachedFiles();
            ImGui::TextUnformatted(i18n::tr("dialog.m3save.prompt"));
            ImGui::Separator();
            ImGui::BeginDisabled(attached == 0);
            ImGui::Checkbox(i18n::tr("dialog.m3save.merge_anims"), &o.mergeAnimations);
            ImGui::EndDisabled();
            if (attached == 0)
                ImGui::TextDisabled("%s", i18n::tr("dialog.m3save.merge_none"));
            else
                ImGui::TextDisabled(i18n::tr("dialog.m3save.merge_count"), static_cast<int>(attached));

            ImGui::Checkbox(i18n::tr("dialog.m3save.convert_sc2"), &o.convertToSc2);
            ImGui::TextDisabled("%s", i18n::tr("dialog.m3save.convert_hint"));

            // The Save As strings: they say nothing MDX-specific. No format row —
            // the bytes are copied as the storage serves them, `.dds` already.
            ImGui::Checkbox(i18n::tr("dialog.saveas.export_textures"), &o.textures);
            ImGui::BeginDisabled(!o.textures);
            ImGui::TextDisabled("%s", i18n::tr("dialog.saveas.export_hint"));
            ImGui::EndDisabled();

            if (!m3SaveError_.empty()) {
                ImGui::Separator();
                ui::ErrorText(i18n::tr("dialog.m3save.failed"), m3SaveError_.c_str());
            }
        },
        [&](const std::filesystem::path& target, const M3SaveChoices& o) {
            // Stays open on failure: the one failure this has is the conversion
            // refusing a material, and the fix for it is a box in this modal.
            M3SaveOptions options;
            options.mergeAnimations = o.mergeAnimations && attachedFiles() > 0;
            options.convertToSc2 = o.convertToSc2;
            options.textures = o.textures;
            const ExportOutcome outcome = exports.SaveM3(target, options);
            LogOutcome("Save M3", outcome);
            m3SaveError_ = outcome.error;
            return outcome.ok;
        });
}

} // namespace whiteout::flakes

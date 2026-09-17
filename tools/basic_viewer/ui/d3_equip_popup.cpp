#include "ui/d3_equip_popup.h"

#include "localization.h"
#include "ui/ui_metrics.h"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cstdio>

namespace whiteout::flakes {

namespace {

/// Items listed per slot combo; the filter reaches the rest.
constexpr usize kItemPickerRows = 200;

// A dye is one value space: 0 undyed, 1 the engine's HIDDEN (naked armour, a
// despawned attachment), 2..22 the dye_ramp rows.
constexpr i32 kDyeHidden = 1;
constexpr i32 kFirstDyeRow = 2;
constexpr i32 kLastDyeRow = 22;

} // namespace

void D3EquipPopup::Build(D3Wardrobe& d3, const std::vector<D3Wardrobe::CharacterSlot>& slots) {
    // ---- Items: dressing by in-game name ----
    // A progress bar stands in while the registry builds. Once ready, these rows
    // ALSO replace the per-slot wardrobe below — the registry drives the same
    // look tags — so only Hair and the extras survive of the manual controls.
    const D3Wardrobe::RegistryState registry = d3.ItemRegistry();
    const bool registryReady = registry.stage == D3Wardrobe::RegistryState::Stage::Ready && !registry.empty;
    const bool registryBuilding = registry.stage == D3Wardrobe::RegistryState::Stage::Building;
    if (registryBuilding) {
        const bool known = registry.progressKnown;
        ImGui::TextUnformatted(known && !registry.step.empty() ? registry.step.c_str()
                                                               : i18n::tr("toolbar.equip.building"));
        char counts[64] = "";
        if (known && registry.total != 0)
            std::snprintf(counts, sizeof(counts), "%llu / %llu", static_cast<unsigned long long>(registry.current),
                          static_cast<unsigned long long>(registry.total));
        const float fraction =
            (known && !registry.indeterminate) ? registry.fraction : -1.0f * static_cast<float>(ImGui::GetTime());
        ImGui::ProgressBar(fraction, ImVec2(ui::kRegistryProgressWidth, 0.0f), counts[0] ? counts : nullptr);
    } else if (!registryReady) {
        ImGui::TextDisabled("%s", i18n::tr("toolbar.equip.noregistry"));
    } else if (const auto rows = d3.OutfitSlots(); !rows.empty()) {
        BuildOutfit(d3, rows);
    }

    // Read once: an appearance carries up to ninety-odd looks, the same list for
    // every row below.
    const auto looks = d3.LookNames();

    // The manual wardrobe is the fallback for a storage with no item tables;
    // with the registry up (or on its way) it would restate the item rows in
    // engine-internal words. Building counts, so the redundant rows do not flash
    // for the seconds the build takes and then vanish.
    const bool manualWardrobe = !registryReady && !registryBuilding;

    // The set first, because one material set on every slot is what wearing a
    // set *is* — the per-slot rows underneath mix pieces from two of them.
    if (manualWardrobe && !looks.empty()) {
        const u32 shared = slots[0].lookIndex;
        bool uniform = true;
        for (const auto& s : slots)
            uniform &= (s.lookIndex == shared);
        const char* label = (uniform && shared < looks.size()) ? looks[shared].c_str() : "";
        ImGui::SetNextItemWidth(ui::kWardrobeSetComboWidth);
        if (ImGui::BeginCombo(i18n::tr("toolbar.equip.set"), label)) {
            for (u32 i = 0; i < static_cast<u32>(looks.size()); ++i) {
                const bool isSel = uniform && (i == shared);
                if (ImGui::Selectable(looks[i].c_str(), isSel))
                    d3.SetCharacterLookForAll(i);
                if (isSel)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", i18n::tr("toolbar.equip.set.tip"));
        ImGui::Separator();
    }

    for (const auto& slot : slots) {
        if (slot.items.empty())
            continue;
        if (!manualWardrobe && slot.registryDriven)
            continue; // the outfit rows above drive this slot now
        const u32 sel = std::min<u32>(slot.selectedItem, static_cast<u32>(slot.items.size()) - 1);
        char id[64];
        std::snprintf(id, sizeof(id), "%s##d3item%d", slot.name.c_str(), slot.slot);
        ImGui::SetNextItemWidth(ui::kWardrobeItemComboWidth);
        if (ImGui::BeginCombo(id, slot.items[sel].c_str())) {
            for (u32 i = 0; i < static_cast<u32>(slot.items.size()); ++i) {
                const bool isSel = (i == sel);
                if (ImGui::Selectable(slot.items[i].c_str(), isSel))
                    d3.SetCharacterItem(slot.slot, i);
                if (isSel)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        // The material this one slot wears. Per slot, because the original reads
        // a look name off each equipped item — a heavy chest and heavy boots from
        // two sets share one material read at two variant indices.
        if (!looks.empty()) {
            ImGui::SameLine();
            std::snprintf(id, sizeof(id), "##d3look%d", slot.slot);
            const u32 li = std::min<u32>(slot.lookIndex, static_cast<u32>(looks.size()) - 1);
            ImGui::SetNextItemWidth(ui::kWardrobeLookComboWidth);
            if (ImGui::BeginCombo(id, looks[li].c_str())) {
                for (u32 i = 0; i < static_cast<u32>(looks.size()); ++i) {
                    const bool isSel = (i == li);
                    if (ImGui::Selectable(looks[i].c_str(), isSel))
                        d3.SetCharacterSlotLook(slot.slot, i);
                    if (isSel)
                        ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
        }
    }

    // Everything no slot claims. Nothing in ActorModel_ApplyLook switches these —
    // a decapitated body is gameplay, not equipment — so they are off until asked
    // for rather than picked between.
    if (const auto extras = d3.CharacterExtras(); !extras.empty()) {
        ImGui::Separator();
        if (ImGui::TreeNode(i18n::tr("toolbar.equip.extras"))) {
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", i18n::tr("toolbar.equip.extras.tip"));
            for (const auto& ex : extras) {
                bool on = ex.shown;
                char exid[160];
                std::snprintf(exid, sizeof(exid), "%s##d3x%u", ex.name.c_str(), ex.geoset);
                if (ImGui::Checkbox(exid, &on))
                    d3.SetCharacterExtra(ex.geoset, on);
            }
            ImGui::TreePop();
        }
    }
}

void D3EquipPopup::BuildOutfit(D3Wardrobe& d3, const std::vector<D3Wardrobe::OutfitRow>& rows) {
    // Presets: a whole outfit by name, saved as item NAMES so it survives a
    // re-parse of the item tables.
    ImGui::SetNextItemWidth(ui::kOutfitPresetComboWidth);
    if (ImGui::BeginCombo("##d3preset", i18n::tr("toolbar.equip.preset"))) {
        for (const auto& p : d3.PresetNames()) {
            if (ImGui::Selectable(p.c_str(), false))
                d3.LoadPreset(p);
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ui::kOutfitPresetNameWidth);
    ImGui::InputTextWithHint("##d3presetname", i18n::tr("toolbar.equip.preset.name"), &presetName_);
    ImGui::SameLine();
    if (ImGui::Button(i18n::tr("toolbar.equip.preset.save")) && !presetName_.empty())
        d3.SavePreset(presetName_);

    bool sheathed = d3.OutfitSheathed();
    if (ImGui::Checkbox(i18n::tr("toolbar.equip.sheathe"), &sheathed))
        d3.SetOutfitSheathed(sheathed);
    // The class gate: the offer is what the focused character can wear
    // (class-neutral always shows); this widens it to the whole registry.
    ImGui::SameLine();
    ImGui::Checkbox(i18n::tr("toolbar.equip.allclasses"), &allClasses_);
    // A whole set in one click, behind the same gate.
    ImGui::SetNextItemWidth(ui::kOutfitItemComboWidth);
    if (ImGui::BeginCombo("##d3set", i18n::tr("toolbar.equip.equipset"))) {
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##d3setfilter", i18n::tr("toolbar.equip.item.search"), &itemFilter_);
        for (const auto& set : d3.SetEntries(itemFilter_, allClasses_)) {
            char setId[96];
            std::snprintf(setId, sizeof(setId), "%s (%d)##%s", set.label.c_str(), static_cast<int>(set.pieces),
                          set.key.c_str());
            if (ImGui::Selectable(setId, false))
                d3.EquipSet(set.key);
        }
        ImGui::EndCombo();
    }
    for (const auto& row : rows) {
        char id[64];
        std::snprintf(id, sizeof(id), "%s##d3out%d", row.name.c_str(), row.visualSlot);
        const char* preview =
            row.equipped.empty() ? i18n::tr("toolbar.equip.item.none") : row.equippedLabel.c_str();
        ImGui::SetNextItemWidth(ui::kOutfitItemComboWidth);
        if (ImGui::BeginCombo(id, preview)) {
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputTextWithHint("##d3itemfilter", i18n::tr("toolbar.equip.item.search"), &itemFilter_);
            if (ImGui::Selectable(i18n::tr("toolbar.equip.item.none"), row.equipped.empty()))
                d3.SetOutfitItem(row.visualSlot, "");
            for (const auto& entry : d3.ItemEntries(row.visualSlot, itemFilter_, kItemPickerRows, allClasses_)) {
                // Display names collide (the art-test dupes); the stem keeps every
                // row a distinct widget.
                char rowId[160];
                std::snprintf(rowId, sizeof(rowId), "%s##%s", entry.label.c_str(), entry.stem.c_str());
                if (ImGui::Selectable(rowId, entry.stem == row.equipped))
                    d3.SetOutfitItem(row.visualSlot, entry.stem);
                if (ImGui::IsItemHovered()) {
                    const std::string tip = d3.ItemTip(entry.stem);
                    if (!tip.empty())
                        ImGui::SetTooltip("%s", tip.c_str());
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        std::snprintf(id, sizeof(id), "##d3dye%d", row.visualSlot);
        const char* dyeLabel;
        char dyeBuf[16];
        if (row.dye == kDyeHidden) {
            dyeLabel = i18n::tr("toolbar.equip.item.hide");
        } else if (row.dye >= kFirstDyeRow) {
            std::snprintf(dyeBuf, sizeof(dyeBuf), "Dye %d", row.dye);
            dyeLabel = dyeBuf;
        } else {
            dyeLabel = i18n::tr("toolbar.equip.item.undyed");
        }
        ImGui::SetNextItemWidth(ui::kOutfitDyeComboWidth);
        if (ImGui::BeginCombo(id, dyeLabel)) {
            if (ImGui::Selectable(i18n::tr("toolbar.equip.item.undyed"), row.dye == 0))
                d3.SetOutfitDye(row.visualSlot, 0);
            if (ImGui::Selectable(i18n::tr("toolbar.equip.item.hide"), row.dye == kDyeHidden))
                d3.SetOutfitDye(row.visualSlot, kDyeHidden);
            for (i32 d = kFirstDyeRow; d <= kLastDyeRow; ++d) {
                char dyeItem[24];
                std::snprintf(dyeItem, sizeof(dyeItem), "Dye %d##d3dyev%d", d, row.visualSlot);
                if (ImGui::Selectable(dyeItem, row.dye == d))
                    d3.SetOutfitDye(row.visualSlot, d);
            }
            ImGui::EndCombo();
        }
    }
    ImGui::Separator();
}

} // namespace whiteout::flakes

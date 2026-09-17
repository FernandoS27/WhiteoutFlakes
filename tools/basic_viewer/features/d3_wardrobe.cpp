#include "features/d3_wardrobe.h"

#include "documents/document_loader.h"
#include "documents/document_manager.h"
#include "features/d3_outfit_presets.h"
#include "features/d3_visual_slots.h"
#include "io/d3/d3_item_registry.h"
#include "io/d3/d3_model_adapter.h"
#include "io/load_task.h"
#include "renderer/model/model_loader.h"
#include "renderer/profiles/diablo3/d3_character_appearance.h"
#include "renderer/render_service.h"
#include "renderer/scene_manager.h"
#include "settings_ini.h"
#include "string_util.h"
#include "whiteout/flakes/util/path_utf8.h"

#include <algorithm>
#include <cstdio>
#include <optional>

namespace whiteout::flakes {

namespace {

namespace d3n = ::whiteout::sno::d3::native;
using VisualSlot = d3n::EVisualSlot;

static_assert(static_cast<i32>(VisualSlot::Head) == kD3VisualSlots[0].ordinal &&
                  static_cast<i32>(VisualSlot::Torso) == kD3VisualSlots[1].ordinal &&
                  static_cast<i32>(VisualSlot::Feet) == kD3VisualSlots[2].ordinal &&
                  static_cast<i32>(VisualSlot::Hands) == kD3VisualSlots[3].ordinal &&
                  static_cast<i32>(VisualSlot::RightHand) == kD3VisualSlots[4].ordinal &&
                  static_cast<i32>(VisualSlot::LeftHand) == kD3VisualSlots[5].ordinal &&
                  static_cast<i32>(VisualSlot::Shoulders) == kD3VisualSlots[6].ordinal &&
                  static_cast<i32>(VisualSlot::Legs) == kD3VisualSlots[7].ordinal,
              "kD3VisualSlots ordinals are native::EVisualSlot");

constexpr std::string_view kRegistryTaskTitle = "Building item registry";

// The focused model's class, off the loaded Appearance stem — the same read
// SetOutfitItem makes for the per-class attachment art. Empty for a
// non-player, which the pickers treat as "show everything".
std::optional<d3n::PlayerClass> ClassOfModel(const std::filesystem::path& modelPath) {
    if (const auto body = d3n::playerFromAppearanceStem(io::PathToUtf8(modelPath.stem())))
        return body->first;
    return std::nullopt;
}

const std::string& ItemLabel(const io::D3ItemRecord& rec) {
    return rec.displayName.empty() ? rec.name : rec.displayName;
}

std::filesystem::path PresetFile() {
    return SettingsIniPath().parent_path() / "d3_outfits.ini";
}

class D3WardrobeImpl final : public D3Wardrobe {
public:
    D3WardrobeImpl(renderer::RenderService& service, io::LoadTaskRunner& tasks, DocumentManager& documents,
                   DocumentLoader& loader)
        : service_(service), tasks_(tasks), documents_(documents), loader_(loader) {}

    // ---- The manual wardrobe ----

    std::vector<CharacterSlot> CharacterSlots() const override {
        const Binding b = Bind();
        if (!b)
            return {};
        std::vector<CharacterSlot> out;
        for (const auto& s : b.appearance->Slots(*b.adapter)) {
            CharacterSlot row;
            row.name = s.name;
            row.slot = static_cast<i32>(s.slot);
            for (const auto& item : s.items)
                row.items.push_back(item.label);
            row.selectedItem = s.selectedItem;
            row.lookIndex = s.lookIndex;
            row.registryDriven = s.slot != d3n::LookSlot::Hair;
            out.push_back(std::move(row));
        }
        return out;
    }

    void SetCharacterItem(i32 slot, u32 itemIndex) override {
        if (const Binding b = Bind()) {
            b.appearance->SetItem(*b.adapter, static_cast<d3n::LookSlot>(slot), itemIndex);
            Restyle();
        }
    }

    void SetCharacterSlotLook(i32 slot, u32 lookIndex) override {
        if (const Binding b = Bind()) {
            b.appearance->SetSlotLook(*b.adapter, static_cast<d3n::LookSlot>(slot), lookIndex);
            Restyle();
        }
    }

    std::vector<std::string> LookNames() const override {
        const Binding b = Bind();
        if (!b)
            return {};
        std::vector<std::string> out;
        for (const auto& n : b.adapter->Looks())
            out.push_back(n);
        return out;
    }

    void SetCharacterLookForAll(u32 lookIndex) override {
        if (const Binding b = Bind()) {
            b.appearance->SetLookForAll(*b.adapter, lookIndex);
            Restyle();
        }
    }

    std::vector<CharacterExtra> CharacterExtras() const override {
        const Binding b = Bind();
        if (!b)
            return {};
        std::vector<CharacterExtra> out;
        for (const auto& e : b.appearance->Extras(*b.adapter))
            out.push_back({e.name, e.geoset, e.shown});
        return out;
    }

    void SetCharacterExtra(u32 geoset, bool shown) override {
        if (const Binding b = Bind()) {
            b.appearance->SetExtra(*b.adapter, geoset, shown);
            Restyle();
        }
    }

    // ---- The item registry ----
    //
    // Where the build stands is kept here rather than asked of the registry:
    // during Building the registry belongs to the task thread and the host must
    // not even ask it.

    void EnsureItemRegistry() override {
        auto& items = service_.Loader().D3Items();
        switch (build_) {
        case Build::Building:
            return;
        case Build::Ready:
            // A profile change can clear the registry under a Ready latch; start
            // again rather than answer "no tables" for an install that has them.
            if (items.Built())
                return;
            build_ = Build::NotStarted;
            [[fallthrough]];
        case Build::NotStarted:
            // A command-line path (--d3-equip) may have built it synchronously.
            if (items.Built()) {
                build_ = Build::Ready;
                return;
            }
            StartBuild();
            return;
        }
    }

    RegistryState ItemRegistry() const override {
        RegistryState state;
        if (build_ == Build::Building) {
            state.stage = RegistryState::Stage::Building;
            // The runner is one task at a time, so the snapshot may belong to
            // whatever the build is queued behind.
            const io::ProgressSnapshot snap = tasks_.Poll();
            if (snap.title == kRegistryTaskTitle) {
                state.progressKnown = true;
                state.step = snap.stage;
                state.current = snap.current;
                state.total = snap.total;
                state.fraction = snap.fraction;
                state.indeterminate = snap.indeterminate;
            }
            return state;
        }
        auto& items = service_.Loader().D3Items();
        if (items.Built()) {
            state.stage = RegistryState::Stage::Ready;
            state.empty = items.Items().empty();
        }
        return state;
    }

    void OnProfileApplying() override {
        if (build_ == Build::Building)
            return;
        service_.Loader().D3Items().Clear();
        build_ = Build::NotStarted;
    }

    // ---- The outfit ----

    std::vector<OutfitRow> OutfitSlots() const override {
        const Binding b = Bind();
        if (!b || !RegistryHasItems())
            return {};
        auto& items = service_.Loader().D3Items();
        const auto outfit = b.appearance->OutfitOf(*b.adapter);
        std::vector<OutfitRow> out;
        for (const D3VisualSlot& slot : kD3VisualSlots) {
            const auto& worn = outfit.slots[slot.ordinal];
            OutfitRow row;
            row.name = slot.displayName;
            row.visualSlot = slot.ordinal;
            row.dye = worn.dyeType;
            row.armour = slot.armour;
            if (worn.itemGbid != -1) {
                if (const auto* rec = items.FindByGbid(static_cast<u32>(worn.itemGbid))) {
                    row.equipped = rec->name;
                    row.equippedLabel = ItemLabel(*rec);
                }
            }
            out.push_back(std::move(row));
        }
        return out;
    }

    std::vector<ItemEntry> ItemEntries(i32 visualSlot, std::string_view filter, usize max,
                                       bool allClasses) const override {
        if (!RegistryHasItems())
            return {};
        auto& items = service_.Loader().D3Items();
        // The equipped stem, so the survivor of a display-name collision below
        // is the row the picker must show as selected.
        std::string equipped;
        if (const Binding b = Bind()) {
            const i32 gbid = b.appearance->OutfitOf(*b.adapter).slots[visualSlot].itemGbid;
            if (gbid != -1)
                if (const auto* rec = items.FindByGbid(static_cast<u32>(gbid)))
                    equipped = rec->name;
        }
        const auto cls = allClasses ? std::nullopt : ClassOfModel(documents_.ActiveState().modelPath);
        std::vector<ItemEntry> out;
        for (const u32 i : items.ItemsForSlot(static_cast<VisualSlot>(visualSlot))) {
            const auto& rec = items.Items()[i];
            if (cls && !rec.CanWear(*cls))
                continue;
            const std::string& label = ItemLabel(rec);
            if (!tools::ContainsIgnoreCase(label, filter) && !tools::ContainsIgnoreCase(rec.name, filter))
                continue;
            out.push_back({label, rec.name});
        }
        // ItemsForSlot is stem-sorted; the picker reads display names, so order by
        // those and only then cap — a filter must reach the whole offer. The
        // equipped stem sorts to the front of its name group so the dedup keeps it.
        std::sort(out.begin(), out.end(), [&](const ItemEntry& a, const ItemEntry& b) {
            if (a.label != b.label)
                return a.label < b.label;
            if ((a.stem == equipped) != (b.stem == equipped))
                return a.stem == equipped;
            return a.stem < b.stem;
        });
        // One display name is one row: the art-test dupes ship one name on six
        // records, and six identical rows offer nothing five of them.
        out.erase(std::unique(out.begin(), out.end(),
                              [](const ItemEntry& a, const ItemEntry& b) { return a.label == b.label; }),
                  out.end());
        if (out.size() > max)
            out.resize(max);
        return out;
    }

    std::vector<SetEntry> SetEntries(std::string_view filter, bool allClasses) const override {
        if (!RegistryHasItems())
            return {};
        auto& items = service_.Loader().D3Items();
        const auto cls = allClasses ? std::nullopt : ClassOfModel(documents_.ActiveState().modelPath);
        std::vector<SetEntry> out;
        for (const auto& set : items.Sets()) {
            if (set.key.empty())
                continue;
            if (cls && !((set.classMask >> static_cast<u32>(*cls)) & 1u))
                continue;
            usize pieces = 0;
            for (const u32 i : set.members)
                pieces += items.Items()[i].slotMask != 0;
            if (pieces == 0)
                continue;
            const std::string& label = set.displayName.empty() ? set.key : set.displayName;
            if (!tools::ContainsIgnoreCase(label, filter) && !tools::ContainsIgnoreCase(set.key, filter))
                continue;
            out.push_back({label, set.key, pieces});
        }
        // One display name is one row: the crafted tiers ship one name on two keys
        // ("Crafted Hell Set 002_104"/"_1xx", "_x1"/"P74_..."), identical to a
        // player. Sets() is display-name sorted, so duplicates are adjacent; keep
        // the fuller offer.
        std::vector<SetEntry> dedup;
        dedup.reserve(out.size());
        for (auto& e : out) {
            if (!dedup.empty() && dedup.back().label == e.label) {
                if (e.pieces > dedup.back().pieces)
                    dedup.back() = std::move(e);
            } else {
                dedup.push_back(std::move(e));
            }
        }
        return dedup;
    }

    bool EquipSet(std::string_view key) override {
        if (build_ == Build::Building)
            return false;
        auto& items = service_.Loader().D3Items();
        items.EnsureBuilt(service_.Scene().ActiveContentProvider());
        const io::D3ItemSet* set = nullptr;
        for (const auto& s : items.Sets()) {
            if (s.key == key) {
                set = &s;
                break;
            }
        }
        if (!set)
            return false;
        // Each piece takes its own slot; weapons fill right hand then left so a
        // paired-blades set dual-wields. Slots the set does not cover keep what
        // they wore — a six-piece armour set must not undress the hands.
        constexpr VisualSlot kOrder[] = {VisualSlot::Head, VisualSlot::Shoulders, VisualSlot::Torso,
                                         VisualSlot::Hands, VisualSlot::Legs, VisualSlot::Feet,
                                         VisualSlot::RightHand, VisualSlot::LeftHand};
        bool any = false;
        u16 taken = 0;
        for (const u32 i : set->members) {
            const auto& rec = items.Items()[i];
            for (const VisualSlot slot : kOrder) {
                const auto bit = static_cast<u16>(1u << static_cast<u32>(slot));
                if (!rec.CanGo(slot) || (taken & bit))
                    continue;
                if (SetOutfitItem(static_cast<i32>(slot), rec.name)) {
                    taken |= bit;
                    any = true;
                }
                break;
            }
        }
        return any;
    }

    bool SetOutfitItem(i32 visualSlot, std::string_view itemName) override {
        const Binding b = Bind();
        if (!b || build_ == Build::Building)
            return false;
        auto& items = service_.Loader().D3Items();
        items.EnsureBuilt(service_.Scene().ActiveContentProvider());

        const auto slot = static_cast<VisualSlot>(visualSlot);
        if (itemName.empty()) {
            b.appearance->SetOutfitItem(*b.adapter, slot, nullptr, {});
            Restyle();
            return true;
        }
        const io::D3ItemRecord* rec = items.FindByName(itemName);
        if (!rec)
            return false;
        // The per-class attachment art needs to know who is wearing this; the
        // loaded path's stem says (Barbarian_Male.app and friends).
        const std::string stem = io::PathToUtf8(documents_.ActiveState().modelPath.stem());
        if (const auto body = d3n::playerFromAppearanceStem(stem))
            b.appearance->SetOutfitBody(*b.adapter, body->first, body->second);
        std::shared_ptr<const d3n::Actor> actor;
        if (rec->snoActor > 0)
            actor = service_.Loader().D3Cache().Actor(rec->snoActor);
        if (!b.appearance->SetOutfitItem(*b.adapter, slot, rec, std::move(actor)))
            return false;
        Restyle();
        return true;
    }

    void SetOutfitDye(i32 visualSlot, i32 dye) override {
        if (const Binding b = Bind()) {
            b.appearance->SetOutfitDye(*b.adapter, static_cast<VisualSlot>(visualSlot), dye);
            Restyle();
        }
    }

    bool OutfitSheathed() const override {
        const Binding b = Bind();
        return b && b.appearance->OutfitOf(*b.adapter).sheathed;
    }

    void SetOutfitSheathed(bool sheathed) override {
        if (const Binding b = Bind()) {
            b.appearance->SetOutfitSheathed(*b.adapter, sheathed);
            Restyle();
        }
    }

    std::string ItemTip(std::string_view itemName) const override {
        if (build_ == Build::Building)
            return {};
        auto& items = service_.Loader().D3Items();
        const io::D3ItemRecord* rec = items.FindByName(itemName);
        if (!rec)
            return {};
        // First line: the record stem and its type — what a preset or a corpus
        // scenario token would spell. Below it, the set and the class lock.
        std::string tip = rec->name;
        const std::string_view type = items.TypeNameOf(rec->gbidItemType);
        if (!type.empty()) {
            tip += "   ";
            tip += type;
        }
        if (const auto* set = items.SetOf(*rec); set && !set->displayName.empty()) {
            tip += "\n";
            tip += set->displayName;
        }
        const u32 m = rec->classMask;
        if (m != io::kD3AllClasses && (m & (m - 1)) == 0) {
            u32 bit = m, ordinal = 0;
            while (bit >>= 1)
                ++ordinal;
            tip += "\n";
            tip += d3n::playerClassName(static_cast<d3n::PlayerClass>(ordinal));
            tip += " only";
        }
        char buf[64];
        std::snprintf(buf, sizeof(buf), "\ngbid 0x%08X   actor %d", rec->gbid, rec->snoActor);
        tip += buf;
        return tip;
    }

    // ---- Presets ----

    std::vector<std::string> PresetNames() const override {
        return ListD3OutfitPresets(PresetFile());
    }

    bool SavePreset(std::string_view name) override {
        if (name.empty())
            return false;
        const Binding b = Bind();
        if (!b || build_ == Build::Building)
            return false;
        auto& items = service_.Loader().D3Items();
        const auto outfit = b.appearance->OutfitOf(*b.adapter);
        D3OutfitPreset preset;
        for (usize s = 0; s < kD3VisualSlots.size(); ++s) {
            const auto& worn = outfit.slots[s];
            if (worn.itemGbid != -1)
                if (const auto* rec = items.FindByGbid(static_cast<u32>(worn.itemGbid)))
                    preset.items[s] = rec->name;
            preset.dyes[s] = worn.dyeType;
        }
        preset.sheathed = outfit.sheathed;
        WriteD3OutfitPreset(PresetFile(), name, preset);
        return true;
    }

    bool LoadPreset(std::string_view name) override {
        const D3OutfitPreset preset = ReadD3OutfitPreset(PresetFile(), name);
        bool any = false;
        for (usize s = 0; s < kD3VisualSlots.size(); ++s) {
            const i32 slot = static_cast<i32>(s);
            const auto& item = preset.items[s];
            // Unknown names report and skip — a preset from a newer snapshot must
            // not wipe the outfit it cannot fully express.
            if (!item)
                SetOutfitItem(slot, "");
            else if (SetOutfitItem(slot, *item))
                any = true;
            else
                std::fprintf(stderr, "[d3-outfit] preset '%.*s': unknown item '%s'\n",
                             static_cast<int>(name.size()), name.data(), item->c_str());
            SetOutfitDye(slot, preset.dyes[s]);
        }
        if (preset.sheathed)
            SetOutfitSheathed(*preset.sheathed);
        return any;
    }

    // ---- Ragdoll ----
    //
    // Straight to the adapter rather than through the appearance: the rig is not
    // a wardrobe, and the models that carry one are mostly not characters —
    // 2,367 breakables against 570 skeletons.

    bool HasRagdoll() const override {
        // A rig exists when some bone would get a dynamic body at the lod the
        // adapter builds at, which is what makes it append a stage. Asked of the
        // adapter so the menu item and the stage cannot disagree. That lod is 1:
        // true for the 570 models carrying a character proxy, false for the
        // breakables, which collapse through the client's other builder.
        const auto adapter = service_.Loader().D3AdapterOf(documents_.ActiveState().focusActor);
        return adapter && adapter->HasPhysicsRig();
    }

    bool Ragdoll() const override {
        const auto adapter = service_.Loader().D3AdapterOf(documents_.ActiveState().focusActor);
        return adapter && adapter->IsRagdoll();
    }

    void SetRagdoll(bool on) override {
        if (auto adapter = service_.Loader().D3AdapterOf(documents_.ActiveState().focusActor))
            adapter->SetRagdoll(on);
    }

private:
    enum class Build : u8 { NotStarted, Building, Ready };

    /// The focus actor's adapter and the appearance that dresses it, resolved
    /// once per call; empty when the focus actor is not a player character. The
    /// adapter is owned by the actor and outlives the call.
    struct Binding {
        io::D3ModelAdapter* adapter = nullptr;
        renderer::profiles::diablo3::D3CharacterAppearance* appearance = nullptr;
        explicit operator bool() const {
            return appearance != nullptr;
        }
    };

    Binding Bind() const {
        const auto adapter = service_.Loader().D3AdapterOf(documents_.ActiveState().focusActor);
        if (!adapter)
            return {};
        auto& appearance = service_.Loader().D3Characters();
        if (!appearance.IsCharacter(*adapter))
            return {};
        return {adapter.get(), &appearance};
    }

    bool RegistryHasItems() const {
        const RegistryState state = ItemRegistry();
        return state.stage == RegistryState::Stage::Ready && !state.empty;
    }

    /// What a character wears is not a function of its pose or of where the
    /// camera stands, so a reload is the fallback, not the path.
    void Restyle() {
        loader_.RestyleOrReload([this](u32 actor) { return service_.Loader().RestyleD3Model(actor); });
    }

    void StartBuild() {
        auto& items = service_.Loader().D3Items();
        auto* provider = service_.Scene().ActiveContentProvider();
        build_ = Build::Building;
        tasks_.Run(
            std::string(kRegistryTaskTitle),
            [&items, provider](io::ProgressMonitor& m) {
                items.EnsureBuilt(provider, &m);
                // Always Ok: a storage with no GameBalance tables is a normal
                // state the popup reads off the emptiness, not an error box in
                // front of a user who pressed a button labelled Equip.
                return io::TaskResult::Ok();
            },
            [this](const io::TaskOutcome&) { build_ = Build::Ready; },
            /*cancellable=*/false,
            // NOT modal: the popup draws the progress where the user is looking.
            /*modal=*/false);
    }

    renderer::RenderService& service_;
    io::LoadTaskRunner& tasks_;
    DocumentManager& documents_;
    DocumentLoader& loader_;
    Build build_ = Build::NotStarted;
};

} // namespace

std::unique_ptr<D3Wardrobe> MakeD3Wardrobe(renderer::RenderService& service, io::LoadTaskRunner& tasks,
                                           DocumentManager& documents, DocumentLoader& loader) {
    return std::make_unique<D3WardrobeImpl>(service, tasks, documents, loader);
}

} // namespace whiteout::flakes

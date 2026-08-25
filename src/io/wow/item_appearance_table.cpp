#include "io/wow/item_appearance_table.h"

#include "whiteout/flakes/content_provider.h"

#include <whiteout/database/parser.h>
#include <whiteout/database/table.h>

#include <algorithm>
#include <cstdio>
#include <optional>
#include <utility>

namespace whiteout::flakes::io::wow {

namespace db = ::whiteout::database;

namespace {

// Path first, for an extracted `dbfilesclient/` dump; fileDataID second,
// because that is the only identity a CASC root with no listfile has. A DB2's
// fileDataID is assigned once and never reissued, and all four below are
// unchanged between the March and August 2026 community listfiles.
struct Source {
    const char* path;
    u32 fileId;
};
constexpr Source kModelFileData{"dbfilesclient/modelfiledata.db2", 1337833};
constexpr Source kItemDisplayInfo{"dbfilesclient/itemdisplayinfo.db2", 1266429};
constexpr Source kModelMatRes{"dbfilesclient/itemdisplayinfomodelmatres.db2", 4050937};
constexpr Source kTextureFileData{"dbfilesclient/texturefiledata.db2", 982459};

// ---- Column positions -------------------------------------------------------
//
// Read by index rather than through a bound Schema, for the reason
// creature_skin_table.cpp gives: a Schema has to name every column to bind, so
// one added column in a later build would take the whole feature down.
//
// ModelFileData — the row id *is* the fileDataID.
constexpr u32 kMfdGeoBox = 0;    // float[6], the shape that identifies the layout
constexpr u32 kMfdResources = 4; // ModelResourcesID
// ItemDisplayInfo — the paired model arrays, ahead of the geoset groups.
constexpr u32 kIdiModelRes = 10; // ModelResourcesID[2]
constexpr u32 kIdiModelType = 12;
// ItemDisplayInfoModelMatRes — the display is the *relationship* key, not a
// field.
constexpr u32 kMmrResources = 0, kMmrTextureType = 1, kMmrModelIndex = 2;
// TextureFileData — the row id is the fileDataID, as in ChrCustomizationTable.
constexpr u32 kTfdUsage = 1, kTfdResources = 2;

/// How many models one display row can carry, which is what makes the model
/// index in ItemDisplayInfoModelMatRes mean anything: a pair of shoulders is
/// one display and two `.m2` files.
constexpr u32 kModelsPerDisplay = 2;

std::optional<db::Table> Read(IContentProvider& provider, const Source& src) {
    auto bytes = provider.ReadFile(src.path);
    if (!bytes || bytes->empty())
        bytes = provider.ReadFile(ContentRef::FromFileId(src.fileId));
    if (!bytes || bytes->empty()) {
        std::fprintf(stderr,
                     "[wow] item tables: %s unreadable by path or by id %u — item skins stay "
                     "unresolved\n",
                     src.path, src.fileId);
        return std::nullopt;
    }
    db::Parser parser;
    auto table = parser.parse(std::move(*bytes));
    if (!table) {
        std::fprintf(stderr, "[wow] item tables: %s failed to parse\n", src.path);
        for (const auto& issue : parser.getIssues())
            std::fprintf(stderr, "[wow]   %s\n", issue.c_str());
    }
    return table;
}

bool Wide(const db::Table& t, u32 minFields, const char* name) {
    if (t.fields().size() > minFields)
        return true;
    std::fprintf(stderr,
                 "[wow] %s has %zu fields, needs more than %u — item skins stay unresolved\n", name,
                 t.fields().size(), minFields);
    return false;
}

} // namespace

void ItemAppearanceTable::Clear() {
    loaded_ = false;
    loadFailed_ = false;
    resByFile_.clear();
    byModelRes_.clear();
    appearances_.clear();
    appearances_.shrink_to_fit();
}

bool ItemAppearanceTable::Load(IContentProvider& provider) {
    if (loaded_)
        return true;
    if (loadFailed_)
        return false;

    auto models = Read(provider, kModelFileData);
    auto displays = Read(provider, kItemDisplayInfo);
    auto matRes = Read(provider, kModelMatRes);
    auto texFiles = Read(provider, kTextureFileData);
    if (!models || !displays || !matRes || !texFiles) {
        loadFailed_ = true;
        return false;
    }
    if (!Wide(*models, kMfdResources, "ModelFileData") ||
        !Wide(*displays, kIdiModelType, "ItemDisplayInfo") ||
        !Wide(*matRes, kMmrModelIndex, "ItemDisplayInfoModelMatRes") ||
        !Wide(*texFiles, kTfdResources, "TextureFileData")) {
        loadFailed_ = true;
        return false;
    }
    // The paired arrays are what the whole join hangs on, and a build that
    // moved them would otherwise read back as a table full of zeros.
    if (models->fields()[kMfdGeoBox].arrayCount != 6 ||
        displays->fields()[kIdiModelRes].arrayCount != kModelsPerDisplay ||
        displays->fields()[kIdiModelType].arrayCount != kModelsPerDisplay) {
        std::fprintf(stderr,
                     "[wow] item tables have an unexpected layout — item skins stay unresolved\n");
        loadFailed_ = true;
        return false;
    }

    Clear();

    // ---- MaterialResourcesID → the `.blp` ----------------------------------
    //
    // Usage 0 is the texture itself; 933 of the 214436 shipped rows carry
    // something else, and taking one of those would bind it as a diffuse.
    std::unordered_map<u32, u32> texByResources;
    texByResources.reserve(texFiles->rowCount());
    for (usize i = 0; i < texFiles->rowCount(); ++i) {
        const db::Row row = texFiles->row(i);
        if (row.isEncrypted() || row.getUInt(kTfdUsage) != 0)
            continue;
        if (const u32 resources = static_cast<u32>(row.getUInt(kTfdResources)); resources != 0)
            texByResources.try_emplace(resources, row.id());
    }

    // ---- ItemDisplayInfoModelMatRes, grouped by the display it belongs to ---
    //
    // A flat vector sorted by display rather than a map of vectors: 141309 rows
    // across 96349 displays is one heap allocation per display the other way.
    struct MatResRow {
        u32 display = 0;
        u32 resources = 0;
        u16 textureType = 0;
        u16 modelIndex = 0;
    };
    std::vector<MatResRow> matRows;
    matRows.reserve(matRes->rowCount());
    for (usize i = 0; i < matRes->rowCount(); ++i) {
        const db::Row row = matRes->row(i);
        if (row.isEncrypted())
            continue;
        const auto display = row.relationId();
        if (!display)
            continue;
        matRows.push_back({*display, static_cast<u32>(row.getUInt(kMmrResources)),
                           static_cast<u16>(row.getUInt(kMmrTextureType)),
                           static_cast<u16>(row.getUInt(kMmrModelIndex))});
    }
    std::sort(matRows.begin(), matRows.end(),
              [](const MatResRow& a, const MatResRow& b) { return a.display < b.display; });

    // ---- One appearance per (model resource, display, model slot) -----------
    //
    // Paired rather than pushed straight into `appearances_`: the runs have to
    // be contiguous per resource, and the table lists displays by id.
    std::vector<std::pair<u32, ItemAppearance>> pairs;
    for (usize i = 0; i < displays->rowCount(); ++i) {
        const db::Row row = displays->row(i);
        if (row.isEncrypted())
            continue;
        const auto first =
            std::lower_bound(matRows.begin(), matRows.end(), row.id(),
                             [](const MatResRow& r, u32 id) { return r.display < id; });
        if (first == matRows.end() || first->display != row.id())
            continue;
        for (u32 slot = 0; slot < kModelsPerDisplay; ++slot) {
            const u32 resources = static_cast<u32>(row.getUInt(kIdiModelRes, slot));
            if (resources == 0)
                continue;
            ItemAppearance look;
            look.displayId = row.id();
            bool any = false;
            for (auto it = first; it != matRows.end() && it->display == row.id(); ++it) {
                if (it->modelIndex != slot)
                    continue;
                const i32 target = ReplaceableSlotOfType(it->textureType);
                if (target < 0)
                    continue;
                const auto tex = texByResources.find(it->resources);
                if (tex == texByResources.end())
                    continue;
                look.texture[target] = tex->second;
                any = true;
            }
            // An appearance that names no texture replaces nothing, so it is
            // not a look a picker should be able to land on.
            if (any)
                pairs.emplace_back(resources, look);
        }
    }

    // By resource, then by display id — the same ordering rule
    // CreatureSkinTable uses, so "appearance 0" is the first look authored for
    // the object rather than whichever row the table happened to list first.
    std::sort(pairs.begin(), pairs.end(), [](const auto& a, const auto& b) {
        if (a.first != b.first)
            return a.first < b.first;
        return a.second.displayId < b.second.displayId;
    });

    appearances_.reserve(pairs.size());
    for (usize i = 0; i < pairs.size();) {
        const u32 resources = pairs[i].first;
        const u32 begin = static_cast<u32>(appearances_.size());
        u32 count = 0;
        for (; i < pairs.size() && pairs[i].first == resources; ++i, ++count)
            appearances_.push_back(pairs[i].second);
        byModelRes_.emplace(resources, Range{begin, count});
    }

    // ---- Which `.m2` wears which resource -----------------------------------
    //
    // Only the files a display actually names. ModelFileData covers every model
    // in the game, creatures and doodads included, and keeping the other 70000
    // would make a miss here mean "an item with no look" rather than "not an
    // item at all".
    for (usize i = 0; i < models->rowCount(); ++i) {
        const db::Row row = models->row(i);
        if (row.isEncrypted())
            continue;
        const u32 resources = static_cast<u32>(row.getUInt(kMfdResources));
        if (resources != 0 && byModelRes_.count(resources) != 0)
            resByFile_.emplace(row.id(), resources);
    }

    loaded_ = true;
    std::printf("[wow] item tables: %zu models, %zu appearances\n", resByFile_.size(),
                appearances_.size());
    return true;
}

std::span<const ItemAppearance> ItemAppearanceTable::ForModel(u32 modelFileDataId) const {
    const auto file = resByFile_.find(modelFileDataId);
    if (file == resByFile_.end())
        return {};
    const auto range = byModelRes_.find(file->second);
    if (range == byModelRes_.end())
        return {};
    return std::span<const ItemAppearance>(appearances_)
        .subspan(range->second.begin, range->second.count);
}

} // namespace whiteout::flakes::io::wow

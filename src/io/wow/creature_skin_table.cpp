#include "io/wow/creature_skin_table.h"

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

constexpr const char* kModelDataPath = "dbfilesclient/creaturemodeldata.db2";
constexpr const char* kDisplayInfoPath = "dbfilesclient/creaturedisplayinfo.db2";

// ---- Column positions -------------------------------------------------------
//
// Read by index rather than through a bound Schema on purpose. A Schema has to
// name *every* column to bind — WDB5 and later match one-to-one against the
// file's field list — so a build that adds one unrelated column would fail the
// bind and take the whole feature with it. Nothing here needs the other 30.
//
// Both positions have been stable since Warlords moved these tables onto
// fileDataIDs, and both are checked below against a shape only that layout has:
// CreatureModelData opens with the 6-float GeoBox, and CreatureDisplayInfo ends
// with the texture-variation array. Verified against WOWSTATIC_12_1_0_68914.
constexpr u32 kModelDataGeoBox = 0;      // float[6]
constexpr u32 kModelDataFileDataId = 2;  // the `.m2` this row describes
constexpr u32 kDisplayInfoModelId = 1;   // → CreatureModelData::ID

bool ParseThrough(IContentProvider& provider, const char* path, db::Parser& parser,
                  std::optional<db::Table>& out) {
    auto bytes = provider.ReadFile(path);
    if (!bytes || bytes->empty()) {
        // The id is worth printing: on a World of Warcraft root the path is a
        // listfile alias for it, so "not found, id 0" is a missing listfile
        // while "not found, id N" is a file the install really cannot produce.
        std::fprintf(stderr, "[wow] creature tables: '%s' not found (id %u)\n", path,
                     provider.FileIdForPath(path));
        return false;
    }
    out = parser.parse(std::move(*bytes));
    if (!out) {
        std::fprintf(stderr, "[wow] creature tables: '%s' failed to parse\n", path);
        for (const auto& issue : parser.getIssues())
            std::fprintf(stderr, "[wow]   %s\n", issue.c_str());
        return false;
    }
    return true;
}

} // namespace

void CreatureSkinTable::Clear() {
    loaded_ = false;
    skins_.clear();
    skins_.shrink_to_fit();
    byModelFile_.clear();
}

bool CreatureSkinTable::Load(IContentProvider& provider) {
    if (loaded_)
        return true;

    db::Parser parser;
    std::optional<db::Table> models;
    std::optional<db::Table> displays;
    if (!ParseThrough(provider, kModelDataPath, parser, models) ||
        !ParseThrough(provider, kDisplayInfoPath, parser, displays))
        return false;

    const auto& modelFields = models->fields();
    const auto& displayFields = displays->fields();
    if (modelFields.size() <= kModelDataFileDataId ||
        modelFields[kModelDataGeoBox].arrayCount != 6) {
        std::fprintf(stderr, "[wow] CreatureModelData has an unexpected layout (%zu fields) — "
                             "monster skins stay unresolved\n",
                     modelFields.size());
        return false;
    }
    // The variation array is last, and there are three of it (a fourth slot
    // appeared in Dragonflight and the client still only replaces 11..13).
    if (displayFields.size() <= kDisplayInfoModelId || displayFields.back().arrayCount < 3) {
        std::fprintf(stderr, "[wow] CreatureDisplayInfo has an unexpected layout (%zu fields) — "
                             "monster skins stay unresolved\n",
                     displayFields.size());
        return false;
    }
    const u32 variationField = static_cast<u32>(displayFields.size()) - 1;

    // CreatureModelData::ID → the `.m2` it names. Every display row is one
    // lookup into this.
    std::unordered_map<u32, u32> fileByModelRow;
    fileByModelRow.reserve(models->rowCount());
    for (usize i = 0; i < models->rowCount(); ++i) {
        const db::Row row = models->row(i);
        if (row.isEncrypted())
            continue;
        const u32 fileId = static_cast<u32>(row.getUInt(kModelDataFileDataId));
        if (fileId != 0)
            fileByModelRow.emplace(row.id(), fileId);
    }

    // Paired rather than pushed straight into `skins_`: the runs have to be
    // contiguous per model, and the table lists displays by id, not by model.
    std::vector<std::pair<u32, MonsterSkin>> pairs;
    pairs.reserve(displays->rowCount());
    for (usize i = 0; i < displays->rowCount(); ++i) {
        const db::Row row = displays->row(i);
        if (row.isEncrypted())
            continue;
        const auto model = fileByModelRow.find(static_cast<u32>(row.getUInt(kDisplayInfoModelId)));
        if (model == fileByModelRow.end())
            continue;
        MonsterSkin skin;
        skin.displayId = row.id();
        bool any = false;
        for (u32 slot = 0; slot < 3; ++slot) {
            skin.texture[slot] = static_cast<u32>(row.getUInt(variationField, slot));
            any = any || skin.texture[slot] != 0;
        }
        // A display that names no texture replaces nothing — the client's loop
        // skips an empty variation rather than blanking the slot — so it is not
        // a variation a picker should be able to land on.
        if (any)
            pairs.emplace_back(model->second, skin);
    }

    // By model, then by display id. The id order is what makes "variation 0"
    // the plain cow rather than whichever row the table happened to list first.
    std::sort(pairs.begin(), pairs.end(), [](const auto& a, const auto& b) {
        if (a.first != b.first)
            return a.first < b.first;
        return a.second.displayId < b.second.displayId;
    });

    skins_.clear();
    skins_.reserve(pairs.size());
    byModelFile_.clear();
    for (usize i = 0; i < pairs.size();) {
        const u32 file = pairs[i].first;
        const u32 begin = static_cast<u32>(skins_.size());
        u32 count = 0;
        for (; i < pairs.size() && pairs[i].first == file; ++i, ++count)
            skins_.push_back(pairs[i].second);
        byModelFile_.emplace(file, Range{begin, count});
    }

    loaded_ = true;
    std::printf("[wow] creature tables: %zu models, %zu skins\n", byModelFile_.size(),
                skins_.size());
    return true;
}

std::span<const MonsterSkin> CreatureSkinTable::ForModel(u32 modelFileDataId) const {
    const auto it = byModelFile_.find(modelFileDataId);
    if (it == byModelFile_.end())
        return {};
    return std::span<const MonsterSkin>(skins_).subspan(it->second.begin, it->second.count);
}

} // namespace whiteout::flakes::io::wow

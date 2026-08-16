#include "io/wow/chr_customization_table.h"

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

// ---- Column positions -------------------------------------------------------
//
// Read by index, never through a bound Schema — see the header. Each table
// below carries a shape check next to it; a build that moves a column fails the
// check and the feature turns itself off rather than compositing garbage.
// Verified against the corpus dump of a 11.x install.

// ChrRaceXChrModel
constexpr u32 kRxmRace = 0, kRxmModel = 1, kRxmSex = 2;
// ChrModel
constexpr u32 kCmSex = 3, kCmDisplay = 4, kCmLayout = 5, kCmSkeleton = 7;
// CreatureDisplayInfo / CreatureModelData — the same two joins CreatureSkinTable
// makes, and for the same reason: ChrModel names a display, not a file.
constexpr u32 kCdiModel = 1;
constexpr u32 kCmdGeoBox = 0, kCmdFile = 2;
// CharComponentTextureSections
constexpr u32 kSecLayout = 0, kSecType = 1, kSecX = 2, kSecY = 3, kSecW = 4, kSecH = 5;
// ChrModelMaterial
constexpr u32 kMatLayout = 1, kMatTexType = 2, kMatW = 3, kMatH = 4;
// ChrModelTextureLayer — the layout is the *relationship* key, not a field.
constexpr u32 kLayTexType = 0, kLayLayer = 1, kLayBlend = 2, kLaySectionMask = 4, kLayTarget = 7;
// ChrCustomizationOption
constexpr u32 kOptName = 0, kOptModel = 4, kOptOrder = 5;
// ChrCustomizationChoice
constexpr u32 kChoName = 0, kChoOption = 2, kChoOrder = 5;
// ChrCustomizationElement
constexpr u32 kElemChoice = 0, kElemRelated = 1, kElemGeoset = 2, kElemMaterial = 4;
// ChrCustomizationGeoset
constexpr u32 kGeoType = 0, kGeoId = 1;
// ChrCustomizationMaterial
constexpr u32 kCmatTarget = 0, kCmatResources = 1;
// TextureFileData — the row id *is* the fileDataID.
constexpr u32 kTfdUsage = 1, kTfdResources = 2;

std::optional<db::Table> Read(IContentProvider& provider, const char* path) {
    auto bytes = provider.ReadFile(path);
    if (!bytes || bytes->empty()) {
        // Retry by fileDataID. On a World of Warcraft root the path is only a
        // listfile alias, and the two routes are not equivalent: the by-name
        // one resolves through the root manifest's name hashes and comes back
        // empty for several tables on a live install — `texturefiledata.db2`
        // among them, which is the one a composite cannot do without. Reading
        // the same id directly works. A zero id means no listfile, and then
        // there is nothing left to try.
        if (const u32 id = provider.FileIdForPath(path); id != 0)
            bytes = provider.ReadFile(ContentRef::FromFileId(id));
    }
    if (!bytes || bytes->empty()) {
        std::fprintf(stderr, "[wow] character tables: '%s' not found (id %u)\n", path,
                     provider.FileIdForPath(path));
        return std::nullopt;
    }
    db::Parser parser;
    auto table = parser.parse(std::move(*bytes));
    if (!table) {
        std::fprintf(stderr, "[wow] character tables: '%s' failed to parse\n", path);
        for (const auto& issue : parser.getIssues())
            std::fprintf(stderr, "[wow]   %s\n", issue.c_str());
    }
    return table;
}

bool Wide(const db::Table& t, u32 minFields, const char* name) {
    if (t.fields().size() > minFields)
        return true;
    std::fprintf(stderr, "[wow] %s has %zu fields, needs more than %u — character customisation "
                         "stays unresolved\n",
                 name, t.fields().size(), minFields);
    return false;
}

} // namespace

const TextureSection* ChrModelInfo::Section(u32 sectionType) const {
    for (const auto& s : sections)
        if (s.sectionType == sectionType)
            return &s;
    return nullptr;
}

const CompositeTarget* ChrModelInfo::Composite(u32 textureType) const {
    for (const auto& c : composites)
        if (c.textureType == textureType)
            return &c;
    return nullptr;
}

void ChrCustomizationTable::Clear() {
    loaded_ = false;
    loadFailed_ = false;
    models_.clear();
    modelByFile_.clear();
    elements_.clear();
    elements_.shrink_to_fit();
    elementsByChoice_.clear();
    textureByResources_.clear();
}

const ChrModelInfo* ChrCustomizationTable::ModelForFile(u32 modelFileId) const {
    const auto byFile = modelByFile_.find(modelFileId);
    if (byFile == modelByFile_.end())
        return nullptr;
    const auto it = models_.find(byFile->second);
    return it == models_.end() ? nullptr : &it->second;
}

std::span<const ChoiceElement> ChrCustomizationTable::Elements(u32 choiceId) const {
    const auto it = elementsByChoice_.find(choiceId);
    if (it == elementsByChoice_.end())
        return {};
    return std::span<const ChoiceElement>(elements_).subspan(it->second.begin, it->second.count);
}

u32 ChrCustomizationTable::TextureFileFor(u32 materialResourcesId) const {
    const auto it = textureByResources_.find(materialResourcesId);
    return it == textureByResources_.end() ? 0u : it->second;
}

bool ChrCustomizationTable::Load(IContentProvider& provider) {
    if (loaded_)
        return true;
    // One attempt per install, unlike CreatureSkinTable, which retries. Thirteen
    // tables and a third of a million rows is too much to re-read on every
    // spawn of every character model in a session whose install cannot serve
    // them. SetContentProvider clears this along with everything else, so
    // pointing at another install does try again.
    if (loadFailed_)
        return false;

    auto raceXModel = Read(provider, "dbfilesclient/chrracexchrmodel.db2");
    auto chrModel = Read(provider, "dbfilesclient/chrmodel.db2");
    auto displays = Read(provider, "dbfilesclient/creaturedisplayinfo.db2");
    auto modelData = Read(provider, "dbfilesclient/creaturemodeldata.db2");
    auto materials = Read(provider, "dbfilesclient/chrmodelmaterial.db2");
    auto layers = Read(provider, "dbfilesclient/chrmodeltexturelayer.db2");
    auto sections = Read(provider, "dbfilesclient/charcomponenttexturesections.db2");
    auto options = Read(provider, "dbfilesclient/chrcustomizationoption.db2");
    auto choices = Read(provider, "dbfilesclient/chrcustomizationchoice.db2");
    auto elements = Read(provider, "dbfilesclient/chrcustomizationelement.db2");
    auto geosets = Read(provider, "dbfilesclient/chrcustomizationgeoset.db2");
    auto cmaterials = Read(provider, "dbfilesclient/chrcustomizationmaterial.db2");
    auto texFiles = Read(provider, "dbfilesclient/texturefiledata.db2");
    if (!raceXModel || !chrModel || !displays || !modelData || !materials || !layers || !sections ||
        !options || !choices || !elements || !geosets || !cmaterials || !texFiles) {
        loadFailed_ = true;
        return false;
    }

    // Shape checks. The two that are more than a field count are the ones a
    // reordering would otherwise pass silently: CreatureModelData opens with
    // the 6-float GeoBox, and ChrModelTextureLayer keys its layout through the
    // relationship map rather than a column, so a build that moved it to a
    // column would leave every layer unclaimed instead of misplaced.
    if (!Wide(*raceXModel, kRxmSex, "ChrRaceXChrModel") ||
        !Wide(*chrModel, kCmSkeleton, "ChrModel") ||
        !Wide(*displays, kCdiModel, "CreatureDisplayInfo") ||
        !Wide(*modelData, kCmdFile, "CreatureModelData") ||
        !Wide(*materials, kMatH, "ChrModelMaterial") ||
        !Wide(*layers, kLayTarget, "ChrModelTextureLayer") ||
        !Wide(*sections, kSecH, "CharComponentTextureSections") ||
        !Wide(*options, kOptOrder, "ChrCustomizationOption") ||
        !Wide(*choices, kChoOrder, "ChrCustomizationChoice") ||
        !Wide(*elements, kElemMaterial, "ChrCustomizationElement") ||
        !Wide(*geosets, kGeoId, "ChrCustomizationGeoset") ||
        !Wide(*cmaterials, kCmatResources, "ChrCustomizationMaterial") ||
        !Wide(*texFiles, kTfdResources, "TextureFileData")) {
        loadFailed_ = true;
        return false;
    }
    if (modelData->fields()[kCmdGeoBox].arrayCount != 6) {
        std::fprintf(stderr, "[wow] CreatureModelData does not open with the GeoBox — character "
                             "customisation stays unresolved\n");
        return false;
    }

    Clear();

    // ---- Identity: which ChrModel each `.m2` is -----------------------------
    std::unordered_map<u32, u32> fileByModelRow; // CreatureModelData::ID → fileDataID
    fileByModelRow.reserve(modelData->rowCount());
    for (usize i = 0; i < modelData->rowCount(); ++i) {
        const db::Row row = modelData->row(i);
        if (row.isEncrypted())
            continue;
        if (const u32 file = static_cast<u32>(row.getUInt(kCmdFile)); file != 0)
            fileByModelRow.emplace(row.id(), file);
    }
    std::unordered_map<u32, u32> fileByDisplay;
    fileByDisplay.reserve(displays->rowCount());
    for (usize i = 0; i < displays->rowCount(); ++i) {
        const db::Row row = displays->row(i);
        if (row.isEncrypted())
            continue;
        const auto it = fileByModelRow.find(static_cast<u32>(row.getUInt(kCdiModel)));
        if (it != fileByModelRow.end())
            fileByDisplay.emplace(row.id(), it->second);
    }

    for (usize i = 0; i < chrModel->rowCount(); ++i) {
        const db::Row row = chrModel->row(i);
        if (row.isEncrypted())
            continue;
        ChrModelInfo info;
        info.id = row.id();
        info.sex = static_cast<u32>(row.getUInt(kCmSex));
        info.displayId = static_cast<u32>(row.getUInt(kCmDisplay));
        info.layoutId = static_cast<u32>(row.getUInt(kCmLayout));
        info.skeletonFileId = static_cast<u32>(row.getUInt(kCmSkeleton));
        if (const auto it = fileByDisplay.find(info.displayId); it != fileByDisplay.end())
            info.modelFileId = it->second;
        models_.emplace(info.id, std::move(info));
    }
    // Which race wears each model. Not needed to dress one — ChrModel carries
    // its own sex, and the options hang off the ChrModelID — but it is the only
    // place a host can learn that model 1 is "Human, male" rather than a bare
    // number, and a model shared by two races reports the first that claims it.
    for (usize i = 0; i < raceXModel->rowCount(); ++i) {
        const db::Row row = raceXModel->row(i);
        if (row.isEncrypted())
            continue;
        const auto it = models_.find(static_cast<u32>(row.getUInt(kRxmModel)));
        if (it != models_.end() && it->second.raceId == 0) {
            it->second.raceId = static_cast<u32>(row.getUInt(kRxmRace));
            it->second.sex = static_cast<u32>(row.getUInt(kRxmSex));
        }
    }
    for (const auto& [id, info] : models_)
        if (info.modelFileId != 0)
            modelByFile_.emplace(info.modelFileId, id);

    // ---- Composites, layers and sections, per layout ------------------------
    std::unordered_map<u32, std::vector<CompositeTarget>> compositesByLayout;
    for (usize i = 0; i < materials->rowCount(); ++i) {
        const db::Row row = materials->row(i);
        if (row.isEncrypted())
            continue;
        CompositeTarget c;
        c.textureType = static_cast<u32>(row.getUInt(kMatTexType));
        c.width = static_cast<u32>(row.getUInt(kMatW));
        c.height = static_cast<u32>(row.getUInt(kMatH));
        if (c.width != 0 && c.height != 0)
            compositesByLayout[static_cast<u32>(row.getUInt(kMatLayout))].push_back(c);
    }
    std::unordered_map<u32, std::vector<CompositeLayer>> layersByLayout;
    for (usize i = 0; i < layers->rowCount(); ++i) {
        const db::Row row = layers->row(i);
        if (row.isEncrypted())
            continue;
        const auto layout = row.relationId();
        if (!layout)
            continue;
        CompositeLayer l;
        l.textureType = static_cast<u32>(row.getUInt(kLayTexType));
        l.layer = static_cast<u32>(row.getUInt(kLayLayer));
        l.blendMode = static_cast<u32>(row.getUInt(kLayBlend));
        l.sectionMask = row.getInt(kLaySectionMask);
        l.target = static_cast<u32>(row.getUInt(kLayTarget, 0));
        layersByLayout[*layout].push_back(l);
    }
    std::unordered_map<u32, std::vector<TextureSection>> sectionsByLayout;
    for (usize i = 0; i < sections->rowCount(); ++i) {
        const db::Row row = sections->row(i);
        if (row.isEncrypted())
            continue;
        TextureSection s;
        s.sectionType = static_cast<u32>(row.getUInt(kSecType));
        s.x = static_cast<u32>(row.getUInt(kSecX));
        s.y = static_cast<u32>(row.getUInt(kSecY));
        s.w = static_cast<u32>(row.getUInt(kSecW));
        s.h = static_cast<u32>(row.getUInt(kSecH));
        sectionsByLayout[static_cast<u32>(row.getUInt(kSecLayout))].push_back(s);
    }

    for (auto& [id, info] : models_) {
        if (const auto it = compositesByLayout.find(info.layoutId); it != compositesByLayout.end())
            info.composites = it->second;
        if (const auto it = sectionsByLayout.find(info.layoutId); it != sectionsByLayout.end())
            info.sections = it->second;
        if (const auto it = layersByLayout.find(info.layoutId); it != layersByLayout.end()) {
            info.layers = it->second;
            std::sort(info.layers.begin(), info.layers.end(),
                      [](const CompositeLayer& a, const CompositeLayer& b) {
                          if (a.textureType != b.textureType)
                              return a.textureType < b.textureType;
                          return a.layer < b.layer;
                      });
        }
    }

    // ---- Options and their choices ------------------------------------------
    std::unordered_map<u32, std::pair<u32, usize>> optionOwner; // optionId → (model, index)
    for (usize i = 0; i < options->rowCount(); ++i) {
        const db::Row row = options->row(i);
        if (row.isEncrypted())
            continue;
        const auto model = models_.find(static_cast<u32>(row.getUInt(kOptModel)));
        if (model == models_.end())
            continue;
        CustomizationOption opt;
        opt.id = row.id();
        opt.orderIndex = static_cast<u32>(row.getUInt(kOptOrder));
        opt.name = std::string(row.getString(kOptName));
        optionOwner.emplace(opt.id, std::pair{model->first, model->second.options.size()});
        model->second.options.push_back(std::move(opt));
    }
    for (usize i = 0; i < choices->rowCount(); ++i) {
        const db::Row row = choices->row(i);
        if (row.isEncrypted())
            continue;
        const auto owner = optionOwner.find(static_cast<u32>(row.getUInt(kChoOption)));
        if (owner == optionOwner.end())
            continue;
        CustomizationChoice c;
        c.id = row.id();
        c.orderIndex = static_cast<u32>(row.getUInt(kChoOrder));
        c.name = std::string(row.getString(kChoName));
        models_[owner->second.first].options[owner->second.second].choices.push_back(std::move(c));
    }
    for (auto& [id, info] : models_) {
        std::sort(info.options.begin(), info.options.end(),
                  [](const CustomizationOption& a, const CustomizationOption& b) {
                      return a.orderIndex < b.orderIndex;
                  });
        for (auto& opt : info.options)
            std::sort(opt.choices.begin(), opt.choices.end(),
                      [](const CustomizationChoice& a, const CustomizationChoice& b) {
                          return a.orderIndex < b.orderIndex;
                      });
    }

    // ---- Elements, resolved through the geoset and material side tables -----
    std::unordered_map<u32, i32> geosetByRow;
    geosetByRow.reserve(geosets->rowCount());
    for (usize i = 0; i < geosets->rowCount(); ++i) {
        const db::Row row = geosets->row(i);
        if (row.isEncrypted())
            continue;
        // `GeosetType * 100 + GeosetID` is the `skinSectionId` the model
        // declares — group 4 value 1 is geoset 401, exactly as GeosRenderPrep
        // spells it.
        geosetByRow.emplace(row.id(), static_cast<i32>(row.getUInt(kGeoType) * 100 +
                                                       row.getUInt(kGeoId)));
    }
    std::unordered_map<u32, std::pair<u32, u32>> materialByRow; // → (target, resources)
    materialByRow.reserve(cmaterials->rowCount());
    for (usize i = 0; i < cmaterials->rowCount(); ++i) {
        const db::Row row = cmaterials->row(i);
        if (row.isEncrypted())
            continue;
        materialByRow.emplace(row.id(), std::pair{static_cast<u32>(row.getUInt(kCmatTarget)),
                                                  static_cast<u32>(row.getUInt(kCmatResources))});
    }

    std::vector<std::pair<u32, ChoiceElement>> pairs;
    pairs.reserve(elements->rowCount());
    for (usize i = 0; i < elements->rowCount(); ++i) {
        const db::Row row = elements->row(i);
        if (row.isEncrypted())
            continue;
        ChoiceElement e;
        e.relatedChoiceId = static_cast<u32>(row.getUInt(kElemRelated));
        if (const u32 g = static_cast<u32>(row.getUInt(kElemGeoset)); g != 0) {
            if (const auto it = geosetByRow.find(g); it != geosetByRow.end())
                e.geoset = it->second;
        }
        if (const u32 m = static_cast<u32>(row.getUInt(kElemMaterial)); m != 0) {
            if (const auto it = materialByRow.find(m); it != materialByRow.end()) {
                e.materialTarget = it->second.first;
                e.materialResourcesId = it->second.second;
            }
        }
        // An element that resolved neither is one this renderer does not act on
        // — a bone set, a skinned model, a voice. Dropping it keeps the runs
        // below to the elements that matter.
        if (e.geoset >= 0 || e.materialResourcesId != 0)
            pairs.emplace_back(static_cast<u32>(row.getUInt(kElemChoice)), e);
    }
    std::stable_sort(pairs.begin(), pairs.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });
    elements_.reserve(pairs.size());
    for (usize i = 0; i < pairs.size();) {
        const u32 choice = pairs[i].first;
        const u32 begin = static_cast<u32>(elements_.size());
        u32 count = 0;
        for (; i < pairs.size() && pairs[i].first == choice; ++i, ++count)
            elements_.push_back(pairs[i].second);
        elementsByChoice_.emplace(choice, Range{begin, count});
    }

    // ---- MaterialResources → the `.blp` ------------------------------------
    //
    // One resources id can name several files — a diffuse sheet and whatever
    // else that material carries. Usage 0 is the sheet a composite pastes, so
    // it is claimed first and a second pass fills in only the ids that had no
    // usage-0 row at all, rather than letting one displace the other by
    // table order.
    textureByResources_.reserve(texFiles->rowCount());
    for (usize i = 0; i < texFiles->rowCount(); ++i) {
        const db::Row row = texFiles->row(i);
        if (row.isEncrypted() || row.getUInt(kTfdUsage) != 0)
            continue;
        if (const u32 resources = static_cast<u32>(row.getUInt(kTfdResources)); resources != 0)
            textureByResources_.try_emplace(resources, row.id());
    }
    for (usize i = 0; i < texFiles->rowCount(); ++i) {
        const db::Row row = texFiles->row(i);
        if (row.isEncrypted())
            continue;
        if (const u32 resources = static_cast<u32>(row.getUInt(kTfdResources)); resources != 0)
            textureByResources_.try_emplace(resources, row.id());
    }

    loaded_ = true;
    std::printf("[wow] character tables: %zu models, %zu with a file, %zu choice elements\n",
                models_.size(), modelByFile_.size(), elements_.size());
    return true;
}

} // namespace whiteout::flakes::io::wow

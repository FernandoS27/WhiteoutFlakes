#include "renderer/profiles/wow/wow_character_appearance.h"

#include "io/m2/m2_model_adapter.h"
#include "io/wow/character_geosets.h"
#include "renderer/model/model_source_utils.h"
#include "whiteout/flakes/content_provider.h"

#include <cstdio>
#include <utility>

namespace whiteout::flakes::renderer::profiles::wow {

namespace wowio = ::whiteout::flakes::io::wow;

void WowCharacterAppearance::SetContentProvider(io::IContentProvider* provider) {
    if (provider_ == provider)
        return;
    provider_ = provider;
    Clear();
}

u32 WowCharacterAppearance::ModelFileId(const ContentRef& ref) const {
    if (ref.IsFileId())
        return ref.fileId;
    if (!provider_ || ref.path.empty())
        return 0;
    return provider_->FileIdForPath(ref.path);
}

WowCharacterAppearance::Selection&
WowCharacterAppearance::SelectionFor(const ContentRef& ref, const wowio::ChrModelInfo& model) {
    Selection& sel = byModel_[ref.Describe()];
    if (sel.chrModelId != model.id || sel.choiceIndex.size() != model.options.size()) {
        sel.chrModelId = model.id;
        // Index 0 of every option, which is the choice the character creator
        // opens on rather than an arbitrary one.
        sel.choiceIndex.assign(model.options.size(), 0);
    }
    return sel;
}

std::vector<CharacterOptionView>
WowCharacterAppearance::Options(const ContentRef& modelRef) const {
    std::vector<CharacterOptionView> out;
    const auto it = byModel_.find(modelRef.Describe());
    if (it == byModel_.end())
        return out;
    const wowio::ChrModelInfo* model = tables_.ModelForFile(ModelFileId(modelRef));
    if (!model || model->id != it->second.chrModelId)
        return out;
    out.reserve(model->options.size());
    for (usize i = 0; i < model->options.size(); ++i) {
        CharacterOptionView v;
        v.name = model->options[i].name;
        v.optionId = model->options[i].id;
        v.choiceCount = static_cast<u32>(model->options[i].choices.size());
        v.selected = i < it->second.choiceIndex.size() ? it->second.choiceIndex[i] : 0;
        out.push_back(std::move(v));
    }
    return out;
}

void WowCharacterAppearance::SetChoice(const ContentRef& modelRef, u32 optionId, u32 choiceIndex) {
    const auto it = byModel_.find(modelRef.Describe());
    if (it == byModel_.end())
        return;
    const wowio::ChrModelInfo* model = tables_.ModelForFile(ModelFileId(modelRef));
    if (!model)
        return;
    for (usize i = 0; i < model->options.size() && i < it->second.choiceIndex.size(); ++i) {
        if (model->options[i].id != optionId)
            continue;
        const u32 count = static_cast<u32>(model->options[i].choices.size());
        // Wraps, so a host can step the value without knowing how many an
        // option has — the same contract WowReplaceableTextures::SetVariation
        // offers for creature skins.
        it->second.choiceIndex[i] = count ? choiceIndex % count : 0;
        return;
    }
}

bool WowCharacterAppearance::Apply(io::M2ModelAdapter& adapter, const ContentRef& modelRef) {
    const auto& model = adapter.SourceModel();
    if (!wowio::IsCharacterModel(model))
        return false;

    const std::vector<u16> declared = wowio::DeclaredGeosets(model, adapter.ProfileIndex());

    // A character model with no databases in reach still gets its geosets cut
    // down to the client's fixed defaults. That is the difference between a
    // bald human and twenty-six overlapping hairstyles, and it needs nothing
    // but the model.
    const wowio::ChrModelInfo* info = nullptr;
    if (provider_ && tables_.Load(*provider_))
        info = tables_.ModelForFile(ModelFileId(modelRef));
    if (!info) {
        adapter.SetVisibleGeosets(wowio::VisibleGeosets(wowio::DefaultSelection(), declared));
        return true;
    }

    Selection& sel = SelectionFor(modelRef, *info);
    std::vector<u32> choiceIds;
    choiceIds.reserve(info->options.size());
    for (usize i = 0; i < info->options.size(); ++i) {
        const auto& choices = info->options[i].choices;
        if (choices.empty())
            continue;
        choiceIds.push_back(choices[sel.choiceIndex[i] % choices.size()].id);
    }

    const wowio::ResolvedAppearance appearance = ResolveAppearance(tables_, *info, choiceIds);
    adapter.SetVisibleGeosets(wowio::VisibleGeosets(appearance.geosets, declared));

    // Read and decode straight through the provider by fileDataID, which is the
    // only identity these rows carry. Synchronous, on the load thread, next to
    // the `.m2` parse that already blocks on IO — and cached per sheet, because
    // one `.blp` routinely feeds several layers of the same composite.
    std::unordered_map<u32, std::pair<std::vector<u8>, std::pair<u32, u32>>> decoded;
    auto fetch = [&](u32 fileId, std::vector<u8>& rgba, u32& w, u32& h) {
        if (const auto it = decoded.find(fileId); it != decoded.end()) {
            if (it->second.first.empty())
                return false;
            rgba = it->second.first;
            w = it->second.second.first;
            h = it->second.second.second;
            return true;
        }
        auto& slot = decoded[fileId];
        auto bytes = provider_->ReadFile(ContentRef::FromFileId(fileId));
        if (bytes && !bytes->empty()) {
            i32 dw = 0, dh = 0;
            const std::string ext = model::SniffTextureExtension(*bytes);
            if (!ext.empty() && model::DecodeToRGBA8(*bytes, ext, slot.first, dw, dh)) {
                slot.second = {static_cast<u32>(dw), static_cast<u32>(dh)};
            } else {
                slot.first.clear();
            }
        }
        if (slot.first.empty())
            return false;
        rgba = slot.first;
        w = slot.second.first;
        h = slot.second.second;
        return true;
    };

    // Moved rather than copied: the body sheet alone is 2048×1024 RGBA.
    std::vector<wowio::ComposedTexture> sheets = ComposeCharacter(*info, appearance, fetch);
    std::vector<io::M2ComposedTexture> composed;
    composed.reserve(sheets.size());
    for (wowio::ComposedTexture& sheet : sheets) {
        io::M2ComposedTexture c;
        c.textureType = sheet.textureType;
        c.width = sheet.width;
        c.height = sheet.height;
        c.rgba = std::move(sheet.rgba);
        composed.push_back(std::move(c));
    }
    if (composed.empty()) {
        std::fprintf(stderr, "[wow] character %s: no composite resolved (ChrModel %u, layout %u)\n",
                     modelRef.Describe().c_str(), info->id, info->layoutId);
    }
    adapter.SetComposedTextures(std::move(composed));
    return true;
}

} // namespace whiteout::flakes::renderer::profiles::wow

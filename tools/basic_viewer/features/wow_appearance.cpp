#include "features/wow_appearance.h"

#include "documents/document_loader.h"
#include "documents/document_manager.h"
#include "documents/model_formats.h"
#include "renderer/model/model_loader.h"
#include "renderer/profiles/wow/wow_character_appearance.h"
#include "renderer/profiles/wow/wow_replaceable_textures.h"
#include "renderer/render_service.h"
#include "whiteout/flakes/content_ref.h"
#include "whiteout/flakes/util/path_utf8.h"

namespace whiteout::flakes {

namespace {

class WowAppearanceImpl final : public WowAppearance {
public:
    WowAppearanceImpl(renderer::RenderService& service, DocumentManager& documents, DocumentLoader& loader)
        : service_(service), documents_(documents), loader_(loader) {}

    std::vector<std::string> SkinNames() const override {
        const auto& path = documents_.ActiveState().modelPath;
        if (path.empty())
            return {};
        std::vector<std::string> names;
        for (const auto& v : service_.Loader().WowReplaceables().Variations(ModelRef(path)))
            names.push_back(v.label);
        return names;
    }

    u32 Skin() const override {
        return service_.Loader().WowReplaceables().Variation();
    }

    void SetSkin(u32 skin) override {
        auto& replaceables = service_.Loader().WowReplaceables();
        if (replaceables.Variation() == skin)
            return;
        replaceables.SetVariation(skin);
        RestyleActiveModel();
    }

    std::vector<CharacterOption> CharacterOptions() const override {
        const auto& path = documents_.ActiveState().modelPath;
        if (path.empty())
            return {};
        std::vector<CharacterOption> out;
        for (const auto& o : service_.Loader().WowCharacters().Options(ModelRef(path)))
            out.push_back({o.name, o.optionId, o.choiceCount, o.selected});
        return out;
    }

    void SetCharacterChoice(u32 optionId, u32 choiceIndex) override {
        const auto& path = documents_.ActiveState().modelPath;
        if (path.empty())
            return;
        const ContentRef ref = ModelRef(path);
        service_.Loader().WowCharacters().SetChoice(ref, optionId, choiceIndex);
        loader_.RestyleOrReload([&](u32 actor) { return service_.Loader().RestyleWowModel(actor, ref); });
    }

    void RestyleActiveModel() override {
        const auto& path = documents_.ActiveState().modelPath;
        if (!IsModelKind(path, ModelKind::ForeignModel))
            return;
        const ContentRef ref = ModelRef(path);
        loader_.RestyleOrReload([&](u32 actor) { return service_.Loader().RestyleWowModel(actor, ref); });
    }

private:
    static ContentRef ModelRef(const std::filesystem::path& path) {
        return ContentRef::FromPath(io::PathToUtf8(path));
    }

    renderer::RenderService& service_;
    DocumentManager& documents_;
    DocumentLoader& loader_;
};

} // namespace

std::unique_ptr<WowAppearance> MakeWowAppearance(renderer::RenderService& service, DocumentManager& documents,
                                                 DocumentLoader& loader) {
    return std::make_unique<WowAppearanceImpl>(service, documents, loader);
}

} // namespace whiteout::flakes

#include <cornflakes/asset/war3_compatibility_validator.hpp>
#include <cornflakes/core/determinism.hpp>
#include <cornflakes/diagnostics/issue_codes.hpp>

namespace whiteout::cornflakes {

namespace {

Issue assetFatal(u32 code, std::string_view message) noexcept {
    Issue issue;
    issue.severity = Severity::Fatal;
    issue.category = Category::Asset;
    issue.code = code;
    issue.message = message;
    return issue;
}

} // namespace

bool War3CompatibilityValidator::validate(const EffectAssetModel& model, IssueBag& issues) const {
    bool ok = true;

    const auto gen = static_cast<u8>(model.generator);
    if (gen != static_cast<u8>(BakerGenerator::Editor) &&
        gen != static_cast<u8>(BakerGenerator::Baker)) {
        issues.push(assetFatal(issues::asset::kGenerator,
                               "WC3 validator: Generator must be EDITOR (0) or BAKER (1)"));
        ok = false;
    }

    if (!model.objects.empty()) {
        if (model.rootEffectUid.empty()) {
            issues.push(assetFatal(issues::asset::kNoRootEffect,
                                   "WC3 validator: no root CParticleEffect object found"));
            ok = false;
        }
        bool hasNodeGraph = false;
        for (const auto& obj : model.objects) {
            if (obj.type == "CParticleNodeGraph") {
                hasNodeGraph = true;
                break;
            }
        }
        if (hasNodeGraph && model.rootLayerUid.empty()) {
            issues.push(assetFatal(issues::asset::kNoRootLayer,
                                   "WC3 validator: source asset has CParticleNodeGraph objects "
                                   "but none with CustomName=\"Root\""));
            ok = false;
        }
    }

    return ok;
}

} // namespace whiteout::cornflakes

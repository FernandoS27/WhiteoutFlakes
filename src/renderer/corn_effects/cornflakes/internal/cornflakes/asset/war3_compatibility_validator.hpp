#pragma once

#include <cornflakes/interface/asset/effect_asset_model.hpp>
#include <cornflakes/interface/diagnostics/issue.hpp>

namespace whiteout::cornflakes {

class War3CompatibilityValidator {
public:
    bool validate(const EffectAssetModel& model, IssueBag& issues) const;
};

}

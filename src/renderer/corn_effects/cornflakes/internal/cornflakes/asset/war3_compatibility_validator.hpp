#pragma once

/// @file
/// @brief Asset-time validator that rejects features not supported by Warcraft 3 Reforged.

#include <cornflakes/interface/asset/effect_asset_model.hpp>
#include <cornflakes/interface/diagnostics/issue.hpp>

namespace whiteout::cornflakes {

/// @brief Pre-bind validator enforcing the WC3 subset of CornFx features.
class War3CompatibilityValidator {
public:
    bool validate(const EffectAssetModel& model, IssueBag& issues) const;
};

} // namespace whiteout::cornflakes

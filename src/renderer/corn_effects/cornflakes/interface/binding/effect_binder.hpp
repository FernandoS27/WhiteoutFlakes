#pragma once

/// @file
/// @brief Front-end binder that turns a parsed asset into an executable plan.

#include <cornflakes/interface/asset/effect_asset_model.hpp>
#include <cornflakes/interface/binding/effect_execution_plan.hpp>
#include <cornflakes/interface/core/arena.hpp>
#include <cornflakes/interface/diagnostics/issue.hpp>
#include <cornflakes/interface/schema/handles.hpp>

#include <optional>

namespace whiteout::cornflakes {

/// @brief Resolves layers, externals, samplers and event routes into an `EffectExecutionPlan`.
class EffectBinder {
public:
    EffectBinder() = default;

    /// @brief Bind `model` into a plan owned by `planArena`.
    /// @return The plan on success, `std::nullopt` if a fatal issue was pushed.
    std::optional<EffectExecutionPlan> bind(const EffectAssetModel& model, EffectId id,
                                            IArena& planArena, IssueBag& issues) const;
};

} // namespace whiteout::cornflakes

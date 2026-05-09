#pragma once

/// @file
/// @brief Channel-name → `LayerId` resolver against the bound `EventRoutingTable`.

#include <cornflakes/interface/binding/tables.hpp>
#include <cornflakes/interface/core/types.hpp>
#include <cornflakes/interface/schema/handles.hpp>

#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

namespace whiteout::cornflakes {

/// @brief Resolves event channels to target layer ids using the effect-wide routing table.
class EventRouter {
public:
    EventRouter() = default;

    /// @brief Collect every layer registered for `channel`.
    std::vector<LayerId> targetsFor(const EventRoutingTable& table, std::string_view channel) const;

    bool hasRoute(const EventRoutingTable& table, std::string_view channel) const noexcept;
};

} // namespace whiteout::cornflakes

#include <cornflakes/core/determinism.hpp>
#include <cornflakes/events/event_router.hpp>

namespace whiteout::cornflakes {

std::vector<LayerId> EventRouter::targetsFor(const EventRoutingTable& table,
                                             std::string_view channel) const {
    std::vector<LayerId> out;
    for (const auto& route : table.routes) {
        if (route.channel == channel) {
            out.push_back(route.target);
        }
    }
    return out;
}

bool EventRouter::hasRoute(const EventRoutingTable& table,
                           std::string_view channel) const noexcept {
    for (const auto& route : table.routes) {
        if (route.channel == channel) {
            return true;
        }
    }
    return false;
}

} // namespace whiteout::cornflakes

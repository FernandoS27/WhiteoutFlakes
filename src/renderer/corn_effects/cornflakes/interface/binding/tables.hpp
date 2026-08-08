#pragma once

#include <cornflakes/interface/core/types.hpp>
#include <cornflakes/interface/schema/handles.hpp>
#include <cornflakes/interface/service/service_types.hpp>

#include <span>
#include <string_view>

namespace whiteout::cornflakes {

struct ExternalBindingSlot {
    std::string_view name;
    u32 slotIndex = 0;
    AttributeValue::Kind kind = AttributeValue::Kind::Float;
};

struct ExternalBindingTable {
    std::span<const ExternalBindingSlot> slots;

    const ExternalBindingSlot* find(std::string_view name) const noexcept {
        for (const auto& s : slots) {
            if (s.name == name) {
                return &s;
            }
        }
        return nullptr;
    }
};

struct EventRoute {
    std::string_view channel;
    LayerId target;
    u32 broadcastMask = 0;
    u32 globalEventSlotId = 0;
    i32 parentLayerSlot = -1;
};

struct EventRoutingTable {
    std::span<const EventRoute> routes;
};

struct PayloadElementView {
    u16 fieldId = 0;
    u16 typeTag = 0;
    u32 byteOffset = 0;
    u32 byteSize = 0;
};

struct PayloadElementViewTable {
    std::span<const PayloadElementView> views;
};

}

#pragma once

/// @file
/// @brief Effect-wide lookup tables: external slots, event routes, payload element views.

#include <cornflakes/interface/core/types.hpp>
#include <cornflakes/interface/schema/handles.hpp>
#include <cornflakes/interface/service/service_types.hpp>

#include <span>
#include <string_view>

namespace whiteout::cornflakes {

/// @brief One effect-wide external slot — the canonical name/slot/kind triple.
struct ExternalBindingSlot {
    std::string_view name;
    u32 slotIndex = 0;
    AttributeValue::Kind kind = AttributeValue::Kind::Float;
};

/// @brief Effect-wide table mapping external names to their canonical slot ids.
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

/// @brief One event-channel routing entry: which layer to kick when a channel fires.
struct EventRoute {
    std::string_view channel;
    LayerId target;
    u32 broadcastMask = 0;
    u32 globalEventSlotId = 0;
    i32 parentLayerSlot = -1; ///< -1 when not bound to a parent slot.
};

/// @brief Effect-wide event routing table; consumed by `EventRouter`.
struct EventRoutingTable {
    std::span<const EventRoute> routes;
};

/// @brief One typed slice into a payload blob — field id, byte offset, byte size.
struct PayloadElementView {
    u16 fieldId = 0;
    u16 typeTag = 0;
    u32 byteOffset = 0;
    u32 byteSize = 0;
};

/// @brief Effect-wide table of payload-element views referenced by the VM.
struct PayloadElementViewTable {
    std::span<const PayloadElementView> views;
};

} // namespace whiteout::cornflakes

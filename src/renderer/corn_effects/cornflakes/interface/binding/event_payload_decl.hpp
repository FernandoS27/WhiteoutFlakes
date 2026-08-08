#pragma once

#include <cornflakes/interface/core/types.hpp>

#include <span>
#include <string_view>

namespace whiteout::cornflakes {

struct EventPayloadElement {
    u32 nameId = 0;
    u8 width = 0;
};

struct KickedEventPayloadDecl {
    std::string_view channel;
    std::span<const EventPayloadElement> elements;
};

inline constexpr u32 payloadNameId(std::string_view name) noexcept {
    u32 h = 0x811C9DC5U;
    for (const char c : name) {
        h ^= static_cast<u32>(static_cast<unsigned char>(c));
        h *= 0x01000193U;
    }
    return h;
}

}

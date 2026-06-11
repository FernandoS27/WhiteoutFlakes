#pragma once

/// @file
/// @brief Event payload element declarations — the engine matches payload elements across a
/// kick by name (`SEvent::FindPayload` on `m_NameGUID`), since the parent append-slot and the
/// child extract-index do NOT align. We parse both decls from the asset and match by name-id.

#include <cornflakes/interface/core/types.hpp>

#include <span>
#include <string_view>

namespace whiteout::cornflakes {

/// @brief One declared payload element (`CLayerCompileCacheEventPayload`). `nameId` is a stable
/// hash of `PayloadName`; `width` is 1..4 for float-family elements (from `PayloadType`), else 0.
struct EventPayloadElement {
    u32 nameId = 0;
    u8 width = 0;
};

/// @brief One emitted event's slot-ordered payload declaration, keyed by channel name.
struct KickedEventPayloadDecl {
    std::string_view channel; ///< EventName (e.g. "__e_Child").
    std::span<const EventPayloadElement> elements;
};

/// @brief FNV-1a 32-bit hash of a payload element name — the cornflakes name-id.
inline constexpr u32 payloadNameId(std::string_view name) noexcept {
    u32 h = 0x811C9DC5U;
    for (const char c : name) {
        h ^= static_cast<u32>(static_cast<unsigned char>(c));
        h *= 0x01000193U;
    }
    return h;
}

} // namespace whiteout::cornflakes

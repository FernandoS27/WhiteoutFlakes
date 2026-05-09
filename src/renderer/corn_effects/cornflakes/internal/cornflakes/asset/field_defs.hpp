#pragma once

/// @file
/// @brief Schema-side handler/field declarations used to validate assets against CornFx types.

#include <cornflakes/interface/core/types.hpp>

#include <cstddef>
#include <span>
#include <string_view>

namespace whiteout::cornflakes {

/// @brief Schema description of one field of an asset handler.
struct FieldDef {
    std::string_view name;
    std::string_view type;
};

/// @brief Schema description of one asset handler (a class + its declared fields).
struct HandlerDef {
    std::string_view name;
    std::span<const FieldDef> fields;
};

/// @brief Look up a handler descriptor by name; returns null when unknown.
const HandlerDef* findHandlerDef(std::string_view handlerName) noexcept;

std::size_t handlerDefCount() noexcept;

} // namespace whiteout::cornflakes

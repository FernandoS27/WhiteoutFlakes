#pragma once

#include <cornflakes/interface/core/types.hpp>

#include <cstddef>
#include <span>
#include <string_view>

namespace whiteout::cornflakes {

struct FieldDef {
    std::string_view name;
    std::string_view type;
};

struct HandlerDef {
    std::string_view name;
    std::span<const FieldDef> fields;
};

enum class HboSchemaVersion {
    V2_5,
    V2_9,
};

constexpr HboSchemaVersion schemaForVersion(u16 major, u16 minor) noexcept {
    return (major > 2 || (major == 2 && minor >= 9)) ? HboSchemaVersion::V2_9
                                                     : HboSchemaVersion::V2_5;
}

const HandlerDef* findHandlerDef(std::string_view handlerName,
                                 HboSchemaVersion schema = HboSchemaVersion::V2_5) noexcept;

std::size_t handlerDefCount(HboSchemaVersion schema = HboSchemaVersion::V2_5) noexcept;

}

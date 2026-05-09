#pragma once

#include "common_types.h"

#include <optional>
#include <string>
#include <vector>

namespace whiteout::flakes::io {

class IContentProvider {
public:
    virtual ~IContentProvider() = default;

    virtual std::optional<std::vector<u8>> ReadFile(
        const std::string& path, std::string* actualExt = nullptr) const = 0;
};

}

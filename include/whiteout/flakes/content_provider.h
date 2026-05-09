#pragma once

// ============================================================================
// WhiteoutFlakes — content-provider interface.
//
// Read-side abstraction the renderer uses to load model assets, textures,
// and MDX referenced files. Hosts implement this with a file-backed,
// MPQ-backed, CASC-backed, or in-memory provider.
// ============================================================================

#include "types.h"

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

}  // namespace whiteout::flakes::io

namespace whiteout::flakes {
using ::whiteout::flakes::io::IContentProvider;
}

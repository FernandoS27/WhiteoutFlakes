#pragma once

/// @file content_provider.h
/// @brief Read-side abstraction the renderer uses to load MDX, BLP, and
///        referenced files.

#include "types.h"

#include <optional>
#include <string>
#include <vector>

namespace whiteout::flakes::io {

/// @brief Pluggable byte-stream source for asset reads.
///
/// Hosts implement this with a disk-backed, MPQ-backed (classic WC3),
/// CASC-backed (Reforged), or in-memory provider. The renderer never
/// touches the host filesystem directly — every asset lookup goes
/// through `ReadFile`.
class IContentProvider {
public:
    virtual ~IContentProvider() = default;

    /// @brief Read the bytes of an asset at the given path.
    /// @param path        Forward- or backslash-separated relative path
    ///                    (e.g. `"Units/Human/Footman/Footman.mdx"`).
    ///                    Case-insensitive matching is recommended since
    ///                    MDX files commonly mix cases.
    /// @param actualExt   Optional out-param the provider may set to the
    ///                    extension of the file it actually returned
    ///                    (e.g. `".dds"` when `.blp` was requested but
    ///                    only `.dds` exists in the archive). Pass `nullptr`
    ///                    if you don't care.
    /// @return The file bytes on success; `std::nullopt` if the path
    ///         doesn't resolve or the file is empty.
    virtual std::optional<std::vector<u8>> ReadFile(const std::string& path,
                                                    std::string* actualExt = nullptr) const = 0;
};

} // namespace whiteout::flakes::io

namespace whiteout::flakes {
using ::whiteout::flakes::io::IContentProvider;
}

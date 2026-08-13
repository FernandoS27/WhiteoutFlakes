#pragma once

/// @file content_ref.h
/// @brief How a piece of content is named. A path for WC3 and SC2; a CASC
///        fileDataID for World of Warcraft.
///
/// Chunked M2 references its `.skin` / `.skel` / `.anim` / texture siblings by
/// fileDataID **only** — there is no path to fall back on — so a path-typed
/// asset chain cannot load one. `ContentRef` is the identity that replaces
/// `std::string` from `IContentProvider` down through `AssetManager`'s slot
/// table, and the discriminant reaches the host so it can tell the two apart.

#include "types.h"

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>

namespace whiteout::flakes {

/// @brief A path- or id-addressed piece of content.
///
/// Not trivially copyable — it holds a `std::string`. An earlier design note
/// claimed it was; it is not, and callers that assumed a POD would be paying
/// for a hidden allocation on every copy. Pass it by `const&` and construct it
/// once per acquire, which is what every call site here does.
///
/// Deliberately *not* annotated `@bind`: this header is outside the bindings
/// module config, and a value struct holding a `std::string` is a shape the
/// emitters do not marshal as a parameter anyway. Every public method that
/// takes one carries `@bind skip` alongside a path-typed twin, so the bound
/// surface stays path-only. The web host reaches the discriminant through its
/// own hand-written bridge (`wf_assets.cpp`), which is the only drain that
/// exists — the generated C ABI has no needs queue to mis-fetch through.
struct ContentRef {
    enum class Kind : u8 {
        Path = 0,
        FileId = 1,
    };

    Kind kind = Kind::Path;
    u32 fileId = 0;      ///< CASC fileDataID; meaningful only when kind == FileId.
    std::string path;    ///< Empty when kind == FileId.

    ContentRef() = default;

    static ContentRef FromPath(std::string_view p) {
        ContentRef r;
        r.kind = Kind::Path;
        r.path.assign(p);
        return r;
    }
    static ContentRef FromFileId(u32 id) {
        ContentRef r;
        r.kind = Kind::FileId;
        r.fileId = id;
        return r;
    }

    bool IsPath() const noexcept {
        return kind == Kind::Path;
    }
    bool IsFileId() const noexcept {
        return kind == Kind::FileId;
    }

    /// @brief True for a default-constructed ref, or one naming nothing. An
    ///        empty path and fileDataID 0 are both "no content" — CASC treats
    ///        0 as unset, not as a valid id.
    bool Empty() const noexcept {
        return IsPath() ? path.empty() : fileId == 0;
    }

    /// @brief Human-readable form for logs and errors: the path itself, or
    ///        `#<id>` for an id-addressed ref.
    std::string Describe() const {
        return IsPath() ? path : ("#" + std::to_string(fileId));
    }

    friend bool operator==(const ContentRef& a, const ContentRef& b) noexcept {
        if (a.kind != b.kind)
            return false;
        return a.IsPath() ? a.path == b.path : a.fileId == b.fileId;
    }
    friend bool operator!=(const ContentRef& a, const ContentRef& b) noexcept {
        return !(a == b);
    }
};

} // namespace whiteout::flakes

namespace whiteout::flakes::io {
using ::whiteout::flakes::ContentRef;
}

template <>
struct std::hash<::whiteout::flakes::ContentRef> {
    std::size_t operator()(const ::whiteout::flakes::ContentRef& r) const noexcept {
        // The discriminant is folded in rather than left implicit: a path ref
        // and an id ref must never collide, because they are deliberately NOT
        // deduped against each other (see AssetManager::refToSlot_).
        const std::size_t k = static_cast<std::size_t>(r.kind) + 0x9e3779b9u;
        const std::size_t v = r.IsPath() ? std::hash<std::string>{}(r.path)
                                         : std::hash<::whiteout::flakes::u32>{}(r.fileId);
        return v ^ (k + (v << 6) + (v >> 2));
    }
};

#pragma once

// ============================================================================
// One StarCraft II mod, map or campaign used as a fallback asset source:
// a folder, or a packed MPQ archive.
//
// A folder document is indexed on first use: every file under its
// Base.SC2Assets/Assets and Assets trees, keyed by its lowercased path from
// the component root, Base.SC2Assets first, so a read is a lookup rather than
// a directory walk. A packed one is read through its MPQ, trying
// `base.sc2assets/<path>` before `<path>` since archives hold both layouts.
// Reads try the usual extension alternatives. Thread-safe.
// ============================================================================

#include "storage_source.h"

#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace whiteout::flakes::io {

class Sc2DocumentSource final : public IStorageSource {
public:
    /// Null when `doc` is not a usable document; `error` says why.
    static std::unique_ptr<Sc2DocumentSource> Open(const std::filesystem::path& doc, std::string& error);

    bool Read(const std::string& path, SourceRead& out) const override;
    void List(const std::function<void(std::string)>& emit) const override;
    const std::string& Root() const override {
        return root_;
    }

private:
    Sc2DocumentSource(std::filesystem::path doc, std::vector<std::filesystem::path> roots);
    Sc2DocumentSource(std::filesystem::path doc, std::unique_ptr<IStorageSource> packed);

    using Index = std::unordered_map<std::string, std::filesystem::path>;
    const Index& EnsureIndex() const;
    bool ReadKey(const std::string& key, SourceRead& out) const;

    std::filesystem::path doc_;
    std::string root_;
    std::vector<std::filesystem::path> roots_;
    mutable std::mutex indexMu_;
    mutable std::optional<Index> index_;
    std::unique_ptr<IStorageSource> packed_; // the MPQ, for a packed document; null for a folder
};

} // namespace whiteout::flakes::io

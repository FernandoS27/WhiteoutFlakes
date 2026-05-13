#pragma once

#include "whiteout/flakes/content_provider.h"
#include "whiteout/flakes/types.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace whiteout::flakes::io {

class FileContentProvider : public IContentProvider {
public:
    FileContentProvider();
    ~FileContentProvider();

    FileContentProvider(FileContentProvider&&) noexcept;
    FileContentProvider& operator=(FileContentProvider&&) noexcept;

    FileContentProvider(const FileContentProvider&) = delete;
    FileContentProvider& operator=(const FileContentProvider&) = delete;

    void SetBasePath(const std::filesystem::path& basePath);

    std::optional<std::vector<u8>> ReadFile(const std::string& path,
                                            std::string* actualExt = nullptr) const override;

    bool HasCasc() const;

    bool HasMpq() const;

    const std::string& Wc3Path() const;

private:
    std::optional<std::vector<u8>> ReadFromCasc(const std::string& path,
                                                std::string* actualExt) const;
    std::optional<std::vector<u8>> ReadFromMpq(const std::string& path,
                                               std::string* actualExt) const;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace whiteout::flakes::io

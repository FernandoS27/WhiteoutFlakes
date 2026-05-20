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

    // ---- IContentProvider async surface ----
    RequestId Request(const std::string& path, CompletionCallback cb) override;
    void Wait(RequestId id) override;
    void Cancel(RequestId id) override;
    void Pump() override;

    bool HasCasc() const;

    bool HasMpq() const;

    // Auto-detected Warcraft III install root from the ctor's blizzard_game_finder
    // scan. Read-only; never reflects user overrides.
    const std::string& Wc3Path() const;

    // Currently-active install root. Both CASC and the MPQ list search from
    // this directory. Defaults to Wc3Path(); pass an empty string to revert.
    // Returned by value because reconfiguration from another thread could
    // invalidate the underlying string between getter call and use.
    std::string InstallPath() const;
    void SetInstallPath(const std::string& path);

    // Per-storage enable switches. When set true, the corresponding storage
    // is closed and ReadFile() skips that fallback branch entirely.
    bool IgnoreCasc() const;
    bool IgnoreMpq() const;
    void SetIgnoreCasc(bool ignore);
    void SetIgnoreMpq(bool ignore);

    // MPQ load order. Each entry is a filename looked up under InstallPath().
    // Earlier entries are searched first when ReadFile() walks the chain.
    // Empty list = no MPQs loaded. Returned by value for the same reason as
    // InstallPath().
    std::vector<std::string> MpqList() const;
    void SetMpqList(std::vector<std::string> list);

    // Built-in MPQ load order used when no user override is set. Exposed so
    // the host UI can wire a "reset to defaults" button.
    static std::vector<std::string> DefaultMpqList();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace whiteout::flakes::io

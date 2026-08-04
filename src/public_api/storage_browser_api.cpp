// ============================================================================
// WhiteoutFlakes — public StorageBrowser implementation.
//
// A PIMPL over io::StorageBrowser. The internal class carries std::map and
// std::optional members in its header, neither of which a binding generator
// can cross, so the public face holds a pointer and forwards.
// ============================================================================

#include "whiteout/flakes/storage_browser.h"

#include "io/storage_browser.h"

#include <algorithm>
#include <cctype>

namespace whiteout::flakes {

namespace detail {

class StorageBrowserImpl {
public:
    io::StorageBrowser browser;
    std::string lastError;
    StorageFileFilter filter = StorageFileFilter::All;
};

} // namespace detail

namespace {

// Case-insensitive suffix test — archive paths keep whatever case the
// artist's tooling wrote.
bool EndsWithCi(const std::string& s, const std::string& suffix) {
    if (s.size() < suffix.size())
        return false;
    return std::equal(suffix.rbegin(), suffix.rend(), s.rbegin(), [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) ==
               std::tolower(static_cast<unsigned char>(b));
    });
}

// The one place that decides model-vs-effect, shared by the filter and by
// the public IsEffect so the two can never disagree.
bool IsEffectName(const std::string& name) {
    return EndsWithCi(name, ".pkb") || EndsWithCi(name, ".pkfx");
}

} // namespace

StorageBrowser::StorageBrowser() : impl_(std::make_unique<detail::StorageBrowserImpl>()) {}
StorageBrowser::~StorageBrowser() = default;

// Opening walks a storage the caller chose, so the failure modes are the
// host's environment rather than this code: a path this platform cannot
// render, a directory that disappears mid-walk, an archive that is not one.
// Bindings reach these through a C ABI, where a thrown exception is undefined
// behaviour rather than an error — so the boundary is here.
bool StorageBrowser::Open(const std::string& root, StorageKind kind) {
    impl_->lastError.clear();
    try {
        return impl_->browser.Open(root, static_cast<io::StorageKind>(kind), &impl_->lastError);
    } catch (const std::exception& e) {
        impl_->lastError = std::string("could not open ") + root + ": " + e.what();
        return false;
    }
}
bool StorageBrowser::OpenAuto(const std::string& path) {
    impl_->lastError.clear();
    try {
        return impl_->browser.OpenAuto(path, &impl_->lastError);
    } catch (const std::exception& e) {
        impl_->lastError = std::string("could not open ") + path + ": " + e.what();
        return false;
    }
}
StorageKind StorageBrowser::GetKind() const {
    return static_cast<StorageKind>(impl_->browser.Kind());
}
bool StorageBrowser::IsOpen() const {
    return impl_->browser.IsOpen();
}
std::string StorageBrowser::GetRoot() const {
    return impl_->browser.Root();
}
std::string StorageBrowser::GetLastError() const {
    return impl_->lastError;
}

std::string StorageBrowser::GetCurrentPath() const {
    return impl_->browser.CurrentPath();
}
std::vector<std::string> StorageBrowser::Breadcrumb() const {
    return impl_->browser.Breadcrumb();
}
std::vector<std::string> StorageBrowser::Folders() const {
    return impl_->browser.Current().folders;
}
std::vector<std::string> StorageBrowser::Files() const {
    const auto& all = impl_->browser.Current().modelFiles;
    if (impl_->filter == StorageFileFilter::All)
        return all;
    const bool wantEffects = impl_->filter == StorageFileFilter::EffectsOnly;
    std::vector<std::string> out;
    out.reserve(all.size());
    for (const auto& name : all) {
        if (IsEffectName(name) == wantEffects)
            out.push_back(name);
    }
    return out;
}

void StorageBrowser::SetFilter(StorageFileFilter filter) {
    impl_->filter = filter;
}
StorageFileFilter StorageBrowser::GetFilter() const {
    return impl_->filter;
}

i32 StorageBrowser::UnfilteredFileCount() const {
    return static_cast<i32>(impl_->browser.Current().modelFiles.size());
}

void StorageBrowser::Descend(const std::string& folderName) {
    impl_->browser.Descend(folderName);
}
void StorageBrowser::Ascend() {
    impl_->browser.Ascend();
}
void StorageBrowser::NavigateTo(const std::string& displayPath) {
    impl_->browser.NavigateTo(displayPath);
}

std::string StorageBrowser::ChildPath(const std::string& fileName) const {
    return impl_->browser.ChildPath(fileName);
}

bool StorageBrowser::IsEffect(const std::string& fileName) const {
    return IsEffectName(fileName);
}

} // namespace whiteout::flakes

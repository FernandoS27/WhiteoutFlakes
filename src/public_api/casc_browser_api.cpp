// ============================================================================
// WhiteoutFlakes — public CascBrowser implementation.
//
// A PIMPL over io::CascBrowser. The internal class carries std::map and
// std::optional members in its header, neither of which a binding generator
// can cross, so the public face holds a pointer and forwards.
// ============================================================================

#include "whiteout/flakes/casc_browser.h"

#include "io/casc_browser.h"

#include <algorithm>
#include <cctype>

namespace whiteout::flakes {

namespace detail {

class CascBrowserImpl {
public:
    io::CascBrowser browser;
    std::string lastError;
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

} // namespace

CascBrowser::CascBrowser() : impl_(std::make_unique<detail::CascBrowserImpl>()) {}
CascBrowser::~CascBrowser() = default;

bool CascBrowser::Open(const std::string& root) {
    impl_->lastError.clear();
    return impl_->browser.Open(root, &impl_->lastError);
}
bool CascBrowser::IsOpen() const {
    return impl_->browser.IsOpen();
}
std::string CascBrowser::GetRoot() const {
    return impl_->browser.Root();
}
std::string CascBrowser::GetLastError() const {
    return impl_->lastError;
}

std::string CascBrowser::GetCurrentPath() const {
    return impl_->browser.CurrentPath();
}
std::vector<std::string> CascBrowser::Breadcrumb() const {
    return impl_->browser.Breadcrumb();
}
std::vector<std::string> CascBrowser::Folders() const {
    return impl_->browser.Current().folders;
}
std::vector<std::string> CascBrowser::Files() const {
    return impl_->browser.Current().modelFiles;
}

void CascBrowser::Descend(const std::string& folderName) {
    impl_->browser.Descend(folderName);
}
void CascBrowser::Ascend() {
    impl_->browser.Ascend();
}
void CascBrowser::NavigateTo(const std::string& displayPath) {
    impl_->browser.NavigateTo(displayPath);
}

std::string CascBrowser::ChildPath(const std::string& fileName) const {
    return impl_->browser.ChildPath(fileName);
}

bool CascBrowser::IsEffect(const std::string& fileName) const {
    return EndsWithCi(fileName, ".pkb") || EndsWithCi(fileName, ".pkfx");
}

} // namespace whiteout::flakes

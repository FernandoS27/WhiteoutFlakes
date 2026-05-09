#include <cornflakes/core/determinism.hpp>
#include <cornflakes/interface/diagnostics/issue.hpp>

namespace whiteout::cornflakes {

void IssueBag::push(Issue issue) {
    if (issue.severity >= Severity::Error) {
        ++m_errorCount;
    }
    if (issue.severity == Severity::Fatal) {
        ++m_fatalCount;
    }
    m_issues.push_back(issue);
}

bool IssueBag::hasErrors() const noexcept {
    return m_errorCount > 0;
}

bool IssueBag::hasFatal() const noexcept {
    return m_fatalCount > 0;
}

std::span<const Issue> IssueBag::view() const noexcept {
    return {m_issues.data(), m_issues.size()};
}

std::size_t IssueBag::size() const noexcept {
    return m_issues.size();
}

void IssueBag::clear() noexcept {
    m_issues.clear();
    m_errorCount = 0;
    m_fatalCount = 0;
}

} // namespace whiteout::cornflakes

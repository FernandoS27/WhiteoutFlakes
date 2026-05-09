#include <cornflakes/core/determinism.hpp>
#include <cornflakes/diagnostics/issue_codes.hpp>
#include <cornflakes/validation/batch_resource_tracker.hpp>

#include <algorithm>

namespace whiteout::cornflakes {

namespace {

Issue trackerFatal(u32 code, std::string_view message) noexcept {
    Issue issue;
    issue.severity = Severity::Fatal;
    issue.category = Category::Diagnostics;
    issue.code = code;
    issue.message = message;
    return issue;
}

bool contains(const std::vector<const void*>& v, const void* p) noexcept {
    return std::find(v.begin(), v.end(), p) != v.end();
}

void addUnique(std::vector<const void*>& v, const void* p) noexcept {
    if (!contains(v, p)) {
        v.push_back(p);
    }
}

} // namespace

void BatchResourceTracker::declareRead(const void* resource) noexcept {
    addUnique(m_reads, resource);
}

void BatchResourceTracker::declareWrite(const void* resource) noexcept {
    addUnique(m_writes, resource);
}

bool BatchResourceTracker::checkRead(const void* resource, IssueBag& issues) const {

    if (contains(m_writes, resource) || contains(m_reads, resource)) {
        return true;
    }
    issues.push(
        trackerFatal(issues::diagnostics::kTrackerUndeclaredRead,
                     "BatchResourceTracker: read access on a resource not declared by this batch"));
    return false;
}

bool BatchResourceTracker::checkWrite(const void* resource, IssueBag& issues) const {
    if (contains(m_writes, resource)) {
        return true;
    }
    issues.push(trackerFatal(
        issues::diagnostics::kTrackerUndeclaredWrite,
        "BatchResourceTracker: write access on a resource not declared by this batch"));
    return false;
}

void BatchResourceTracker::reset() noexcept {
    m_reads.clear();
    m_writes.clear();
}

} // namespace whiteout::cornflakes

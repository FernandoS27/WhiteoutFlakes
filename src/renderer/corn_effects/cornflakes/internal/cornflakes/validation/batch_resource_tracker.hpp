#pragma once

#include <cornflakes/interface/core/types.hpp>
#include <cornflakes/interface/diagnostics/issue.hpp>

#include <cstddef>
#include <string_view>
#include <vector>

namespace whiteout::cornflakes {

class BatchResourceTracker {
public:
    BatchResourceTracker() = default;
    explicit BatchResourceTracker(std::string_view batchName) noexcept : m_name(batchName) {}

    void setBatchName(std::string_view name) noexcept {
        m_name = name;
    }
    std::string_view batchName() const noexcept {
        return m_name;
    }

    void declareRead(const void* resource) noexcept;
    void declareWrite(const void* resource) noexcept;

    bool checkRead(const void* resource, IssueBag& issues) const;
    bool checkWrite(const void* resource, IssueBag& issues) const;

    void reset() noexcept;

    std::size_t declaredReadCount() const noexcept {
        return m_reads.size();
    }
    std::size_t declaredWriteCount() const noexcept {
        return m_writes.size();
    }

private:
    std::string_view m_name;
    std::vector<const void*> m_reads;
    std::vector<const void*> m_writes;
};

}

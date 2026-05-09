#pragma once

/// @file
/// @brief Per-frame keyed cache mapping `PayloadKey` to its in-arena byte blob.

#include <cornflakes/interface/core/types.hpp>
#include <cornflakes/events/payload_key.hpp>

#include <cstddef>
#include <span>
#include <vector>

namespace whiteout::cornflakes {

/// @brief In-frame map from `PayloadKey` → blob span. Spans alias caller-owned arena memory.
class PayloadCacheStore {
public:
    PayloadCacheStore() = default;

    void add(const PayloadKey& key, std::span<std::byte> blob);

    std::span<std::byte> get(const PayloadKey& key) const noexcept;
    std::span<const std::byte> getConst(const PayloadKey& key) const noexcept;

    bool contains(const PayloadKey& key) const noexcept;

    std::size_t size() const noexcept;
    void clear() noexcept;

private:
    struct Entry {
        PayloadKey key;
        std::span<std::byte> blob;
    };
    std::vector<Entry> m_entries;
};

} // namespace whiteout::cornflakes

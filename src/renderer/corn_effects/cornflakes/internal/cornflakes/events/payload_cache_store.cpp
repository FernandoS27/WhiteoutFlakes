#include <cornflakes/core/determinism.hpp>
#include <cornflakes/events/payload_cache_store.hpp>

namespace whiteout::cornflakes {

void PayloadCacheStore::add(const PayloadKey& key, std::span<std::byte> blob) {
    for (auto& e : m_entries) {
        if (e.key == key) {
            e.blob = blob;
            return;
        }
    }
    m_entries.push_back({key, blob});
}

std::span<std::byte> PayloadCacheStore::get(const PayloadKey& key) const noexcept {
    for (const auto& e : m_entries) {
        if (e.key == key) {
            return e.blob;
        }
    }
    return {};
}

std::span<const std::byte> PayloadCacheStore::getConst(const PayloadKey& key) const noexcept {
    const auto blob = get(key);
    return std::span<const std::byte>{blob.data(), blob.size()};
}

bool PayloadCacheStore::contains(const PayloadKey& key) const noexcept {
    for (const auto& e : m_entries) {
        if (e.key == key) {
            return true;
        }
    }
    return false;
}

std::size_t PayloadCacheStore::size() const noexcept {
    return m_entries.size();
}

void PayloadCacheStore::clear() noexcept {
    m_entries.clear();
}

} // namespace whiteout::cornflakes

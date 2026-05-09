#pragma once

/// @file
/// @brief Bump-allocator arena interface and helpers used for all per-frame / per-bind storage.

#include <cornflakes/interface/core/types.hpp>

#include <cstddef>
#include <memory>
#include <new>
#include <span>
#include <utility>
#include <vector>

namespace whiteout::cornflakes {

/// @brief Bump-allocator interface. Allocations are valid until the next `reset()`.
class IArena {
public:
    IArena() = default;
    virtual ~IArena() = default;

    IArena(const IArena&) = delete;
    IArena& operator=(const IArena&) = delete;
    IArena(IArena&&) = delete;
    IArena& operator=(IArena&&) = delete;

    virtual void* allocate(std::size_t size, std::size_t align) = 0;
    virtual void reset() = 0;
    virtual std::size_t bytesInUse() const noexcept = 0;
    virtual std::size_t capacity() const noexcept = 0;
};

/// @brief Default `IArena` impl backed by a chain of geometrically-growing chunks.
class ExpandingArena final : public IArena {
public:
    static constexpr std::size_t kDefaultFirstChunkBytes = 64 * 1024;

    explicit ExpandingArena(std::size_t firstChunkBytes = kDefaultFirstChunkBytes);
    ~ExpandingArena() override = default;

    ExpandingArena(const ExpandingArena&) = delete;
    ExpandingArena& operator=(const ExpandingArena&) = delete;
    ExpandingArena(ExpandingArena&&) = delete;
    ExpandingArena& operator=(ExpandingArena&&) = delete;

    void* allocate(std::size_t size, std::size_t align) override;
    void reset() override;
    std::size_t bytesInUse() const noexcept override;
    std::size_t capacity() const noexcept override;

private:
    struct Chunk {
        std::unique_ptr<std::byte[]> buffer;
        std::size_t capacity = 0;
        std::size_t cursor = 0;
    };

    void addChunkForRequest(std::size_t size, std::size_t align);

    std::vector<Chunk> m_chunks;
    std::size_t m_firstChunkBytes;
};

/// @brief Construct a `T` in arena storage; lifetime ends at the next `reset()`.
template <typename T, typename... Args>
T* arenaNew(IArena& arena, Args&&... args) {
    void* mem = arena.allocate(sizeof(T), alignof(T));
    return new (mem) T(std::forward<Args>(args)...);
}

/// @brief Default-construct an array of `count` `T`s in arena storage.
template <typename T>
std::span<T> arenaArray(IArena& arena, std::size_t count) {
    if (count == 0) {
        return {};
    }
    void* mem = arena.allocate(sizeof(T) * count, alignof(T));
    T* typed = new (mem) T[count]();
    return {typed, count};
}

} // namespace whiteout::cornflakes

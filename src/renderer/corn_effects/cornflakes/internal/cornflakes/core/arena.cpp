#include <cornflakes/core/determinism.hpp>
#include <cornflakes/interface/core/arena.hpp>

#include <cstdint>

namespace whiteout::cornflakes {

namespace {

std::size_t alignUp(std::size_t value, std::size_t align) noexcept {
    return (value + (align - 1)) & ~(align - 1);
}

std::size_t pickChunkCapacity(std::size_t requested, std::size_t previousCapacity,
                              std::size_t firstChunkBytes) noexcept {
    const std::size_t grown = previousCapacity == 0 ? firstChunkBytes : previousCapacity * 2;
    return requested > grown ? requested : grown;
}

} // namespace

ExpandingArena::ExpandingArena(std::size_t firstChunkBytes) : m_firstChunkBytes(firstChunkBytes) {}

void ExpandingArena::addChunkForRequest(std::size_t size, std::size_t align) {
    const std::size_t worstCase = size + align;
    const std::size_t previous = m_chunks.empty() ? 0 : m_chunks.back().capacity;
    const std::size_t cap = pickChunkCapacity(worstCase, previous, m_firstChunkBytes);

    Chunk chunk;
    chunk.buffer = std::make_unique<std::byte[]>(cap);
    chunk.capacity = cap;
    chunk.cursor = 0;
    m_chunks.push_back(std::move(chunk));
}

void* ExpandingArena::allocate(std::size_t size, std::size_t align) {
    if (m_chunks.empty()) {
        addChunkForRequest(size, align);
    }

    for (;;) {
        Chunk& last = m_chunks.back();
        const auto base = reinterpret_cast<std::uintptr_t>(last.buffer.get()) + last.cursor;
        const auto alignedBase = alignUp(base, align);
        const std::size_t padding = alignedBase - base;
        if (last.cursor + padding + size <= last.capacity) {
            last.cursor += padding + size;
            return reinterpret_cast<void*>(alignedBase);
        }
        addChunkForRequest(size, align);
    }
}

void ExpandingArena::reset() {
    if (m_chunks.empty()) {
        return;
    }

    m_chunks[0].cursor = 0;
    m_chunks.resize(1);
}

std::size_t ExpandingArena::bytesInUse() const noexcept {
    std::size_t total = 0;
    for (const auto& chunk : m_chunks) {
        total += chunk.cursor;
    }
    return total;
}

std::size_t ExpandingArena::capacity() const noexcept {
    std::size_t total = 0;
    for (const auto& chunk : m_chunks) {
        total += chunk.capacity;
    }
    return total;
}

} // namespace whiteout::cornflakes

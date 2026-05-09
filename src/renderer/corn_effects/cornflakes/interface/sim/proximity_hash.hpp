#pragma once

/// @file
/// @brief Sparse cell-based spatial hash for closest-neighbour and neighbour-count queries.

#include <cornflakes/interface/core/types.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace whiteout::cornflakes {

/// @brief One inserted point — position plus the owning particle's selfId.
struct ProximityEntry {
    std::array<f32, 3> position{0.0F, 0.0F, 0.0F};

    u64 sourceSelfId = 0U;
};

/// @brief Integer 3D cell coordinate (cell-size-quantised position).
struct CellCoord {
    i32 x = 0;
    i32 y = 0;
    i32 z = 0;
    bool operator==(const CellCoord& o) const noexcept {
        return x == o.x && y == o.y && z == o.z;
    }
};

/// @brief FNV-1a hasher for `CellCoord`.
struct CellCoordHash {
    std::size_t operator()(const CellCoord& c) const noexcept {

        std::uint64_t h = 14695981039346656037ULL;
        const auto mix = [&](i32 v) {
            h ^= static_cast<std::uint64_t>(static_cast<u32>(v));
            h *= 1099511628211ULL;
        };
        mix(c.x);
        mix(c.y);
        mix(c.z);
        return static_cast<std::size_t>(h);
    }
};

/// @brief Cell-bucketed spatial hash for proximity queries (closestN + neighbourCount).
class ProximityHash {
public:
    static constexpr u32 kMaxCellsPerQuery = 64U * 64U * 64U; ///< Above this, fall back to full scan.

    explicit ProximityHash(f32 cellSize = 0.75F) noexcept : cellSize_(cellSize) {}

    void setCellSize(f32 size) noexcept {
        cellSize_ = (size > 0.0F) ? size : 0.75F;
    }
    f32 cellSize() const noexcept {
        return cellSize_;
    }
    std::size_t entryCount() const noexcept {
        return entryCount_;
    }

    /// @brief Insert `position`. Non-finite components are rejected.
    bool insert(const std::array<f32, 3>& position, u64 sourceSelfId) {
        const f32 x = position[0];
        const f32 y = position[1];
        const f32 z = position[2];
        if (!isFiniteF32(x) || !isFiniteF32(y) || !isFiniteF32(z)) {
            return false;
        }
        const CellCoord c = cellOf(position);
        cells_[c].push_back(ProximityEntry{position, sourceSelfId});
        ++entryCount_;
        return true;
    }

    void clear() noexcept {
        cells_.clear();
        entryCount_ = 0;
    }

    /// @brief Find the `n`-th closest entry within `radius` of `target`. Null if fewer than n+1 hits.
    const ProximityEntry* closestN(const std::array<f32, 3>& target, f32 radius,
                                   u32 n) const noexcept {
        if (radius <= 0.0F || cells_.empty()) {
            return nullptr;
        }
        const f32 rSq = radius * radius;

        struct Candidate {
            f32 distSq;
            const ProximityEntry* entry;
        };
        std::vector<Candidate> hits;
        hits.reserve(64);
        const auto consider = [&](const std::vector<ProximityEntry>& cell) {
            for (const auto& e : cell) {
                const f32 dx = e.position[0] - target[0];
                const f32 dy = e.position[1] - target[1];
                const f32 dz = e.position[2] - target[2];
                const f32 dSq = dx * dx + dy * dy + dz * dz;
                if (dSq <= rSq) {
                    hits.push_back(Candidate{dSq, &e});
                }
            }
        };

        const f32 inv = 1.0F / cellSize_;
        const i32 cxMin = floorI32((target[0] - radius) * inv);
        const i32 cxMax = floorI32((target[0] + radius) * inv);
        const i32 cyMin = floorI32((target[1] - radius) * inv);
        const i32 cyMax = floorI32((target[1] + radius) * inv);
        const i32 czMin = floorI32((target[2] - radius) * inv);
        const i32 czMax = floorI32((target[2] + radius) * inv);
        const u64 cellsX = static_cast<u64>(cxMax - cxMin + 1);
        const u64 cellsY = static_cast<u64>(cyMax - cyMin + 1);
        const u64 cellsZ = static_cast<u64>(czMax - czMin + 1);
        const u64 totalCells = cellsX * cellsY * cellsZ;
        if (totalCells > static_cast<u64>(kMaxCellsPerQuery) ||
            totalCells > static_cast<u64>(cells_.size())) {

            for (const auto& kv : cells_) {
                consider(kv.second);
            }
        } else {
            for (i32 cx = cxMin; cx <= cxMax; ++cx) {
                for (i32 cy = cyMin; cy <= cyMax; ++cy) {
                    for (i32 cz = czMin; cz <= czMax; ++cz) {
                        auto it = cells_.find(CellCoord{cx, cy, cz});
                        if (it == cells_.end()) {
                            continue;
                        }
                        consider(it->second);
                    }
                }
            }
        }
        if (hits.empty() || n >= hits.size()) {
            return nullptr;
        }
        std::nth_element(
            hits.begin(), hits.begin() + n, hits.end(),
            [](const Candidate& a, const Candidate& b) { return a.distSq < b.distSq; });
        return hits[n].entry;
    }

    /// @brief Count entries within `radius` of `target`.
    u32 neighborCount(const std::array<f32, 3>& target, f32 radius) const noexcept {
        if (radius <= 0.0F || cells_.empty()) {
            return 0U;
        }
        const f32 rSq = radius * radius;
        const f32 inv = 1.0F / cellSize_;
        const i32 cxMin = floorI32((target[0] - radius) * inv);
        const i32 cxMax = floorI32((target[0] + radius) * inv);
        const i32 cyMin = floorI32((target[1] - radius) * inv);
        const i32 cyMax = floorI32((target[1] + radius) * inv);
        const i32 czMin = floorI32((target[2] - radius) * inv);
        const i32 czMax = floorI32((target[2] + radius) * inv);
        const u64 cellsX = static_cast<u64>(cxMax - cxMin + 1);
        const u64 cellsY = static_cast<u64>(cyMax - cyMin + 1);
        const u64 cellsZ = static_cast<u64>(czMax - czMin + 1);
        const u64 totalCells = cellsX * cellsY * cellsZ;
        u32 count = 0U;
        const auto consider = [&](const std::vector<ProximityEntry>& cell) {
            for (const auto& e : cell) {
                const f32 dx = e.position[0] - target[0];
                const f32 dy = e.position[1] - target[1];
                const f32 dz = e.position[2] - target[2];
                if (dx * dx + dy * dy + dz * dz <= rSq) {
                    ++count;
                }
            }
        };
        if (totalCells > static_cast<u64>(kMaxCellsPerQuery) ||
            totalCells > static_cast<u64>(cells_.size())) {
            for (const auto& kv : cells_) {
                consider(kv.second);
            }
        } else {
            for (i32 cx = cxMin; cx <= cxMax; ++cx) {
                for (i32 cy = cyMin; cy <= cyMax; ++cy) {
                    for (i32 cz = czMin; cz <= czMax; ++cz) {
                        auto it = cells_.find(CellCoord{cx, cy, cz});
                        if (it == cells_.end()) {
                            continue;
                        }
                        consider(it->second);
                    }
                }
            }
        }
        return count;
    }

private:
    static bool isFiniteF32(f32 v) noexcept {
        u32 bits = 0U;
        std::memcpy(&bits, &v, sizeof(bits));
        const u32 exp = (bits >> 23) & 0xFFU;
        return exp != 0xFFU;
    }
    static i32 floorI32(f32 v) noexcept {
        const i32 i = static_cast<i32>(v);
        return (v < 0.0F && static_cast<f32>(i) != v) ? (i - 1) : i;
    }
    CellCoord cellOf(const std::array<f32, 3>& pos) const noexcept {
        const f32 inv = 1.0F / cellSize_;
        return CellCoord{
            floorI32(pos[0] * inv),
            floorI32(pos[1] * inv),
            floorI32(pos[2] * inv),
        };
    }

    std::unordered_map<CellCoord, std::vector<ProximityEntry>, CellCoordHash> cells_;
    f32 cellSize_ = 0.75F;
    std::size_t entryCount_ = 0U;
};

} // namespace whiteout::cornflakes

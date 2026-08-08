#pragma once

#include <cornflakes/interface/core/types.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace whiteout::cornflakes {

inline constexpr u32 spatialPayloadNameHash(std::string_view name) noexcept {
    u32 h = 2166136261U;
    for (const char c : name) {
        h ^= static_cast<u32>(static_cast<unsigned char>(c));
        h *= 16777619U;
    }
    return h;
}

inline constexpr std::size_t kMaxProximityPayloads = 8;

struct ProximityPayload {
    u32 nameHash = 0;
    u8 components = 0;
    std::array<f32, 4> value{0.0F, 0.0F, 0.0F, 0.0F};
};

struct ProximityEntry {
    std::array<f32, 3> position{0.0F, 0.0F, 0.0F};
    std::array<f32, 3> payload{0.0F, 0.0F, 0.0F};

    u64 sourceSelfId = 0U;

    u32 insertSeq = 0U;

    u8 payloadCount = 0;
    std::array<ProximityPayload, kMaxProximityPayloads> payloads{};

    const ProximityPayload* findPayload(u32 nameHash) const noexcept {
        for (u8 i = 0; i < payloadCount && i < kMaxProximityPayloads; ++i) {
            if (payloads[i].nameHash == nameHash) {
                return &payloads[i];
            }
        }
        return nullptr;
    }
};

struct CellCoord {
    i32 x = 0;
    i32 y = 0;
    i32 z = 0;
    bool operator==(const CellCoord& o) const noexcept {
        return x == o.x && y == o.y && z == o.z;
    }
};

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

class ProximityHash {
public:
    static constexpr u32 kMaxCellsPerQuery = 64U * 64U * 64U;

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

    bool insert(const std::array<f32, 3>& position, u64 sourceSelfId) {
        return insert(position, position, sourceSelfId);
    }

    bool insert(const std::array<f32, 3>& position, const std::array<f32, 3>& payload,
                u64 sourceSelfId) {
        return insert(position, payload, sourceSelfId, {});
    }

    bool insert(const std::array<f32, 3>& position, const std::array<f32, 3>& payload,
                u64 sourceSelfId, std::span<const ProximityPayload> named) {
        if (!isFiniteF32(position[0]) || !isFiniteF32(position[1]) || !isFiniteF32(position[2])) {
            return false;
        }
        const CellCoord c = cellOf(position);
        ProximityEntry e{};
        e.position = position;
        e.payload = payload;
        e.sourceSelfId = sourceSelfId;
        const std::size_t n = std::min(named.size(), kMaxProximityPayloads);
        for (std::size_t i = 0; i < n; ++i) {
            e.payloads[i] = named[i];
        }
        e.payloadCount = static_cast<u8>(n);
        e.insertSeq = static_cast<u32>(entryCount_);
        cells_[c].push_back(e);
        ++entryCount_;
        return true;
    }

    void clear() noexcept {
        cells_.clear();
        entryCount_ = 0;
    }

    std::vector<ProximityEntry> entriesInInsertionOrder() const {
        std::vector<ProximityEntry> out;
        out.reserve(entryCount_);
        for (const auto& kv : cells_) {
            out.insert(out.end(), kv.second.begin(), kv.second.end());
        }
        std::sort(out.begin(), out.end(), [](const ProximityEntry& a, const ProximityEntry& b) {
            return a.insertSeq < b.insertSeq;
        });
        return out;
    }

    template <typename Fn>
    void forEachInRadius(const std::array<f32, 3>& target, f32 radius, Fn&& fn) const {
        if (radius <= 0.0F || cells_.empty()) {
            return;
        }
        const f32 rSq = radius * radius;
        const auto consider = [&](const std::vector<ProximityEntry>& cell) {
            for (const auto& e : cell) {
                const f32 dx = e.position[0] - target[0];
                const f32 dy = e.position[1] - target[1];
                const f32 dz = e.position[2] - target[2];
                const f32 dSq = dx * dx + dy * dy + dz * dz;
                if (dSq <= rSq) {
                    fn(e, dSq);
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
            return;
        }
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

    const ProximityEntry* closestN(const std::array<f32, 3>& target, f32 radius,
                                   u32 n) const noexcept {
        struct Candidate {
            f32 distSq;
            const ProximityEntry* entry;
        };
        std::vector<Candidate> hits;
        hits.reserve(64);
        forEachInRadius(target, radius,
                        [&](const ProximityEntry& e, f32 dSq) { hits.push_back({dSq, &e}); });
        if (hits.empty() || n >= hits.size()) {
            return nullptr;
        }
        std::nth_element(
            hits.begin(), hits.begin() + n, hits.end(),
            [](const Candidate& a, const Candidate& b) { return a.distSq < b.distSq; });
        return hits[n].entry;
    }

    u32 neighborCount(const std::array<f32, 3>& target, f32 radius) const noexcept {
        u32 count = 0U;
        forEachInRadius(target, radius, [&](const ProximityEntry&, f32) { ++count; });
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

}

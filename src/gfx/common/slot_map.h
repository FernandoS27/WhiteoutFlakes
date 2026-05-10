#pragma once

// ============================================================================
// SlotMap<Payload> + opaque-handle encoding shared by every gfx backend.
//
// Each backend's resource entry types (BufferEntry, TextureEntry, ...) live
// in a SlotMap<T>. The public-facing handle types (BufferHandle, etc.) are
// `u64` enums whose value packs (slot index + generation), so a stale
// handle whose slot was freed and reused fails validation via the
// generation mismatch.
//
// Lives in `whiteout::flakes::gfx` (the parent namespace) so per-backend
// child namespaces (gfx::d3d11, gfx::d3d12, gfx::vulkan) all see the same
// SlotMap / Make-/Index-/Gen helpers via ordinary name lookup.
// ============================================================================

#include "whiteout/flakes/types.h"

#include <utility>
#include <vector>

namespace whiteout::flakes::gfx {

inline constexpr u64 kIndexBits = 48;
inline constexpr u64 kIndexMask = (u64{1} << kIndexBits) - 1;
inline constexpr u64 kGenShift  = kIndexBits;

inline u64 MakeHandle(u32 index, u16 gen) {
    return (static_cast<u64>(gen) << kGenShift) | static_cast<u64>(index + 1);
}

inline u32 HandleIndex(u64 h) {
    return static_cast<u32>((h & kIndexMask) - 1);
}

inline u16 HandleGen(u64 h) {
    return static_cast<u16>(h >> kGenShift);
}

template<typename Payload>
class SlotMap {
public:
    struct Slot {
        Payload  data{};
        u16      generation = 1;
        bool     alive      = false;
    };

    u64 Insert(Payload&& p) {
        u32 idx;
        if (!freeList_.empty()) {
            idx = freeList_.back();
            freeList_.pop_back();
        } else {
            idx = static_cast<u32>(slots_.size());
            slots_.push_back({});
        }
        auto& s = slots_[idx];
        s.data  = std::move(p);
        s.alive = true;
        return MakeHandle(idx, s.generation);
    }

    Payload* Get(u64 h) {
        if (h == 0) return nullptr;
        u32 idx = HandleIndex(h);
        if (idx >= slots_.size()) return nullptr;
        auto& s = slots_[idx];
        if (!s.alive || s.generation != HandleGen(h)) return nullptr;
        return &s.data;
    }

    const Payload* Get(u64 h) const {
        if (h == 0) return nullptr;
        u32 idx = HandleIndex(h);
        if (idx >= slots_.size()) return nullptr;
        auto& s = slots_[idx];
        if (!s.alive || s.generation != HandleGen(h)) return nullptr;
        return &s.data;
    }

    void Remove(u64 h) {
        if (h == 0) return;
        u32 idx = HandleIndex(h);
        if (idx >= slots_.size()) return;
        auto& s = slots_[idx];
        if (!s.alive || s.generation != HandleGen(h)) return;
        s.data  = Payload{};
        s.alive = false;
        s.generation++;
        freeList_.push_back(idx);
    }

    template<typename Fn>
    void ForEach(Fn&& fn) {
        for (auto& s : slots_) {
            if (s.alive) fn(s.data);
        }
    }

    void Clear() {
        slots_.clear();
        freeList_.clear();
    }

private:
    std::vector<Slot> slots_;
    std::vector<u32>  freeList_;
};

}  // namespace whiteout::flakes::gfx

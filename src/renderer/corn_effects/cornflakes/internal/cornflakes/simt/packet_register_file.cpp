#include <cornflakes/interface/simt/packet_register_file.hpp>

#include <cornflakes/interface/simt/scratch_validator.hpp>

#include <algorithm>
#include <array>
#include <unordered_set>

namespace whiteout::cornflakes::simt {

void PacketRegisterLayout::build(std::span<const CBEMInstruction> program,
                                 std::size_t constantsPoolBytes) {
    banks_ = {};
    slotSources_.clear();
    liveInSlots_.clear();
    total_ = 0U;

    std::array<u32, kScopeRegisterBuckets> lo{};
    std::array<u32, kScopeRegisterBuckets> hi{};
    std::array<bool, kScopeRegisterBuckets> seen{};
    lo.fill(0xFFFFFFFFU);

    const auto visit = [&](u32 regId) {
        if (regId == kRegVoid) {
            return;
        }
        const auto d = decodeRegId(regId);
        if (constPoolHit(d, constantsPoolBytes) || d.scope >= kScopeRegisterBuckets) {
            return;
        }
        const u32 idx = d.localIdx;
        seen[d.scope] = true;
        lo[d.scope] = std::min(lo[d.scope], idx);
        hi[d.scope] = std::max(hi[d.scope], idx);
    };
    for (const auto& ins : program) {
        forEachRegisterOperand(ins, visit, visit);
    }

    for (std::size_t b = 0; b < kScopeRegisterBuckets; ++b) {
        if (!seen[b]) {
            continue;
        }
        Bank& bank = banks_[b];
        bank.firstIndex = lo[b];
        bank.dense.assign(static_cast<std::size_t>(hi[b] - lo[b]) + 1U, kUnused);
    }

    const auto mark = [&](u32 regId) {
        if (regId == kRegVoid) {
            return;
        }
        const auto d = decodeRegId(regId);
        if (constPoolHit(d, constantsPoolBytes) || d.scope >= kScopeRegisterBuckets) {
            return;
        }
        Bank& bank = banks_[d.scope];
        bank.dense[static_cast<std::size_t>(d.localIdx) - bank.firstIndex] = 0U;
    };
    for (const auto& ins : program) {
        forEachRegisterOperand(ins, mark, mark);
    }

    u32 next = 0U;
    for (std::size_t b = 0; b < kScopeRegisterBuckets; ++b) {
        Bank& bank = banks_[b];
        bank.base = next;
        u32 within = 0U;
        for (std::size_t cellIx = 0; cellIx < bank.dense.size(); ++cellIx) {
            if (bank.dense[cellIx] != kUnused) {
                bank.dense[cellIx] = within++;
                slotSources_.emplace_back(static_cast<u8>(b),
                                          bank.firstIndex + static_cast<u32>(cellIx));
            }
        }
        bank.count = within;
        next += within;
    }
    total_ = next;

    std::array<std::unordered_set<u32>, kScopeRegisterBuckets> written;
    std::unordered_set<u32> liveIn;
    for (const auto& ins : program) {
        forEachRegisterOperand(
            ins,
            [&](u32 regId) {
                if (regId == kRegVoid) {
                    return;
                }
                const auto d = decodeRegId(regId);
                if (constPoolHit(d, constantsPoolBytes) || d.scope >= kScopeRegisterBuckets) {
                    return;
                }
                if (written[d.scope].count(d.localIdx) != 0U) {
                    return;
                }
                const u32 slot = slotFor(d.scope, d.localIdx);
                if (slot != kInvalidSlot) {
                    liveIn.insert(slot);
                }
            },
            [](u32) {});
        forEachRegisterOperand(
            ins, [](u32) {},
            [&](u32 regId) {
                if (regId == kRegVoid) {
                    return;
                }
                const auto d = decodeRegId(regId);
                if (constPoolHit(d, constantsPoolBytes) || d.scope >= kScopeRegisterBuckets) {
                    return;
                }
                written[d.scope].insert(d.localIdx);
            });
    }
    liveInSlots_.assign(liveIn.begin(), liveIn.end());
    std::sort(liveInSlots_.begin(), liveInSlots_.end());
}

bool programReadsBeforeWrite(std::span<const CBEMInstruction> program,
                             std::size_t constantsPoolBytes) noexcept {
    std::array<std::unordered_set<u32>, kScopeRegisterBuckets> written;
    bool found = false;

    for (const auto& ins : program) {
        forEachRegisterOperand(
            ins,
            [&](u32 regId) {
                if (found || regId == kRegVoid) {
                    return;
                }
                const auto d = decodeRegId(regId);
                if (constPoolHit(d, constantsPoolBytes) || d.scope >= kScopeRegisterBuckets) {
                    return;
                }
                if (written[d.scope].count(d.localIdx) == 0U) {
                    found = true;
                }
            },
            [](u32) {});
        if (found) {
            return true;
        }
        forEachRegisterOperand(
            ins, [](u32) {},
            [&](u32 regId) {
                if (regId == kRegVoid) {
                    return;
                }
                const auto d = decodeRegId(regId);
                if (constPoolHit(d, constantsPoolBytes) || d.scope >= kScopeRegisterBuckets) {
                    return;
                }
                written[d.scope].insert(d.localIdx);
            });
    }
    return false;
}

}

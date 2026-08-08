#pragma once

#include <cornflakes/interface/binding/layer_program.hpp>
#include <cornflakes/interface/core/types.hpp>
#include <cornflakes/interface/vm/bytecode_exec_context.hpp>
#include <cornflakes/interface/vm/bytecode_trace.hpp>
#include <cornflakes/interface/vm/register_value.hpp>

#include <array>
#include <cstddef>
#include <map>
#include <string>
#include <tuple>
#include <vector>

namespace whiteout::cornflakes::simt {

struct BankSizing {
    u32 declaredCount = 0U;

    u32 highestIndexUsed = 0U;

    u32 distinctIndices = 0U;

    u32 maxLive = 0U;

    u32 reads = 0U;
    u32 writes = 0U;

    u32 readBeforeWrite = 0U;
};

struct ScopeSizing {
    std::size_t instructions = 0U;

    bool undecoded = false;

    std::array<BankSizing, kScopeRegisterBuckets> banks{};

    u32 highestExternalSlot = 0U;
    u32 distinctExternalSlots = 0U;

    u32 totalRegisters() const noexcept {
        u32 sum = 0U;
        for (const auto& b : banks) {
            sum += b.highestIndexUsed;
        }
        return sum;
    }
};

struct LayerSizing {
    LayerId id{};
    std::array<ScopeSizing, 4> scopes{};

    std::size_t externalStorageSize = 0U;
};

ScopeSizing analyseProgramSizing(std::span<const CBEMInstruction> program,
                                 std::size_t constantsPoolBytes,
                                 const std::array<u32, 5>& registerCounts);

LayerSizing analyseLayerSizing(const LayerProgram& layer);

class IScopeRunObserver {
public:
    IScopeRunObserver() = default;
    IScopeRunObserver(const IScopeRunObserver&) = delete;
    IScopeRunObserver& operator=(const IScopeRunObserver&) = delete;
    virtual ~IScopeRunObserver() = default;

    virtual void onScopeBegin(const LayerProgram& layer, const VMProgramDescriptor& scope,
                              BytecodeExecContext& ctx) = 0;

    virtual void onScopeEnd(const LayerProgram& layer, const VMProgramDescriptor& scope,
                            const BytecodeExecContext& ctx) = 0;
};

IScopeRunObserver* scopeRunObserver() noexcept;

void setScopeRunObserver(IScopeRunObserver* observer) noexcept;

struct TagCensusTotals {
    std::size_t scopeRuns = 0U;

    std::size_t registerCellsObserved = 0U;
    std::size_t externalCellsObserved = 0U;
    std::size_t writesObserved = 0U;

    std::size_t registerIntraTickVariances = 0U;
    std::size_t externalIntraTickVariances = 0U;
    std::size_t writeSiteIntraTickVariances = 0U;

    std::size_t registerCrossTickChanges = 0U;
    std::size_t externalCrossTickChanges = 0U;

    std::size_t traceUnavailableScopeRuns = 0U;

    std::size_t variances() const noexcept {
        return registerIntraTickVariances + externalIntraTickVariances +
               writeSiteIntraTickVariances;
    }
};

class RegisterTagCensus final : public IScopeRunObserver {
public:
    void onScopeBegin(const LayerProgram& layer, const VMProgramDescriptor& scope,
                      BytecodeExecContext& ctx) override;
    void onScopeEnd(const LayerProgram& layer, const VMProgramDescriptor& scope,
                    const BytecodeExecContext& ctx) override;

    void beginTick();

    void finish();

    const TagCensusTotals& totals() const noexcept {
        return totals_;
    }
    const std::vector<std::string>& detail() const noexcept {
        return detail_;
    }

private:
    struct Tag {
        u8 componentCount = 0U;
        u8 typeBank = 0U;
        bool operator==(const Tag& o) const noexcept {
            return componentCount == o.componentCount && typeBank == o.typeBank;
        }
    };

    using RegKey = std::tuple<u32, u8, u8, u32>;
    using ExtKey = std::tuple<u32, u32>;
    using SiteKey = std::tuple<u32, u8, u32, u8, u32>;

    struct TouchedRegisters {
        std::array<std::vector<u32>, kScopeRegisterBuckets> banks;
        bool unscannable = false;
    };

    const TouchedRegisters& touchedFor(const VMProgramDescriptor& scope);

    void note(std::string line);

    BytecodeTrace trace_{};
    bool traceInstalled_ = false;

    std::map<const VMProgramDescriptor*, TouchedRegisters> touched_;
    std::map<RegKey, Tag> regThisTick_;
    std::map<ExtKey, Tag> extThisTick_;
    std::map<SiteKey, Tag> siteThisTick_;
    std::map<RegKey, Tag> regAcrossTicks_;
    std::map<ExtKey, Tag> extAcrossTicks_;

    TagCensusTotals totals_{};
    std::vector<std::string> detail_;
    int tickIndex_ = 0;
};

}

#pragma once

#include <cornflakes/interface/binding/external_binding.hpp>
#include <cornflakes/interface/binding/ir_to_cbem_lowerer.hpp>
#include <cornflakes/interface/diagnostics/issue.hpp>
#include <cornflakes/interface/simt/simt_packet.hpp>

#include <array>
#include <cstddef>
#include <span>

namespace whiteout::cornflakes::simt {

class PacketRegisterLayout;
class PoolRegisterFile;

struct PoolPublishTargets {
    std::array<PoolRegisterFile*, kScopeRegisterBuckets> banks{};

    bool any() const noexcept {
        for (auto* f : banks) {
            if (f != nullptr) {
                return true;
            }
        }
        return false;
    }
};

struct SimtRunResult {
    std::size_t instructionsExecuted = 0U;
    std::size_t laneInstructionsExecuted = 0U;
    LaneMask finalMask = 0U;

    LaneMask killedMask = 0U;
    bool ok = true;
};

bool instructionHasSideEffects(const CBEMInstruction& ins,
                               std::span<const FunctionBinding> functions) noexcept;

bool containsSideEffects(std::span<const CBEMInstruction> program,
                         std::span<const FunctionBinding> functions) noexcept;

bool containsUnorderableSideEffects(std::span<const CBEMInstruction> program,
                                    std::span<const FunctionBinding> functions) noexcept;

class SimtInterpreter {
public:
    SimtRunResult run(std::span<const CBEMInstruction> program, SimtPacket& packet,
                      IssueBag& issues, const PacketRegisterLayout* preBuiltLayout = nullptr,
                      const PoolPublishTargets* poolTargets = nullptr) const;
};

}

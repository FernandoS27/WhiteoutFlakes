#pragma once

#include <cornflakes/interface/binding/ir_to_cbem_lowerer.hpp>
#include <cornflakes/interface/diagnostics/issue.hpp>
#include <cornflakes/interface/vm/bytecode_exec_context.hpp>

#include <map>
#include <span>
#include <string>

namespace whiteout::cornflakes {

class CBEMInterpreter {
public:
    CBEMInterpreter() = default;

    bool step(const CBEMInstruction& ins, BytecodeExecContext& ctx, IssueBag& issues) const;

    std::size_t run(std::span<const CBEMInstruction> program, BytecodeExecContext& ctx,
                    IssueBag& issues) const;
};

const std::map<std::string, u64>& vmFunctionCallCounts() noexcept;

void vmResetFunctionCallCounts() noexcept;

inline bool g_vmFunctionCallCounting = false;

inline void vmSetFunctionCallCounting(bool enabled) noexcept {
    g_vmFunctionCallCounting = enabled;
}
inline bool vmFunctionCallCountingEnabled() noexcept {
    return g_vmFunctionCallCounting;
}

}

#pragma once

/// @file
/// @brief Stack-free bytecode VM that executes one CBEM/IR instruction at a time.

#include <cornflakes/interface/binding/ir_to_cbem_lowerer.hpp>
#include <cornflakes/interface/diagnostics/issue.hpp>
#include <cornflakes/interface/vm/bytecode_exec_context.hpp>

#include <map>
#include <span>
#include <string>

namespace whiteout::cornflakes {

/// @brief Interpreter for the CBEM/IR bytecode emitted by the IRToCBEMLowerer.
///
/// The VM is stateless across calls — all per-evaluation state lives in the
/// caller-owned BytecodeExecContext. Errors are reported via IssueBag; a
/// returned `false` always corresponds to a Fatal issue having been pushed.
class CBEMInterpreter {
public:
    CBEMInterpreter() = default;

    /// @brief Execute one decoded instruction against `ctx`.
    /// @return `true` on success, `false` on a fatal error (issue already pushed).
    bool step(const CBEMInstruction& ins, BytecodeExecContext& ctx, IssueBag& issues) const;

    /// @brief Execute `program` sequentially, stopping at the first fatal issue.
    /// @return Number of instructions that ran (≤ `program.size()`).
    std::size_t run(std::span<const CBEMInstruction> program, BytecodeExecContext& ctx,
                    IssueBag& issues) const;
};

/// @brief Process-global histogram of FunctionCall dispatches keyed by symbol.
/// @note Aggregated across every CBEMInterpreter; useful for coverage reports.
const std::map<std::string, u64>& vmFunctionCallCounts() noexcept;

/// @brief Clear the histogram returned by vmFunctionCallCounts().
void vmResetFunctionCallCounts() noexcept;

} // namespace whiteout::cornflakes

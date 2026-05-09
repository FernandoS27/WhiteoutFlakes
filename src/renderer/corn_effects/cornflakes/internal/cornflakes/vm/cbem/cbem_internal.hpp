#pragma once

#include <cornflakes/interface/binding/ir_to_cbem_lowerer.hpp>
#include <cornflakes/interface/diagnostics/issue.hpp>
#include <cornflakes/interface/vm/bytecode_exec_context.hpp>
#include <cornflakes/vm/math_functions.hpp>
#include <cornflakes/interface/vm/register_value.hpp>

#include <map>
#include <string>
#include <string_view>

namespace whiteout::cornflakes {

std::map<std::string, u64>& getMutableFunctionCallCounts() noexcept;

Issue vmFatal(u32 code, std::string_view message) noexcept;
Issue vmWarn(u32 code, std::string_view message) noexcept;

bool readSrc(const BytecodeExecContext& ctx, u32 regId, RegisterValue& out,
             IssueBag& issues) noexcept;
bool writeDst(BytecodeExecContext& ctx, u32 regId, const RegisterValue& v,
              IssueBag& issues) noexcept;

u16 canonicalExternalSlot(const BytecodeExecContext& ctx, u16 byteSlot) noexcept;

bool applyMathOp(u8 op, const RegisterValue& a, const RegisterValue& b, u8 components,
                 bool integerOp, RegisterValue& out, IssueBag& issues) noexcept;

bool execNop(const CBEMInstruction& ins, BytecodeExecContext& ctx, IssueBag& issues) noexcept;
bool execLoadExternal(const CBEMInstruction& ins, BytecodeExecContext& ctx,
                      IssueBag& issues) noexcept;
bool execStoreToExternal(const CBEMInstruction& ins, BytecodeExecContext& ctx,
                         IssueBag& issues) noexcept;
bool execMove(const CBEMInstruction& ins, BytecodeExecContext& ctx, IssueBag& issues) noexcept;
bool execVecCtor(const CBEMInstruction& ins, BytecodeExecContext& ctx, IssueBag& issues) noexcept;
bool execVecSwizzle(const CBEMInstruction& ins, BytecodeExecContext& ctx,
                    IssueBag& issues) noexcept;
bool execMathOp(const CBEMInstruction& ins, BytecodeExecContext& ctx, IssueBag& issues) noexcept;
bool execMathFunc1(const CBEMInstruction& ins, BytecodeExecContext& ctx,
                   IssueBag& issues) noexcept;
bool execMathFunc2(const CBEMInstruction& ins, BytecodeExecContext& ctx,
                   IssueBag& issues) noexcept;
bool execMathFunc3(const CBEMInstruction& ins, BytecodeExecContext& ctx,
                   IssueBag& issues) noexcept;
bool execSelect(const CBEMInstruction& ins, BytecodeExecContext& ctx, IssueBag& issues) noexcept;

bool execExternalClear(const CBEMInstruction& ins, BytecodeExecContext& ctx,
                       IssueBag& issues) noexcept;
bool execBroadcast(const CBEMInstruction& ins, BytecodeExecContext& ctx,
                   IssueBag& issues) noexcept;
bool execBinaryArith(const CBEMInstruction& ins, BytecodeExecContext& ctx, IssueBag& issues,
                     MathOp op) noexcept;
bool execMadd(const CBEMInstruction& ins, BytecodeExecContext& ctx, IssueBag& issues) noexcept;
bool execIDivMulInv(const CBEMInstruction& ins, BytecodeExecContext& ctx,
                    IssueBag& issues) noexcept;
bool execFunctionProlog(const CBEMInstruction& ins, BytecodeExecContext& ctx,
                        IssueBag& issues) noexcept;
bool execFunctionEpilog(const CBEMInstruction& ins, BytecodeExecContext& ctx,
                        IssueBag& issues) noexcept;

bool execFunctionCall(const CBEMInstruction& ins, BytecodeExecContext& ctx,
                      IssueBag& issues) noexcept;

} // namespace whiteout::cornflakes

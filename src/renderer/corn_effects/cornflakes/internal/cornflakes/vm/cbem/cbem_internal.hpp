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

bool readConst(const BytecodeExecContext& ctx, u32 slot, u8 bankByte, RegisterValue& out,
               IssueBag& issues) noexcept;

bool isConstPoolHit(const BytecodeExecContext& ctx, const DecodedRegId& d) noexcept;

using ResolvedCallFn = bool (*)(const CBEMInstruction&, BytecodeExecContext&, RegisterValue&,
                                IssueBag&);

ResolvedCallFn resolveExactCall(std::string_view canonicalName) noexcept;

struct CurveSampleTarget {
    const SamplerCurve* curve = nullptr;
    u8 components = 0U;
};

bool resolveCurveSample(const CBEMInstruction& ins, const BytecodeExecContext& ctx,
                        CurveSampleTarget& out) noexcept;

std::string_view canonicalizeSymbol(std::string_view sym) noexcept;

bool applyMathOp(u8 op, const RegisterValue& a, const RegisterValue& b, u8 components,
                 bool integerOp, RegisterValue& out, IssueBag& issues) noexcept;

bool execFunctionCall(const CBEMInstruction& ins, BytecodeExecContext& ctx,
                      IssueBag& issues) noexcept;

}

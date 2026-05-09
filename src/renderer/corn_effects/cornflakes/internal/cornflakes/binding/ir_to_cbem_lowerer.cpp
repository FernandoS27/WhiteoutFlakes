#include <cornflakes/interface/binding/ir_to_cbem_lowerer.hpp>

#include <cornflakes/core/determinism.hpp>
#include <cornflakes/diagnostics/issue_codes.hpp>

namespace whiteout::cornflakes {

namespace {

Issue bindingFatal(u32 code, std::string_view message) noexcept {
    Issue issue;
    issue.severity = Severity::Fatal;
    issue.category = Category::Binding;
    issue.code = code;
    issue.message = message;
    return issue;
}

} // namespace

std::optional<CBEMInstruction> IRToCBEMLowerer::lowerOne(const IRInstruction& ir,
                                                         IssueBag& issues) const {

    if (isIROpcode(ir.opcode) || isCBEMOpcode(ir.opcode)) {
        CBEMInstruction out;
        out.opcode = ir.opcode;
        out.operandCount = ir.operandCount;
        out.operands = ir.operands;
        out.extraOperands = ir.extraOperands;
        return out;
    }

    issues.push(
        bindingFatal(issues::binding::kLowerOutOfRange,
                     "IRToCBEMLowerer: opcode outside IR (0x42-0x53) and CBEM (0x69-0x7D) ranges"));
    return std::nullopt;
}

CBEMInstruction CBEMEncoder::encodeMadd(u32 dst, u32 src0, u32 src1, u32 src2) noexcept {
    CBEMInstruction out;
    out.opcode = Opcode::Madd;
    out.operandCount = kMaddOperandCount;

    out.operands[kMaddDstIndex] = dst;
    out.operands[kMaddSrc0Index] = src0;
    out.operands[kMaddSrc1Index] = src1;
    out.operands[kMaddSrc2Index] = src2;
    return out;
}

} // namespace whiteout::cornflakes

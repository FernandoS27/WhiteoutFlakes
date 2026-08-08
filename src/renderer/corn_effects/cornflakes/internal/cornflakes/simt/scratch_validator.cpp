#include <cornflakes/interface/simt/scratch_validator.hpp>

#include <cstdio>
#include <string>
#include <unordered_set>

namespace whiteout::cornflakes::simt {

ScratchReport analyseLocalScratch(std::span<const CBEMInstruction> program) {
    ScratchReport report;
    report.instructionsScanned = program.size();

    std::unordered_set<u32> written;

    for (std::size_t i = 0; i < program.size(); ++i) {
        const auto& ins = program[i];

        forEachRegisterOperand(
            ins,
            [&](u32 regId) {
                const auto d = decodeRegId(regId);
                if (d.scope != scope::kLocal) {
                    return;
                }
                ++report.localReads;
                if (written.count(d.localIdx) != 0U) {
                    return;
                }
                LocalReadBeforeWrite f;
                f.instructionIndex = i;
                f.streamOffset = ins.streamOffset;
                f.opcode = ins.opcode;
                f.registerId = regId;
                f.localIndex = d.localIdx;
                report.findings.push_back(f);
            },
            [](u32) {});

        forEachRegisterOperand(
            ins, [](u32) {},
            [&](u32 regId) {
                const auto d = decodeRegId(regId);
                if (d.scope != scope::kLocal) {
                    return;
                }
                ++report.localWrites;
                written.insert(d.localIdx);
            });
    }

    return report;
}

namespace {

u64 violationCount = 0U;
std::string& firstViolationStorage() {
    static std::string s;
    return s;
}

}

u64 boundProgramViolationCount() noexcept {
    return violationCount;
}

const char* firstBoundProgramViolation() noexcept {
    return firstViolationStorage().c_str();
}

void resetBoundProgramViolations() noexcept {
    violationCount = 0U;
    firstViolationStorage().clear();
}

void checkBoundProgram(std::span<const CBEMInstruction> program) noexcept {
    if (program.empty()) {
        return;
    }
    try {
        const auto report = analyseLocalScratch(program);
        if (report.clean()) {
            return;
        }
        violationCount += report.findings.size();
        if (firstViolationStorage().empty()) {
            const auto& f = report.findings.front();
            char buf[192];
            std::snprintf(
                buf, sizeof(buf),
                "local vr%u read before written (instruction %zu, stream offset 0x%x, %s)",
                f.localIndex, f.instructionIndex, f.streamOffset, opcodeName(f.opcode));
            firstViolationStorage() = buf;
        }
    } catch (...) {
        return;
    }
#ifndef NDEBUG
    std::fprintf(stderr, "[cornflakes] SIMT scratch violation: %s\n",
                 firstViolationStorage().c_str());
#endif
}

}

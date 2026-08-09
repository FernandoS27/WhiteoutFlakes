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

// Body of checkBoundProgram, split out so the exception guard around it stays
// a thin wrapper — the web build compiles with -fno-exceptions.
// Returns true when the program has at least one violation.
bool recordViolations(std::span<const CBEMInstruction> program) {
    const auto report = analyseLocalScratch(program);
    if (report.clean()) {
        return false;
    }
    violationCount += report.findings.size();
    if (firstViolationStorage().empty()) {
        const auto& f = report.findings.front();
        char buf[192];
        std::snprintf(buf, sizeof(buf),
                      "local vr%u read before written (instruction %zu, stream offset 0x%x, %s)",
                      f.localIndex, f.instructionIndex, f.streamOffset, opcodeName(f.opcode));
        firstViolationStorage() = buf;
    }
    return true;
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
#if defined(__cpp_exceptions)
    try {
        if (!recordViolations(program)) {
            return;
        }
    } catch (...) {
        return;
    }
#else
    if (!recordViolations(program)) {  // -fno-exceptions: nothing to catch
        return;
    }
#endif
#ifndef NDEBUG
    std::fprintf(stderr, "[cornflakes] SIMT scratch violation: %s\n",
                 firstViolationStorage().c_str());
#endif
}

}

#include <cornflakes/interface/schema/opcodes.hpp>
#include <cornflakes/interface/vm/bytecode_trace.hpp>

#include <cstdio>
#include <cstring>

namespace whiteout::cornflakes {

std::size_t formatTraceEvent(const TraceEvent& ev, char* out, std::size_t outCap) noexcept {
    if (out == nullptr || outCap == 0) {
        return 0;
    }
    int written = 0;
    if (ev.dstKind == TraceDstKind::None) {
        written = std::snprintf(out, outCap, "@%04X op=0x%02X %s", ev.streamOffset, ev.opcode,
                                opcodeName(ev.opcode));
    } else {
        const char* dstTag = (ev.dstKind == TraceDstKind::Register) ? "r" : "ext";

        char laneBuf[96] = {0};
        const u8 comps = (ev.value.componentCount == 0)
                             ? 1U
                             : (ev.value.componentCount > 4U ? 4U : ev.value.componentCount);
        int laneOff = 0;
        for (u8 i = 0; i < comps && laneOff < static_cast<int>(sizeof(laneBuf)) - 1; ++i) {
            int n = 0;
            if (ev.value.typeBank == bank::kBool) {
                u32 v;
                std::memcpy(&v, &ev.value.lanes[i], sizeof(v));
                n = std::snprintf(laneBuf + laneOff, sizeof(laneBuf) - laneOff, "%s%s",
                                  i == 0 ? "" : ", ", v != 0U ? "true" : "false");
            } else if (bankIsIntegral(ev.value.typeBank)) {
                i32 v;
                std::memcpy(&v, &ev.value.lanes[i], sizeof(v));
                n = std::snprintf(laneBuf + laneOff, sizeof(laneBuf) - laneOff, "%s%d",
                                  i == 0 ? "" : ", ", v);
            } else {
                n = std::snprintf(laneBuf + laneOff, sizeof(laneBuf) - laneOff, "%s%g",
                                  i == 0 ? "" : ", ", static_cast<double>(ev.value.lanes[i]));
            }
            if (n <= 0)
                break;
            laneOff += n;
        }
        if (ev.opcode == Opcode::FunctionCall && !ev.symbol.empty()) {
            written =
                std::snprintf(out, outCap, "@%04X op=0x%02X FunctionCall(%.*s) %s=0x%08X -> [%s]",
                              ev.streamOffset, ev.opcode, static_cast<int>(ev.symbol.size()),
                              ev.symbol.data(), dstTag, ev.dstId, laneBuf);
        } else {
            written =
                std::snprintf(out, outCap, "@%04X op=0x%02X %s %s=0x%08X -> [%s]", ev.streamOffset,
                              ev.opcode, opcodeName(ev.opcode), dstTag, ev.dstId, laneBuf);
        }
    }
    if (written < 0) {
        out[0] = '\0';
        return 0;
    }
    if (static_cast<std::size_t>(written) >= outCap) {

        return outCap - 1U;
    }
    return static_cast<std::size_t>(written);
}

} // namespace whiteout::cornflakes

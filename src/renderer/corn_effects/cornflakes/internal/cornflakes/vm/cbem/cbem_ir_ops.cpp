#include "cbem_internal.hpp"

#include <cornflakes/diagnostics/issue_codes.hpp>
#include <cornflakes/interface/binding/ir_to_cbem_lowerer.hpp>
#include <cornflakes/interface/vm/register_value.hpp>

#include <cmath>
#include <set>
#include <string>
#include <string_view>

namespace whiteout::cornflakes {

bool applyMathOp(u8 op, const RegisterValue& a, const RegisterValue& b, u8 components,
                 bool integerOp, RegisterValue& out, IssueBag& issues) noexcept {
    out.componentCount = components;
    auto warnUnknownOnce = [&]() {
        static std::set<u32> reportedOps;
        const auto [it, inserted] = reportedOps.insert(op);
        if (inserted) {
            static std::set<std::string> stubMessages;
            std::string msg = "IR: MathOp sub-id not implemented (engine no-op): ";
            msg.append(std::to_string(op));
            const auto [mit, _] = stubMessages.insert(std::move(msg));
            issues.push(vmWarn(issues::vm::kUnknownMathOp, std::string_view{*mit}));
        }
    };
    for (u8 i = 0; i < components; ++i) {
        if (!integerOp) {
            const f32 av = a.lanes[i];
            const f32 bv = b.lanes[i];
            f32 r = 0.0F;
            switch (static_cast<MathOp>(op)) {
            case MathOp::Add:
                r = av + bv;
                break;
            case MathOp::Sub:
                r = av - bv;
                break;
            case MathOp::Mul:
                r = av * bv;
                break;
            case MathOp::Div:
                r = av / bv;
                break;
            case MathOp::Mod:
                r = std::fmod(av, bv);
                break;
            case MathOp::Neg:
                r = -av;
                break;
            case MathOp::Lt:
                setLaneI32(out, i, av < bv ? 1 : 0);
                continue;
            case MathOp::Le:
                setLaneI32(out, i, av <= bv ? 1 : 0);
                continue;
            case MathOp::Gt:
                setLaneI32(out, i, av > bv ? 1 : 0);
                continue;
            case MathOp::Ge:
                setLaneI32(out, i, av >= bv ? 1 : 0);
                continue;
            case MathOp::Eq:
                setLaneI32(out, i, av == bv ? 1 : 0);
                continue;
            case MathOp::Ne:
                setLaneI32(out, i, av != bv ? 1 : 0);
                continue;
            default:
                warnUnknownOnce();
                r = av;
                break;
            }
            out.lanes[i] = r;
        } else {
            const i32 av = laneAsI32(a, i);
            const i32 bv = laneAsI32(b, i);
            i32 r = 0;
            switch (static_cast<MathOp>(op)) {
            case MathOp::Add:
                r = av + bv;
                break;
            case MathOp::Sub:
                r = av - bv;
                break;
            case MathOp::Mul:
                r = av * bv;
                break;
            case MathOp::Div:
                r = (bv == 0) ? 0 : av / bv;
                break;
            case MathOp::Mod:
                r = (bv == 0) ? 0 : av % bv;
                break;
            case MathOp::Neg:
                r = -av;
                break;
            case MathOp::Shl:
                r = av << (bv & 31);
                break;
            case MathOp::Shr:
                r = av >> (bv & 31);
                break;
            case MathOp::BitAnd:
                r = av & bv;
                break;
            case MathOp::BitOr:
                r = av | bv;
                break;
            case MathOp::BitXor:
                r = av ^ bv;
                break;
            case MathOp::BitNot:
                r = ~av;
                break;
            case MathOp::Lt:
                r = av < bv ? 1 : 0;
                break;
            case MathOp::Le:
                r = av <= bv ? 1 : 0;
                break;
            case MathOp::Gt:
                r = av > bv ? 1 : 0;
                break;
            case MathOp::Ge:
                r = av >= bv ? 1 : 0;
                break;
            case MathOp::Eq:
                r = av == bv ? 1 : 0;
                break;
            case MathOp::Ne:
                r = av != bv ? 1 : 0;
                break;
            default:
                warnUnknownOnce();
                r = av;
                break;
            }
            setLaneI32(out, i, r);
        }
    }
    return true;
}

}

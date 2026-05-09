#include <cornflakes/core/determinism.hpp>
#include <cornflakes/diagnostics/issue_codes.hpp>
#include <cornflakes/vm/math_functions.hpp>

#include <cmath>

namespace whiteout::cornflakes {

namespace {

Issue vmFatal(u32 code, std::string_view message) noexcept {
    Issue issue;
    issue.severity = Severity::Fatal;
    issue.category = Category::Vm;
    issue.code = code;
    issue.message = message;
    return issue;
}

} // namespace

f32 mathLerp(f32 a, f32 b, f32 t) noexcept {
    return a + t * (b - a);
}

f32 mathClamp(f32 x, f32 lo, f32 hi) noexcept {
    return std::fmin(std::fmax(x, lo), hi);
}

f32 mathWithin(f32 x, f32 lo, f32 hi) noexcept {
    return (x >= lo && x <= hi) ? 1.0F : 0.0F;
}

std::optional<f32> mathFunc3Eval(u8 id, f32 a, f32 b, f32 c, IssueBag& issues) {
    switch (static_cast<MathFunc3>(id)) {
    case MathFunc3::Lerp:
        return mathLerp(a, b, c);
    case MathFunc3::Clamp:
        return mathClamp(a, b, c);
    case MathFunc3::Within:
        return mathWithin(a, b, c);
    case MathFunc3::Count:
        break;
    }
    issues.push(vmFatal(issues::vm::kUnknownMathFunc3, "MathFunc3: unknown function id"));
    return std::nullopt;
}

f32 mathSign(f32 x) noexcept {
    if (x > 0.0F) {
        return 1.0F;
    }
    if (x < 0.0F) {
        return -1.0F;
    }
    return 0.0F;
}

f32 mathFracUnsigned(f32 x) noexcept {
    return x - std::floor(x);
}

f32 mathFrac(f32 x) noexcept {
    return x - std::trunc(x);
}

std::optional<f32> mathFunc1Eval(u8 id, f32 x, IssueBag& issues) {

    switch (static_cast<MathFunc1>(id)) {
    case MathFunc1::Sign:
        return mathSign(x);
    case MathFunc1::FracUnsigned:
        return mathFracUnsigned(x);
    case MathFunc1::Frac:
        return mathFrac(x);
    default:
        break;
    }
    issues.push(vmFatal(issues::vm::kUnknownMathFunc1, "MathFunc1: unknown function id"));
    return std::nullopt;
}

} // namespace whiteout::cornflakes

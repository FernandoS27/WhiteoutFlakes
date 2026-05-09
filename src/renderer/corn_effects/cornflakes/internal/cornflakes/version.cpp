#include <cornflakes/core/determinism.hpp>
#include <cornflakes/version.hpp>

namespace whiteout::cornflakes {

namespace {

constexpr Version kVersion{0, 1, 0};
constexpr std::string_view kVersionString{"0.1.0"};

} // namespace

Version libraryVersion() noexcept {
    return kVersion;
}

std::string_view libraryVersionString() noexcept {
    return kVersionString;
}

} // namespace whiteout::cornflakes

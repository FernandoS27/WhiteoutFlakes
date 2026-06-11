#pragma once

#include <cornflakes/interface/diagnostics/issue.hpp>

#include <optional>

namespace whiteout::cornflakes {

template <typename T>
struct ServiceOutcome {
    std::optional<T> value;
    IssueBag issues;

    bool ok() const noexcept {
        return value.has_value() && !issues.hasErrors();
    }
};

template <>
struct ServiceOutcome<void> {
    IssueBag issues;

    bool ok() const noexcept {
        return !issues.hasErrors();
    }
};

}

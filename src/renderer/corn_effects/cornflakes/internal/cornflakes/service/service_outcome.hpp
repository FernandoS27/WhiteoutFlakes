#pragma once

/// @file
/// @brief `ServiceOutcome<T>` — value+IssueBag pair returned by every service-facade method.

#include <cornflakes/interface/diagnostics/issue.hpp>

#include <optional>

namespace whiteout::cornflakes {

/// @brief Value-or-issues result. `ok()` is the canonical success check.
template <typename T>
struct ServiceOutcome {
    std::optional<T> value;
    IssueBag issues;

    /// @brief True when a value is present and no errors were pushed.
    bool ok() const noexcept {
        return value.has_value() && !issues.hasErrors();
    }
};

/// @brief Void specialisation — only the issue bag carries success/failure.
template <>
struct ServiceOutcome<void> {
    IssueBag issues;

    bool ok() const noexcept {
        return !issues.hasErrors();
    }
};

} // namespace whiteout::cornflakes

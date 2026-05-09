#pragma once

/// @file
/// @brief Read-only diagnostics surface — last-frame issues + frame trace counters.

#include <cornflakes/interface/core/types.hpp>
#include <cornflakes/diagnostics/frame_trace.hpp>
#include <cornflakes/interface/diagnostics/issue.hpp>

#include <span>

namespace whiteout::cornflakes {

/// @brief Diagnostics facade exposed by `CornFlakesService::diagnostics()`.
class DiagnosticsFacade {
public:
    DiagnosticsFacade() = default;
    virtual ~DiagnosticsFacade() = default;

    DiagnosticsFacade(const DiagnosticsFacade&) = delete;
    DiagnosticsFacade& operator=(const DiagnosticsFacade&) = delete;
    DiagnosticsFacade(DiagnosticsFacade&&) = delete;
    DiagnosticsFacade& operator=(DiagnosticsFacade&&) = delete;

    virtual std::span<const Issue> lastFrameIssues() const noexcept = 0;

    virtual f32 lastTickDt() const noexcept = 0;

    virtual const FrameTrace& lastFrameTrace() const noexcept = 0;
};

} // namespace whiteout::cornflakes

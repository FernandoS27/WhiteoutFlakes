#pragma once

/// @file
/// @brief Issue/Severity/Category model and the `IssueBag` collector used everywhere as out-param.

#include <cornflakes/interface/core/types.hpp>

#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

namespace whiteout::cornflakes {

/// @brief Diagnostic severity. `Fatal` aborts the operation that pushed it.
enum class Severity : u8 {
    Info,
    Warning,
    Error,
    Fatal,
};

/// @brief Subsystem source of an `Issue`. Used for filtering and code-namespacing.
enum class Category : u8 {
    Asset,
    Binding,
    Schema,
    Vm,
    Sampler,
    Sim,
    Events,
    Render,
    Scheduler,
    Service,
    Diagnostics,
    Core,
};

/// @brief Optional structured context attached to every `Issue` for triage.
struct IssueContext {
    u64 effectId = 0;
    u64 emitterId = 0;
    u64 frameId = 0;
    u32 opcodeOffset = 0;
    u32 prngSeed = 0;
    f32 timeWindowStart = 0.0F;
    f32 timeWindowEnd = 0.0F;
};

/// @brief One diagnostic record. `code` is namespaced by `category` (see issue_codes.hpp).
struct Issue {
    Severity severity = Severity::Info;
    Category category = Category::Core;
    u32 code = 0;
    std::string_view message;
    IssueContext context;
};

/// @brief Append-only sink for diagnostics; callers pass one in by reference.
class IssueBag {
public:
    IssueBag() = default;

    void push(Issue issue);

    bool hasErrors() const noexcept;
    bool hasFatal() const noexcept;

    std::span<const Issue> view() const noexcept;
    std::size_t size() const noexcept;

    void clear() noexcept;

private:
    std::vector<Issue> m_issues;
    u32 m_errorCount = 0;
    u32 m_fatalCount = 0;
};

} // namespace whiteout::cornflakes

#pragma once

#include <cornflakes/interface/core/types.hpp>

#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

namespace whiteout::cornflakes {

enum class Severity : u8 {
    Info,
    Warning,
    Error,
    Fatal,
};

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

struct IssueContext {
    u64 effectId = 0;
    u64 emitterId = 0;
    u64 frameId = 0;
    u32 opcodeOffset = 0;
    u32 prngSeed = 0;
    f32 timeWindowStart = 0.0F;
    f32 timeWindowEnd = 0.0F;
};

struct Issue {
    Severity severity = Severity::Info;
    Category category = Category::Core;
    u32 code = 0;
    std::string_view message;
    IssueContext context;
};

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

}

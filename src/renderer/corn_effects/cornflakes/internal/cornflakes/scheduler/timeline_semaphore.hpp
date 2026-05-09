#pragma once

/// @file
/// @brief Monotonic timeline semaphore used for cross-task ordering on the worker pool.

#include <cornflakes/interface/core/types.hpp>

namespace whiteout::cornflakes {

/// @brief Monotonic counter; signals only ever advance, waits block until reached.
class ITimelineSemaphore {
public:
    using Value = u64;

    ITimelineSemaphore() = default;
    virtual ~ITimelineSemaphore() = default;

    ITimelineSemaphore(const ITimelineSemaphore&) = delete;
    ITimelineSemaphore& operator=(const ITimelineSemaphore&) = delete;
    ITimelineSemaphore(ITimelineSemaphore&&) = delete;
    ITimelineSemaphore& operator=(ITimelineSemaphore&&) = delete;

    virtual Value value() const noexcept = 0;

    /// @brief Advance the timeline to `v`. Regression is a fatal scheduler issue.
    virtual void signal(Value v) noexcept = 0;

    /// @brief Block until `value() >= v`.
    virtual void wait(Value v) = 0;

    /// @brief Atomically reserve and return the next signal value to use.
    virtual Value next() noexcept = 0;
};

} // namespace whiteout::cornflakes

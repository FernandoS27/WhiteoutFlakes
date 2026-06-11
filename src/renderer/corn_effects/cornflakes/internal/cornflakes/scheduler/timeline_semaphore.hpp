#pragma once

#include <cornflakes/interface/core/types.hpp>

namespace whiteout::cornflakes {

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

    virtual void signal(Value v) noexcept = 0;

    virtual void wait(Value v) = 0;

    virtual Value next() noexcept = 0;
};

}

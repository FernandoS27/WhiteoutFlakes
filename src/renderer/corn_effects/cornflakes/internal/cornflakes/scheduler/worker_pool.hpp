#pragma once

/// @file
/// @brief Worker pool interface and `WorkerTask` carrier with timeline-semaphore wait/signal slots.

#include <cornflakes/interface/core/types.hpp>
#include <cornflakes/scheduler/timeline_semaphore.hpp>

#include <cstddef>
#include <functional>
#include <memory>

namespace whiteout::cornflakes {

/// @brief One unit of work plus its optional wait-before / signal-after timeline pair.
struct WorkerTask {
    std::function<void()> fn;
    ITimelineSemaphore* waitSemaphore = nullptr;
    ITimelineSemaphore::Value waitValue = 0;
    ITimelineSemaphore* signalSemaphore = nullptr;
    ITimelineSemaphore::Value signalValue = 0;
};

/// @brief Worker-pool interface; concrete impls are serial or threaded.
class IWorkerPool {
public:
    IWorkerPool() = default;
    virtual ~IWorkerPool() = default;

    IWorkerPool(const IWorkerPool&) = delete;
    IWorkerPool& operator=(const IWorkerPool&) = delete;
    IWorkerPool(IWorkerPool&&) = delete;
    IWorkerPool& operator=(IWorkerPool&&) = delete;

    virtual void submit(const WorkerTask& task) = 0;
    virtual void waitIdle() = 0;
    virtual std::size_t threadCount() const noexcept = 0;
    virtual std::unique_ptr<ITimelineSemaphore> createTimelineSemaphore() = 0;
};

} // namespace whiteout::cornflakes

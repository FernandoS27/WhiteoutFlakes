#pragma once

#include <cornflakes/interface/core/types.hpp>
#include <cornflakes/scheduler/timeline_semaphore.hpp>

#include <cstddef>
#include <functional>
#include <memory>

namespace whiteout::cornflakes {

struct WorkerTask {
    std::function<void()> fn;
    ITimelineSemaphore* waitSemaphore = nullptr;
    ITimelineSemaphore::Value waitValue = 0;
    ITimelineSemaphore* signalSemaphore = nullptr;
    ITimelineSemaphore::Value signalValue = 0;
};

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

}

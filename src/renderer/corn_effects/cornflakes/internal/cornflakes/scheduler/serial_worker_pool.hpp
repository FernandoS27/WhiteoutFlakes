#pragma once

#include <cornflakes/scheduler/timeline_semaphore.hpp>
#include <cornflakes/scheduler/worker_pool.hpp>

#include <cstddef>
#include <memory>

namespace whiteout::cornflakes {

class IssueBag;

class SerialWorkerPool final : public IWorkerPool {
public:
    explicit SerialWorkerPool(IssueBag& issues) noexcept;
    ~SerialWorkerPool() override = default;

    SerialWorkerPool(const SerialWorkerPool&) = delete;
    SerialWorkerPool& operator=(const SerialWorkerPool&) = delete;
    SerialWorkerPool(SerialWorkerPool&&) = delete;
    SerialWorkerPool& operator=(SerialWorkerPool&&) = delete;

    void submit(const WorkerTask& task) override;
    void waitIdle() override;
    std::size_t threadCount() const noexcept override;
    std::unique_ptr<ITimelineSemaphore> createTimelineSemaphore() override;

private:
    IssueBag* m_issues;
    bool m_inTask = false;
};

}

#include <cornflakes/core/determinism.hpp>
#include <cornflakes/diagnostics/issue_codes.hpp>
#include <cornflakes/interface/diagnostics/issue.hpp>
#include <cornflakes/scheduler/serial_worker_pool.hpp>

#include <memory>
#include <utility>

namespace whiteout::cornflakes {

namespace {

Issue schedulerFatal(u32 code, std::string_view message) noexcept {
    Issue issue;
    issue.severity = Severity::Fatal;
    issue.category = Category::Scheduler;
    issue.code = code;
    issue.message = message;
    return issue;
}

class SerialTimelineSemaphore final : public ITimelineSemaphore {
public:
    explicit SerialTimelineSemaphore(IssueBag& issues) noexcept : m_issues(&issues) {}

    Value value() const noexcept override {
        return m_value;
    }

    void signal(Value v) noexcept override {
        if (v <= m_value) {
            m_issues->push(schedulerFatal(issues::scheduler::kSemaphoreRegressed,
                                          "TimelineSemaphore.signal must strictly increase value"));
            return;
        }
        m_value = v;
        if (v > m_nextValue) {
            m_nextValue = v;
        }
    }

    void wait(Value v) override {
        if (v > m_value) {

            m_issues->push(
                schedulerFatal(issues::scheduler::kSerialWaitUnsignaled,
                               "TimelineSemaphore.wait on value not yet signaled (serial pool)"));
        }
    }

    Value next() noexcept override {
        return ++m_nextValue;
    }

private:
    IssueBag* m_issues;
    Value m_value = 0;
    Value m_nextValue = 0;
};

} // namespace

SerialWorkerPool::SerialWorkerPool(IssueBag& issues) noexcept : m_issues(&issues) {}

void SerialWorkerPool::submit(const WorkerTask& task) {
    if (m_inTask) {
        m_issues->push(schedulerFatal(
            issues::scheduler::kFlatDagBreach,
            "WorkerPool.submit called from inside a running task (flat-DAG violation)"));
        return;
    }

    if (task.waitSemaphore != nullptr && task.waitSemaphore->value() < task.waitValue) {
        m_issues->push(
            schedulerFatal(issues::scheduler::kSignalBeforeWait,
                           "WorkerTask.waitValue not signaled before submit (signal-before-wait)"));
        return;
    }

    m_inTask = true;
    if (task.fn) {
        task.fn();
    }
    m_inTask = false;

    if (task.signalSemaphore != nullptr) {
        task.signalSemaphore->signal(task.signalValue);
    }
}

void SerialWorkerPool::waitIdle() {}

std::size_t SerialWorkerPool::threadCount() const noexcept {
    return 1;
}

std::unique_ptr<ITimelineSemaphore> SerialWorkerPool::createTimelineSemaphore() {
    return std::make_unique<SerialTimelineSemaphore>(*m_issues);
}

} // namespace whiteout::cornflakes

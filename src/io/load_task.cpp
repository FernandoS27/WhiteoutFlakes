#include "io/load_task.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

namespace whiteout::flakes::io {

namespace {
struct QueuedTask {
    TaskId id = kInvalidTaskId;
    std::string title;
    LoadTaskRunner::Body body;
    LoadTaskRunner::OnDone onDone;
    bool cancellable = true;
    bool modal = true;
};
} // namespace

struct LoadTaskRunner::Impl {
    ProgressState state;

    mutable std::mutex mu;
    std::condition_variable cv;
    std::deque<QueuedTask> queue;
    // Set while a body is running. Kept separate from `queue` so Busy() covers
    // the window between popping a task and finishing it.
    bool running = false;
    bool stopping = false;
    std::atomic<TaskId> nextId{1};

    // Worker pushes, Pump drains. Its own lock: a completion must not wait on
    // the queue lock a running task's submission is holding.
    std::mutex doneMu;
    std::deque<std::pair<TaskOutcome, OnDone>> completed;

    std::thread worker;

    void Loop() {
        while (true) {
            QueuedTask task;
            {
                std::unique_lock lk(mu);
                cv.wait(lk, [&] { return stopping || !queue.empty(); });
                if (stopping && queue.empty())
                    return;
                task = std::move(queue.front());
                queue.pop_front();
                running = true;
            }

            state.Begin(task.title, task.cancellable, task.modal);

            TaskResult result;
            {
                // Scoped so the monitor closes its budget — and pushes the
                // final position — before Finish stamps the terminal state.
                ProgressMonitor monitor(&state);
                result = task.body ? task.body(monitor) : TaskResult::Ok();
            }

            TaskOutcome outcome;
            outcome.id = task.id;
            outcome.cancelled = state.Cancelled();
            // Cancellation outranks whatever the body returned: a body that
            // stops early usually reports its own failure on the way out, and
            // "cancelled" is the more useful thing to tell the host.
            outcome.ok = result.ok && !outcome.cancelled;
            outcome.error = outcome.cancelled ? std::string("Cancelled") : std::move(result.error);

            state.Finish(outcome.ok, outcome.error);

            {
                std::lock_guard lk(doneMu);
                completed.emplace_back(std::move(outcome), std::move(task.onDone));
            }
            {
                std::lock_guard lk(mu);
                running = false;
            }
        }
    }
};

LoadTaskRunner::LoadTaskRunner() : impl_(std::make_unique<Impl>()) {
    impl_->worker = std::thread([this] { impl_->Loop(); });
}

LoadTaskRunner::~LoadTaskRunner() {
    Shutdown();
}

void LoadTaskRunner::Shutdown() {
    if (!impl_)
        return;
    {
        std::lock_guard lk(impl_->mu);
        if (impl_->stopping && !impl_->worker.joinable())
            return;
        impl_->stopping = true;
    }
    // The running body is asked to stop; queued ones are dropped by the loop's
    // stopping check. A body with no cancellation checks still has to finish.
    impl_->state.RequestCancel();
    impl_->cv.notify_all();
    if (impl_->worker.joinable())
        impl_->worker.join();
    // Deliberately not delivered: OnDone runs on the host thread, and at
    // shutdown there is no longer one to run on.
    std::lock_guard lk(impl_->doneMu);
    impl_->completed.clear();
}

TaskId LoadTaskRunner::Run(std::string title, Body body, OnDone onDone, bool cancellable,
                           bool modal) {
    QueuedTask task;
    const TaskId id = impl_->nextId.fetch_add(1, std::memory_order_relaxed);
    task.id = id;
    task.title = std::move(title);
    task.body = std::move(body);
    task.onDone = std::move(onDone);
    task.cancellable = cancellable;
    task.modal = modal;

    {
        std::lock_guard lk(impl_->mu);
        if (impl_->stopping)
            return kInvalidTaskId;
        impl_->queue.push_back(std::move(task));
    }
    impl_->cv.notify_one();
    return id;
}

bool LoadTaskRunner::Busy() const noexcept {
    std::lock_guard lk(impl_->mu);
    return impl_->running || !impl_->queue.empty();
}

usize LoadTaskRunner::Queued() const noexcept {
    std::lock_guard lk(impl_->mu);
    return impl_->queue.size();
}

ProgressSnapshot LoadTaskRunner::Poll() const {
    return impl_->state.Poll();
}

void LoadTaskRunner::RequestCancel() noexcept {
    impl_->state.RequestCancel();
}

void LoadTaskRunner::Pump() {
    std::deque<std::pair<TaskOutcome, OnDone>> ready;
    {
        std::lock_guard lk(impl_->doneMu);
        ready.swap(impl_->completed);
    }
    // Callbacks run outside the lock: an OnDone that queues the next task must
    // not deadlock against a worker delivering into the same deque.
    for (auto& [outcome, cb] : ready) {
        if (cb)
            cb(outcome);
    }
}

} // namespace whiteout::flakes::io

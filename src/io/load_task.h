#pragma once

// ============================================================================
// One background thread for the loads that would otherwise freeze the window.
//
// Opening a CASC install, walking a root manifest and reading fourteen client
// databases are all seconds of work that used to run on whichever thread asked
// first — usually the host's, which is why the window stopped repainting. A
// task moves the work here; the host keeps drawing and polls a snapshot.
//
// Deliberately ONE thread, and tasks run one at a time:
//
//   * Two concurrent opens cannot be drawn as one honest bar.
//   * casc::Storage fans its own index parsing and BLTE decoding across the
//     shared SimpleThreadPool (2-4 threads). Running the task there too would
//     have a task squatting on a worker of the pool it is waiting for.
//   * AcquireSharedCasc already serialises two opens of the same install, so
//     the concurrency would be imaginary for the common case anyway.
//
// A task body may call IContentProvider::ReadFile. That looks forbidden — the
// comment there says "must be called from the Pump thread" — but Wait() has a
// branch for exactly this: a non-Pump caller blocks on the completion CV while
// the host thread delivers, which is how ModelTemplateManager's loader already
// reads. The requirement is only that SOMETHING keeps pumping the provider,
// which the host's frame loop does.
//
// Which is also the one way to deadlock this: never drive the progress modal
// from a nested loop that stops the frame from running. Draw it in the normal
// ImGui pass and return.
// ============================================================================

#include "io/progress.h"
#include "whiteout/flakes/types.h"

#include <functional>
#include <memory>
#include <string>

namespace whiteout::flakes::io {

using TaskId = u64;
inline constexpr TaskId kInvalidTaskId = 0;

/// What a body reports back. Cancellation is not a failure the body has to
/// detect — the runner reads it off the monitor.
struct TaskResult {
    bool ok = true;
    std::string error;

    static TaskResult Ok() {
        return {};
    }
    static TaskResult Fail(std::string why) {
        return {false, std::move(why)};
    }
};

/// Delivered once, on the host thread, from Pump().
struct TaskOutcome {
    TaskId id = kInvalidTaskId;
    bool ok = false;
    bool cancelled = false;
    std::string error;
};

class LoadTaskRunner {
public:
    /// Runs on the task thread. Report through the monitor; check
    /// `monitor.Cancelled()` anywhere a long loop can stop.
    using Body = std::function<TaskResult(ProgressMonitor&)>;
    /// Runs on the thread that calls Pump().
    using OnDone = std::function<void(const TaskOutcome&)>;

    LoadTaskRunner();
    ~LoadTaskRunner();

    LoadTaskRunner(const LoadTaskRunner&) = delete;
    LoadTaskRunner& operator=(const LoadTaskRunner&) = delete;

    /// Queue @p body. Tasks run in submission order, never concurrently.
    /// @param cancellable Whether the UI should offer a Cancel button. A body
    ///        that cannot honour cancellation must say so, or the button lies.
    /// @param modal Whether the host should block the screen for this. True
    ///        for work the user asked for; false for opportunistic work like
    ///        a table prewarm, where a modal in front of a model the user is
    ///        already looking at would be worse than the wait it replaces.
    TaskId Run(std::string title, Body body, OnDone onDone = {}, bool cancellable = true,
               bool modal = true);

    /// A task is running or queued.
    bool Busy() const noexcept;
    /// Tasks waiting behind the running one.
    usize Queued() const noexcept;

    /// State of the running task. Host thread, once per frame.
    ProgressSnapshot Poll() const;

    /// Ask the running task to stop. Best effort: a body between two
    /// cancellation checks finishes what it is doing first.
    void RequestCancel() noexcept;

    /// Host thread. Fires the OnDone of anything that finished since the last
    /// call. Never call this from inside a task body.
    void Pump();

    /// Cancel, drain and join. Called by the destructor; safe to call twice.
    void Shutdown();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace whiteout::flakes::io

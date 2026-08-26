#pragma once

// ============================================================================
// Progress for the loads that take long enough to notice.
//
// Two objects, because there are two jobs and they have opposite shapes:
//
//   ProgressMonitor  what a worker writes. Hierarchical: a step that turns out
//                    to be four sub-steps hands each one a Split() and does not
//                    care where in the overall bar it sits.
//   ProgressState    what the UI reads. One snapshot, overwritten in place,
//                    polled once per frame.
//
// Why the reader polls rather than subscribing: every progress event is a
// complete description of the current state, not a delta, so keeping only the
// latest is lossless for anything that draws a bar. That is also what lets the
// writer never block — a worker that cannot take the lock skips the sample
// instead of waiting for a UI thread, which is the same rule
// casc::ProgressReporter already follows one layer down.
//
// Terminal events are the exception. "Finished", "failed", "cancelled" are
// transitions rather than state and must not be dropped, so they are delivered
// once, by the task runner, on the host thread (see load_task.h).
//
// An uninstrumented caller passes nothing and pays one null check per report.
// ============================================================================

#include "whiteout/flakes/types.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <string_view>

namespace whiteout::flakes::io {

// What the UI draws. A value: copied out of the cell under its lock, then read
// without one for the rest of the frame.
struct ProgressSnapshot {
    std::string title;  // "Opening World of Warcraft" — set once per operation
    std::string stage;  // "Loading VFS manifests", "Client databases"
    std::string object; // "ENCODING", "chrmodel.db2", a .idx name — may be empty

    // The innermost countable unit, for a "7 / 14" line. Both zero when the
    // active step cannot count itself.
    u64 current = 0;
    u64 total = 0;
    u64 bytesDone = 0;
    u64 bytesTotal = 0;

    // Completion of the WHOLE operation in [0,1]. Meaningless when
    // `indeterminate` — draw a marquee then, not a bar at 0%.
    f32 fraction = 0.0f;
    bool indeterminate = true;

    f64 elapsedMs = 0.0;

    bool cancellable = false;
    bool cancelRequested = false;
    // Whether this operation should take over the screen. True for work
    // the user asked for; false for work that merely improves what is
    // already on screen, where a modal would be worse than the wait —
    // see tools/common/progress_dialog.h.
    bool modal = true;

    // Lifecycle. `active` covers the window between Begin and Finish; `done`
    // is set once and stays set, so a dialog can hold the final state for a
    // frame rather than vanishing mid-draw.
    bool active = false;
    bool done = false;
    bool failed = false;
    std::string error;
};

// The cell. One writer chain (the monitor tree of one operation) and one
// reader (the host thread).
class ProgressState {
public:
    // Arm for a new operation. Clears cancellation and restarts the clock.
    void Begin(std::string title, bool cancellable, bool modal = true);

    // Terminal. Idempotent — a second call is ignored, so a body that reports
    // its own failure and then returns one does not race the runner.
    void Finish(bool ok, std::string error = {});

    ProgressSnapshot Poll() const;

    void RequestCancel() noexcept {
        cancel_.store(true, std::memory_order_release);
    }
    bool Cancelled() const noexcept {
        return cancel_.load(std::memory_order_acquire);
    }

private:
    friend class ProgressMonitor;

    // Called from the monitor tree. `fraction` is already absolute.
    void Write(f32 fraction, bool indeterminate, std::string_view stage, std::string_view object,
               u64 current, u64 total, u64 bytesDone, u64 bytesTotal);

    using Clock = std::chrono::steady_clock;

    mutable std::mutex mu_;
    ProgressSnapshot snap_;
    Clock::time_point start_{};
    std::atomic<bool> cancel_{false};
    // Read before the lock is attempted, so a per-item report costs one
    // relaxed load and a comparison when it is too soon to matter.
    std::atomic<i64> lastWriteUs_{0};
};

/**
 * @brief What a worker writes progress through.
 *
 * Hierarchical. A monitor owns an absolute window `[lo, hi]` of the whole
 * operation and counts `0..total` inside it; `Split(budget)` carves the next
 * `budget/total` of that window off for a child, which counts in its own terms
 * and never learns where it sits. Nesting composes without arithmetic at any
 * call site:
 *
 * @code
 * m.Begin("Opening storages", roots.size());
 * for (auto& root : roots)
 *     OpenOne(root, m.Split(1));   // child reports 0..N of its own work
 * @endcode
 *
 * **A child closes its budget when it is destroyed**, whether it finished,
 * returned early or failed. That is the property worth having: the load paths
 * this exists for are full of early returns, and every one of them would
 * otherwise leave the bar parked until the whole operation ended.
 *
 * Not thread-safe per instance. Siblings on different threads are fine — they
 * write disjoint windows — but the bar then shows whichever reported last, so
 * prefer to Split sequential work.
 *
 * A monitor built on a null state is inert: every call is a branch and a
 * return, and Cancelled() is false.
 */
class ProgressMonitor {
public:
    ProgressMonitor() = default;
    /// Root monitor over @p state. Null is legal and makes it inert.
    explicit ProgressMonitor(ProgressState* state) : state_(state) {}

    ~ProgressMonitor();
    ProgressMonitor(ProgressMonitor&&) noexcept;
    ProgressMonitor& operator=(ProgressMonitor&&) noexcept;
    ProgressMonitor(const ProgressMonitor&) = delete;
    ProgressMonitor& operator=(const ProgressMonitor&) = delete;

    /// Name this level's work. @p total 0 means it cannot count itself, which
    /// draws as a marquee rather than a bar stuck at zero.
    void Begin(std::string_view stage, u64 total = 0);

    /// Advance by @p items of `total`.
    void Worked(u64 items = 1);

    /// Absolute position, for adapting a source that reports that way — the
    /// shape casc::ProgressInfo arrives in.
    void SetProgress(f64 done, f64 total);

    /// What is being worked on right now. Copied; the caller's buffer is not
    /// retained (see casc::ProgressInfo::object, which is a borrowed view).
    void Note(std::string_view object);

    /// For a step whose honest unit is bytes rather than items.
    void Bytes(u64 done, u64 total);

    /// Stage, object and position in one write, for adapting a source that
    /// reports all three together — the shape casc::ProgressInfo arrives in.
    /// Pushes immediately when the stage or object changed (those are the
    /// transitions a dropped sample would leave the wrong label for) and
    /// throttles when only the counters moved.
    void Report(std::string_view stage, std::string_view object, f64 done, f64 total);

    /// Reserve @p budget units of MY total for a child.
    [[nodiscard]] ProgressMonitor Split(u64 budget);

    /// Askable without reporting anything — which is the point, for a loop
    /// over three quarters of a million manifest entries.
    bool Cancelled() const noexcept {
        return state_ && state_->Cancelled();
    }

    /// True when this monitor reports nowhere.
    bool Inert() const noexcept {
        return state_ == nullptr;
    }

private:
    void Push(bool force);

    ProgressState* state_ = nullptr;
    f64 lo_ = 0.0, hi_ = 1.0; // my window of the whole operation
    f64 done_ = 0.0;          // my position, in my own units
    f64 total_ = 0.0;         // 0 = indeterminate
    u64 bytesDone_ = 0, bytesTotal_ = 0;
    std::string stage_;
    std::string object_;
    // The parent's stage, put back when this child is destroyed, so a bar does
    // not keep the label of a sub-step that has finished.
    std::string restoreStage_;
    bool hasRestore_ = false;
};

} // namespace whiteout::flakes::io

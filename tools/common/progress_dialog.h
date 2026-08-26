#pragma once

// ============================================================================
// The window that appears instead of the freeze.
//
// Two presentations over one LoadTaskRunner, and which one a host draws is a
// statement about who asked for the work:
//
//   DrawProgressModal   the user asked — a Settings commit, a game switch, a
//                       document that needs another install. The app genuinely
//                       cannot proceed, so say so, and offer Cancel.
//   DrawProgressStatus  nobody asked — the WoW client-database prewarm. A
//                       modal for a skin table would be worse than the model
//                       appearing in its default look and restyling when the
//                       tables land.
//
// Both are polls: they read a snapshot the task thread wrote and never touch
// the storage lock, which is what lets them draw *during* the open they are
// reporting on.
//
// Draw from the normal ImGui pass. Never spin a nested frame loop around this
// waiting for the task to finish — a task body blocked in
// IContentProvider::ReadFile needs the host's Pump to keep running, and a
// nested loop that stops the frame is how that deadlocks.
// ============================================================================

namespace whiteout::flakes::io {
class LoadTaskRunner;
}

namespace whiteout::flakes::tools {

/// Modal for the running task. Opens itself while the runner is busy and
/// closes when it is not; safe to call every frame regardless.
void DrawProgressModal(io::LoadTaskRunner& runner);

/// One line, no modal — for a status bar. Draws nothing when idle.
void DrawProgressStatus(io::LoadTaskRunner& runner);

} // namespace whiteout::flakes::tools

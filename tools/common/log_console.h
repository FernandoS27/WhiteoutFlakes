#pragma once

// ============================================================================
// LogConsole — in-app development log.
//
// The viewer ships as a GUI-subsystem app in Release (no console window), so
// end users never see the flood of stdout/stderr the engine emits. This class
// captures that stream at the OS file-descriptor level — so every fprintf /
// printf / std::cout / std::cerr across the engine and tools is caught without
// touching a single call site — into a ring buffer, and renders it in an ImGui
// window on demand (View ▸ Debug ▸ Log Console).
//
// Cross-platform: uses _pipe/_dup2 on Windows, pipe/dup2 elsewhere. On a
// console build (Debug on Windows, any terminal launch on macOS/Linux) the
// captured output is also echoed back to the original terminal, so developers
// keep their normal console too.
// ============================================================================

#include <atomic>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#include <imgui.h>

namespace whiteout::flakes::tools {

class LogConsole {
public:
    static LogConsole& Instance();

    // Redirect stdout + stderr into the capture pipe and spin up the drain
    // thread. Call once, as early in main() as possible so startup logs are
    // caught. Idempotent; silently no-ops if the pipe can't be created.
    void Begin();

    // Restore the original stdout/stderr and join the drain thread. Called by
    // the destructor, so an explicit call at shutdown is optional.
    void End();

    // ImGui window. When `*open` is false the body is skipped; the window's own
    // close button clears `*open`. Safe to call every frame.
    void DrawUi(bool* open);

    ~LogConsole();

private:
    LogConsole() = default;
    LogConsole(const LogConsole&) = delete;
    LogConsole& operator=(const LogConsole&) = delete;

    enum class Severity { Info, Warn, Error };
    struct Line {
        std::string text;
        Severity sev;
    };

    void DrainLoop();
    void PushLine(std::string text);
    // Redirects one C stream's fd onto `pipeWrite`, saving the original fd in
    // `saved` (or -1 when the stream had no backing fd — a GUI app with no
    // console) and recording the overwritten fd in `streamFd` for restore.
    void RedirectStream(std::FILE* stream, int pipeWrite, int& saved, int& streamFd);

    bool started_ = false;
    int pipeRead_ = -1;
    int savedOut_ = -1, savedErr_ = -1; // originals, for restore + terminal echo
    int outFd_ = -1, errFd_ = -1;       // the stdout/stderr fds we overwrote
    std::thread drainThread_;

    std::mutex mutex_;
    std::deque<Line> lines_;
    static constexpr std::size_t kMaxLines = 8000;

    ImGuiTextFilter filter_;
    bool autoScroll_ = true;
};

} // namespace whiteout::flakes::tools

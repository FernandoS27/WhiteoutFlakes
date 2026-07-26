#include "log_console.h"

#include <algorithm>
#include <cctype>
#include <utility>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#define WF_DUP _dup
#define WF_DUP2 _dup2
#define WF_FILENO _fileno
#define WF_READ _read
#define WF_WRITE _write
#define WF_CLOSE _close
#else
#include <unistd.h>
#define WF_DUP dup
#define WF_DUP2 dup2
#define WF_FILENO fileno
#define WF_READ read
#define WF_WRITE write
#define WF_CLOSE close
#endif

namespace whiteout::flakes::tools {

namespace {

int MakePipe(int fds[2]) {
#if defined(_WIN32)
    return _pipe(fds, 1 << 16, _O_BINARY);
#else
    return pipe(fds);
#endif
}

// Case-insensitive substring test — used for the cheap severity guess in
// PushLine (WC3/engine logs carry no structured level).
bool ContainsCI(const std::string& hay, const char* needle) {
    const std::size_t n = std::char_traits<char>::length(needle);
    if (n == 0 || hay.size() < n) return false;
    for (std::size_t i = 0; i + n <= hay.size(); ++i) {
        std::size_t j = 0;
        for (; j < n; ++j) {
            const char a = static_cast<char>(std::tolower(static_cast<unsigned char>(hay[i + j])));
            const char b = static_cast<char>(std::tolower(static_cast<unsigned char>(needle[j])));
            if (a != b) break;
        }
        if (j == n) return true;
    }
    return false;
}

} // namespace

LogConsole& LogConsole::Instance() {
    static LogConsole inst;
    return inst;
}

LogConsole::~LogConsole() { End(); }

void LogConsole::RedirectStream(std::FILE* stream, int pipeWrite, int& saved, int& streamFd) {
    int fd = WF_FILENO(stream);
#if defined(_WIN32)
    if (fd < 0) {
        // GUI-subsystem app with no console: the stream has no backing fd. Give
        // it one on the null device so we have something to dup2 the pipe onto.
        std::FILE* reopened = nullptr;
        freopen_s(&reopened, "NUL", "w", stream);
        fd = WF_FILENO(stream);
        saved = -1; // no real terminal to echo back to
    } else {
        saved = WF_DUP(fd); // remember the console/terminal for echo + restore
    }
#else
    saved = WF_DUP(fd);
#endif
    if (fd < 0) return; // still no fd — give up on this stream
    streamFd = fd;
    WF_DUP2(pipeWrite, fd);
    std::setvbuf(stream, nullptr, _IONBF, 0); // unbuffered so logs appear promptly
}

void LogConsole::Begin() {
    if (started_) return;

    int fds[2];
    if (MakePipe(fds) != 0) return; // capture unavailable; logs stay on stdout
    pipeRead_ = fds[0];
    const int pipeWrite = fds[1];

    RedirectStream(stdout, pipeWrite, savedOut_, outFd_);
    RedirectStream(stderr, pipeWrite, savedErr_, errFd_);

    // The write end is now duplicated onto both stream fds; drop our spare copy
    // so the pipe has exactly those two writers (needed for a clean EOF later).
    WF_CLOSE(pipeWrite);

    started_ = true;
    drainThread_ = std::thread([this] { DrainLoop(); });
}

void LogConsole::End() {
    if (!started_) return;

    // Break the pipe's write side so DrainLoop hits EOF and returns: restore the
    // saved originals where we had them, else just close the fd we overwrote.
    if (outFd_ >= 0) {
        if (savedOut_ >= 0)
            WF_DUP2(savedOut_, outFd_);
        else
            WF_CLOSE(outFd_);
    }
    if (errFd_ >= 0) {
        if (savedErr_ >= 0)
            WF_DUP2(savedErr_, errFd_);
        else
            WF_CLOSE(errFd_);
    }

    if (drainThread_.joinable()) drainThread_.join();

    if (pipeRead_ >= 0) WF_CLOSE(pipeRead_);
    if (savedOut_ >= 0) WF_CLOSE(savedOut_);
    if (savedErr_ >= 0) WF_CLOSE(savedErr_);

    pipeRead_ = savedOut_ = savedErr_ = outFd_ = errFd_ = -1;
    started_ = false;
}

void LogConsole::DrainLoop() {
    char buf[4096];
    std::string pending;
    // stdout and stderr are merged into this one pipe; echo everything to the
    // original stdout when there was a terminal (dev/console builds).
    const int echoFd = savedOut_ >= 0 ? savedOut_ : savedErr_;
    for (;;) {
        const int n = WF_READ(pipeRead_, buf, sizeof(buf));
        if (n <= 0) break; // EOF from End(), or read error
        if (echoFd >= 0) WF_WRITE(echoFd, buf, static_cast<unsigned>(n));

        pending.append(buf, static_cast<std::size_t>(n));
        std::size_t nl;
        while ((nl = pending.find('\n')) != std::string::npos) {
            std::string line = pending.substr(0, nl);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            PushLine(std::move(line));
            pending.erase(0, nl + 1);
        }
    }
    if (!pending.empty()) PushLine(std::move(pending));
}

void LogConsole::PushLine(std::string text) {
    Severity sev = Severity::Info;
    if (ContainsCI(text, "error") || ContainsCI(text, "fail") || ContainsCI(text, "fatal"))
        sev = Severity::Error;
    else if (ContainsCI(text, "warn"))
        sev = Severity::Warn;

    std::lock_guard<std::mutex> lk(mutex_);
    lines_.push_back({std::move(text), sev});
    if (lines_.size() > kMaxLines) lines_.pop_front();
}

void LogConsole::DrawUi(bool* open) {
    if (!open || !*open) return;

    ImGui::SetNextWindowSize(ImVec2(760, 340), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Log Console", open)) {
        ImGui::End();
        return;
    }

    if (ImGui::Button("Clear")) {
        std::lock_guard<std::mutex> lk(mutex_);
        lines_.clear();
    }
    ImGui::SameLine();
    const bool copy = ImGui::Button("Copy");
    ImGui::SameLine();
    ImGui::Checkbox("Auto-scroll", &autoScroll_);
    ImGui::SameLine();
    filter_.Draw("Filter", 180.0f);

    ImGui::Separator();
    ImGui::BeginChild("log_scroll", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);

    const ImVec4 warnCol(0.95f, 0.77f, 0.24f, 1.0f);
    const ImVec4 errCol(0.94f, 0.35f, 0.32f, 1.0f);

    if (copy) ImGui::LogToClipboard();
    {
        // Held across the draw so the drain thread can't pop_front mid-iteration.
        // The thread only appends, so contention is negligible.
        std::lock_guard<std::mutex> lk(mutex_);
        for (const Line& ln : lines_) {
            if (!filter_.PassFilter(ln.text.c_str())) continue;
            const bool tinted = ln.sev != Severity::Info;
            if (tinted)
                ImGui::PushStyleColor(ImGuiCol_Text, ln.sev == Severity::Error ? errCol : warnCol);
            ImGui::TextUnformatted(ln.text.c_str());
            if (tinted) ImGui::PopStyleColor();
        }
    }
    if (copy) ImGui::LogFinish();

    // Stick to the bottom only while the view is already there, so a user who
    // scrolls up to read isn't yanked back down by new lines.
    if (autoScroll_ && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f)
        ImGui::SetScrollHereY(1.0f);

    ImGui::EndChild();
    ImGui::End();
}

} // namespace whiteout::flakes::tools

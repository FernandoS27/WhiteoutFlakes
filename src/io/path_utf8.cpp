#include "whiteout/flakes/util/path_utf8.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <climits>
#include <unistd.h>
#elif defined(__APPLE__)
#include <climits>
#include <mach-o/dyld.h>
#endif

#include <iterator>

namespace whiteout::flakes::io {

std::filesystem::path FsPathFromUtf8(std::string_view utf8) {
#ifdef _WIN32
    if (utf8.empty())
        return {};
    const i32 wlen =
        ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<i32>(utf8.size()), nullptr, 0);
    if (wlen <= 0)
        return std::filesystem::path(utf8);
    std::wstring wide(static_cast<usize>(wlen), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<i32>(utf8.size()), wide.data(),
                          wlen);
    return std::filesystem::path(std::move(wide));
#else
    return std::filesystem::path(utf8);
#endif
}

std::filesystem::path ExecutableDirectory() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH * 4] = {};
    DWORD len = ::GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    if (len == 0 || len >= std::size(buf))
        return {};
    return std::filesystem::path(std::wstring(buf, buf + len)).parent_path();
#elif defined(__linux__)
    char buf[PATH_MAX] = {};
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0)
        return {};
    return std::filesystem::path(std::string(buf, static_cast<usize>(n))).parent_path();
#elif defined(__APPLE__)
    // _NSGetExecutablePath writes the path used to launch the process;
    // canonicalise via std::filesystem to resolve symlinks. When the
    // executable lives inside a .app bundle (`.../X.app/Contents/MacOS/X`)
    // the asset search root is Contents/Resources/ — that's where macOS
    // wants read-only ship-with-the-binary data (and where codesign won't
    // choke on our non-Mach-O `.bls` files). Detect that case by checking
    // for the `Contents/MacOS` suffix on the exe's parent.
    char buf[PATH_MAX] = {};
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) != 0)
        return {};
    std::error_code ec;
    std::filesystem::path resolved = std::filesystem::canonical(std::filesystem::path(buf), ec);
    if (ec)
        resolved = std::filesystem::path(buf);
    std::filesystem::path dir = resolved.parent_path();
    if (dir.filename() == "MacOS" && dir.parent_path().filename() == "Contents")
        return dir.parent_path() / "Resources";
    return dir;
#else
    return {};
#endif
}

} // namespace whiteout::flakes::io

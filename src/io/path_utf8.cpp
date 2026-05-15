#include "whiteout/flakes/util/path_utf8.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

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

} // namespace whiteout::flakes::io

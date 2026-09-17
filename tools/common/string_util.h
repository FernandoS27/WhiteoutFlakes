#pragma once

// ============================================================================
// ASCII case folding and trimming for host code.
//
// ASCII on purpose: what these compare is file extensions, flag values and
// sequence, item and path names, and a locale-aware fold would read UTF-8
// continuation bytes as Latin-1 letters. The process never calls setlocale, so
// this is also exactly what the `std::tolower` loops it replaces did.
// ============================================================================

#include <string>
#include <string_view>

namespace whiteout::flakes::tools {

constexpr char ToLowerAscii(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
}

inline std::string ToLowerAscii(std::string_view s) {
    std::string out(s);
    for (char& c : out)
        c = ToLowerAscii(c);
    return out;
}

constexpr bool EqualsIgnoreCase(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (ToLowerAscii(a[i]) != ToLowerAscii(b[i]))
            return false;
    return true;
}

/// An empty needle is found everywhere, which is what a search box wants.
constexpr bool ContainsIgnoreCase(std::string_view hay, std::string_view needle) noexcept {
    if (needle.size() > hay.size())
        return false;
    for (std::size_t i = 0; i + needle.size() <= hay.size(); ++i)
        if (EqualsIgnoreCase(hay.substr(i, needle.size()), needle))
            return true;
    return false;
}

/// Spaces, tabs and line breaks off both ends.
constexpr std::string_view TrimWhitespace(std::string_view s) noexcept {
    const auto isWs = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    std::size_t a = 0;
    std::size_t b = s.size();
    while (a < b && isWs(s[a]))
        ++a;
    while (b > a && isWs(s[b - 1]))
        --b;
    return s.substr(a, b - a);
}

} // namespace whiteout::flakes::tools

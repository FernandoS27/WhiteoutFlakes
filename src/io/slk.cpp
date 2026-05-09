#include "slk.h"
#include "whiteout/flakes/types.h"

#include <cctype>
#include <string_view>

namespace whiteout::flakes::io {

namespace {

struct Lexer {
    std::string_view line;
    usize            pos = 0;

    bool empty() const { return pos >= line.size(); }

    std::string_view next() {
        if (pos >= line.size()) return {};
        const usize start = pos;
        while (pos < line.size() && line[pos] != ';') ++pos;
        std::string_view field = line.substr(start, pos - start);
        if (pos < line.size()) ++pos;
        return field;
    }
};

bool ParseInt(std::string_view s, i32& out) {
    if (s.empty()) return false;
    i32 sign = 1;
    usize i = 0;
    if (s[0] == '-') { sign = -1; i = 1; }
    i32 v = 0;
    bool any = false;
    for (; i < s.size(); ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
        v = v * 10 + (s[i] - '0');
        any = true;
    }
    if (!any) return false;
    out = sign * v;
    return true;
}

std::string DecodeCellValue(std::string_view field) {
    if (field.empty() || field[0] != 'K') return std::string(field);
    std::string_view body = field.substr(1);
    if (body.size() >= 2 && body.front() == '"' && body.back() == '"') {
        std::string out;
        out.reserve(body.size() - 2);
        for (usize i = 1; i + 1 < body.size(); ++i) {
            if (body[i] == '"' && i + 1 < body.size() - 1 && body[i + 1] == '"') {
                out += '"';
                ++i;
            } else {
                out += body[i];
            }
        }
        return out;
    }
    return std::string(body);
}

bool IEqual(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (usize i = 0; i < a.size(); ++i) {
        if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i]))
            return false;
    }
    return true;
}

void EnsureCell(std::vector<std::vector<std::string>>& rows, i32 rowIdx, i32 colIdx) {
    if (rowIdx < 0 || colIdx < 0) return;
    if ((usize)rowIdx >= rows.size()) rows.resize(rowIdx + 1);
    auto& r = rows[rowIdx];
    if ((usize)colIdx >= r.size()) r.resize(colIdx + 1);
}

}

i32 SlkTable::FindColumn(std::string_view name) const {
    if (rows.empty()) return -1;
    const auto& header = rows[0];
    for (i32 i = 0; i < (i32)header.size(); ++i) {
        if (IEqual(header[i], name)) return i;
    }
    return -1;
}

std::string_view SlkTable::Cell(usize row, i32 col) const {
    if (col < 0 || row >= rows.size()) return {};
    const auto& r = rows[row];
    if ((usize)col >= r.size()) return {};
    return r[col];
}

SlkTable ParseSlk(std::span<const char> bytes) {
    SlkTable table;
    i32 curRow = 1;
    i32 curCol = 1;

    usize i = 0;
    while (i < bytes.size()) {
        usize lineStart = i;
        while (i < bytes.size() && bytes[i] != '\n' && bytes[i] != '\r') ++i;
        std::string_view line(&bytes[lineStart], i - lineStart);

        while (i < bytes.size() && (bytes[i] == '\n' || bytes[i] == '\r')) ++i;
        if (line.empty()) continue;
        const char rec = line.front();
        if (rec == 'E') break;
        if (rec != 'C') continue;

        Lexer lx{line};
        lx.next();
        std::string value;
        bool sawValue = false;
        i32 rowOverride = curRow;
        i32 colOverride = curCol;
        while (!lx.empty()) {
            std::string_view field = lx.next();
            if (field.empty()) continue;
            const char tag = field.front();
            std::string_view body = field.substr(1);
            switch (tag) {
                case 'Y': {
                    i32 y; if (ParseInt(body, y)) rowOverride = y;
                    break;
                }
                case 'X': {
                    i32 x; if (ParseInt(body, x)) colOverride = x;
                    break;
                }
                case 'K': {
                    value = DecodeCellValue(field);
                    sawValue = true;
                    break;
                }
                default:
                    break;
            }
        }
        curRow = rowOverride;
        curCol = colOverride;
        if (sawValue) {

            EnsureCell(table.rows, curRow - 1, curCol - 1);
            table.rows[curRow - 1][curCol - 1] = std::move(value);
        }
    }
    return table;
}

SlkTable ParseSlk(std::span<const u8> bytes) {
    return ParseSlk(std::span<const char>(reinterpret_cast<const char*>(bytes.data()),
                                           bytes.size()));
}

}

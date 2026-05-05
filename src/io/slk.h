#pragma once

#include "common_types.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace WhiteoutDex::io {

class SlkTable {
public:

    std::vector<std::vector<std::string>> rows;

    i32 FindColumn(std::string_view name) const;

    std::string_view Cell(usize row, i32 col) const;

    usize RowCount() const { return rows.size(); }
    usize HeaderCount() const { return rows.empty() ? 0 : rows[0].size(); }
};

SlkTable ParseSlk(std::span<const char> bytes);
SlkTable ParseSlk(std::span<const u8> bytes);

}

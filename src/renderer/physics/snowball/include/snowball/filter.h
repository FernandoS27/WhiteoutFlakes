//===----------------------------------------------------------------------===//
// snowball/filter.h -- collision filtering.
//
// Box2D's category/mask/group semantics. The widths are contractual: the group index is a
// full 32 bits, while the category and include masks really are 16 -- debug output that
// formats them as plain ints makes them look wider than they are, but storage and matching
// are 16-bit.
//===----------------------------------------------------------------------===//
#pragma once

#include "snowball/common_types.h"

namespace snowball {

struct Filter {
    u16 categoryBits{1};
    u16 includeMask{0xFFFF};
    i32 groupIndex{0};
};

/// @brief Do these two filters permit a contact?
///
/// A matching non-zero group decides outright, by its sign; otherwise both masks must accept
/// the other's category.
constexpr bool ShouldCollide(const Filter& a, const Filter& b) {
    if (a.groupIndex == b.groupIndex && a.groupIndex != 0) {
        return a.groupIndex > 0;
    }
    return (b.categoryBits & a.includeMask) != 0 && (b.includeMask & a.categoryBits) != 0;
}

}  // namespace snowball

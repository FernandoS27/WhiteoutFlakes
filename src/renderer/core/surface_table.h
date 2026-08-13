#pragma once

// ============================================================================
// ISurfaceTable — the static half of a model's material data, owned by the
// product that understands it.
//
// The split this enforces: a surface table holds what is *fixed* at load time.
// Per-frame animated values — layer alpha, swapped texture ids, fresnel — do
// not live here; they arrive through FrameState and flow via RenderableView, so
// two actors sharing one template can animate independently.
//
// The core only ever sees this interface. A shading model recovers its own
// concrete table by asserting `Product()` and downcasting, which is what makes
// a table built by the wrong product a loud failure rather than a
// reinterpret_cast into unrelated memory.
// ============================================================================

#include "whiteout/flakes/types.h"

#include <cassert>

namespace whiteout::flakes::renderer::core {

// Which game's data a table holds. Detected from the storage the content came
// from; MPQ-backed classic WC3 has no product record at all and reports Wc3
// unconditionally. `Neutral` is for tables that carry no product data — the
// unlit debug table, and anything that only needs a surface count.
enum class ProductId : u8 {
    Neutral = 0,
    Wc3 = 1,
    Wow = 2,
    Sc2 = 3,
};

class ISurfaceTable {
public:
    virtual ~ISurfaceTable() = default;

    /// @brief Which product built this table. The downcast contract: a model
    ///        must check this before casting to its own concrete type.
    virtual ProductId Product() const = 0;

    /// @brief How many surfaces the table holds. `SurfaceKey::surface` indexes
    ///        into it, so this is what bounds-checks that index.
    virtual usize Count() const = 0;
};

/// @brief Assert-then-downcast. Returns null for a null table or a product
///        mismatch, so callers that legitimately run before a table is built
///        (an actor mid-load) get a null rather than a trap — but a genuine
///        product mismatch trips the assert in a debug build first.
template <class Table>
const Table* SurfaceTableCast(const ISurfaceTable* table) {
    if (!table)
        return nullptr;
    assert(table->Product() == Table::kProduct && "surface table crossed a product boundary");
    if (table->Product() != Table::kProduct)
        return nullptr;
    return static_cast<const Table*>(table);
}

template <class Table>
Table* SurfaceTableCast(ISurfaceTable* table) {
    if (!table)
        return nullptr;
    assert(table->Product() == Table::kProduct && "surface table crossed a product boundary");
    if (table->Product() != Table::kProduct)
        return nullptr;
    return static_cast<Table*>(table);
}

} // namespace whiteout::flakes::renderer::core

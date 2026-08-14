#pragma once

// ============================================================================
// VertexLayoutCache — interning for MeshBuffer vertex layouts.
//
// A baked vertex buffer describes itself (MeshBuffer::attributes), which makes
// the input layout runtime data rather than a compile-time constant. Two things
// follow, and this class is both of them:
//
//   1. A PSO has to key on the layout. Keying on a vector of attributes would
//      mean a deep compare per draw; interning turns it into a u32.
//   2. `gfx::InputElement` holds a `const char*` semantic and the PSO desc
//      holds a *span* of elements, so the array has to outlive the staging
//      data it was derived from. Entries are heap-stable for that reason.
//
// Layout id 0 is reserved for WC3's built-in interleaved `Vertex` and is never
// returned by Intern — a WC3 geoset has no MeshBuffer and its layout is
// hardcoded in the shading models that bind it.
// ============================================================================

#include "gfx/gfx_pipeline_types.h"
#include "whiteout/flakes/model_types.h"

#include <memory>
#include <span>
#include <vector>

namespace whiteout::flakes::renderer::core {

using ::whiteout::flakes::renderer::model::VertexAttribute;
using ::whiteout::flakes::renderer::model::VertexSemantic;

/// @brief The shader semantic name one @ref VertexSemantic binds to. Static
///        storage, because `gfx::InputElement` keeps the pointer.
inline const char* SemanticName(VertexSemantic s) {
    switch (s) {
    case VertexSemantic::Position:
        return "POSITION";
    case VertexSemantic::Normal:
        return "NORMAL";
    case VertexSemantic::Tangent:
        return "TANGENT";
    case VertexSemantic::TexCoord:
        return "TEXCOORD";
    case VertexSemantic::Color:
        return "COLOR";
    case VertexSemantic::BoneIndices:
        return "BLENDINDICES";
    case VertexSemantic::BoneWeights:
        return "BLENDWEIGHT";
    }
    return "POSITION";
}

/// @brief One entry of a shader's declared vertex inputs. The index matters
///        whenever a shader reads two of the same semantic.
struct SemanticRef {
    VertexSemantic semantic = VertexSemantic::Position;
    u8 semanticIndex = 0;
};

class VertexLayoutCache {
public:
    /// @brief WC3's interleaved 48-byte `Vertex`. Not an interned entry —
    ///        the reserved "this geoset carries no MeshBuffer" value.
    static constexpr u32 kWc3Interleaved = 0;

    /// @brief Stable id for @p attrs, reusing an existing entry when the
    ///        attribute list matches exactly. Never returns 0.
    u32 Intern(std::span<const VertexAttribute> attrs) {
        for (u32 i = 0; i < entries_.size(); ++i) {
            if (Same(entries_[i]->attrs, attrs))
                return i + 1;
        }
        auto e = std::make_unique<Entry>();
        e->attrs.assign(attrs.begin(), attrs.end());
        entries_.push_back(std::move(e));
        return static_cast<u32>(entries_.size());
    }

    std::span<const VertexAttribute> Attributes(u32 layoutId) const {
        const Entry* e = Get(layoutId);
        return e ? std::span<const VertexAttribute>(e->attrs) : std::span<const VertexAttribute>{};
    }

    bool Has(u32 layoutId, VertexSemantic s, u8 semanticIndex = 0) const {
        return Find(layoutId, s, semanticIndex) != nullptr;
    }

    /// @brief The elements a PSO should declare: the intersection of what the
    ///        buffer carries and what @p want asks for, **in `want`'s order**.
    ///
    /// Order is load-bearing, not cosmetic. Vulkan, WebGPU and Metal assign a
    /// non-`ATTR` semantic its shader location from the element's position in
    /// this array, so the array has to match the VS input struct's field order
    /// or the shader reads the wrong attribute. Passing `want` as the shader's
    /// declaration order makes that automatic.
    ///
    /// Returns fewer elements than asked for when the buffer lacks one; the
    /// caller checks with @ref Has first and picks a permutation that fits.
    std::vector<gfx::InputElement> Subset(u32 layoutId,
                                          std::span<const VertexSemantic> want) const {
        std::vector<SemanticRef> refs;
        refs.reserve(want.size());
        for (VertexSemantic s : want)
            refs.push_back({s, 0});
        return Subset(layoutId, std::span<const SemanticRef>(refs));
    }

    /// @overload Indexed form, for a shader that reads more than one of a
    ///           semantic — M2 samples two UV sets, so asking for TexCoord
    ///           twice has to mean TEXCOORD0 then TEXCOORD1 rather than
    ///           TEXCOORD0 twice (which is a duplicate element, and a location
    ///           collision on the backends that derive locations positionally).
    std::vector<gfx::InputElement> Subset(u32 layoutId,
                                          std::span<const SemanticRef> want) const {
        std::vector<gfx::InputElement> out;
        out.reserve(want.size());
        for (const SemanticRef& s : want) {
            const VertexAttribute* a = Find(layoutId, s.semantic, s.semanticIndex);
            if (!a)
                continue;
            out.push_back(gfx::InputElement{
                .semantic = SemanticName(a->semantic),
                .semanticIndex = a->semanticIndex,
                .format = a->format,
                .offset = a->offset,
                .inputSlot = 0,
            });
        }
        return out;
    }

private:
    struct Entry {
        std::vector<VertexAttribute> attrs;
    };

    const Entry* Get(u32 layoutId) const {
        if (layoutId == kWc3Interleaved || layoutId > entries_.size())
            return nullptr;
        return entries_[layoutId - 1].get();
    }

    const VertexAttribute* Find(u32 layoutId, VertexSemantic s, u8 semanticIndex) const {
        const Entry* e = Get(layoutId);
        if (!e)
            return nullptr;
        for (const auto& a : e->attrs)
            if (a.semantic == s && a.semanticIndex == semanticIndex)
                return &a;
        return nullptr;
    }

    static bool Same(const std::vector<VertexAttribute>& a,
                     std::span<const VertexAttribute> b) {
        if (a.size() != b.size())
            return false;
        for (usize i = 0; i < a.size(); ++i) {
            if (a[i].semantic != b[i].semantic || a[i].semanticIndex != b[i].semanticIndex ||
                a[i].format != b[i].format || a[i].offset != b[i].offset)
                return false;
        }
        return true;
    }

    // Heap-stable so a span handed out earlier survives a later Intern.
    std::vector<std::unique_ptr<Entry>> entries_;
};

} // namespace whiteout::flakes::renderer::core

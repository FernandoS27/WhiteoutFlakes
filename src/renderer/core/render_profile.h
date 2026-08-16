#pragma once

// ============================================================================
// IRenderProfile — what a product's frame looks like: which passes run, in what
// order, into what targets, in what colour space, at what world scale.
//
// Deliberately NOT a frame graph. Four profiles, each with a fixed pass order
// and a target set known at construction, do not need scheduling, aliasing or
// automatic barriers — the gfx layer already auto-transitions render targets
// when they are bound as SRVs. What is here is an ordered list plus a startup
// check that every pass's inputs are produced upstream. Wc3SdProfile costs one
// colour target, one depth target and three passes; the simple case staying
// simple is the acceptance test for this abstraction.
// ============================================================================

#include "core/ribbon_dialect.h"
#include "core/surface_vocabulary.h"
#include "gfx/gfx.h"
#include "whiteout/flakes/util/coordinate_system.h"
#include "whiteout/flakes/types.h"

#include <functional>
#include <span>
#include <string>
#include <vector>

namespace whiteout::flakes::renderer::shading {
class IShadingModel;
}

namespace whiteout::flakes::renderer::core {

// Logical render targets a pass can read or write. Logical, not physical: the
// profile maps each to a format and a resolution scale, and two profiles can
// disagree about what SceneColor is (UNORM for SD, R11G11B10F for HD) without
// the pass list caring.
enum class TargetSlot : u8 {
    SceneColor = 0,
    Depth = 1,
    LinearDepth = 2,
    Normal = 3,
    ShadowMap = 4,
    AmbientOcclusion = 5,
    SceneColorCopy = 6,
    Bloom = 7,
    Backbuffer = 8,

    Count,
};

using TargetMask = u32;

inline constexpr TargetMask TargetBit(TargetSlot s) {
    return 1u << static_cast<u32>(s);
}
inline constexpr TargetMask TargetBits(std::initializer_list<TargetSlot> slots) {
    TargetMask m = 0;
    for (TargetSlot s : slots)
        m |= TargetBit(s);
    return m;
}

struct TargetDesc {
    TargetSlot slot = TargetSlot::SceneColor;
    gfx::Format format = gfx::Format::R8G8B8A8_UNORM;
    f32 scale = 1.0f; // fraction of the viewport's size
};

// One entry in a profile's chain.
//
// The condition is why this is not a bare PassSlot list. SC2's deferred
// local-light stage is an optional additive pass gated on a graphics flag, and
// SC2 itself does not allocate its Diffuse/Specular targets when the flag is
// off. WC3's profiles return true unconditionally — but designing the predicate
// in now costs a field, whereas retrofitting it is a rework of every profile.
// Two things a walk over this list must not assume, both learned by trying it
// against WC3's real frame:
//
//  1. A slot is not a render pass. `OpaqueColor`/`GBuffer` and
//     `TransparentScene` are two submissions inside ONE
//     BeginRenderPass/EndRenderPass block, with the grid, splats, debug
//     overlays and SD's ImGui interleaved between them. Dispatching per slot
//     with a pass boundary each would change both pixels and cost.
//
//  2. `condition` gates the pass, NOT the per-frame service update. The DoF and
//     bloom services are poked every frame with `enabled` as a *parameter* and
//     no-op internally; skipping the call because the condition is false would
//     also skip the param push and leave the service on stale state. Hoist the
//     param updates out of the gated body before driving execution from here.
struct PassEntry {
    PassSlot slot = PassSlot::OpaqueColor;
    // Evaluated once per frame. Answers "does this pass contribute", which is
    // what validation reasons about — see the caveats above before using it as
    // the execution gate.
    std::function<bool()> condition;
    // Required inputs: the pass is wrong without them.
    TargetMask reads = 0;
    // Sampled-if-present inputs. Not a weaker `reads` but a different
    // relationship, and WC3 has it: the scene pass samples the shadow map, yet
    // with shadows off it compiles a permutation that does not, so the cascade
    // never existing is correct rather than broken. Declaring these as required
    // makes the validator reject the real frame; folding them into `reads`
    // silently makes it useless.
    TargetMask optionalReads = 0;
    TargetMask writes = 0;

    bool Enabled() const {
        return !condition || condition();
    }
    bool Unconditional() const {
        return !condition;
    }
};

class IRenderProfile {
public:
    virtual ~IRenderProfile() = default;

    virtual const char* Name() const = 0;

    /// @brief The ordered chain. Index order is execution order.
    virtual std::span<const PassEntry> Passes() const = 0;

    /// @brief Every target the chain needs, with its format and scale.
    virtual std::span<const TargetDesc> TargetSet() const = 0;

    /// @brief UNORM for a gamma profile, R11G11B10F for a linear one.
    virtual gfx::Format SceneColorFormat() const = 0;

    /// @brief True when shading happens in linear space and the chain ends in
    ///        a tonemap. SD is gamma unless SceneHdrInSd opts it in.
    virtual bool LinearShading() const = 0;

    /// @brief Game units → renderer units. Identity for WC3; a WoW creature is
    ///        2–5 yards against WC3 camera constants sized in the hundreds, so
    ///        this is what keeps an M2 from rendering as a sub-pixel dot.
    virtual f32 WorldScale() const = 0;

    /// @brief The coordinate space the product's source data is authored in.
    ///        Identity for WC3.
    virtual CoordSpace SourceSpace() const = 0;

    /// @brief How UnlitShading lights this product's surfaces. Flat by
    ///        default, so the WC3 profiles are unchanged and their goldens
    ///        stay byte-identical by construction rather than by inspection.
    virtual UnlitLightingModel UnlitLighting() const {
        return UnlitLightingModel::Flat;
    }

    /// @brief Which variant of Blizzard's CRibbonEmitter this product's ribbon
    ///        trails run. See core/ribbon_dialect.h: the two runtimes share one
    ///        simulation and differ in a handful of measured details.
    ///
    /// Warcraft III by default, so the MDX path is unchanged by construction
    /// rather than by inspection — the same argument UnlitLighting makes.
    virtual RibbonBehavior Ribbons() const {
        return RibbonBehavior::Wc3();
    }

    /// @brief The shading models this profile can dispatch to. Binds a profile
    ///        to the registry: a build with a product disabled never lists its
    ///        models here, so nothing can name one.
    virtual std::span<shading::IShadingModel* const> ShadingModels() const = 0;
};

// ---------------------------------------------------------------------------
// Startup validation
// ---------------------------------------------------------------------------

struct ProfileValidation {
    bool ok = true;
    std::string error;
};

/// @brief Check that every pass reads only what an earlier pass wrote, and
///        that no unconditional pass depends on a conditional one's output.
///
/// The second rule is the one worth having: a conditional pass that is the
/// only producer of a target an unconditional consumer reads is a frame that
/// works until someone toggles the flag. Device-free, so it is a G0 test.
ProfileValidation ValidateProfile(const IRenderProfile& profile);

} // namespace whiteout::flakes::renderer::core

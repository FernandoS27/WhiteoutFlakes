#include "core/render_profile.h"

#include <sstream>

namespace whiteout::flakes::renderer::core {

namespace {

const char* SlotName(PassSlot s) {
    switch (s) {
    case PassSlot::ShadowMap: return "ShadowMap";
    case PassSlot::DepthPrepass: return "DepthPrepass";
    case PassSlot::OpaqueColor: return "OpaqueColor";
    case PassSlot::GBuffer: return "GBuffer";
    case PassSlot::Gtao: return "Gtao";
    case PassSlot::SceneColorCopy: return "SceneColorCopy";
    case PassSlot::TransparentScene: return "TransparentScene";
    case PassSlot::Tonemap: return "Tonemap";
    case PassSlot::Bloom: return "Bloom";
    case PassSlot::Dof: return "Dof";
    case PassSlot::Fxaa: return "Fxaa";
    case PassSlot::Debug: return "Debug";
    case PassSlot::ImGui: return "ImGui";
    default: return "?";
    }
}

const char* TargetName(TargetSlot s) {
    switch (s) {
    case TargetSlot::SceneColor: return "SceneColor";
    case TargetSlot::Depth: return "Depth";
    case TargetSlot::LinearDepth: return "LinearDepth";
    case TargetSlot::Normal: return "Normal";
    case TargetSlot::ShadowMap: return "ShadowMap";
    case TargetSlot::AmbientOcclusion: return "AmbientOcclusion";
    case TargetSlot::SceneColorCopy: return "SceneColorCopy";
    case TargetSlot::Bloom: return "Bloom";
    case TargetSlot::Backbuffer: return "Backbuffer";
    default: return "?";
    }
}

TargetSlot FirstSetBit(TargetMask m) {
    for (u32 i = 0; i < static_cast<u32>(TargetSlot::Count); ++i) {
        if (m & (1u << i))
            return static_cast<TargetSlot>(i);
    }
    return TargetSlot::Count;
}

} // namespace

ProfileValidation ValidateProfile(const IRenderProfile& profile) {
    // Every target the chain names must be in the declared target set —
    // otherwise the profile is asking for something it never sized.
    TargetMask declared = 0;
    for (const auto& t : profile.TargetSet())
        declared |= TargetBit(t.slot);

    // Written-so-far, split by whether the writer can be switched off. A
    // target only in `conditional` is one the frame may or may not have.
    TargetMask unconditional = 0;
    TargetMask conditional = 0;

    std::ostringstream os;
    for (const auto& p : profile.Passes()) {
        const TargetMask touched = p.reads | p.optionalReads | p.writes;
        if (const TargetMask undeclared = touched & ~declared) {
            os << profile.Name() << ": pass " << SlotName(p.slot) << " uses "
               << TargetName(FirstSetBit(undeclared)) << ", which is not in the target set";
            return {false, os.str()};
        }
        if (const TargetMask missing = p.reads & ~(unconditional | conditional)) {
            os << profile.Name() << ": pass " << SlotName(p.slot) << " reads "
               << TargetName(FirstSetBit(missing)) << " before any pass writes it";
            return {false, os.str()};
        }
        // The rule that earns its keep: an always-on consumer of a
        // switchable producer is a frame that works until the flag flips.
        if (p.Unconditional()) {
            if (const TargetMask fragile = p.reads & conditional & ~unconditional) {
                os << profile.Name() << ": unconditional pass " << SlotName(p.slot) << " reads "
                   << TargetName(FirstSetBit(fragile)) << ", which only a conditional pass writes";
                return {false, os.str()};
            }
        }
        if (p.Unconditional())
            unconditional |= p.writes;
        else
            conditional |= p.writes;
    }
    return {true, {}};
}

} // namespace whiteout::flakes::renderer::core

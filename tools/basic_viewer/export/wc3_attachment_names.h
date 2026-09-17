#pragma once

// ============================================================================
// Warcraft III attachment names in StarCraft II's vocabulary
// (WC3_TO_SC2_COMPLETION_PLAN.md §4.1). Game knowledge, so it lives with the
// export driver; its own file so a device-free test can link it alone.
// ============================================================================

#include <string>

#include <whiteout/models/wem/model.h>

namespace whiteout::flakes {

/// A Warcraft III attachment name in StarCraft II's vocabulary.
struct Wc3AttachmentName {
    std::string name;
    /// False when the name is none the table knows: the rule still applies
    /// (`Ref_` + the title-cased name without its `ref` suffix), and the
    /// export says so.
    bool known = false;
};

/// @brief `Origin Ref` -> `Ref_Origin`, `Sprite First Ref` -> `Ref_Hardpoint 01`
///        (WC3_TO_SC2_COMPLETION_PLAN.md §4.1).
Wc3AttachmentName Wc3AttachmentNameToSc2(const std::string& name);

/// Where StarCraft II's targeting volume sits on a Warcraft III model
/// (WC3_TO_SC2_COMPLETION_PLAN.md C3.2): the box around every collision shape,
/// and a bone scale that makes the volume's 0.5 sphere 0.72 of that box's
/// half extents -- what Blizzard's conversions state on 253 of the 462 paired
/// models with CLID, the rest carrying an artist's sphere scale the `.mdx`
/// does not hold. No collision shape: the origin at scale 1.2, their default.
struct Wc3TargetVolume {
    Vector3f center{0.0f, 0.0f, 0.0f}; ///< Warcraft III units and axes.
    /// The `Vol_Target` bone's scale, in StarCraft II's axis order.
    Vector3f scale{1.2f, 1.2f, 1.2f};
    bool fromCollision = false;
};

Wc3TargetVolume Wc3TargetVolumeOf(const models::wem::Model& model);

} // namespace whiteout::flakes

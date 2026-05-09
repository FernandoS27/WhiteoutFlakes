#include "renderer/scene_manager.h"

#include "renderer/model/model_instance.h"
#include "renderer/model/model_source.h"

namespace whiteout::flakes::renderer {

using namespace ::whiteout::flakes::renderer::model;

void SceneManager::Update(f32 dtSec) {
    const i32 dtMs = (dtSec > 0.0f) ? (i32)(dtSec * 1000.0f + 0.5f) : 0;
    if (dtMs > 0) animationTimeMs_.fetch_add(dtMs);

    // Advance each Unit actor's own playback clock. External actors are driven
    // by the host (Max plugin scrubs the timeline), and PE1/SPN/Attachment
    // children derive their cursor from wall-clock minus birth — neither path
    // touches Advance.
    for (auto& [h, mi] : actors_.All()) {
        if (mi->role != model::ActorRole::Unit) continue;
        mi->Advance(dtSec);
    }
}

}

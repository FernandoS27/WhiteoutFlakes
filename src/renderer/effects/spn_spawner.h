#pragma once

#include "whiteout/flakes/types.h"
#include "types.h"
#include <string>
#include <vector>

namespace whiteout::flakes::renderer { class RenderService; }

namespace whiteout::flakes::renderer::effects {

class SpnSpawner {
public:
    explicit SpnSpawner(RenderService& rs) : rs_(rs) {}

    void Spawn(u32                parentActor,
               const std::string& mdxPath,
               const Matrix44f&   parentNodeWorld,
               i32                nowMs);

    void Tick(i32 nowMs);

    void RemoveSpawnsOf(u32 parentActor);

    void Clear();

private:
    struct Pending {
        u32         parentActor;
        std::string mdxPath;
        Matrix44f   parentWorld;
        i32         birthMs;
    };
    struct Active {
        u32 parentActor;
        u32 handle;
        i32 expiryMs;
    };

    RenderService&       rs_;
    std::vector<Pending> pending_;
    std::vector<Active>  active_;
};

}

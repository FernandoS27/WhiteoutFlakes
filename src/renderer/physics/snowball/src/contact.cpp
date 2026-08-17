#include "snowball/contact.h"

#include <cmath>

namespace snowball {

void MatchManifold(const Manifold& previous, Manifold& fresh) {
    if (fresh.count <= 0) {
        return;
    }
    // The manifold-wide friction state moves across wholesale, before any point is looked at --
    // it belongs to the patch rather than to a point, so there is nothing to match it against.
    fresh.frictionImpulse = previous.frictionImpulse;
    fresh.token = previous.token;

    bool claimed[4] = {};
    for (i32 i = 0; i < fresh.count; ++i) {
        ManifoldPoint& point = fresh.points[static_cast<usize>(i)];
        point.normalImpulse = 0.0f;
        point.isNew = true;
        for (i32 j = 0; j < previous.count; ++j) {
            const ManifoldPoint& old = previous.points[static_cast<usize>(j)];
            if (claimed[j] || old.id != point.id) {
                continue;
            }
            claimed[j] = true;
            point.normalImpulse = old.normalImpulse;
            point.isNew = false;
            break;
        }
    }
}

f32 MixFriction(f32 a, f32 b) { return std::sqrt(a * b); }

f32 MixRestitution(f32 a, f32 b) { return a > b ? a : b; }

}  // namespace snowball

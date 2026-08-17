#include "snowball/math.h"

namespace snowball {

Vec4 Solve33(const Mtx& a, const Vec4& b) {
    const Vec4 cofactor = Cross3(a.c[1], a.c[2]);
    const f32 det = Dot3(a.c[0], cofactor);

    // Guarded reciprocal: a singular system has to come back exactly zero rather than NaN or
    // infinity, so |det| must clear kZeroSafe before the approximate Rcp is applied.
    const f32 scale = (det < 0.0f ? -det : det) > constants::kZeroSafe.x ? Rcp(det) : 0.0f;

    return Vec4{Dot3(b, cofactor), Dot3(a.c[0], Cross3(b, a.c[2])),
                Dot3(a.c[0], Cross3(a.c[1], b)), 0.0f} *
           scale;
}

}  // namespace snowball

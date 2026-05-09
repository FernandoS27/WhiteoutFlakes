#pragma once

#include "whiteout/flakes/types.h"
#include "types.h"
#include "whiteout/flakes/util/coordinate_system.h"
#include <algorithm>
#include <cmath>

namespace whiteout::flakes::renderer {

struct CameraMatrices {
    Matrix44f viewLH, viewRH;
    Matrix44f projLH, projRH;
};

class Camera {
public:
    enum class Mode {
        Orbital,
        Direct,
    };

    static constexpr f32 kDefaultPitch    = 0.3f;
    static inline const f32 kDefaultYaw =
        []{
            Vector3f frontMax{0.0f, -1.0f, 0.0f};
            Vector3f frontDefault =
                CoordinateSystem::ConvertDirection(CoordSpace::Max,
                                                   CoordinateSystem::Default(),
                                                   frontMax);
            return std::atan2(frontDefault.y, frontDefault.x);
        }();
    static constexpr f32 kDefaultDistance = 350.0f;
    static constexpr f32 kMinPitch        = -1.5607963f;
    static constexpr f32 kMaxPitch        =  1.5607963f;
    static constexpr f32 kMinDistance     = 15.0f;
    static constexpr f32 kMaxDistance     = 8000.0f;
    static constexpr f32 kFactorPitch     = 0.02f;
    static constexpr f32 kFactorYaw       = 0.02f;
    static constexpr f32 kFactorDistance  = 0.002f;
    static constexpr f32 kFactorMove      = 0.004f;
    static constexpr f32 kFactorRelDist   = 500.0f;
    static constexpr f32 kFactorRelMove   = 500.0f;

    static constexpr f32 kDefaultFovDiagonal = 1.30f;
    static constexpr f32 kDefaultNearZ       = 1.0f;
    static constexpr f32 kDefaultFarZ        = 10000.0f;

    Camera() { Reset(); }

    void Reset() {
        mode_     = Mode::Orbital;
        pitch_    = kDefaultPitch;
        yaw_      = kDefaultYaw;
        distance_ = kDefaultDistance;
        target_   = {0.f, 0.f, 50.f};
        roll_     = 0.0f;
        localYaw_ = localPitch_ = localRoll_ = 0.0f;
        fovDiagonal_ = kDefaultFovDiagonal;
        zNear_ = kDefaultNearZ;
        zFar_  = kDefaultFarZ;
        directPosition_ = {0.f, 0.f, 0.f};
        directTarget_   = {0.f, 0.f, 0.f};
    }

    void SetFromModel(f32 boundsRadius) {
        distance_ = (std::max)(boundsRadius * 2.0f, kDefaultDistance);
        distance_ = std::clamp(distance_, kMinDistance, kMaxDistance);
    }

    void Rotate(i32 dx, i32 dy) {
        yaw_   -= dx * kFactorYaw;
        pitch_ += dy * kFactorPitch;
        pitch_  = std::clamp(pitch_, kMinPitch, kMaxPitch);
    }
    void Pan(i32 dx, i32 dy) {
        f32 speed = distance_ * 0.003f;
        f32 cosP = cosf(pitch_), sinP = sinf(pitch_);
        f32 cosY = cosf(yaw_),   sinY = sinf(yaw_);
        f32 rx = -sinY,               ry =  cosY;
        f32 ux = -sinP * cosY,        uy = -sinP * sinY, uz = cosP;
        f32 mx = dx * speed, my = dy * speed;
        target_.x += rx * mx + ux * my;
        target_.y += ry * mx + uy * my;
        target_.z +=            uz * my;
    }
    void Zoom(i32 delta) {
        f32 factor = distance_ / kFactorRelDist;
        distance_ -= delta * factor;
        distance_  = std::clamp(distance_, kMinDistance, kMaxDistance);
    }
    void ZoomSmooth(f32 amount) {
        distance_ -= amount;
        distance_  = std::clamp(distance_, kMinDistance, kMaxDistance);
    }

    f32 GetPitch()    const { return pitch_; }
    f32 GetYaw()      const { return yaw_; }
    f32 GetDistance() const { return distance_; }
    Vector3f GetTarget() const {
        return (mode_ == Mode::Direct) ? directTarget_ : target_;
    }
    Vector3f GetSource() const {
        if (mode_ == Mode::Direct) return directPosition_;
        f32 cosP = cosf(pitch_);
        return Vector3f(
            target_.x + distance_ * cosP * cosf(yaw_),
            target_.y + distance_ * cosP * sinf(yaw_),
            target_.z + distance_ * sinf(pitch_)
        );
    }

    void SetPitch(f32 p)    { pitch_ = std::clamp(p, kMinPitch, kMaxPitch); }
    void SetYaw(f32 y)      { yaw_ = y; }
    void SetDistance(f32 d) { distance_ = std::clamp(d, kMinDistance, kMaxDistance); }
    void SetTarget(f32 x, f32 y, f32 z) { target_ = {x, y, z}; }
    void SetTarget(const Vector3f& t) { target_ = t; }

    f32 GetFovDiagonal() const { return fovDiagonal_; }
    f32 GetNearZ()       const { return zNear_; }
    f32 GetFarZ()        const { return zFar_; }
    void  SetFovDiagonal(f32 rad) { fovDiagonal_ = rad; }
    void  SetClip(f32 nearZ, f32 farZ) { zNear_ = nearZ; zFar_ = farZ; }

    f32 GetRoll() const { return roll_; }
    void  SetRoll(f32 r) { roll_ = r; }

    void  SetLocalEuler(f32 yaw, f32 pitch, f32 roll) {
        localYaw_ = yaw; localPitch_ = pitch; localRoll_ = roll;
    }

    Mode GetMode() const { return mode_; }
    void SetOrbitalMode() { mode_ = Mode::Orbital; }
    void SetDirectPose(const Vector3f& pos, const Vector3f& target,
                       f32 rollRad = 0.0f) {
        mode_ = Mode::Direct;
        directPosition_ = pos;
        directTarget_   = target;
        roll_           = rollRad;
    }

    Matrix44f ViewLH() const {
        Vector3f up = (mode_ == Mode::Direct)
                          ? ComputeUpFromLookDirection()
                          : ComputeUpFromAngles();
        Matrix44f view = Matrix44f::look_at_lh_sgcompat(GetSource(), GetTarget(), up);
        if (localYaw_ != 0.0f || localPitch_ != 0.0f || localRoll_ != 0.0f) {
            view = view * LocalEulerMatrix();
        }
        return view;
    }

    Matrix44f ViewRH() const {
        return Matrix44f::look_at_rh(GetSource(), GetTarget(), GetUp());
    }

    Matrix44f ProjectionLH(f32 aspect) const {
        return Matrix44f::perspective_diag_sgcompat(fovDiagonal_, aspect, zNear_, zFar_);
    }

    Matrix44f ProjectionRH(f32 aspect) const {
        const f32 invDiag = 1.0f / std::sqrt(aspect * aspect + 1.0f);
        const f32 fovY    = 2.0f * std::atan(std::tan(0.5f * fovDiagonal_ * invDiag));
        return Matrix44f::perspective_fov_rh(fovY, aspect, zNear_, zFar_);
    }

    CameraMatrices Compute(f32 aspect) const {
        CameraMatrices m;
        m.viewLH = ViewLH();
        m.viewRH = ViewRH();
        m.projLH = ProjectionLH(aspect);
        m.projRH = ProjectionRH(aspect);
        return m;
    }

    Matrix44f GetViewMatrix() const { return ViewRH(); }
    Vector3f GetUp() const { return Vector3f(0.f, 0.f, 1.f); }

    // Snap to one of the 6 standard view-cube faces (Max-space face indices,
    // matching the debug viewcube widget). Index out of range is a no-op.
    void SnapToViewCubeFace(i32 faceIndex) {
        if (faceIndex < 0 || faceIndex > 5) return;
        static constexpr Vector3f kFaceNormalsMax[6] = {
            { 0,  1,  0},
            { 0, -1,  0},
            {-1,  0,  0},
            { 1,  0,  0},
            { 0,  0,  1},
            { 0,  0, -1},
        };
        const Vector3f n = CoordinateSystem::ConvertDirection(
            CoordSpace::Max, CoordinateSystem::Default(),
            kFaceNormalsMax[faceIndex]);

        constexpr f32 kTopBottomPitch = 1.55f;
        if (std::abs(n.z) > 0.99f) {
            SetYaw(kDefaultYaw);
            SetPitch(n.z > 0 ? kTopBottomPitch : -kTopBottomPitch);
        } else {
            SetYaw(std::atan2(n.y, n.x));
            SetPitch(0.0f);
        }
    }

private:

    Vector3f ComputeUpFromLookDirection() const {
        Vector3f forward = GetTarget() - GetSource();
        const f32 fLen = forward.length();
        if (fLen < 1e-4f) return {0.0f, 0.0f, 1.0f};
        forward = forward / fLen;

        Vector3f worldUp{0.0f, 0.0f, 1.0f};
        Vector3f right = cross(forward, worldUp);

        if (right.length_squared() < 1e-6f) {
            right = cross(forward, Vector3f{1.0f, 0.0f, 0.0f});
        }
        right = right.normalized();
        Vector3f upBase = cross(right, forward).normalized();

        if (roll_ == 0.0f) return upBase;
        const f32 c = std::cos(roll_), s = std::sin(roll_);
        return {
            c * upBase.x + s * right.x,
            c * upBase.y + s * right.y,
            c * upBase.z + s * right.z,
        };
    }

    Vector3f ComputeUpFromAngles() const {
        const f32 sinDir = std::sin(yaw_),   cosDir = std::cos(yaw_);
        const f32 sinAoa = std::sin(pitch_), cosAoa = std::cos(pitch_);
        const f32 sinRoll = std::sin(roll_), cosRoll = std::cos(roll_);
        Vector3f noRoll(-cosDir * sinAoa, -sinDir * sinAoa,  cosAoa);
        Vector3f allRoll(sinDir,          -cosDir,           0.0f);
        return {
            cosRoll * noRoll.x + sinRoll * allRoll.x,
            cosRoll * noRoll.y + sinRoll * allRoll.y,
            cosRoll * noRoll.z + sinRoll * allRoll.z
        };
    }

    Matrix44f LocalEulerMatrix() const {
        const f32 cz = std::cos(localYaw_),   sz = std::sin(localYaw_);
        const f32 cy = std::cos(localPitch_), sy = std::sin(localPitch_);
        const f32 cx = std::cos(localRoll_),  sx = std::sin(localRoll_);
        Matrix44f r{};
        r.data[0][0] =  cz*cy;
        r.data[0][1] =  cz*sy*sx - sz*cx;
        r.data[0][2] =  cz*sy*cx + sz*sx;
        r.data[0][3] = 0.0f;
        r.data[1][0] =  sz*cy;
        r.data[1][1] =  sz*sy*sx + cz*cx;
        r.data[1][2] =  sz*sy*cx - cz*sx;
        r.data[1][3] = 0.0f;
        r.data[2][0] = -sy;
        r.data[2][1] =  cy*sx;
        r.data[2][2] =  cy*cx;
        r.data[2][3] = 0.0f;
        r.data[3][0] = 0.0f; r.data[3][1] = 0.0f; r.data[3][2] = 0.0f; r.data[3][3] = 1.0f;
        return r;
    }

    f32 pitch_;
    f32 yaw_;
    f32 distance_;
    Vector3f target_;

    Mode mode_ = Mode::Orbital;
    Vector3f directPosition_;
    Vector3f directTarget_;

    f32 roll_ = 0.0f;
    f32 localYaw_ = 0.0f, localPitch_ = 0.0f, localRoll_ = 0.0f;
    f32 fovDiagonal_ = kDefaultFovDiagonal;
    f32 zNear_ = kDefaultNearZ;
    f32 zFar_  = kDefaultFarZ;
};

}

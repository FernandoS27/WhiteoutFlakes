#pragma once

#include "whiteout/flakes/types.h"
#include "bls_cb_layout.h"
#include "bls_mat_params.h"
#include "types.h"

namespace whiteout::flakes::renderer::bls {

struct FrameInputs {
    Matrix44f  world          = Matrix44f::identity();
    Matrix44f  view           = Matrix44f::identity();
    Matrix44f  projection     = Matrix44f::identity();

    Vector4f   fogParams      = {0, 0, 0, 0};
    Vector4f   fogColor       = {0, 0, 0, 0};
    Vector4f   viewportRect   = {1, 1, 0, 0};

    f32        effectTime     = 0.0f;
    i32        numLights      = 0;

    i32        useNdf         = 1;

    f32        envFromMipEnd  = 0.0f;
    f32        envToMipEnd    = 0.0f;
    f32        envTransitionT = 0.0f;

    ShaderTexMtx texMtx0      = {{ Vector4f{1, 0, 0, 0}, Vector4f{0, 1, 0, 0} }};
    ShaderTexMtx texMtx1      = {{ Vector4f{1, 0, 0, 0}, Vector4f{0, 1, 0, 0} }};

    ShaderLight lights[kMaxLights] = {};
};

void BuildSdVsCbA(SdVsCbA& out, const FrameInputs& in, const MatParams& mat);
void BuildSdPsCbA(SdPsCbA& out, const FrameInputs& in, const MatParams& mat);

void BuildHdVsCb    (HdVsCb&     out, const FrameInputs& in, const MatParams& mat);
void BuildHdPsCb    (HdPsCb&     out, const FrameInputs& in, const MatParams& mat);
void BuildSdOnHdPsCb(SdOnHdPsCb& out, const FrameInputs& in, const MatParams& mat);

void PackBone(ShaderBone& out, const Matrix44f& m);
void BuildBonePalette(BonePaletteCb& out, const Matrix44f* src, i32 numBones);
void PackBoneVertex(BoneVertex& out, const i32 indices[4], const f32 weights[4]);

inline ShaderTexMtx IdentityTexMtx() {
    return { { Vector4f{1, 0, 0, 0}, Vector4f{0, 1, 0, 0} } };
}

ShaderTexMtx ComposeTexAnimMatrix(const Quaternion& rot,
                                  const Vector3f&   scale,
                                  const Vector3f&   trans);

}

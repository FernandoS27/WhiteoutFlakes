// Column-major 4x4 helpers. Output layout matches Matrix44f so JS can
// hand its 16 floats straight to the WASM heap.

export function identityMat(out) {
    out[0] = 1;  out[1] = 0;  out[2] = 0;  out[3] = 0;
    out[4] = 0;  out[5] = 1;  out[6] = 0;  out[7] = 0;
    out[8] = 0;  out[9] = 0;  out[10] = 1; out[11] = 0;
    out[12] = 0; out[13] = 0; out[14] = 0; out[15] = 1;
    return out;
}

// Compose from translation, quaternion [x, y, z, w], non-uniform scale.
export function composeTRS(out, t, q, s) {
    const x = q[0], y = q[1], z = q[2], w = q[3];
    const xx = x * x, yy = y * y, zz = z * z;
    const xy = x * y, xz = x * z, yz = y * z;
    const wx = w * x, wy = w * y, wz = w * z;
    const sx = s[0], sy = s[1], sz = s[2];
    out[0]  = (1 - 2 * (yy + zz)) * sx;
    out[1]  = (2 * (xy + wz)) * sx;
    out[2]  = (2 * (xz - wy)) * sx;
    out[3]  = 0;
    out[4]  = (2 * (xy - wz)) * sy;
    out[5]  = (1 - 2 * (xx + zz)) * sy;
    out[6]  = (2 * (yz + wx)) * sy;
    out[7]  = 0;
    out[8]  = (2 * (xz + wy)) * sz;
    out[9]  = (2 * (yz - wx)) * sz;
    out[10] = (1 - 2 * (xx + yy)) * sz;
    out[11] = 0;
    out[12] = t[0];
    out[13] = t[1];
    out[14] = t[2];
    out[15] = 1;
    return out;
}

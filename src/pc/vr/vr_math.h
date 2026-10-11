#ifndef VR_MATH_H
#define VR_MATH_H

// Small 4x4 matrix helpers for the VR layer.
// Convention matches the game and gfx_pc: row vectors, p' = p * M, translation in row 3.
// A rigid "world-from-local" matrix therefore has the local X/Y/Z axes in rows 0/1/2.

#include <math.h>
#include <string.h>

typedef float VrMat[4][4];

static inline void vrm_identity(VrMat m) {
    memset(m, 0, sizeof(VrMat));
    m[0][0] = m[1][1] = m[2][2] = m[3][3] = 1.0f;
}

static inline void vrm_copy(VrMat dst, const VrMat src) {
    memcpy(dst, src, sizeof(VrMat));
}

// out = a * b (apply a first, then b). out may alias a or b.
static inline void vrm_mul(VrMat out, const VrMat a, const VrMat b) {
    VrMat t;
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            t[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j] + a[i][3] * b[3][j];
        }
    }
    memcpy(out, t, sizeof(VrMat));
}

// Rigid transform from a quaternion (x, y, z, w) and a position.
static inline void vrm_from_pose(VrMat m, const float q[4], const float p[3]) {
    float x = q[0], y = q[1], z = q[2], w = q[3];
    m[0][0] = 1 - 2 * (y * y + z * z); m[0][1] = 2 * (x * y + z * w);     m[0][2] = 2 * (x * z - y * w);     m[0][3] = 0;
    m[1][0] = 2 * (x * y - z * w);     m[1][1] = 1 - 2 * (x * x + z * z); m[1][2] = 2 * (y * z + x * w);     m[1][3] = 0;
    m[2][0] = 2 * (x * z + y * w);     m[2][1] = 2 * (y * z - x * w);     m[2][2] = 1 - 2 * (x * x + y * y); m[2][3] = 0;
    m[3][0] = p[0];                    m[3][1] = p[1];                    m[3][2] = p[2];                    m[3][3] = 1;
}

// Inverse of a rigid transform (rotation + translation, no scale).
static inline void vrm_rigid_inverse(VrMat out, const VrMat m) {
    VrMat t;
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            t[i][j] = m[j][i];
        }
        t[i][3] = 0;
    }
    for (int j = 0; j < 3; j++) {
        t[3][j] = -(m[3][0] * t[0][j] + m[3][1] * t[1][j] + m[3][2] * t[2][j]);
    }
    t[3][3] = 1;
    memcpy(out, t, sizeof(VrMat));
}

// General inverse (for the game's view matrix, which may carry a tiny scale).
static inline int vrm_inverse(VrMat out, const VrMat m) {
    const float *a = &m[0][0];
    float inv[16];
    inv[0] = a[5] * a[10] * a[15] - a[5] * a[11] * a[14] - a[9] * a[6] * a[15] + a[9] * a[7] * a[14] + a[13] * a[6] * a[11] - a[13] * a[7] * a[10];
    inv[4] = -a[4] * a[10] * a[15] + a[4] * a[11] * a[14] + a[8] * a[6] * a[15] - a[8] * a[7] * a[14] - a[12] * a[6] * a[11] + a[12] * a[7] * a[10];
    inv[8] = a[4] * a[9] * a[15] - a[4] * a[11] * a[13] - a[8] * a[5] * a[15] + a[8] * a[7] * a[13] + a[12] * a[5] * a[11] - a[12] * a[7] * a[9];
    inv[12] = -a[4] * a[9] * a[14] + a[4] * a[10] * a[13] + a[8] * a[5] * a[14] - a[8] * a[6] * a[13] - a[12] * a[5] * a[10] + a[12] * a[6] * a[9];
    inv[1] = -a[1] * a[10] * a[15] + a[1] * a[11] * a[14] + a[9] * a[2] * a[15] - a[9] * a[3] * a[14] - a[13] * a[2] * a[11] + a[13] * a[3] * a[10];
    inv[5] = a[0] * a[10] * a[15] - a[0] * a[11] * a[14] - a[8] * a[2] * a[15] + a[8] * a[3] * a[14] + a[12] * a[2] * a[11] - a[12] * a[3] * a[10];
    inv[9] = -a[0] * a[9] * a[15] + a[0] * a[11] * a[13] + a[8] * a[1] * a[15] - a[8] * a[3] * a[13] - a[12] * a[1] * a[11] + a[12] * a[3] * a[9];
    inv[13] = a[0] * a[9] * a[14] - a[0] * a[10] * a[13] - a[8] * a[1] * a[14] + a[8] * a[2] * a[13] + a[12] * a[1] * a[10] - a[12] * a[2] * a[9];
    inv[2] = a[1] * a[6] * a[15] - a[1] * a[7] * a[14] - a[5] * a[2] * a[15] + a[5] * a[3] * a[14] + a[13] * a[2] * a[7] - a[13] * a[3] * a[6];
    inv[6] = -a[0] * a[6] * a[15] + a[0] * a[7] * a[14] + a[4] * a[2] * a[15] - a[4] * a[3] * a[14] - a[12] * a[2] * a[7] + a[12] * a[3] * a[6];
    inv[10] = a[0] * a[5] * a[15] - a[0] * a[7] * a[13] - a[4] * a[1] * a[15] + a[4] * a[3] * a[13] + a[12] * a[1] * a[7] - a[12] * a[3] * a[5];
    inv[14] = -a[0] * a[5] * a[14] + a[0] * a[6] * a[13] + a[4] * a[1] * a[14] - a[4] * a[2] * a[13] - a[12] * a[1] * a[6] + a[12] * a[2] * a[5];
    inv[3] = -a[1] * a[6] * a[11] + a[1] * a[7] * a[10] + a[5] * a[2] * a[11] - a[5] * a[3] * a[10] - a[9] * a[2] * a[7] + a[9] * a[3] * a[6];
    inv[7] = a[0] * a[6] * a[11] - a[0] * a[7] * a[10] - a[4] * a[2] * a[11] + a[4] * a[3] * a[10] + a[8] * a[2] * a[7] - a[8] * a[3] * a[6];
    inv[11] = -a[0] * a[5] * a[11] + a[0] * a[7] * a[9] + a[4] * a[1] * a[11] - a[4] * a[3] * a[9] - a[8] * a[1] * a[7] + a[8] * a[3] * a[5];
    inv[15] = a[0] * a[5] * a[10] - a[0] * a[6] * a[9] - a[4] * a[1] * a[10] + a[4] * a[2] * a[9] + a[8] * a[1] * a[6] - a[8] * a[2] * a[5];
    float det = a[0] * inv[0] + a[1] * inv[4] + a[2] * inv[8] + a[3] * inv[12];
    if (fabsf(det) < 1e-12f) {
        vrm_identity(out);
        return 0;
    }
    det = 1.0f / det;
    for (int i = 0; i < 16; i++) {
        (&out[0][0])[i] = inv[i] * det;
    }
    return 1;
}

// Rotation about +Y by `a` radians (right-hand rule).
static inline void vrm_rot_y(VrMat m, float a) {
    float c = cosf(a), s = sinf(a);
    vrm_identity(m);
    m[0][0] = c;  m[0][2] = -s;
    m[2][0] = s;  m[2][2] = c;
}

static inline void vrm_translate(VrMat m, float x, float y, float z) {
    vrm_identity(m);
    m[3][0] = x; m[3][1] = y; m[3][2] = z;
}

static inline void vrm_scale(VrMat m, float s) {
    vrm_identity(m);
    m[0][0] = m[1][1] = m[2][2] = s;
}

static inline void vrm_point(float out[3], const float p[3], const VrMat m) {
    float x = p[0] * m[0][0] + p[1] * m[1][0] + p[2] * m[2][0] + m[3][0];
    float y = p[0] * m[0][1] + p[1] * m[1][1] + p[2] * m[2][1] + m[3][1];
    float z = p[0] * m[0][2] + p[1] * m[1][2] + p[2] * m[2][2] + m[3][2];
    out[0] = x; out[1] = y; out[2] = z;
}

static inline void vrm_dir(float out[3], const float d[3], const VrMat m) {
    float x = d[0] * m[0][0] + d[1] * m[1][0] + d[2] * m[2][0];
    float y = d[0] * m[0][1] + d[1] * m[1][1] + d[2] * m[2][1];
    float z = d[0] * m[0][2] + d[1] * m[1][2] + d[2] * m[2][2];
    out[0] = x; out[1] = y; out[2] = z;
}

// OpenGL-style projection from tangent half-angles (left/down negative), row-vector form.
// Same layout as guPerspective, so the rest of gfx_pc treats it like the game's own matrix.
static inline void vrm_projection(VrMat m, float tl, float tr, float tu, float td, float n, float f) {
    memset(m, 0, sizeof(VrMat));
    m[0][0] = 2.0f / (tr - tl);
    m[1][1] = 2.0f / (tu - td);
    m[2][0] = (tr + tl) / (tr - tl);
    m[2][1] = (tu + td) / (tu - td);
    m[2][2] = -(f + n) / (f - n);
    m[2][3] = -1.0f;
    m[3][2] = -2.0f * f * n / (f - n);
}

// NB: the game exports its own atan2f (built on atan2s, different argument meaning) which
// replaces libm's at link time, so the VR code must not call atan2f.
static inline float vr_atan2(float y, float x) {
    return (float) atan2((double) y, (double) x);
}

static inline float vrv_len(const float v[3]) {
    return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

static inline float vrv_dot(const float a[3], const float b[3]) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static inline void vrv_cross(float out[3], const float a[3], const float b[3]) {
    float x = a[1] * b[2] - a[2] * b[1];
    float y = a[2] * b[0] - a[0] * b[2];
    float z = a[0] * b[1] - a[1] * b[0];
    out[0] = x; out[1] = y; out[2] = z;
}

static inline float vrv_normalize(float v[3]) {
    float l = vrv_len(v);
    if (l > 1e-8f) {
        v[0] /= l; v[1] /= l; v[2] /= l;
    }
    return l;
}

// Yaw (radians, about +Y, 0 = looking down -Z) of a rotation quaternion's forward vector.
static inline float vrq_yaw(const float q[4]) {
    VrMat m;
    const float zero[3] = { 0, 0, 0 };
    vrm_from_pose(m, q, zero);
    // forward = -Z axis = -row 2
    return vr_atan2(m[2][0], m[2][2]);
}

static inline float vr_wrap_angle(float a) {
    while (a > (float) M_PI) a -= 2.0f * (float) M_PI;
    while (a < -(float) M_PI) a += 2.0f * (float) M_PI;
    return a;
}

#endif

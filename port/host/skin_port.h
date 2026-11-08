/* skin_port.h: C versions of njplus.c's VU0 skinning blocks (RECVX_PORT). */
#pragma once

/* o = p * m (point: rows 0..2 + row 3), xyz */
static inline void skin_pt(const float *m, const float *p, float *o)
{
    float x = p[0], y = p[1], z = p[2];
    o[0] = m[0] * x + m[4] * y + m[8] * z + m[12];
    o[1] = m[1] * x + m[5] * y + m[9] * z + m[13];
    o[2] = m[2] * x + m[6] * y + m[10] * z + m[14];
}

/* o = p * m (direction: rows 0..2), xyz */
static inline void skin_vec(const float *m, const float *p, float *o)
{
    float x = p[0], y = p[1], z = p[2];
    o[0] = m[0] * x + m[4] * y + m[8] * z;
    o[1] = m[1] * x + m[5] * y + m[9] * z;
    o[2] = m[2] * x + m[6] * y + m[10] * z;
}

/* weighted vertex: a = p*m0, b = p*m1, out = a + (b - a) * lv; w from m1 row 2 */
static inline void skin_blend_pt(float lv, const float *p, float *out, const float *m0, const float *m1)
{
    float a[3], b[3];
    int k;
    skin_pt(m0, p, a);
    skin_pt(m1, p, b);
    for (k = 0; k < 3; k++)
        out[k] = (b[k] - a[k]) * lv + a[k];
    out[3] = m1[11];
}

/* weighted normal; w from m0 row 2 */
static inline void skin_blend_vec(float lv, const float *p, float *out, const float *m0, const float *m1)
{
    float a[3], b[3];
    int k;
    skin_vec(m0, p, a);
    skin_vec(m1, p, b);
    for (k = 0; k < 3; k++)
        out[k] = (b[k] - a[k]) * lv + a[k];
    out[3] = m0[11];
}

/* single-bone vertex + normal (p0[0..7] -> p1[0..7]) */
static inline void skin_one(const float *p0, float *p1, const float *m)
{
    skin_pt(m, p0, p1);
    p1[3] = p0[3];
    skin_vec(m, p0 + 4, p1 + 4);
    p1[7] = p0[7];
}

/* bone matrix pair: both copies = owp->mtx with translation minus the base
 * position (xyz) */
static inline void skin_init_pair(const float *mtx, const float *pos, float *dst)
{
    int k;
    memcpy(dst, mtx, 16 * sizeof(float));
    for (k = 0; k < 3; k++)
        dst[12 + k] = mtx[12 + k] - pos[k];
    memcpy(dst + 16, dst, 16 * sizeof(float));
}

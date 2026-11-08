/* prim_port.h: C version of the VU0 vertex loop shared by ps2_dummy.c's
 * Ps2AddPrim3D* (RECVX_PORT).  Per vertex (3 quadwords in, 3 out: ST,
 * RGBAQ, XYZF):
 *   z' = clamp(-za - zb * q, 0, 65534); x, y, z', w to 12.4 fixed point;
 *   a vertex whose last-three clip flags (guard band 2048 +- 2047 in x / y,
 *   z' check against the near plane) are set gets ADC (0x8000) so its
 *   triangle is not drawn; the low 16 bits of the input ST.w are or-ed in.
 * Returns the average z' (stored after the vertices like the original). */
#pragma once

static inline int prim_ftoi4(float f) { return (int)(f * 16.0f); }

static inline unsigned prim_clip6(float x, float y, float z, float w)
{
    float aw = w < 0 ? -w : w;
    unsigned f = 0;
    if (x > aw) f |= 0x01; if (x < -aw) f |= 0x02;
    if (y > aw) f |= 0x04; if (y < -aw) f |= 0x08;
    if (z > aw) f |= 0x10; if (z < -aw) f |= 0x20;
    return f;
}

static inline void prim_xform(const float *zab, const void *dp_, void *p_, unsigned num, int rgba_float)
{
    const unsigned *dp = (const unsigned *)dp_;
    unsigned *p = (unsigned *)p_;
    float za = -zab[2], zb = -zab[0];
    unsigned clip = 0, i;
    float zsum = 0.0f;
    for (i = 0; i < num; i++, dp += 12, p += 12) {
        const float *st = (const float *)dp, *xyz = (const float *)(dp + 8);
        float x = xyz[0], y = xyz[1], z = xyz[2], q = st[2], zz;
        unsigned flags = dp[3] & 0xFFFF, w16;
        float cz = z + -2048.0625f + z * 0.062501907f;
        clip = ((clip << 6) | prim_clip6(x - 2048.0f, y - 2048.0f, cz, 2047.0f)) & 0xFFFFFF;
        zz = -za + q * -zb;
        if (zz < 0.0f) zz = 0.0f;
        if (zz > 65534.0f) zz = 65534.0f;
        zsum += zz;
        w16 = (unsigned)prim_ftoi4(xyz[3]) & 0xFFFF;
        if (clip & 0x3FFFF)
            flags |= 0x8000;
        w16 |= flags;
        p[0] = dp[0]; p[1] = dp[1]; p[2] = dp[2]; p[3] = dp[3];
        if (rgba_float) {
            const float *c = (const float *)(dp + 4);
            p[4] = (unsigned)(int)c[0]; p[5] = (unsigned)(int)c[1];
            p[6] = (unsigned)(int)c[2]; p[7] = (unsigned)(int)c[3];
        } else {
            p[4] = dp[4]; p[5] = dp[5]; p[6] = dp[6]; p[7] = dp[7];
        }
        p[8] = (unsigned)prim_ftoi4(x);
        p[9] = (unsigned)prim_ftoi4(y);
        p[10] = (unsigned)prim_ftoi4(zz);
        p[11] = (unsigned)(int)(short)w16;
    }
    ((float *)p)[3] = num ? zsum * (1.0f / (float)num) : 0.0f;
}

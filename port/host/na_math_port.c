/* na_math_port.c: C versions of the Ninja maths functions that the PS2 game
 * wrote in EE / VU0 macro-mode assembly (src/ps2/veronica/prog/ps2_NaMath.c,
 * ps2_NaMatrix.c; guarded out there with RECVX_PORT).
 *
 * Conventions read off the original assembly:
 *  - NJS_MATRIX is 16 floats, four rows; points are row vectors:
 *        p' = p.x*row0 + p.y*row1 + p.z*row2 + row3
 *  - njMultiMatrix(md, ms): md = ms * md (rows of ms combine rows of md)
 *  - angles are 16-bit (0x10000 = 360 degrees), from a quarter-wave table
 *  - VU0 registers that carry results between functions are kept in vu0:
 *    vf28..vf31 (the "CN" matrix) and vf18 (the last transformed point).
 *  - Where the assembly left a matrix row's w lane to whatever a VU
 *    register held, the C keeps the row's existing w. */
#include "types.h"
#include "ps2_NaMath.h"
#include "ps2_NaMatrix.h"
#include "ps2_NaView.h"
#include "ps2_NaFog.h"
#include "ps2_dummy.h"
#include "vu0_state.h"

Vu0State vu0;

extern int lNaMatIsUnitMatrix;
extern int lNaMatMatrixStuckMax;
extern int lNaMatMatrixStuckCnt;
extern NJS_MATRIX *pNaMatMatrixStuckPtr;
extern NJS_MATRIX *pNaMatMatrixStuckTop;

#define ROW(m, i) (&(*(m))[(i) * 4])
#define CUR(m) ((m) ? (m) : pNaMatMatrixStuckPtr)

/* ---- sin / cos ------------------------------------------------------------ */

/* the same quarter-wave table as ps2_NaMath.c's static SinTable */
static float port_sin[16384];
static int port_sin_ready;

static void sin_table(void)
{
    int i;
    for (i = 0; i < 16384; i++)
        port_sin[i] = sinf(0.0000958738f * i);
    port_sin_ready = 1;
}

static void sincos16(int n, float *s, float *c)
{
    int idx = n & 16383;
    float a, b;
    if (!port_sin_ready)
        sin_table();
    if (n & 0x4000) {
        a = port_sin[16383 - idx];   /* sin magnitude */
        b = port_sin[idx];           /* cos magnitude */
        *s = (n & 0x8000) ? -a : a;
        *c = (n & 0x8000) ? b : -b;
    } else {
        a = port_sin[idx];
        b = port_sin[16383 - idx];
        *s = (n & 0x8000) ? -a : a;
        *c = (n & 0x8000) ? -b : b;
    }
}

Float njSin(Angle n)
{
    float s, c;
    sincos16(n, &s, &c);
    return s;
}

Float njCos(Angle n)
{
    float s, c;
    sincos16(n, &s, &c);
    return c;
}

void njSinCos(int lAngle, float *sin, float *cos)
{
    sincos16(lAngle, sin, cos);
    vu0.vf11[0] = *sin;
    vu0.vf12[0] = *cos;
}

/* VU sqrt / rsqrt work on |x| */
Float njSqrt(Float n) { return sqrtf(fabsf(n)); }

Float njInvertSqrt(Float n)
{
    float a = fabsf(n);
    return a == 0.0f ? 3.4028235e38f : 1.0f / sqrtf(a);
}

void njLinear(Float *idata, Float *odata, NJS_SPLINE *attr, Float frame)
{
    int i;
    (void)attr;
    for (i = 0; i < 3; i++)
        odata[i] = (idata[3 + i] - idata[i]) * frame + idata[i];
}

/* Catmull-Rom through idata[0..11] (four points); the VU0 constants
 * (vf2 = 3, 4, 5) come from njInitMatrix */
void njOverhauserSpline(Float *idata, Float *odata, NJS_SPLINE *attr, Float frame)
{
    float t = frame, t2 = t * t, t3 = t2 * t;
    float w0 = (t2 + t2) - t3 - t;
    float w1 = (t3 * 3.0f - t2 * 5.0f) + 2.0f;
    float w2 = (t2 * 4.0f + t) + t3 * -3.0f;
    float w3 = t3 - t2;
    int i;
    (void)attr;
    for (i = 0; i < 3; i++)
        odata[i] = (idata[i] * w0 + idata[3 + i] * w1 + idata[6 + i] * w2 + idata[9 + i] * w3) * 0.5f;
}

/* ---- matrix stack ----------------------------------------------------- */

void njInitMatrix(NJS_MATRIX *m, Sint32 n, Int flag)
{
    pNaMatMatrixStuckTop = m;
    pNaMatMatrixStuckPtr = m;
    lNaMatMatrixStuckCnt = 0;
    lNaMatMatrixStuckMax = n;
    lNaMatIsUnitMatrix = flag;
    if (!port_sin_ready)
        sin_table();
}

Bool njPushMatrix(NJS_MATRIX *m)
{
    NJS_MATRIX *src;
    if (lNaMatMatrixStuckMax <= lNaMatMatrixStuckCnt)
        return FALSE;
    lNaMatMatrixStuckCnt++;
    src = m ? m : pNaMatMatrixStuckPtr;
    pNaMatMatrixStuckPtr++;
    memmove(pNaMatMatrixStuckPtr, src, sizeof(NJS_MATRIX));
    return TRUE;
}

void njUnitTransPortion(NJS_MATRIX *m)
{
    float *r = ROW(CUR(m), 3);
    r[0] = r[1] = r[2] = 0.0f;
    r[3] = 1.0f;
}

void njUnitRotPortion(NJS_MATRIX *m)
{
    float *a = *CUR(m);
    int i;
    for (i = 0; i < 12; i++)
        a[i] = (i % 5 == 0) ? 1.0f : 0.0f;
}

void njUnitMatrix(NJS_MATRIX *m)
{
    float *a = *CUR(m);
    int i;
    for (i = 0; i < 16; i++)
        a[i] = (i % 5 == 0) ? 1.0f : 0.0f;
}

void njSetMatrix(NJS_MATRIX *md, NJS_MATRIX *ms) { memmove(*CUR(md), *ms, sizeof(NJS_MATRIX)); }
void njGetMatrix(NJS_MATRIX *m) { memmove(*m, *pNaMatMatrixStuckPtr, sizeof(NJS_MATRIX)); }
void njCopyMatrix(NJS_MATRIX *d, NJS_MATRIX *s) { memmove(*d, *s, sizeof(NJS_MATRIX)); }
void njSetMatrixCN(NJS_MATRIX *m) { memcpy(vu0.cn, *m, sizeof(NJS_MATRIX)); }

void njGetTranslation(NJS_MATRIX *m, NJS_POINT3 *p)
{
    float *r = ROW(CUR(m), 3);
    p->x = r[0];
    p->y = r[1];
    p->z = r[2];
}

/* ---- matrix products ---------------------------------------------------- */

/* md = ms * md; the result's w lanes are ms's */
void njMultiMatrix(NJS_MATRIX *md, NJS_MATRIX *ms)
{
    float a[16], *b = *ms, *d;
    int i, k;
    md = CUR(md);
    d = *md;
    memcpy(a, d, sizeof a);
    for (i = 0; i < 4; i++) {
        for (k = 0; k < 3; k++) {
            float v = b[i * 4 + 0] * a[k] + b[i * 4 + 1] * a[4 + k] + b[i * 4 + 2] * a[8 + k];
            if (i == 3)
                v += a[12 + k];
            d[i * 4 + k] = v;
        }
        d[i * 4 + 3] = b[i * 4 + 3];
    }
}

/* vf28..31 = m2 * m1 (all four lanes) */
void njMulMatrixCN(NJS_MATRIX *pSrcMat1, NJS_MATRIX *pSrcMat2)
{
    float *a = *pSrcMat1, *b = *CUR(pSrcMat2);
    float r[16];
    int i, k;
    for (i = 0; i < 4; i++)
        for (k = 0; k < 4; k++) {
            float v = b[i * 4 + 0] * a[k] + b[i * 4 + 1] * a[4 + k] + b[i * 4 + 2] * a[8 + k];
            if (i == 3)
                v += a[12 + k];
            r[i * 4 + k] = v;
        }
    memcpy(vu0.cn, r, sizeof r);
}

static void xform(const float *m, const float *p, float *o)
{
    float x = p[0], y = p[1], z = p[2];
    o[0] = m[0] * x + m[4] * y + m[8] * z + m[12];
    o[1] = m[1] * x + m[5] * y + m[9] * z + m[13];
    o[2] = m[2] * x + m[6] * y + m[10] * z + m[14];
}

static void xform_vec(const float *m, const float *p, float *o)
{
    float x = p[0], y = p[1], z = p[2];
    o[0] = m[0] * x + m[4] * y + m[8] * z;
    o[1] = m[1] * x + m[5] * y + m[9] * z;
    o[2] = m[2] * x + m[6] * y + m[10] * z;
}

/* ---- translate / rotate / scale -------------------------------------- */

static void translate(float *m, float x, float y, float z)
{
    float p[3] = { x, y, z };
    xform(m, p, &m[12]);
}

void njTranslate(NJS_MATRIX *m, Float x, Float y, Float z) { translate(*CUR(m), x, y, z); }
void njTranslateV(NJS_MATRIX *m, NJS_VECTOR *v) { translate(*CUR(m), v->x, v->y, v->z); }

void njTranslateEx(NJS_VECTOR *v)
{
    float *m = *pNaMatMatrixStuckPtr;
    translate(m, v->x, v->y, v->z);
    memcpy(vu0.cn, m, sizeof(NJS_MATRIX));
}

/* rows a, b: a' = a*c + b*s, b' = b*c - a*s (xyz; w kept) */
static void rot_rows(float *a, float *b, float s, float c)
{
    int k;
    for (k = 0; k < 3; k++) {
        float x = a[k], y = b[k];
        a[k] = x * c + y * s;
        b[k] = x * -s + y * c;
    }
}

void njRotateX(NJS_MATRIX *m, Angle ang)
{
    float s, c;
    njSinCos(ang & 0xFFFF, &s, &c);
    m = CUR(m);
    rot_rows(ROW(m, 1), ROW(m, 2), s, c);
}

void njRotateY(NJS_MATRIX *m, Angle ang)
{
    float s, c;
    njSinCos(ang & 0xFFFF, &s, &c);
    m = CUR(m);
    rot_rows(ROW(m, 0), ROW(m, 2), -s, c);
}

void njRotateZ(NJS_MATRIX *m, Angle ang)
{
    float s, c;
    njSinCos(ang & 0xFFFF, &s, &c);
    m = CUR(m);
    rot_rows(ROW(m, 0), ROW(m, 1), s, c);
}

/* X, then Y, then Z, on pMatrix itself (not the stack top) */
void njRotXYZ(NJS_MATRIX *pMatrix, int lAngleX, int lAngleY, int lAngleZ)
{
    float s, c;
    njSinCos(lAngleX & 0xFFFF, &s, &c);
    rot_rows(ROW(pMatrix, 1), ROW(pMatrix, 2), s, c);
    njSinCos(lAngleY & 0xFFFF, &s, &c);
    rot_rows(ROW(pMatrix, 0), ROW(pMatrix, 2), -s, c);
    njSinCos(lAngleZ & 0xFFFF, &s, &c);
    rot_rows(ROW(pMatrix, 0), ROW(pMatrix, 1), s, c);
}

/* rotation by ang about axis v (quaternion from the half angle) */
void njRotate(NJS_MATRIX *m, NJS_VECTOR *v, Angle ang)
{
    float s, c, l2, inv, x, y, z, w, w2, r[3][3];
    float *a;
    int i, k;
    m = CUR(m);
    ang &= 0xFFFF;
    ang /= 2;
    njSinCos(ang, &s, &c);
    l2 = v->x * v->x + v->y * v->y + v->z * v->z;
    inv = l2 == 0.0f ? 3.4028235e38f : 1.0f / sqrtf(l2);
    s *= inv;
    x = v->x * s; y = v->y * s; z = v->z * s; w = c;
    w2 = w * w;
    r[0][0] = w2 + x * x - y * y - z * z;
    r[0][1] = 2.0f * (x * y + z * w);
    r[0][2] = 2.0f * (x * z - y * w);
    r[1][0] = 2.0f * (x * y - z * w);
    r[1][1] = w2 - x * x + y * y - z * z;
    r[1][2] = 2.0f * (y * z + x * w);
    r[2][0] = 2.0f * (x * z + y * w);
    r[2][1] = 2.0f * (y * z - x * w);
    r[2][2] = w2 - x * x - y * y + z * z;
    a = *m;
    {
        float o[12];
        memcpy(o, a, sizeof o);
        for (i = 0; i < 3; i++)
            for (k = 0; k < 3; k++)
                a[i * 4 + k] = o[k] * r[i][0] + o[4 + k] * r[i][1] + o[8 + k] * r[i][2];
    }
}

void njScale(NJS_MATRIX *m, Float sx, Float sy, Float sz)
{
    float *a = *CUR(m);
    int k;
    for (k = 0; k < 3; k++) {
        a[k] *= sx;
        a[4 + k] *= sy;
        a[8 + k] *= sz;
    }
}

void njScaleV(NJS_MATRIX *m, NJS_VECTOR *v) { njScale(m, v->x, v->y, v->z); }

/* rows 0..2 scaled in all four lanes; CN rows 0..2 follow */
void njScaleEx(NJS_VECTOR *v)
{
    float *a = *pNaMatMatrixStuckPtr;
    int k;
    for (k = 0; k < 4; k++) {
        a[k] *= v->x;
        a[4 + k] *= v->y;
        a[8 + k] *= v->z;
    }
    memcpy(vu0.cn, a, 12 * sizeof(float));
}

/* rigid inverse: transpose the rotation (w lanes 0), t' = -(t * R^T) */
Bool njInvertMatrix(NJS_MATRIX *m)
{
    float *a = *CUR(m), o[16];
    int i, k;
    memcpy(o, a, sizeof o);
    for (i = 0; i < 3; i++) {
        for (k = 0; k < 3; k++)
            a[i * 4 + k] = o[k * 4 + i];
        a[i * 4 + 3] = 0.0f;
    }
    for (k = 0; k < 3; k++)
        a[12 + k] = -(o[12] * a[k] + o[13] * a[4 + k] + o[14] * a[8 + k]);
    return TRUE;
}

void njTransposeMatrix(NJS_MATRIX *m)
{
    float *a = *CUR(m), o[12];
    int i, k;
    memcpy(o, a, sizeof o);
    for (i = 0; i < 3; i++)
        for (k = 0; k < 3; k++)
            a[i * 4 + k] = o[k * 4 + i];
}

/* reflection in the plane through (px,py,pz) with normal (vx,vy,vz):
 * m = M * m, with M = I - 2nn^T and translation -2dn, d = -(n.p) */
void njMirror(NJS_MATRIX *m, NJS_PLANE *pl)
{
    NJS_MATRIX mat;
    float nx = pl->vx, ny = pl->vy, nz = pl->vz, l, d;
    float *a = mat;
    m = CUR(m);
    l = sqrtf(nx * nx + ny * ny + nz * nz);
    if (l == 0.0f)
        return;
    nx /= l; ny /= l; nz /= l;
    d = -(nx * pl->px + ny * pl->py + nz * pl->pz);
    a[0] = 1 - 2 * nx * nx; a[1] = -2 * nx * ny; a[2] = -2 * nx * nz; a[3] = 0;
    a[4] = -2 * ny * nx; a[5] = 1 - 2 * ny * ny; a[6] = -2 * ny * nz; a[7] = 0;
    a[8] = -2 * nz * nx; a[9] = -2 * nz * ny; a[10] = 1 - 2 * nz * nz; a[11] = 0;
    a[12] = -2 * d * nx; a[13] = -2 * d * ny; a[14] = -2 * d * nz; a[15] = 1;
    njMultiMatrix(m, &mat);
}

/* ---- points and vectors ----------------------------------------------- */

void njCalcPoint(NJS_MATRIX *m, NJS_POINT3 *ps, NJS_POINT3 *pd)
{
    xform(*CUR(m), &ps->x, vu0.vf18);
    pd->x = vu0.vf18[0];
    pd->y = vu0.vf18[1];
    pd->z = vu0.vf18[2];
}

void njCalcPoints(NJS_MATRIX *m, NJS_POINT3 *ps, NJS_POINT3 *pd, Int num)
{
    float *a = *CUR(m);
    int i;
    for (i = 0; i < num; i++) {
        xform(a, &ps[i].x, vu0.vf18);
        pd[i].x = vu0.vf18[0];
        pd[i].y = vu0.vf18[1];
        pd[i].z = vu0.vf18[2];
    }
}

void njCalcPoint4(NJS_MATRIX *m, NJS_POINT4 *ps, NJS_POINT4 *pd)
{
    xform(*CUR(m), &ps->x, vu0.vf18);
    pd->x = vu0.vf18[0];
    pd->y = vu0.vf18[1];
    pd->z = vu0.vf18[2];
    pd->w = vu0.vf18[3];
}

void njCalcPointCN(NJS_POINT3 *ps, NJS_POINT3 *pd)
{
    xform(vu0.cn, &ps->x, vu0.vf18);
    pd->x = vu0.vf18[0];
    pd->y = vu0.vf18[1];
    pd->z = vu0.vf18[2];
}

void njCalcVector(NJS_MATRIX *m, NJS_VECTOR *vs, NJS_VECTOR *vd)
{
    xform_vec(*CUR(m), &vs->x, vu0.vf18);
    vd->x = vu0.vf18[0];
    vd->y = vu0.vf18[1];
    vd->z = vu0.vf18[2];
}

void njAddVector(NJS_VECTOR *vd, NJS_VECTOR *vs)
{
    vd->x += vs->x;
    vd->y += vs->y;
    vd->z += vs->z;
}

void njSubVector(NJS_VECTOR *vd, NJS_VECTOR *vs)
{
    vd->x -= vs->x;
    vd->y -= vs->y;
    vd->z -= vs->z;
}

Float njScalor2(NJS_VECTOR *v) { return v->x * v->x + v->y * v->y + v->z * v->z; }
Float njScalor(NJS_VECTOR *v) { return sqrtf(njScalor2(v)); }

Float njUnitVector(NJS_VECTOR *v)
{
    float l2 = njScalor2(v), q;
    if (l2 == 0.0f)
        return 0.0f;
    q = 1.0f / sqrtf(l2);
    v->x *= q;
    v->y *= q;
    v->z *= q;
    return l2 * q;
}

Float njInnerProduct(NJS_VECTOR *v1, NJS_VECTOR *v2)
{
    return v1->x * v2->x + v1->y * v2->y + v1->z * v2->z;
}

Float njOuterProduct(NJS_VECTOR *v1, NJS_VECTOR *v2, NJS_VECTOR *ov)
{
    float x = v1->y * v2->z - v2->y * v1->z;
    float y = v1->z * v2->x - v2->z * v1->x;
    float z = v1->x * v2->y - v2->x * v1->y;
    ov->x = x;
    ov->y = y;
    ov->z = z;
    return sqrtf(x * x + y * y + z * z);
}

/* ---- projection --------------------------------------------------------- */

static void persp(const float *p, float *x, float *y, float *iz)
{
    float q = p[2] == 0.0f ? 3.4028235e38f : 1.0f / p[2];
    float k = _nj_screen_.dist * q;
    *x = p[0] * k + fNaViwOffsetX;
    *y = p[1] * k + fNaViwOffsetY;
    *iz = q;
}

void njProjectScreen(NJS_MATRIX *m, NJS_POINT3 *p3, NJS_POINT2 *p2)
{
    NJS_POINT3 pt;
    float iz;
    njMulMatrixCN(&NaViewScreenMatrix, CUR(m));
    njCalcPointCN(p3, &pt);
    {
        float q = vu0.vf18[2] == 0.0f ? 3.4028235e38f : _nj_screen_.dist / vu0.vf18[2];
        p2->x = vu0.vf18[0] * q + fNaViwOffsetX;
        p2->y = vu0.vf18[1] * q + fNaViwOffsetY;
    }
    (void)iz;
}

void njRotTransPers(NJS_POINT3 *pPoint, NJS_SCRVECTOR *pScreen)
{
    njMulMatrixCN(&NaViewScreenMatrix, NULL);
    njCalcPointCN(pPoint, (NJS_POINT3 *)&pScreen->x);
    persp(vu0.vf18, &pScreen->x, &pScreen->y, &pScreen->iz);
    pScreen->fog = njCalcFogPower(pScreen->z);
}

void njPers(NJS_SCRVECTOR *pScreen)
{
    vu0.vf18[0] = pScreen->x;
    vu0.vf18[1] = pScreen->y;
    vu0.vf18[2] = pScreen->z;
    persp(vu0.vf18, &pScreen->x, &pScreen->y, &pScreen->iz);
}

#include <string.h>
/* vu_strip.c: the 3D model pipeline the PS2 ran on its vector units, in C.
 *
 * On the PS2 (vsm/ps2_vu0.vsm, vsm/ps2_vu1.vsm):
 *  - a chunk model's vertex list goes through the VU0 microcode
 *    VU0_CALCPOINT: each vertex is transformed by the current matrix,
 *    projected, lit by up to four lights and written to a VU1_STRIP_BUF
 *    (view position, screen position, 1/z, view normal z, light intensity,
 *    fog);
 *  - the strip chunks copy those vertices and their texture coordinates to
 *    the VU1 microcode (DoubleMain / SingleMain), which colours each vertex
 *    with one of eleven colour routines (light x material diffuse / ambient
 *    / specular), turns it into GS coordinates, cuts the triangles that
 *    cross the view volume and sends them to the GS;
 *  - translucent strips instead build VU1_PRIM_BUF lists in C (colour by
 *    the VU0 versions of the same routines) for Ps2AddPrim3DEx, which puts
 *    them in the sorted draw list (OT).
 *
 * Here: njCnkCvVnPs2 does VU0_CALCPOINT's work; solid strips go straight
 * to the GPU through gs_draw_tris with the view depth as w, so the 3DS GPU
 * clips them against the camera and maps textures with perspective (the
 * VU1 cutting code is not needed); translucent strips build the same
 * VU1_PRIM_BUF lists as the original and go to Ps2AddPrim3DEx. */
#include <stdio.h>
#include <math.h>
#include "types.h"
#include "ps2_NinjaCnk.h"
#include "ps2_Vu1Strip.h"
#include "ps2_NaView.h"
#include "ps2_NaFog.h"
#include "ps2_NaDraw2D.h"
#include "ps2_NaMatrix.h"
#include "ps2_dummy.h"
#include "objitm.h"
#include "plat.h"
#include "rend.h"

unsigned prof_vtx_us, prof_strip_us;     /* 3D time this log period */

/* ps2_NinjaCnk.c (the vertex buffer and clip flag made visible by hostify) */
extern VU1_STRIP_BUF *pNaCnkVerBufTop;
extern unsigned int Vu0ClipFlag;
extern unsigned int ulCnkCurrentDrawMode;
extern unsigned short *uspCnkCrntTexColCalcTbl;
extern CNK_LIGHTING *pNaCnkCrntLighting;
extern unsigned char ucNaCnkAttr;
extern unsigned int ulNaCnkFlagConstAttr;
extern unsigned int ulNaCnkFlagConstMaterial;
extern VU1_COLOR NaCnkConstantMaterial;
extern float fNaCnkConstantA;
extern float fNaCnkAlphaMaterial;
extern void (*pCnkCsVu1FuncTbl[32])(unsigned long long, VU1_STRIP_BUF *, unsigned short, unsigned short);
/* ps2_Vu1Strip.c */
extern void (*pColorCalcFuncTbl[11])(VU1_STRIP_BUF *, VU1_PRIM_BUF *);
extern float fVu1InvNearClip;
/* ps2_NaView.c */
extern float _fNaViwClipNear, _fNaViwClipFar;
void _Make_ClipMatrix(sceVu0FMATRIX sc, float scr, float near, float far);
/* gs.c */
void gs_draw_tris(unsigned int prim, RVert *v, int n);

/* a GIF tag's PRIM field (bits 47..57) as the strip code builds it */
#define TAG_PRIM(p) ((unsigned long long)(p) << 47)
#define PRIM_OF(tag) ((unsigned int)((tag) >> 47) & 0x7FF)
#define TAG_TME TAG_PRIM(0x10)
#define TAG_ABE TAG_PRIM(0x40)

/* ---- the VU1 parameter setters (ps2_Vu1Strip.c; the VU0 register copies
 * they also made are not needed) ------------------------------------------ */
void vu1SetScreenProjection(float fProjection)
{
    fVu1Projection = fProjection;
    _Make_ClipMatrix(ClipMatrix2, fVu1Projection, _fNaViwClipNear, _fNaViwClipFar);
}

void vu1SetNearFarClip(float fNear, float fFar)
{
    fVu1NearClip = fNear;
    fVu1FarClip = fFar;
    fVu1InvNearClip = 1.0f / fVu1NearClip;
    _Make_ClipMatrix(ClipMatrix2, fVu1Projection, _fNaViwClipNear, _fNaViwClipFar);
}

void vu1SetScreenOffset(float fOffsetX, float fOffsetY)
{
    fVu1OffsetX = fOffsetX;
    fVu1OffsetY = fOffsetY;
}

void vu1SetScreenAspect(float fAspectW, float fAspectH)
{
    fVu1AspectW = fAspectW;
    fVu1AspectH = fAspectH;
}

void vu1SetDiffuseMaterial(VU1_COLOR *p)
{
    vu1Diffuse.fR = 128.0f * p->fR;
    vu1Diffuse.fG = 128.0f * p->fG;
    vu1Diffuse.fB = 128.0f * p->fB;
}

void vu1SetSpeculaMaterial(VU1_COLOR *p)
{
    vu1Specula.fR = 31.0f * p->fR;
    vu1Specula.fG = 31.0f * p->fG;
    vu1Specula.fB = 31.0f * p->fB;
}

void vu1SetAmbient(VU1_COLOR *p)
{
    vu1Ambient.fR = p->fR;
    vu1Ambient.fG = p->fG;
    vu1Ambient.fB = p->fB;
}

void vu1SetAlphaRatio(float fAlpha)
{
    fVu1AlphaRatio = 128.0f * fAlpha;
}

/* ---- the colour routines (vu1GetVertexColor*): light intensity in
 * fIr / fIg / fIb, out GS colour (0x80 = 1.0).  As the VU1 microcode does
 * them (the VU0 copies read two registers by mistake). ---------------------- */
static inline float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
static inline float pow17(float v)
{
    float v2 = v * v, v4 = v2 * v2, v8 = v4 * v4;
    return v8 * v8 * v;
}

#define COLOR_FN(name, R, G, B)                                     \
    void name(VU1_STRIP_BUF *s, VU1_PRIM_BUF *p)                    \
    {                                                               \
        float ir = s->fIr, ig = s->fIg, ib = s->fIb;                \
        p->fR = (R); p->fG = (G); p->fB = (B);                      \
        p->fA = fVu1AlphaRatio;                                     \
        (void)ir; (void)ig; (void)ib;                               \
    }

void vu1GetVertexColor(VU1_STRIP_BUF *s, VU1_PRIM_BUF *p)
{
    p->fR = s->fIr * 128.0f;
    p->fG = s->fIg * 128.0f;
    p->fB = s->fIb * 128.0f;
    p->fA = s->fA * 255.0f;
}
COLOR_FN(vu1GetVertexColorCM, vu1Diffuse.fR, vu1Diffuse.fG, vu1Diffuse.fB)
COLOR_FN(vu1GetVertexColorIgnore, 128.0f, 128.0f, 128.0f)
COLOR_FN(vu1GetVertexColorDif,
         clamp01(ir) * vu1Diffuse.fR, clamp01(ig) * vu1Diffuse.fG, clamp01(ib) * vu1Diffuse.fB)
COLOR_FN(vu1GetVertexColorDifAmb,
         clamp01(ir + vu1Ambient.fR) * vu1Diffuse.fR,
         clamp01(ig + vu1Ambient.fG) * vu1Diffuse.fG,
         clamp01(ib + vu1Ambient.fB) * vu1Diffuse.fB)
COLOR_FN(vu1GetVertexColorDifSpe1,
         clamp01(ir) * vu1Diffuse.fR + clamp01(ir - 1.0f) * vu1Specula.fR,
         clamp01(ig) * vu1Diffuse.fG + clamp01(ig - 1.0f) * vu1Specula.fG,
         clamp01(ib) * vu1Diffuse.fB + clamp01(ib - 1.0f) * vu1Specula.fB)
COLOR_FN(vu1GetVertexColorDifSpe2,
         clamp01(ir) * vu1Diffuse.fR + clamp01(ir + vu1Ambient.fR - 1.0f) * vu1Specula.fR,
         clamp01(ig) * vu1Diffuse.fG + clamp01(ig + vu1Ambient.fG - 1.0f) * vu1Specula.fG,
         clamp01(ib) * vu1Diffuse.fB + clamp01(ib + vu1Ambient.fB - 1.0f) * vu1Specula.fB)
COLOR_FN(vu1GetVertexColorDifSpe3,
         clamp01(ir) * vu1Diffuse.fR + clamp01(pow17(ir)) * vu1Specula.fR,
         clamp01(ig) * vu1Diffuse.fG + clamp01(pow17(ig)) * vu1Specula.fG,
         clamp01(ib) * vu1Diffuse.fB + clamp01(pow17(ib)) * vu1Specula.fB)
COLOR_FN(vu1GetVertexColorDifSpe1Amb,
         clamp01(ir + vu1Ambient.fR) * vu1Diffuse.fR + clamp01(ir - 1.0f) * vu1Specula.fR,
         clamp01(ig + vu1Ambient.fG) * vu1Diffuse.fG + clamp01(ig - 1.0f) * vu1Specula.fG,
         clamp01(ib + vu1Ambient.fB) * vu1Diffuse.fB + clamp01(ib - 1.0f) * vu1Specula.fB)
COLOR_FN(vu1GetVertexColorDifSpe2Amb,
         clamp01(ir + vu1Ambient.fR) * vu1Diffuse.fR + clamp01(ir + vu1Ambient.fR - 1.0f) * vu1Specula.fR,
         clamp01(ig + vu1Ambient.fG) * vu1Diffuse.fG + clamp01(ig + vu1Ambient.fG - 1.0f) * vu1Specula.fG,
         clamp01(ib + vu1Ambient.fB) * vu1Diffuse.fB + clamp01(ib + vu1Ambient.fB - 1.0f) * vu1Specula.fB)
COLOR_FN(vu1GetVertexColorDifSpe3Amb,
         clamp01(ir + vu1Ambient.fR) * vu1Diffuse.fR +
             clamp01((ir + vu1Ambient.fR + 1.0f) * pow17(ir + vu1Ambient.fR)) * vu1Specula.fR,
         clamp01(ig + vu1Ambient.fG) * vu1Diffuse.fG +
             clamp01((ig + vu1Ambient.fG + 1.0f) * pow17(ig + vu1Ambient.fG)) * vu1Specula.fG,
         clamp01(ib + vu1Ambient.fB) * vu1Diffuse.fB +
             clamp01((ib + vu1Ambient.fB + 1.0f) * pow17(ib + vu1Ambient.fB)) * vu1Specula.fB)

typedef void (*ColorFn)(VU1_STRIP_BUF *, VU1_PRIM_BUF *);
static inline ColorFn color_fn(unsigned mode)
{
    mode &= 0xF;
    return pColorCalcFuncTbl[mode <= 10 ? mode : 2];
}

/* ---- view-volume test: CLIPw.xyz of [x y z 1] x ClipMatrix2 ------------- */
static inline unsigned clip_flags(float x, float y, float z)
{
    const float *m = (const float *)ClipMatrix2;
    float cx = x * m[0] + y * m[4] + z * m[8] + m[12];
    float cy = x * m[1] + y * m[5] + z * m[9] + m[13];
    float cz = x * m[2] + y * m[6] + z * m[10] + m[14];
    float cw = x * m[3] + y * m[7] + z * m[11] + m[15];
    float aw = cw < 0.0f ? -cw : cw;
    unsigned f = 0;
    if (cx > aw) f |= 0x01;
    if (cx < -aw) f |= 0x02;
    if (cy > aw) f |= 0x04;
    if (cy < -aw) f |= 0x08;
    if (cz > aw) f |= 0x10;
    if (cz < -aw) f |= 0x20;
    return f;
}

int _CVV(float *v0)
{
    return (int)clip_flags(v0[0], v0[1], v0[2]);
}

/* the end of a chunk by its size field (vertex chunks count 32-bit words,
 * strip chunks 16-bit words, after the 4-byte head): where the next chunk
 * starts.  The parse should end there too; a mismatch is logged once. */
static CHUNK_HEAD *chunk_end_long(CHUNK_HEAD *c, const void *parsed)
{
    CHUNK_HEAD *e = (CHUNK_HEAD *)((unsigned int *)c + 1 + c->usSize);
    static int warned;
    if ((const void *)e != parsed && !warned++)
        plat_log("3d: vertex chunk type %u: size says %d bytes, read %d\n", c->ucType,
                 (int)((char *)e - (char *)c), (int)((const char *)parsed - (char *)c));
    return e;
}

static CHUNK_HEAD *chunk_end_short(CHUNK_HEAD *c, const void *parsed)
{
    CHUNK_HEAD *e = (CHUNK_HEAD *)((unsigned short *)c + 2 + c->usSize);
    static int warned;
    if ((const void *)e != parsed && !warned++)
        plat_log("3d: strip chunk type %u: size says %d bytes, read %d\n", c->ucType,
                 (int)((char *)e - (char *)c), (int)((const char *)parsed - (char *)c));
    return e;
}

/* ---- the vertex list (VU0_CALCPOINT) -------------------------------------
 * chunk: head, [ushort first index, ushort count], 14 words, then per
 * vertex x y z - nx ny nz - (8 words) */
static int vu_dump_left;
void vu_dump_begin(void) { vu_dump_left = 16; }

static CHUNK_HEAD *cv_vn_ps2(CHUNK_HEAD *pCnk);
extern int g_skip_draw;
int prof_enter(int b);
void prof_leave(int prev);
extern int prof_on;
CHUNK_HEAD *njCnkCvVnPs2(CHUNK_HEAD *pCnk)
{
    unsigned t0;
    extern int gs_models_dropped(void);
    if (g_skip_draw || gs_models_dropped()) {
        /* frame skip, or a frame buffer the port leaves out (a shadow
         * pass): the model counts as off-screen, its strips are not walked
         * (njCnkDrawModelLocal) */
        Vu0ClipFlag = 0x01;
        return (CHUNK_HEAD *)((unsigned int *)pCnk + 1 + pCnk->usSize);
    }
    t0 = prof_on ? plat_us() : 0;
    int pv = prof_enter(1 /* PB_VTX */);
    CHUNK_HEAD *r = cv_vn_ps2(pCnk);
    prof_leave(pv);
    if (prof_on)
        prof_vtx_us += plat_us() - t0;
    return r;
}

static CHUNK_HEAD *cv_vn_ps2(CHUNK_HEAD *pCnk)
{
    const float *m = *pNaMatMatrixStuckPtr;
    const unsigned short *hdr = (const unsigned short *)&pCnk[1];
    VU1_STRIP_BUF *b = &pNaCnkVerBufTop[hdr[0]];
    unsigned n = hdr[1], i, clipand = 0x3F;
    const float *fp = (const float *)&pCnk[2] + 14;
    CNK_LIGHT *lp = pNaCnkCrntLighting->pLightTop;
    int nl = pNaCnkCrntLighting->lLightMax, l;
    float kx = fVu1AspectW * fVu1Projection, ky = fVu1AspectH * fVu1Projection;
    int use_clip = ((const float *)ClipMatrix2)[0] != 0.0f;
    if (nl > 4)
        nl = 4;
    for (i = 0; i < n; i++, b++, fp += 8) {
        float x = fp[0], y = fp[1], z = fp[2], nx = fp[4], ny = fp[5], nz = fp[6];
        float X = m[0] * x + m[4] * y + m[8] * z + m[12];
        float Y = m[1] * x + m[5] * y + m[9] * z + m[13];
        float Z = m[2] * x + m[6] * y + m[10] * z + m[14];
        float NX = m[0] * nx + m[4] * ny + m[8] * nz;
        float NY = m[1] * nx + m[5] * ny + m[9] * nz;
        float NZ = m[2] * nx + m[6] * ny + m[10] * nz;
        float iz = Z != 0.0f ? 1.0f / Z : 1.0e6f;
        float r = 0.0f, g = 0.0f, bl = 0.0f;
        b->fVx = X;
        b->fVy = Y;
        b->fVz = Z;
        b->fSx = X * kx * iz;
        b->fSy = Y * ky * iz;
        b->fIz = iz;
        b->fNz = NZ;
        for (l = 0; l < nl; l++) {
            const CNK_LIGHT *L = &lp[l];
            float f;
            if (!L->ulState)
                continue;
            if (L->ulMode == 0) {           /* parallel: max(N . C * I, 0) */
                f = (NX * L->fCx + NY * L->fCy + NZ * L->fCz) * L->fI;
                if (f < 0.0f)
                    f = 0.0f;
            } else {                        /* point */
                float lx = L->fCx - X, ly = L->fCy - Y, lz = L->fCz - Z;
                float d2 = lx * lx + ly * ly + lz * lz, dt = lx * NX + ly * NY + lz * NZ;
                if (dt < 0.0f || L->fFarRR < d2 || d2 <= 0.0f)
                    continue;
                f = dt / sqrtf(d2);
                if (L->fNearRR < d2)
                    f *= L->fNearRR / d2;
            }
            r += f * L->fR;
            g += f * L->fG;
            bl += f * L->fB;
        }
        b->fIr = r;
        b->fIg = g;
        b->fIb = bl;
        b->fFog = njCalcFogPowerEx(iz);
        if (vu_dump_left > 0 && (i % 37) == 0) {
            /* a sample of the vertices the frame of a primitive dump lights:
             * where they are, which way they face, what each light gives */
            char line[400];
            int k = snprintf(line, sizeof line, "  vtx (%.1f %.1f %.1f) n (%.2f %.2f %.2f) I %.2f %.2f %.2f fog %.0f |",
                             X, Y, Z, NX, NY, NZ, r, g, bl, b->fFog);
            for (l = 0; l < nl && k < (int)sizeof line - 60; l++) {
                const CNK_LIGHT *L = &lp[l];
                if (!L->ulState)
                    continue;
                if (L->ulMode == 0) {
                    k += snprintf(line + k, sizeof line - k, " L%d dir dot %.2f", l,
                                  NX * L->fCx + NY * L->fCy + NZ * L->fCz);
                } else {
                    float lx = L->fCx - X, ly = L->fCy - Y, lz = L->fCz - Z;
                    float d2 = lx * lx + ly * ly + lz * lz;
                    float cs = d2 > 0.0f ? (lx * NX + ly * NY + lz * NZ) / sqrtf(d2) : 0.0f;
                    k += snprintf(line + k, sizeof line - k, " L%d d %.1f cos %.2f", l, sqrtf(d2), cs);
                }
            }
            plat_log("%s\n", line);
            vu_dump_left--;
        }
        if (use_clip)
            clipand &= clip_flags(X, Y, Z);
    }
    /* every vertex outside the same side of the view volume: the model's
     * strips are skipped (njCnkDrawModelLocal) */
    Vu0ClipFlag = use_clip ? clipand : 0;
    {
        extern unsigned clip_skips[7];
        if (Vu0ClipFlag) {
            int b;
            for (b = 0; b < 6; b++)
                if (Vu0ClipFlag & (1u << b))
                    clip_skips[b]++;
        } else
            clip_skips[6]++;
    }
    return chunk_end_long(pCnk, fp);
}

/* ---- solid strips: straight to the GPU ------------------------------------ */
#define STRIP_MAX 1024
static RVert tri_buf[STRIP_MAX * 3];
static VU1_STRIP_BUF strip_buf[STRIP_MAX];
static unsigned st_models, st_tris;

static inline unsigned pack_rgba(const VU1_PRIM_BUF *c)
{
    float f[4] = { c->fR, c->fG, c->fB, c->fA };
    unsigned out = 0, k;
    for (k = 0; k < 4; k++) {
        float v = f[k];
        unsigned u = v <= 0.0f ? 0u : (v >= 255.0f ? 255u : (unsigned)v);
        out |= u << (k * 8);
    }
    return out;
}

/* visibility assist (the brightness steps above 5, set by the platform):
 * lit surfaces get at least some light and the fog is thinner.  The game's
 * rooms are lit for a TV: near-black walls and thick fog are unreadable on
 * the 3DS screen.  Level 0 is the PS2's picture. */
static float assist_floor, assist_fog = 1.0f;
void vu_set_assist(int level)
{
    static const float fl[4] = { 0.0f, 0.25f, 0.45f, 0.65f };
    static const float fg[4] = { 1.0f, 0.60f, 0.30f, 0.0f };
    if (level < 0)
        level = 0;
    if (level > 3)
        level = 3;
    assist_floor = fl[level];
    assist_fog = fg[level];
}

static inline void assist_colour(VU1_PRIM_BUF *c, unsigned mode)
{
    float r, g, b;
    mode &= 0xF;
    if (assist_floor <= 0.0f || mode == 1 || mode == 2)    /* constant / unlit colours */
        return;
    if (mode == 0) {
        r = g = b = assist_floor * 128.0f;
    } else {
        r = assist_floor * vu1Diffuse.fR;
        g = assist_floor * vu1Diffuse.fG;
        b = assist_floor * vu1Diffuse.fB;
    }
    if (c->fR < r) c->fR = r;
    if (c->fG < g) c->fG = g;
    if (c->fB < b) c->fB = b;
}

static inline float assist_fogf(float f255)
{
    return 255.0f - (255.0f - f255) * assist_fog;
}

static inline void make_vert(RVert *o, const VU1_STRIP_BUF *s, ColorFn cf, unsigned mode)
{
    VU1_PRIM_BUF c;
    float vz = s->fVz, sx = s->fSx, sy = s->fSy, iz = s->fIz;
    cf((VU1_STRIP_BUF *)s, &c);
    assist_colour(&c, mode);
    if (vz < 1.0e-3f && vz > -1.0e-3f) {
        /* on the camera plane: keep the screen position finite (the GPU
         * clips the triangle there anyway) */
        float kx = fVu1AspectW * fVu1Projection, ky = fVu1AspectH * fVu1Projection;
        vz = vz < 0.0f ? -1.0e-3f : 1.0e-3f;
        iz = 1.0f / vz;
        sx = s->fVx * kx * iz;
        sy = s->fVy * ky * iz;
    }
    o->x = fVu1OffsetX + sx;                    /* GS pixels */
    o->y = fVu1OffsetY + sy;
    o->z = -Ps2_zbuff_a - Ps2_zbuff_b * iz;     /* GS z (not clamped: the GPU clips) */
    o->w = vz;                                  /* view depth: perspective + clipping */
    o->u = s->fU;
    o->v = s->fV;
    o->rgba = pack_rgba(&c);
    o->fog = assist_fogf(s->fFog) * (1.0f / 255.0f);
}

/* a texture the game pages in on use must be in GS memory first */
static int texture_ready(unsigned long long ulType)
{
    TIM2_PICTUREHEADER_EX *timp;
    if (!(ulType & TAG_TME))
        return 1;
    if (Ps2_now_tex == NULL)
        return 0;
    timp = (TIM2_PICTUREHEADER_EX *)Ps2_now_tex->texinfo.texsurface.pSurface;
    if (timp != NULL && timp->TpFlag != 0) {
        Ps2_tex_load_tp_cancel = 1;
        Ps2TexLoad(Ps2_now_tex);
        Ps2_tex_load_tp_cancel = 0;
    }
    return 1;
}

/* n strip vertices (UVs in fU / fV) as a GS triangle strip; single: drop
 * the triangles that face away (all three view normals pointing away, as
 * the PS2 code tests) */
static int tri_n;                   /* triangles' vertices waiting in tri_buf */
static unsigned long long tri_type;

static void tri_flush(void)
{
    if (tri_n) {
        gs_draw_tris(PRIM_OF(tri_type), tri_buf, tri_n);
        st_tris += (unsigned)tri_n / 3;
        tri_n = 0;
    }
}

/* the strip's triangles go into tri_buf; a model chunk's strips all share
 * one GS state, so they are drawn together (tri_flush) */
static void strip_direct(unsigned long long ulType, const VU1_STRIP_BUF *s, int n, unsigned mode, int single)
{
    static RVert v[STRIP_MAX];
    ColorFn cf = color_fn(mode);
    int i;
    if (n < 3)
        return;
    if (n > STRIP_MAX)
        n = STRIP_MAX;
    if (tri_n && (tri_type != ulType || tri_n + (n - 2) * 3 > STRIP_MAX * 3))
        tri_flush();
    tri_type = ulType;
    for (i = 0; i < n; i++)
        make_vert(&v[i], &s[i], cf, mode);
    for (i = 2; i < n; i++) {
        if (single && s[i - 2].fNz > 0.0f && s[i - 1].fNz > 0.0f && s[i].fNz > 0.0f)
            continue;
        tri_buf[tri_n++] = v[i - 2];
        tri_buf[tri_n++] = v[i - 1];
        tri_buf[tri_n++] = v[i];
    }
}

void vu1DrawTriangleStripOpaqueSingle(unsigned long long ulType, VU1_STRIP_BUF *pS, unsigned short usStripMax, unsigned short usMode)
{
    if (texture_ready(ulType)) {
        strip_direct(ulType, pS, usStripMax, usMode, 1);
        tri_flush();
    }
}

void vu1DrawTriangleStripOpaqueDouble(unsigned long long ulType, VU1_STRIP_BUF *pS, unsigned short usStripMax, unsigned short usMode)
{
    if (texture_ready(ulType)) {
        strip_direct(ulType, pS, usStripMax, usMode, 0);
        tri_flush();
    }
}

/* ---- translucent strips: VU1_PRIM_BUF lists for the sorted draw list ---- */
static void strip_trans(unsigned long long ulType, VU1_STRIP_BUF *pS, int n, unsigned mode, int single)
{
    VU1_PRIM_BUF *pP = (VU1_PRIM_BUF *)Ps2_DRAW_TMP;
    ColorFn cf = color_fn(mode);
    int i, max = (int)(sizeof Ps2_DRAW_TMP / sizeof(VU1_PRIM_BUF));
    if (n > max)
        n = max;
    for (i = 0; i < n; i++, pP++) {
        VU1_STRIP_BUF *s = &pS[i];
        float iz = s->fIz;
        cf(s, pP);
        assist_colour(pP, mode);
        pP->fS = s->fU * iz;
        pP->fT = s->fV * iz;
        pP->fQ = iz;
        pP->ulKick = 0;
        pP->fX = fVu1OffsetX + s->fSx;
        pP->fY = fVu1OffsetY + s->fSy;
        pP->fZ = s->fVz;
        pP->fF = assist_fogf(s->fFog);
        if (single && i >= 2 && pS[i - 2].fNz > 0.0f && pS[i - 1].fNz > 0.0f && s->fNz > 0.0f)
            pP->ulKick = 0x8000;
    }
    Ps2AddPrim3DEx(ulType, Ps2_DRAW_TMP, (unsigned)n);
}

void vu1DrawTriangleStripTransSingle(unsigned long long ulType, VU1_STRIP_BUF *pS, unsigned short usStripMax, unsigned short usMode)
{
    strip_trans(ulType, pS, usStripMax, usMode, 1);
}

void vu1DrawTriangleStripTransDouble(unsigned long long ulType, VU1_STRIP_BUF *pS, unsigned short usStripMax, unsigned short usMode)
{
    strip_trans(ulType, pS, usStripMax, usMode, 0);
}

void vu1DrawTriangleStripTransDoubleI(unsigned long long ulType, VU1_STRIP_BUF *pS, unsigned short usStripMax, unsigned short usMode)
{
    strip_trans(ulType, pS, usStripMax, usMode, 0);
}

/* ---- the textured strip chunks (njCnkCsUvh: UVs in 1/1023, njCnkCsUvn:
 * in 1/255).  Per strip: a count (negative: the other winding), then per
 * vertex an index into the vertex buffer and U, V. ------------------------ */
/* the per-chunk vertex cache of strip_chunk, and one strip's vertices */
#define VC_N 4096
static RVert vc[VC_N];
static unsigned vc_stamp[VC_N], vc_gen;
static RVert sv[STRIP_MAX];
static unsigned char snz[STRIP_MAX];        /* faces away (single-sided strips) */

/* a strip (sv, snz) as triangles into tri_buf */
static void strip_tris(unsigned long long ulType, int n, int single)
{
    int i;
    if (n < 3)
        return;
    if (tri_n && (tri_type != ulType || tri_n + (n - 2) * 3 > STRIP_MAX * 3))
        tri_flush();
    tri_type = ulType;
    for (i = 2; i < n; i++) {
        if (single && snz[i - 2] && snz[i - 1] && snz[i])
            continue;
        tri_buf[tri_n++] = sv[i - 2];
        tri_buf[tri_n++] = sv[i - 1];
        tri_buf[tri_n++] = sv[i];
    }
}

/* ---- the fast path: each vertex a strip chunk uses is lit and turned
 * into the GPU's format once (gv[], stamped with the chunk), and each
 * triangle written once, straight into the frame's vertex ring
 * (rend_tris_ptr).  The 3DS's memory is slow: the old way copied every
 * vertex of every triangle five times on its way to the GPU. */
static RGpuVtx gv[VC_N];
static unsigned char gnz[VC_N];             /* faces away (single-sided strips) */
static unsigned gv_stamp[VC_N], gv_gen;
static unsigned short ri[STRIP_MAX];        /* one strip: vertex indices, UVs */
static float ru[STRIP_MAX], rvv[STRIP_MAX];

/* exp(-d * x) for the mist, from a table (x in steps of 4, up to 2048;
 * beyond that the mist is complete): expf per vertex was a library call */
static float mist_tab[514], mist_tab_d = -1.0f;
static float mist_exp(float d, float x)
{
    float t;
    int i;
    if (d != mist_tab_d) {
        for (i = 0; i < 514; i++)
            mist_tab[i] = expf(-d * (float)i * 4.0f);
        mist_tab_d = d;
    }
    t = x * 0.25f;
    if (t >= 512.0f)
        return mist_tab[512];
    i = (int)t;
    t -= (float)i;
    return mist_tab[i] + (mist_tab[i + 1] - mist_tab[i]) * t;
}

static void gpu_vert(RGpuVtx *g, const VU1_STRIP_BUF *src, ColorFn cf, unsigned mode, const GsXf *xf)
{
    RVert r;
    float f;
    make_vert(&r, src, cf, mode);
    g->x = r.x - xf->ox;
    g->y = r.y - xf->oy;
    g->z = r.z * (2.0f / 65535.0f) - 1.0f;
    g->w = r.w != 0.0f ? r.w : 1.0f;
    g->u = g->v = 0.0f;
    g->c[0] = (uint8_t)(r.rgba & 0xFF);
    g->c[1] = (uint8_t)((r.rgba >> 8) & 0xFF);
    g->c[2] = (uint8_t)((r.rgba >> 16) & 0xFF);
    g->c[3] = (uint8_t)(r.rgba >> 24);
    f = xf->fog_game ? r.fog : 1.0f;
    if (xf->mist_d > 0.0f && r.w > xf->mist_start)
        f *= mist_exp(xf->mist_d, r.w - xf->mist_start);
    g->pad = f;
}

static const RGpuVtx *gpu_vert_of(unsigned idx, ColorFn cf, unsigned mode, const GsXf *xf)
{
    static RGpuVtx tmp[4];
    static int t;
    if (idx < VC_N) {
        if (gv_stamp[idx] != gv_gen) {
            gpu_vert(&gv[idx], &pNaCnkVerBufTop[idx], cf, mode, xf);
            gnz[idx] = pNaCnkVerBufTop[idx].fNz > 0.0f;
            gv_stamp[idx] = gv_gen;
        }
        return &gv[idx];
    }
    t = (t + 1) & 3;
    gpu_vert(&tmp[t], &pNaCnkVerBufTop[idx], cf, mode, xf);
    return &tmp[t];
}

/* one strip's triangles (ri, ru, rvv; k vertices) into the vertex ring */
static unsigned emit_strip(int k, int single, ColorFn cf, unsigned mode, const RState *rs, const GsXf *xf)
{
    RGpuVtx *o;
    unsigned n = 0;
    int i;
    if (k < 3)
        return 0;
    const RGpuVtx *a, *b, *c;
    int za, zb, zc;
    o = rend_tris_ptr(rs, (k - 2) * 3);
    if (!o)
        return 0;
    /* each triangle shares two vertices with the one before: carried on */
    a = gpu_vert_of(ri[0], cf, mode, xf);
    za = ri[0] < VC_N && gnz[ri[0]];
    b = gpu_vert_of(ri[1], cf, mode, xf);
    zb = ri[1] < VC_N && gnz[ri[1]];
    for (i = 2; i < k; i++, a = b, za = zb, b = c, zb = zc) {
        c = gpu_vert_of(ri[i], cf, mode, xf);
        zc = ri[i] < VC_N && gnz[ri[i]];
        if (single && za && zb && zc)
            continue;
        o[0] = *a; o[0].u = ru[i - 2]; o[0].v = rvv[i - 2];
        o[1] = *b; o[1].u = ru[i - 1]; o[1].v = rvv[i - 1];
        o[2] = *c; o[2].u = ru[i]; o[2].v = rvv[i];
        if (!xf->gouraud) {                 /* flat: the third vertex's colour */
            memcpy(o[0].c, c->c, 4);
            memcpy(o[1].c, c->c, 4);
        }
        o += 3;
        n++;
    }
    rend_tris_commit((int)n * 3);
    return n;
}

static CHUNK_HEAD *strip_chunk_fast(CHUNK_HEAD *pCnk, unsigned short *us, unsigned nstrip, float uvscale,
                                    ColorFn cf, unsigned mode, int single, const RState *rs, const GsXf *xf)
{
    unsigned ntris = 0;
    if (++gv_gen == 0) {
        memset(gv_stamp, 0, sizeof gv_stamp);
        gv_gen = 1;
    }
    for (; nstrip; nstrip--) {
        unsigned short head = *us++;
        int n = (head & 0x8000) ? (unsigned short)(~head + 1) : head, i, k = 0;
        for (i = 0; i < n; i++, us += 3) {
            if (k == STRIP_MAX) {           /* very long strip: continue it */
                ntris += emit_strip(k, single, cf, mode, rs, xf);
                ri[0] = ri[k - 2]; ru[0] = ru[k - 2]; rvv[0] = rvv[k - 2];
                ri[1] = ri[k - 1]; ru[1] = ru[k - 1]; rvv[1] = rvv[k - 1];
                k = 2;
            }
            ri[k] = us[0];
            ru[k] = uvscale * us[1];
            rvv[k] = uvscale * us[2];
            k++;
        }
        ntris += emit_strip(k, single, cf, mode, rs, xf);
    }
    gs_tris_count((int)ntris, xf->tme);
    st_tris += ntris;
    return chunk_end_short(pCnk, us);
}

static CHUNK_HEAD *strip_chunk(CHUNK_HEAD *pCnk, float uvscale)
{
    unsigned char flag = ulNaCnkFlagConstAttr ? ucNaCnkAttr : pCnk->ucHeadBits;
    unsigned long long ulType = TAG_PRIM(SCE_GS_PRIM_TRISTRIP | 0x10 | 0x20);   /* textured, fog */
    unsigned short *us = (unsigned short *)&pCnk[1];
    unsigned short cc, nstrip;
    void (*func)(unsigned long long, VU1_STRIP_BUF *, unsigned short, unsigned short);
    if (flag & 0x08)
        ulType |= TAG_ABE;                      /* alpha blend */
    if (!(flag & 0x20))
        ulType |= TAG_PRIM(0x08);               /* gouraud */
    if (ulNaCnkFlagConstMaterial) {
        vu1SetDiffuseMaterial(&NaCnkConstantMaterial);
        vu1SetAlphaRatio(fNaCnkConstantA);
        cc = 1;
    } else {
        vu1SetDiffuseMaterial(pNaCnkCrntLighting->pDiffuse);
        vu1SetSpeculaMaterial(pNaCnkCrntLighting->pSpecula);
        vu1SetAmbient(pNaCnkCrntLighting->pAmbient);
        vu1SetAlphaRatio(fNaCnkAlphaMaterial);
        cc = uspCnkCrntTexColCalcTbl[flag & 7];
    }
    func = pCnkCsVu1FuncTbl[(flag & 0x18) | ulCnkCurrentDrawMode];
    nstrip = *us++ & 0x3FFF;
    st_models++;
    if (ulDrawGeneralPurposeWater || (func != vu1DrawTriangleStripTransDouble &&
                                      func != vu1DrawTriangleStripTransDoubleI)) {
        /* the VU1 path: drawn at once.  A vertex several strips share is
         * lit and converted once per chunk (vc[], stamped with the chunk);
         * each use only sets its own UV */
        int single = func == vu1DrawTriangleStripTransSingle || func == vu1DrawTriangleStripOpaqueSingle;
        int ok = texture_ready(ulType), fast;
        ColorFn cf = color_fn(cc);
        RState rs;
        GsXf xf;
        if (ok) {
            tri_flush();                        /* anything queued goes first */
            fast = gs_tris_begin(PRIM_OF(ulType), &rs, &xf);
            if (fast == 0)
                ok = 0;                         /* nothing drawn (a left-out buffer, frame skip) */
            else if (fast > 0)
                return strip_chunk_fast(pCnk, us, nstrip, uvscale, cf, cc, single, &rs, &xf);
        }
        if (!ok) {
            for (; nstrip; nstrip--) {
                unsigned short head = *us++;
                int n = (head & 0x8000) ? (unsigned short)(~head + 1) : head;
                us += 3 * n;
            }
            return chunk_end_short(pCnk, us);
        }
        if (++vc_gen == 0) {
            memset(vc_stamp, 0, sizeof vc_stamp);
            vc_gen = 1;
        }
        for (; nstrip; nstrip--) {
            unsigned short head = *us++;
            int n = (head & 0x8000) ? (unsigned short)(~head + 1) : head, i, k = 0;
            for (i = 0; i < n; i++, us += 3) {
                unsigned idx = us[0];
                if (k == STRIP_MAX) {           /* very long strip: continue it */
                    strip_tris(ulType, k, single);
                    sv[0] = sv[k - 2]; snz[0] = snz[k - 2];
                    sv[1] = sv[k - 1]; snz[1] = snz[k - 1];
                    k = 2;
                }
                if (idx < VC_N) {
                    if (vc_stamp[idx] != vc_gen) {
                        make_vert(&vc[idx], &pNaCnkVerBufTop[idx], cf, cc);
                        vc_stamp[idx] = vc_gen;
                    }
                    sv[k] = vc[idx];
                } else {
                    make_vert(&sv[k], &pNaCnkVerBufTop[idx], cf, cc);
                }
                sv[k].u = uvscale * us[1];
                sv[k].v = uvscale * us[2];
                snz[k] = pNaCnkVerBufTop[idx].fNz > 0.0f;
                k++;
            }
            strip_tris(ulType, k, single);
        }
        tri_flush();
    } else {
        /* translucent, two-sided: in pieces of up to 60 vertices (two shared
         * with the next piece), each skipped when it lies wholly outside
         * one side of the view volume */
        for (; nstrip; nstrip--) {
            unsigned short head = *us++;
            unsigned short clip = head & 0x8000;
            int num = clip ? (unsigned short)(~head + 1) : head;
            for (;;) {
                int max = num, i, last = 1;
                unsigned cflag = 0x3F;
                if (max > 60) {
                    num -= 58;
                    max = 60;
                    last = 0;
                }
                for (i = 0; i < max; i++, us += 3) {
                    VU1_STRIP_BUF *pV = &pNaCnkVerBufTop[us[0]];
                    strip_buf[i] = *pV;
                    strip_buf[i].fU = uvscale * us[1];
                    strip_buf[i].fV = uvscale * us[2];
                    cflag &= (unsigned)_CVV(&pV->fVx);
                }
                if (cflag == 0)
                    func(ulType, strip_buf, (unsigned short)max, (unsigned short)(clip | cc));
                if (last)
                    break;
                us -= 6;
            }
        }
    }
    return chunk_end_short(pCnk, us);
}

static CHUNK_HEAD *timed_strip(CHUNK_HEAD *pCnk, float uvscale)
{
    unsigned t0;
    if (g_skip_draw)
        return (CHUNK_HEAD *)((unsigned short *)pCnk + 2 + pCnk->usSize);
    t0 = prof_on ? plat_us() : 0;
    int pv = prof_enter(2 /* PB_STRIP */);
    CHUNK_HEAD *r = strip_chunk(pCnk, uvscale);
    prof_leave(pv);
    if (prof_on)
        prof_strip_us += plat_us() - t0;
    return r;
}
CHUNK_HEAD *njCnkCsUvh(CHUNK_HEAD *pCnk) { return timed_strip(pCnk, 1.0f / 1023.0f); }
CHUNK_HEAD *njCnkCsUvn(CHUNK_HEAD *pCnk) { return timed_strip(pCnk, 1.0f / 255.0f); }

/* for the log: the lights, materials and fog the models are drawn with */
extern CNK_LIGHTING NaCnkLighting[4];
extern unsigned int ulNaFogState;
extern float fNaFogNear, fNaFogFar, fNaFogDensity;
extern unsigned int ulNaFogR, ulNaFogG, ulNaFogB;
void vu_light_log(void)
{
    int m, l;
    plat_log("[light] fog %s near %.1f far %.1f density %.2f colour %02X%02X%02X, clip near %.2f far %.1f, draw mode %u\n",
             ulNaFogState ? "on" : "off", fNaFogNear, fNaFogFar, fNaFogDensity, ulNaFogR, ulNaFogG, ulNaFogB,
             _fNaViwClipNear, _fNaViwClipFar, ulCnkCurrentDrawMode);
    for (m = 0; m < 4; m++) {
        const CNK_LIGHTING *L = &NaCnkLighting[m];
        plat_log("[light] mode %d: %d lights, diffuse %.2f %.2f %.2f ambient %.2f %.2f %.2f\n", m, L->lLightMax,
                 L->pDiffuse ? L->pDiffuse->fR : -1.f, L->pDiffuse ? L->pDiffuse->fG : -1.f,
                 L->pDiffuse ? L->pDiffuse->fB : -1.f, L->pAmbient ? L->pAmbient->fR : -1.f,
                 L->pAmbient ? L->pAmbient->fG : -1.f, L->pAmbient ? L->pAmbient->fB : -1.f);
        for (l = 0; l < L->lLightMax && l < 4 && L->pLightTop; l++) {
            const CNK_LIGHT *g = &L->pLightTop[l];
            plat_log("[light]   %d: on %u mode %u I %.2f rgb %.2f %.2f %.2f C %.1f %.1f %.1f W %.1f %.1f %.1f near %.1f far %.1f\n",
                     l, g->ulState, g->ulMode, g->fI, g->fR, g->fG, g->fB, g->fCx, g->fCy, g->fCz,
                     g->fWx, g->fWy, g->fWz, sqrtf(g->fNearRR), sqrtf(g->fFarRR));
        }
    }
}

/* for the log: models and triangles since the last call */
void vu_strip_stats(unsigned *models, unsigned *tris)
{
    *models = st_models;
    *tris = st_tris;
    st_models = st_tris = 0;
}

/* chunks the game passes over (njCnkDefaultLong): each type once in the
 * log, all of them counted for the primitive dump */
static unsigned cnk_skips[256];
void cnk_skip_note(int type, int size)
{
    if (!cnk_skips[type & 255]++)
        plat_log("[cnk] chunk type %d (size %d) not drawn\n", type, size);
}

unsigned clip_skips[7];
void cnk_skip_log(void)
{
    int i;
    plat_log("  [clip] since the last dump: models drawn %u; left out wholly right %u left %u below %u above %u far %u near %u\n",
             clip_skips[6], clip_skips[0], clip_skips[1], clip_skips[2], clip_skips[3], clip_skips[4], clip_skips[5]);
    memset(clip_skips, 0, sizeof clip_skips);
    for (i = 0; i < 256; i++)
        if (cnk_skips[i])
            plat_log("  [cnk] type %d passed over %u times\n", i, cnk_skips[i]);
}

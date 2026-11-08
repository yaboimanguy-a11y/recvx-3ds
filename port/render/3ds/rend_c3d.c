/* rend_c3d.c: the GS emulation's GPU backend (port/ps2/rend.h) on citro3d.
 *
 * The game's 640 x 480 frame is drawn into a canvas (bottom-left of a
 * 1024 x 512 render-target texture, picture row y at memory row 32 + y, as
 * the PICA stores textures bottom row first), then scaled onto the top
 * screen.  Primitives with the same state are batched into one draw.
 *
 * GS colour arithmetic: vertex colour 0x80 = 1.0, so the texture combiner
 * doubles RGB and alpha; texture alpha (0x80 = 1.0) is doubled when the
 * texture is uploaded.  Blending (A - B) * C + D maps onto PICA blend
 * factors for the combinations the game uses. */
#include <3ds.h>
#include <citro3d.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "rend.h"
#include "../../platform/3ds/ui_3ds.h"
#include "plat.h"

extern const unsigned char r2d_shbin[];
extern const unsigned int r2d_shbin_size;

#define CANVAS_W 640
#define CANVAS_H 480
#define MAX_TEX 1024
#define VRING_BYTES (2u << 20)
#define NRING 3                 /* frame being recorded, frame being drawn, frame the GPU may still read */
#define MAX_DEAD 1024

typedef RGpuVtx RVtx;          /* rend.h: x, y, z, w, u, v, colour bytes, fog */

static struct {
    shaderProgram_s prog;
    DVLB_s *dvlb;
    int u_scale, u_rotx, u_roty, u_uvscale;
    C3D_Tex canvas;
    C3D_RenderTarget *canvas_rt, *top_rt;
    C3D_Tex white;
    C3D_Tex fogramp;        /* 256 x 8 grey ramp: fog factor -> colour */
    uint8_t *vring[NRING];
    size_t vused;
    uint32_t frame;
    uint32_t tgen;          /* texture generation: a new one at each frame start and hand-over
                             * (a texture of an earlier one may be on the GPU: patched as a copy) */
    int in_frame;
    struct { C3D_Tex *t; float su, sv; uint32_t fresh; uint8_t full; } tex[MAX_TEX];   /* fresh: frame + 1 when made; full: RGBA8 from now on */
    struct { C3D_Tex *t; uint32_t frame; } dead[MAX_DEAD];
    int ndead;
    RVtx *batch;            /* the batch being built: in the frame's vertex ring, at vused */
    int nb;
    RState bs;
    int have_bs;
    int draws, tris;
    int stretch;
    int bright;             /* present brightness level 0..5 */
    int vw, vh;             /* the drawing target's size (canvas: 640 x 480) */
    int cur_target;         /* -1: the canvas */
    struct { int used; unsigned fbp; C3D_Tex tex; C3D_RenderTarget *rt; int handle, w, h; } tg[2];
    float ssx, ssy;         /* GS pixels -> target pixels (targets can be smaller) */
    float sox;              /* x offset of the drawing area (the right eye's half) */
    int u_eye;
    C3D_RenderTarget *top_rt_r; /* the right eye */
    int stereo_on;          /* 3D shown this frame */
    float eye_shift;        /* the current eye's shift (NDC units, at infinity) */
} R;

/* the frame's draws, recorded and replayed at the end (once, or once per
 * eye with the 3D slider up): the state, the texture as it was when drawn,
 * the target and the vertices (already in the frame's vertex ring) */
typedef struct {
    RState s;
    C3D_Tex *tex;
    float su, sv;
    int target;
    uint8_t *vb;
    int n;
} Cmd;
#define MAX_CMD 8192
static Cmd cmdbuf[2][MAX_CMD];     /* recorded (game thread) / replayed (render thread) */
static Cmd *cmds = cmdbuf[0];
static int ncmd, cmd_side;

/* the GPU side's own state (the render thread), apart from the recording */
static struct {
    int vw, vh;             /* the drawing target's size */
    int cur_target;         /* -1: the canvas */
    float ssx, ssy, sox;    /* GS pixels -> target pixels; the eye's x offset */
    float eye_shift;
    int stereo_on;
} P;

/* one frame handed to the render thread */
static struct {
    const Cmd *cmds;
    int ncmd;
    uint8_t *vring;
    size_t vused;
} J;
#define STEREO_CONV 35.0f     /* view depth at the screen plane (the room's middle) */
#define STEREO_SEP 0.05f      /* far separation per eye (NDC) at full slider */
#define STEREO_POP 0.25f      /* nearer things come out at most this part of it */

static uint8_t morton[64];

static void init_morton(void)
{
    int x, y;
    for (y = 0; y < 8; y++)
        for (x = 0; x < 8; x++)
            morton[y * 8 + x] = (uint8_t)((x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) |
                                          ((x & 4) << 2) | ((y & 4) << 3));
}

static inline uint32_t tile_index(uint32_t x, uint32_t y, uint32_t pw)
{
    return (((y >> 3) * (pw >> 3) + (x >> 3)) << 6) | morton[((y & 7) << 3) | (x & 7)];
}

static int pow2(int v)
{
    int p = 8;
    while (p < v && p < 1024)
        p <<= 1;
    return p;
}

static void put_pixels(C3D_Tex *t, int w, int h, const uint32_t *rgba);

int rend_init(void)
{
    C3D_DEPTHTYPE depth24 = { .__i = GPU_RB_DEPTH24_STENCIL8 };
    memset(&R, 0, sizeof R);
    init_morton();
    R.dvlb = DVLB_ParseFile((u32 *)r2d_shbin, r2d_shbin_size);
    if (!R.dvlb)
        return -1;
    shaderProgramInit(&R.prog);
    shaderProgramSetVsh(&R.prog, &R.dvlb->DVLE[0]);
    R.u_scale = shaderInstanceGetUniformLocation(R.prog.vertexShader, "scale");
    R.u_rotx = shaderInstanceGetUniformLocation(R.prog.vertexShader, "rotx");
    R.u_roty = shaderInstanceGetUniformLocation(R.prog.vertexShader, "roty");
    R.u_uvscale = shaderInstanceGetUniformLocation(R.prog.vertexShader, "uvscale");
    R.u_eye = shaderInstanceGetUniformLocation(R.prog.vertexShader, "eye");
    if (!C3D_TexInitVRAM(&R.canvas, 1024, 512, GPU_RGBA8))
        return -2;
    R.canvas_rt = C3D_RenderTargetCreateFromTex(&R.canvas, GPU_TEXFACE_2D, 0, depth24);
    {
        C3D_DEPTHTYPE nodepth = { .__i = -1 };
        const u32 xfer = GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |
            GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) |
            GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO);
        R.top_rt = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, nodepth);
        R.top_rt_r = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, nodepth);
        if (!R.canvas_rt || !R.top_rt)
            return -3;
        C3D_RenderTargetSetOutput(R.top_rt, GFX_TOP, GFX_LEFT, xfer);
        if (R.top_rt_r)
            C3D_RenderTargetSetOutput(R.top_rt_r, GFX_TOP, GFX_RIGHT, xfer);
    }
    (void)depth24;
    if (!C3D_TexInit(&R.white, 8, 8, GPU_RGBA8))
        return -4;
    memset(R.white.data, 0xFF, R.white.size);
    C3D_TexFlush(&R.white);
    if (!C3D_TexInit(&R.fogramp, 256, 8, GPU_RGBA8))
        return -6;
    {
        static uint32_t ramp[256 * 8];
        int x, y;
        for (y = 0; y < 8; y++)
            for (x = 0; x < 256; x++)
                ramp[y * 256 + x] = (uint32_t)x | ((uint32_t)x << 8) | ((uint32_t)x << 16) | 0x7F000000u;
        put_pixels(&R.fogramp, 256, 8, ramp);
        C3D_TexSetFilter(&R.fogramp, GPU_LINEAR, GPU_LINEAR);
        C3D_TexSetWrap(&R.fogramp, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    }
    R.vw = CANVAS_W;
    R.vh = CANVAS_H;
    R.ssx = R.ssy = 1.0f;
    R.cur_target = -1;
    P.vw = CANVAS_W;
    P.vh = CANVAS_H;
    P.ssx = P.ssy = 1.0f;
    P.cur_target = -1;
    gfxSet3D(true);
    R.vring[0] = linearAlloc(VRING_BYTES);
    R.vring[1] = linearAlloc(VRING_BYTES);
    R.vring[2] = linearAlloc(VRING_BYTES);
    if (!R.vring[0] || !R.vring[1])
        return -5;
    return 0;
}

void rend_set_stretch(int on) { R.stretch = on; }
void rend_set_brightness(int level) { R.bright = level; }
static uint32_t mist_clear = 0x000000FF;    /* canvas clear for 3D frames, 0xRRGGBBAA */
void rend_set_mist(uint32_t rgb, int on)
{
    mist_clear = on ? ((rgb & 0xFF) << 24 | ((rgb >> 8) & 0xFF) << 16 | ((rgb >> 16) & 0xFF) << 8 | 0xFF)
                    : 0x000000FF;
}
void rend_stats(int *draws, int *tris) { *draws = R.draws; *tris = R.tris; }

static void apply_state(const RState *s, C3D_Tex *tex, float su, float sv);
static void bind_pipeline(int canvas);
static inline void put(RVtx *o, const RVert *v, float u, float vv);

/* ---- the bottom screen: drawn by citro2d (port/platform/3ds/ui_3ds.c) */
void rend_bot_tri(float x0, float y0, float x1, float y1, float x2, float y2, uint32_t rgba)
{
    ui_radar_tri(x0, y0, x1, y1, x2, y2, rgba);
}

/* ---- textures -------------------------------------------------------------- */
/* GS alpha (0..0x80) of a texel: which GPU format can hold a picture.
 * 0: opaque (RGB565), 1: on/off (RGBA5551), 2: smooth alpha (RGBA8) */
#define A_FULL 0x7C
static int alpha_class(const uint32_t *rgba, int stride, int x0, int y0, int w, int h)
{
    int x, y, k = 0;
    for (y = y0; y < y0 + h; y++) {
        const uint32_t *s = rgba + (size_t)y * stride;
        for (x = x0; x < x0 + w; x++) {
            uint32_t a = s[x] >> 24;
            if (a >= A_FULL)
                continue;
            if (a)
                return 2;
            k = 1;
        }
    }
    return k;
}

static int fmt_class(GPU_TEXCOLOR f)
{
    return f == GPU_RGB565 ? 0 : f == GPU_RGBA5551 ? 1 : 2;
}

/* texels (0xAABBGGRR, GS alpha) into the tiled texture; rgba[y * stride + x] */
static void store_rect(C3D_Tex *t, const uint32_t *rgba, int stride, int x0, int y0, int w, int h)
{
    int x, y;
    for (y = y0; y < y0 + h; y++) {
        uint32_t my = (uint32_t)(t->height - 1 - y);
        const uint32_t *s = rgba + (size_t)y * stride;
        uint32_t rowbase = ((my >> 3) * (t->width >> 3)) << 6;
        const uint8_t *mrow = morton + ((my & 7) << 3);
        if (t->fmt == GPU_RGBA8) {
            uint32_t *d = (uint32_t *)t->data + rowbase;
            for (x = x0; x < x0 + w; x++) {
                uint32_t c = s[x];
                uint32_t a = (c >> 24) * 2;
                if (a > 255)
                    a = 255;
                /* PICA RGBA8 word: R << 24 | G << 16 | B << 8 | A */
                d[(((uint32_t)x >> 3) << 6) + mrow[x & 7]] = (__builtin_bswap32(c) & 0xFFFFFF00u) | a;
            }
        } else if (t->fmt == GPU_RGB565) {
            uint16_t *d = (uint16_t *)t->data + rowbase;
            for (x = x0; x < x0 + w; x++) {
                uint32_t c = s[x];
                d[(((uint32_t)x >> 3) << 6) + mrow[x & 7]] =
                    (uint16_t)(((c << 8) & 0xF800) | ((c >> 5) & 0x07E0) | ((c >> 19) & 0x001F));
            }
        } else {
            uint16_t *d = (uint16_t *)t->data + rowbase;
            for (x = x0; x < x0 + w; x++) {
                uint32_t c = s[x];
                d[(((uint32_t)x >> 3) << 6) + mrow[x & 7]] =
                    (uint16_t)(((c << 8) & 0xF800) | ((c >> 5) & 0x07C0) | ((c >> 18) & 0x003E) |
                               ((c >> 24) >= A_FULL ? 1 : 0));
            }
        }
    }
}

static void put_pixels(C3D_Tex *t, int w, int h, const uint32_t *rgba)
{
    store_rect(t, rgba, w, 0, 0, w, h);
    C3D_TexFlush(t);
}

static void retire(C3D_Tex *t)
{
    if (!t)
        return;
    if (R.ndead == MAX_DEAD) {
        C3D_TexDelete(t);   /* better than leaking */
        free(t);
        return;
    }
    R.dead[R.ndead].t = t;
    R.dead[R.ndead].frame = R.frame;
    R.ndead++;
}

static void free_dead(void)
{
    int i, k = 0;
    for (i = 0; i < R.ndead; i++) {
        if (R.dead[i].frame + 2 < R.frame) {      /* the render thread and the GPU are done with it */
            C3D_TexDelete(R.dead[i].t);
            free(R.dead[i].t);
        } else {
            R.dead[k++] = R.dead[i];
        }
    }
    R.ndead = k;
}

static C3D_Tex *tex_make(int w, int h, const uint32_t *rgba, float *su, float *sv, int full)
{
    int pw = pow2(w), ph = pow2(h);
    C3D_Tex *t = malloc(sizeof *t);
    if (!t)
        return NULL;
    static const GPU_TEXCOLOR fmts[3] = { GPU_RGB565, GPU_RGBA5551, GPU_RGBA8 };
    if (!C3D_TexInit(t, (u16)pw, (u16)ph, fmts[full ? 2 : alpha_class(rgba, w, 0, 0, w, h)])) {
        free(t);
        return NULL;
    }
    if (w != pw || h != ph)
        memset(t->data, 0, t->size);
    put_pixels(t, w, h, rgba);
    *su = (float)w / pw;
    *sv = (float)h / ph;
    return t;
}

int rend_tex_create(int w, int h, const uint32_t *rgba)
{
    int i;
    for (i = 0; i < MAX_TEX; i++)
        if (!R.tex[i].t) {
            R.tex[i].full = 0;
            R.tex[i].t = tex_make(w, h, rgba, &R.tex[i].su, &R.tex[i].sv, 0);
            R.tex[i].fresh = R.tgen + 1;
            return R.tex[i].t ? i : -1;
        }
    return -1;
}

void rend_tex_update(int handle, const uint32_t *rgba)
{
    C3D_Tex *old;
    int w, h;
    if (handle < 0 || handle >= MAX_TEX || !R.tex[handle].t)
        return;
    old = R.tex[handle].t;
    w = (int)(R.tex[handle].su * old->width + 0.5f);
    h = (int)(R.tex[handle].sv * old->height + 0.5f);
    /* the GPU may still read the old one this frame: replace it */
    R.tex[handle].t = tex_make(w, h, rgba, &R.tex[handle].su, &R.tex[handle].sv, R.tex[handle].full);
    R.tex[handle].fresh = R.tgen + 1;
    retire(old);
}

int rend_tex_patch(int handle, const uint32_t *rgba, int stride, int x0, int y0, int w, int h)
{
    C3D_Tex *t;
    uint8_t *d;
    int ty0, ty1, bpp;
    if (handle < 0 || handle >= MAX_TEX || !R.tex[handle].t || w <= 0 || h <= 0)
        return 0;
    t = R.tex[handle].t;
    /* new alpha the texture's format cannot hold: the caller makes it
     * again, as RGBA8 from now on (a page the game keeps reloading would
     * otherwise be made again and again) */
    if (fmt_class(t->fmt) < 2 &&
        alpha_class(rgba, stride, x0, y0, w, h) > fmt_class(t->fmt)) {
        R.tex[handle].full = 1;
        return -1;
    }
    if (R.tex[handle].fresh != R.tgen + 1) {
        /* the GPU may still read this one for the frame before: work on a
         * copy (once a frame; later patches this frame go into the copy) */
        C3D_Tex *n = malloc(sizeof *n);
        if (!n)
            return 0;
        if (!C3D_TexInit(n, t->width, t->height, t->fmt)) {
            free(n);
            return 0;
        }
        memcpy(n->data, t->data, t->size);
        retire(t);
        R.tex[handle].t = t = n;
        R.tex[handle].fresh = R.tgen + 1;
        C3D_TexFlush(t);
    }
    store_rect(t, rgba, stride, x0, y0, w, h);
    /* flush the 8-row tile strips that changed */
    d = (uint8_t *)t->data;
    bpp = t->fmt == GPU_RGBA8 ? 4 : 2;
    ty0 = (t->height - (y0 + h)) >> 3;
    ty1 = (t->height - 1 - y0) >> 3;
    GSPGPU_FlushDataCache(d + (size_t)ty0 * t->width * 8 * bpp,
                          (uint32_t)(ty1 - ty0 + 1) * t->width * 8 * bpp);
    return 0;
}

unsigned rend_tex_bytes(int handle)
{
    return handle >= 0 && handle < MAX_TEX && R.tex[handle].t ? R.tex[handle].t->size : 0;
}

/* little linear memory left (textures live there) */
unsigned rend_linear_free_kb(void) { return (unsigned)(linearSpaceFree() >> 10); }

int rend_mem_low(void)
{
    return linearSpaceFree() < (3u << 20);
}

void rend_tex_free(int handle)
{
    if (handle < 0 || handle >= MAX_TEX || !R.tex[handle].t)
        return;
    retire(R.tex[handle].t);
    R.tex[handle].t = NULL;
}

/* ---- state --------------------------------------------------------------------- */
static void set_blend(const RState *s)
{
    GPU_BLENDFACTOR src = GPU_SRC_ALPHA, dst = GPU_ONE_MINUS_SRC_ALPHA;
    GPU_BLENDEQUATION eq = GPU_BLEND_ADD;
    GPU_BLENDFACTOR ca = GPU_SRC_ALPHA, cia = GPU_ONE_MINUS_SRC_ALPHA;
    if (!s->abe) {
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
        return;
    }
    if (s->c == 2) {
        unsigned f = s->fix * 2u;
        C3D_BlendingColor((f > 255 ? 255 : f) << 24);
        ca = GPU_CONSTANT_ALPHA;
        cia = GPU_ONE_MINUS_CONSTANT_ALPHA;
    } else if (s->c == 1) {
        ca = GPU_DST_ALPHA;
        cia = GPU_ONE_MINUS_DST_ALPHA;
    }
    /* (A - B) * C + D */
    if (s->a == 0 && s->b == 1 && s->d == 1) { src = ca; dst = cia; }                  /* mix */
    else if (s->a == 0 && s->b == 2 && s->d == 1) { src = ca; dst = GPU_ONE; }          /* add */
    else if (s->a == 2 && s->b == 0 && s->d == 1) {                                      /* subtract */
        src = ca; dst = GPU_ONE; eq = GPU_BLEND_REVERSE_SUBTRACT;
    } else if (s->a == 0 && s->b == 2 && s->d == 2) { src = ca; dst = GPU_ZERO; }       /* Cs * C */
    else if (s->a == 1 && s->b == 0 && s->d == 0) {                                      /* Cd + (Cd - Cs) C */
        src = cia; dst = ca;
    } else if (s->a == 2 && s->b == 2 && s->d == 1) { src = GPU_ZERO; dst = GPU_ONE; }  /* keep Cd */
    else if (s->a == 1 && s->b == 2 && s->d == 0) { src = GPU_ONE; dst = ca; eq = GPU_BLEND_ADD; }
    C3D_AlphaBlend(eq, GPU_BLEND_ADD, src, dst, GPU_ONE, GPU_ZERO);
}

static void apply_state(const RState *s, C3D_Tex *tex, float su, float sv)
{
    static const GPU_TESTFUNC at[8] = { GPU_NEVER, GPU_ALWAYS, GPU_LESS, GPU_LEQUAL,
                                        GPU_EQUAL, GPU_GEQUAL, GPU_GREATER, GPU_NOTEQUAL };
    static const GPU_TESTFUNC zt[4] = { GPU_NEVER, GPU_ALWAYS, GPU_GEQUAL, GPU_GREATER };
    C3D_TexEnv *env;
    int i;
    set_blend(s);
    if (s->ate && s->afail == 0) {
        unsigned ref = s->aref * 2u;
        C3D_AlphaTest(true, at[s->atst & 7], ref > 255 ? 255 : (int)ref);
    } else {
        C3D_AlphaTest(false, GPU_ALWAYS, 0);
    }
    if (s->zte && P.cur_target < 0)
        C3D_DepthTest(true, zt[s->ztst & 3], s->zmsk ? GPU_WRITE_COLOR : GPU_WRITE_ALL);
    else
        C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    {
        int x0 = s->sc_x0, x1 = s->sc_x1 + 1, y0 = s->sc_y0, y1 = s->sc_y1 + 1;
        if (x1 > P.vw) x1 = P.vw;
        if (y1 > P.vh) y1 = P.vh;
        if (x0 <= 0 && y0 <= 0 && x1 >= P.vw && y1 >= P.vh && P.sox == 0.0f && P.ssx == 1.0f)
            C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
        else
            C3D_SetScissor(GPU_SCISSOR_NORMAL, (u32)(x0 * P.ssx + P.sox), (u32)((P.vh - y1) * P.ssy),
                           (u32)(x1 * P.ssx + P.sox), (u32)((P.vh - y0) * P.ssy));
    }
    env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    if (tex) {
        C3D_Tex *t = tex;
        GPU_TEXTURE_FILTER_PARAM f = s->linear ? GPU_LINEAR : GPU_NEAREST;
        C3D_TexSetFilter(t, f, f);
        C3D_TexSetWrap(t, s->clamp_s ? GPU_CLAMP_TO_EDGE : GPU_REPEAT,
                       s->clamp_t ? GPU_CLAMP_TO_EDGE : GPU_REPEAT);
        C3D_TexBind(0, t);
        C3D_FVUnifSet(GPU_VERTEX_SHADER, R.u_uvscale, su, sv, 0.f, 0.f);
        if (s->tfx == 1) {                       /* decal */
            C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
            C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
        } else {                                 /* modulate (highlight approximated) */
            C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
            C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);
            C3D_TexEnvScale(env, C3D_Both, GPU_TEVSCALE_2);
        }
    } else {
        C3D_TexBind(0, &R.white);
        C3D_FVUnifSet(GPU_VERTEX_SHADER, R.u_uvscale, 1.f, 1.f, 0.f, 0.f);
        C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
        C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
        C3D_TexEnvScale(env, C3D_Both, GPU_TEVSCALE_2);
    }
    i = 1;
    if (s->fge) {
        /* GS fog: colour = f * colour + (1 - f) * fog colour, f per vertex.
         * f comes in as texcoord1 and reads a grey ramp texture, so the
         * GPU interpolates it like the GS does */
        C3D_TexEnv *fe = C3D_GetTexEnv(1);
        C3D_TexBind(1, &R.fogramp);
        C3D_TexEnvInit(fe);
        C3D_TexEnvSrc(fe, C3D_RGB, GPU_PREVIOUS, GPU_CONSTANT, GPU_TEXTURE1);
        C3D_TexEnvFunc(fe, C3D_RGB, GPU_INTERPOLATE);
        C3D_TexEnvColor(fe, 0xFF000000u | (s->fogcol & 0xFFFFFF));
        C3D_TexEnvSrc(fe, C3D_Alpha, GPU_PREVIOUS, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
        C3D_TexEnvFunc(fe, C3D_Alpha, GPU_REPLACE);
        i = 2;
    }
    for (; i < 6; i++)
        C3D_TexEnvInit(C3D_GetTexEnv(i));
    C3D_FVUnifSet(GPU_VERTEX_SHADER, R.u_eye, s->stereo ? P.eye_shift : 0.0f, STEREO_CONV, -STEREO_POP, 0.f);
}

static void bind_pipeline(int canvas)
{
    C3D_AttrInfo *ai;
    C3D_BindProgram(&R.prog);
    ai = C3D_GetAttrInfo();
    AttrInfo_Init(ai);
    AttrInfo_AddLoader(ai, 0, GPU_FLOAT, 4);
    AttrInfo_AddLoader(ai, 1, GPU_FLOAT, 2);
    AttrInfo_AddLoader(ai, 2, GPU_UNSIGNED_BYTE, 4);
    AttrInfo_AddLoader(ai, 3, GPU_FLOAT, 1);
    C3D_DepthMap(true, -1.0f, 0.0f);
    C3D_CullFace(GPU_CULL_NONE);
    if (canvas) {
        C3D_FVUnifSet(GPU_VERTEX_SHADER, R.u_scale, 2.f / P.vw, -2.f / P.vh, -1.f, 1.f);
        C3D_FVUnifSet(GPU_VERTEX_SHADER, R.u_rotx, 1.f, 0.f, 0.f, 0.f);
        C3D_FVUnifSet(GPU_VERTEX_SHADER, R.u_roty, 0.f, 1.f, 0.f, 0.f);
    } else {
        C3D_FVUnifSet(GPU_VERTEX_SHADER, R.u_scale, 2.f / 400.f, -2.f / 240.f, -1.f, 1.f);
        C3D_FVUnifSet(GPU_VERTEX_SHADER, R.u_rotx, 0.f, 1.f, 0.f, 0.f);
        C3D_FVUnifSet(GPU_VERTEX_SHADER, R.u_roty, -1.f, 0.f, 0.f, 0.f);
    }
    C3D_FVUnifSet(GPU_VERTEX_SHADER, R.u_eye, 0.f, STEREO_CONV, -STEREO_POP, 0.f);
}

/* the batch (built in place in the frame's vertex ring) as a command,
 * drawn at the end of the frame */
static void flush(void)
{
    size_t bytes;
    Cmd *c;
    if (!R.nb || !R.in_frame) {
        R.nb = 0;
        return;
    }
    if (ncmd == MAX_CMD) {          /* too many: dropped */
        R.nb = 0;
        return;
    }
    bytes = sizeof(RVtx) * (size_t)R.nb;
    c = &cmds[ncmd++];
    c->s = R.bs;
    c->tex = NULL;
    c->su = c->sv = 1.0f;
    if (R.bs.tex >= 0 && R.bs.tex < MAX_TEX && R.tex[R.bs.tex].t) {
        c->tex = R.tex[R.bs.tex].t;
        c->su = R.tex[R.bs.tex].su;
        c->sv = R.tex[R.bs.tex].sv;
    }
    c->target = R.cur_target;
    c->vb = (uint8_t *)R.batch;
    c->n = R.nb;
    R.draws++;
    R.tris += R.nb / 3;
    R.vused += (bytes + 15) & ~(size_t)15;
    R.batch = (RVtx *)(R.vring[R.frame % NRING] + R.vused);
    R.nb = 0;
}

/* room in the vertex ring for k more vertices of this batch */
static inline int room(int k)
{
    if (R.vused + sizeof(RVtx) * (size_t)(R.nb + k) <= VRING_BYTES)
        return 1;
    flush();
    return R.vused + sizeof(RVtx) * (size_t)k <= VRING_BYTES;
}

/* triangles written straight into the vertex ring by the caller (the
 * models, vu_strip.c): up to n vertices at the pointer, then
 * rend_tris_commit with how many were written */
RGpuVtx *rend_tris_ptr(const RState *s, int n)
{
    if (!R.in_frame)
        return NULL;
    if (!R.have_bs || memcmp(s, &R.bs, sizeof *s) != 0) {
        flush();
        R.bs = *s;
        R.have_bs = 1;
    }
    if (!room(n))
        return NULL;
    return (RGpuVtx *)&R.batch[R.nb];
}

void rend_tris_commit(int n)
{
    R.nb += n;
}

static inline void put(RVtx *o, const RVert *v, float u, float vv)
{
    o->x = v->x;
    o->y = v->y;
    o->z = v->z * 2.0f - 1.0f;
    o->w = v->w != 0.0f ? v->w : 1.0f;
    o->u = u;
    o->v = vv;
    o->c[0] = (uint8_t)(v->rgba & 0xFF);
    o->c[1] = (uint8_t)((v->rgba >> 8) & 0xFF);
    o->c[2] = (uint8_t)((v->rgba >> 16) & 0xFF);
    o->c[3] = (uint8_t)(v->rgba >> 24);
    o->pad = v->fog;                /* the shader passes it on as texcoord1 */
}

static void quad(const RVert *a, const RVert *b, float x0, float y0, float x1, float y1,
                 float u0, float v0, float u1, float v1)
{
    RVert c0 = *b, c1 = *b, c2 = *b, c3 = *b;
    c0.x = x0; c0.y = y0; c1.x = x1; c1.y = y0; c2.x = x0; c2.y = y1; c3.x = x1; c3.y = y1;
    (void)a;
    put(&R.batch[R.nb++], &c0, u0, v0);
    put(&R.batch[R.nb++], &c1, u1, v0);
    put(&R.batch[R.nb++], &c2, u0, v1);
    put(&R.batch[R.nb++], &c1, u1, v0);
    put(&R.batch[R.nb++], &c3, u1, v1);
    put(&R.batch[R.nb++], &c2, u0, v1);
}

void rend_draw(const RState *s, int prim, const RVert *v, int n)
{
    if (!R.in_frame)
        return;
    if (!R.have_bs || memcmp(s, &R.bs, sizeof *s) != 0) {
        flush();
        R.bs = *s;
        R.have_bs = 1;
    }
    if (!room(12))
        return;
    if (prim == RPRIM_TRI && n >= 3) {
        put(&R.batch[R.nb++], &v[0], v[0].u, v[0].v);
        put(&R.batch[R.nb++], &v[1], v[1].u, v[1].v);
        put(&R.batch[R.nb++], &v[2], v[2].u, v[2].v);
    } else if (prim == RPRIM_SPRITE && n >= 2) {
        quad(&v[0], &v[1], v[0].x, v[0].y, v[1].x, v[1].y, v[0].u, v[0].v, v[1].u, v[1].v);
    } else if (prim == RPRIM_LINE && n >= 2) {
        float dx = v[1].x - v[0].x, dy = v[1].y - v[0].y;
        float ox = (dy > dx || -dy > dx) ? 0.5f : 0.0f, oy = ox ? 0.0f : 0.5f;
        RVert a = v[0], b = v[1];
        RVert a2 = a, b2 = b;
        a.x -= ox; a.y -= oy; b.x -= ox; b.y -= oy;
        a2.x += ox; a2.y += oy; b2.x += ox; b2.y += oy;
        put(&R.batch[R.nb++], &a, a.u, a.v);
        put(&R.batch[R.nb++], &b, b.u, b.v);
        put(&R.batch[R.nb++], &a2, a2.u, a2.v);
        put(&R.batch[R.nb++], &b, b.u, b.v);
        put(&R.batch[R.nb++], &b2, b2.u, b2.v);
        put(&R.batch[R.nb++], &a2, a2.u, a2.v);
    } else if (prim == RPRIM_POINT && n >= 1) {
        quad(&v[0], &v[0], v[0].x, v[0].y, v[0].x + 1, v[0].y + 1, v[0].u, v[0].v, v[0].u, v[0].v);
    }
}

void rend_draw_tris(const RState *s, const RVert *v, int n)
{
    int i;
    if (!R.in_frame)
        return;
    if (!R.have_bs || memcmp(s, &R.bs, sizeof *s) != 0) {
        flush();
        R.bs = *s;
        R.have_bs = 1;
    }
    for (i = 0; i + 2 < n; i += 3) {
        if (!room(3))
            return;
        put(&R.batch[R.nb++], &v[i], v[i].u, v[i].v);
        put(&R.batch[R.nb++], &v[i + 1], v[i + 1].u, v[i + 1].v);
        put(&R.batch[R.nb++], &v[i + 2], v[i + 2].u, v[i + 2].v);
    }
}

/* ---- frame ------------------------------------------------------------------ */
void rend_frame_begin(void)
{
    if (R.in_frame)
        return;
    R.frame++;
    R.tgen++;
    free_dead();
    R.vused = 0;
    R.nb = 0;
    R.batch = (RVtx *)R.vring[R.frame % NRING];
    R.have_bs = 0;
    R.draws = R.tris = 0;
    R.in_frame = 1;
    ncmd = 0;
    R.cur_target = -1;
    R.vw = CANVAS_W;
    R.vh = CANVAS_H;
}

/* ---- off-screen targets: GS frame buffers the game draws into and then
 * uses as a texture (shadows, the scrolling intro text) ------------------ */
int rend_target_handle(unsigned fbp, int w, int *tex_w, int *tex_h)
{
    int i, k = -1, tw = w <= 512 ? 512 : 1024;     /* the GS area it covers */
    for (i = 0; i < 2; i++)
        if (R.tg[i].used && R.tg[i].fbp == fbp) {
            *tex_w = R.tg[i].w;
            *tex_h = R.tg[i].h;
            return R.tg[i].handle;
        }
    for (i = 0; i < 2; i++)
        if (!R.tg[i].used) {
            k = i;
            break;
        }
    if (k < 0)
        return -1;
    /* 512 x 512 texels whatever the area (graphics memory is short): a
     * 1024-wide buffer is drawn at half its width */
    if (!C3D_TexInitVRAM(&R.tg[k].tex, 512, 512, GPU_RGBA8) &&
        !C3D_TexInit(&R.tg[k].tex, 512, 512, GPU_RGBA8)) {
        plat_log("rend: no memory for an off-screen target\n");
        return -1;
    }
    R.tg[k].rt = NULL;              /* made by the render thread (draw_on) */
    for (i = 0; i < MAX_TEX; i++)
        if (!R.tex[i].t)
            break;
    if (i == MAX_TEX) {
        C3D_TexDelete(&R.tg[k].tex);
        return -1;
    }
    R.tex[i].t = &R.tg[k].tex;
    R.tex[i].su = R.tex[i].sv = 1.0f;
    R.tex[i].fresh = 0;
    R.tg[k].used = 1;
    R.tg[k].fbp = fbp;
    R.tg[k].handle = i;
    R.tg[k].w = tw;
    R.tg[k].h = 512;
    *tex_w = tw;
    *tex_h = 512;
    return i;
}

/* the following draws go into a target (its texture handle) or the canvas (-1) */
void rend_use_target(int handle)
{
    int i, k = -1;
    if (!R.in_frame)
        rend_frame_begin();
    for (i = 0; i < 2; i++)
        if (R.tg[i].used && R.tg[i].handle == handle)
            k = i;
    if (handle >= 0 && k < 0)
        return;
    if (k == R.cur_target)
        return;
    flush();
    R.have_bs = 0;
    R.cur_target = k;
    R.vw = k < 0 ? CANVAS_W : R.tg[k].w;
    R.vh = k < 0 ? CANVAS_H : R.tg[k].h;
}

/* drawing on the GPU: target k (-1 canvas), eye 0 (mono / left) or 1 */
static void draw_on(int k, int eye, int stereo)
{
    if (k < 0) {
        C3D_FrameDrawOn(R.canvas_rt);
        P.vw = CANVAS_W;
        P.vh = CANVAS_H;
        if (stereo) {                   /* each eye: 320 x 240, side by side */
            P.ssx = P.ssy = 0.5f;
            P.sox = eye ? 320.0f : 0.0f;
            C3D_SetViewport(eye ? 320 : 0, 0, 320, 240);
        } else {
            P.ssx = P.ssy = 1.0f;
            P.sox = 0.0f;
            C3D_SetViewport(0, 0, CANVAS_W, CANVAS_H);
        }
    } else {
        if (!R.tg[k].rt) {
            C3D_DEPTHTYPE nodepth = { .__i = -1 };
            R.tg[k].rt = C3D_RenderTargetCreateFromTex(&R.tg[k].tex, GPU_TEXFACE_2D, 0, nodepth);
            if (R.tg[k].rt)
                C3D_RenderTargetClear(R.tg[k].rt, C3D_CLEAR_COLOR, 0, 0);
        }
        if (!R.tg[k].rt)
            return;
        C3D_FrameDrawOn(R.tg[k].rt);
        P.vw = R.tg[k].w;
        P.vh = R.tg[k].h;
        P.ssx = 512.0f / (float)P.vw;
        P.ssy = 512.0f / (float)P.vh;
        P.sox = 0.0f;
        C3D_SetViewport(0, 0, 512, 512);
    }
    P.cur_target = k;
    bind_pipeline(1);
}

/* the recorded draws for one eye; off-screen targets are drawn in the first
 * pass only */
static void replay(int eye, int stereo)
{
    int i, cur = -2;
    C3D_BufInfo *bi;
    P.eye_shift = stereo ? (eye ? 1.0f : -1.0f) * STEREO_SEP * osGet3DSliderState() : 0.0f;
    const Cmd *last = NULL;
    for (i = 0; i < J.ncmd; i++) {
        const Cmd *c = &J.cmds[i];
        if (eye && c->target >= 0)
            continue;
        if (c->target != cur) {
            draw_on(c->target, eye, stereo);
            cur = c->target;
            last = NULL;
        }
        /* the GPU state only when it changes (most batches in a row share
         * blending, tests and texture stages) */
        if (!last || c->tex != last->tex || c->su != last->su || c->sv != last->sv ||
            memcmp(&c->s, &last->s, sizeof c->s) != 0)
            apply_state(&c->s, c->tex, c->su, c->sv);
        last = c;
        bi = C3D_GetBufInfo();
        BufInfo_Init(bi);
        BufInfo_Add(bi, c->vb, sizeof(RVtx), 4, 0x3210);
        C3D_DrawArrays(GPU_TRIANGLES, 0, c->n);
    }
    if (cur != -1)
        draw_on(-1, eye, stereo);
}

/* the canvas onto the top screen: 4:3 (320 x 240) or stretched (400 x 240) */
static void present(int eye, int stereo)
{
    RVtx q[6];
    float x0 = g_ui.stretch ? 0.f : 40.f, x1 = g_ui.stretch ? 400.f : 360.f;
    /* the canvas part to show: all of 640 x 480, or the eye's 320 x 240 */
    float u0 = stereo ? (eye ? 320.f : 0.f) / 1024.f : 0.f;
    float u1 = stereo ? (eye ? 640.f : 320.f) / 1024.f : (float)CANVAS_W / 1024.f;
    float vtop = (stereo ? 240.f : (float)CANVAS_H) / 512.f;
    RVert a;
    memset(&a, 0, sizeof a);
    a.rgba = 0x80808080u;
    a.z = 0.0f;
    C3D_FrameDrawOn(eye ? R.top_rt_r : R.top_rt);
    P.ssx = P.ssy = 1.0f;
    P.sox = 0.0f;
    P.cur_target = -1;
    bind_pipeline(0);
    C3D_TexSetFilter(&R.canvas, GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetWrap(&R.canvas, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    {
        RState s;
        memset(&s, 0, sizeof s);
        s.tex = -1;
        s.sc_x1 = 1023; s.sc_y1 = 1023;
        apply_state(&s, NULL, 1.f, 1.f);
    }
    C3D_TexBind(0, &R.canvas);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, R.u_uvscale, 1.f, 1.f, 0.f, 0.f);
    {
        /* brightness: out = lerp(x, 1 - (1 - x)^p, k), which raises the
         * dark and middle tones and keeps black and white.  Level 0 is the
         * PS2 picture; the game's rooms are made for a TV and are very dark
         * on the 3DS screen. */
        static const struct { int p4; uint8_t k; } lv[6] = {
            { 0, 0 }, { 0, 0x40 }, { 0, 0x99 }, { 0, 0xFF }, { 1, 0xB3 }, { 1, 0xFF } };
        int L = R.bright < 0 ? 0 : (R.bright > 5 ? 5 : R.bright), st = 0;
        C3D_TexEnv *env = C3D_GetTexEnv(st++);
        C3D_TexEnvInit(env);
        if (L == 0) {
            C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
            C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
        } else {
            /* (1 - x)^2 */
            C3D_TexEnvSrc(env, C3D_RGB, GPU_TEXTURE0, GPU_TEXTURE0, GPU_PRIMARY_COLOR);
            C3D_TexEnvOpRgb(env, GPU_TEVOP_RGB_ONE_MINUS_SRC_COLOR, GPU_TEVOP_RGB_ONE_MINUS_SRC_COLOR,
                            GPU_TEVOP_RGB_SRC_COLOR);
            C3D_TexEnvFunc(env, C3D_RGB, GPU_MODULATE);
            C3D_TexEnvSrc(env, C3D_Alpha, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
            C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
            if (lv[L].p4) {                       /* squared again: (1 - x)^4 */
                env = C3D_GetTexEnv(st++);
                C3D_TexEnvInit(env);
                C3D_TexEnvSrc(env, C3D_RGB, GPU_PREVIOUS, GPU_PREVIOUS, GPU_PRIMARY_COLOR);
                C3D_TexEnvFunc(env, C3D_RGB, GPU_MODULATE);
                C3D_TexEnvSrc(env, C3D_Alpha, GPU_PREVIOUS, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
                C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
            }
            /* k * (1 - prev) + (1 - k) * x */
            env = C3D_GetTexEnv(st++);
            C3D_TexEnvInit(env);
            C3D_TexEnvSrc(env, C3D_RGB, GPU_PREVIOUS, GPU_TEXTURE0, GPU_CONSTANT);
            C3D_TexEnvOpRgb(env, GPU_TEVOP_RGB_ONE_MINUS_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR,
                            GPU_TEVOP_RGB_SRC_COLOR);
            C3D_TexEnvFunc(env, C3D_RGB, GPU_INTERPOLATE);
            C3D_TexEnvColor(env, 0xFF000000u | lv[L].k * 0x010101u);
            C3D_TexEnvSrc(env, C3D_Alpha, GPU_PREVIOUS, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
            C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
        }
        for (; st < 6; st++)
            C3D_TexEnvInit(C3D_GetTexEnv(st));
    }
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    a.x = x0; a.y = 0; put(&q[0], &a, u0, vtop);
    a.x = x1; a.y = 0; put(&q[1], &a, u1, vtop);
    a.x = x0; a.y = 240; put(&q[2], &a, u0, 0);
    q[3] = q[1];
    a.x = x1; a.y = 240; put(&q[4], &a, u1, 0);
    q[5] = q[2];
    if (J.vused + sizeof q <= VRING_BYTES) {
        uint8_t *dst = J.vring + J.vused;
        C3D_BufInfo *bi;
        memcpy(dst, q, sizeof q);
        J.vused += (sizeof q + 15) & ~(size_t)15;
        bi = C3D_GetBufInfo();
        BufInfo_Init(bi);
        BufInfo_Add(bi, dst, sizeof(RVtx), 4, 0x3210);
        C3D_DrawArrays(GPU_TRIANGLES, 0, 6);
    }
    {
        int st;
        for (st = 1; st < 6; st++)      /* the brightness stages are for this pass only */
            C3D_TexEnvInit(C3D_GetTexEnv(st));
    }
}

/* ---- the render thread: the GPU work of a frame (core 2 on the New 3DS),
 * while the game goes on with the next one ------------------------------ */
static void render_job(void)
{
    int stereo, i;
    /* the 3D slider: both eyes drawn, else the one picture (full size) */
    stereo = R.top_rt_r && osGet3DSliderState() > 0.01f;
    if (stereo != P.stereo_on) {
        gfxSet3D(stereo);
        P.stereo_on = stereo;
    }
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    {
        /* a frame with the 3D scene: the background is the mist */
        uint32_t cc = 0x000000FF;
        for (i = 0; i < J.ncmd; i++)
            if (J.cmds[i].s.stereo && J.cmds[i].s.mist) {
                cc = mist_clear;
                break;
            }
        C3D_RenderTargetClear(R.canvas_rt, C3D_CLEAR_ALL, cc, 0);
    }
    C3D_RenderTargetClear(R.top_rt, C3D_CLEAR_COLOR, 0x000000FF, 0);
    if (stereo)
        C3D_RenderTargetClear(R.top_rt_r, C3D_CLEAR_COLOR, 0x000000FF, 0);
    replay(0, stereo);
    if (stereo)
        replay(1, stereo);
    present(0, stereo);
    if (stereo)
        present(1, stereo);
    ui_render();                /* the bottom screen */
    if (J.vused)
        GSPGPU_FlushDataCache(J.vring, J.vused);
    C3D_FrameEnd(0);
}

static Thread rthr;
static LightSemaphore rjob, rdone;
static volatile int rbusy, rquit;

static void render_thread(void *arg)
{
    (void)arg;
    for (;;) {
        LightSemaphore_Acquire(&rjob, 1);
        if (rquit)
            break;
        render_job();
        LightSemaphore_Release(&rdone, 1);
    }
}

/* the render thread has finished the frame it was given */
void rend_wait_idle(void)
{
    int ms;
    if (!rbusy)
        return;
    /* at most a second: a thread stuck on a GPU the system took back
     * (the app closing from the HOME menu) must not hold the app open */
    for (ms = 0; ms < 1000; ms++) {
        if (LightSemaphore_TryAcquire(&rdone, 1) == 0)
            break;
        svcSleepThread(1000000);
    }
    if (ms == 1000)
        plat_log("renderer: the frame on its thread did not finish (closing?)\n");
    rbusy = 0;
}

void rend_start_thread(void)
{
    s32 prio = 0x30;
    LightSemaphore_Init(&rjob, 0, 1);
    LightSemaphore_Init(&rdone, 0, 1);
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    /* core 2 (New 3DS), below the audio thread there; without it the frame
     * is drawn on the game's thread as before */
    rthr = threadCreate(render_thread, NULL, 64 * 1024, prio - 1, 2, true);
    plat_log("renderer: %s\n", rthr ? "GPU work on its own thread (core 2)" : "GPU work on the game thread");
}

/* 1: the render thread stopped (citro3d can be shut down); 0: it is stuck
 * on the GPU, the app must just exit */
int rend_shutdown(void)
{
    int ok = 1;
    rend_wait_idle();
    if (rthr) {
        rquit = 1;
        LightSemaphore_Release(&rjob, 1);
        ok = R_SUCCEEDED(threadJoin(rthr, 1000000000ull));
        rthr = NULL;
    }
    return ok;
}

void rend_frame_end(void)
{
    extern void plat_frame_poll(void);
    extern void ui_radar_commit(void);
    if (!R.in_frame)
        rend_frame_begin();
    flush();
    /* the frame before must be done: then the system (HOME menu, sleep)
     * may take the GPU, and this frame's buffers are handed over */
    rend_wait_idle();
    plat_frame_poll();
    {
        extern int plat_quitting(void);
        if (plat_quitting()) {          /* closing: nothing more for the GPU */
            ncmd = 0;
            R.in_frame = 0;
            return;
        }
    }
    J.cmds = cmds;
    J.ncmd = ncmd;
    J.vring = R.vring[R.frame % NRING];
    J.vused = R.vused;
    ui_radar_commit();
    R.tgen++;
    cmd_side ^= 1;
    cmds = cmdbuf[cmd_side];
    ncmd = 0;
    R.in_frame = 0;
    if (rthr) {
        rbusy = 1;
        LightSemaphore_Release(&rjob, 1);
    } else {
        render_job();
    }
}

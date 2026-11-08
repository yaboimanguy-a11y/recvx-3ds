/* rend.h: the GPU backend the GS emulation (port/ps2/gs.c) draws through.
 * Implemented by port/platform/3ds/rend_c3d.c.  Plain C types only. */
#pragma once
#include <stdint.h>

/* one vertex in GS canvas pixels (640 x 480), depth 0..1 (1 = near),
 * texture coordinates normalized, colour 0xAABBGGRR with GS alpha
 * (0x80 = 1.0) */
typedef struct {
    float x, y, z;
    float w;                /* clip w: 1 for 2D; for 3D the view depth (x, y, z
                             * stay divided), so the GPU clips against the
                             * camera and maps textures with perspective */
    float u, v;
    uint32_t rgba;
    float fog;              /* 1 = no fog, 0 = fog colour */
} RVert;

/* a vertex as the GPU takes it (port/render/3ds/r2d.v.pica): x, y in
 * canvas pixels, z NDC (-1 far .. 1 near), w clip w, u, v normalized,
 * colour bytes r g b a (GS scale, 0x80 = 1.0), fog factor (1 = none) */
typedef struct {
    float x, y, z, w;
    float u, v;
    uint8_t c[4];
    float pad;
} RGpuVtx;

typedef struct {
    int tex;                /* rend_tex handle, or -1 for no texture */
    uint8_t tfx;            /* 0 modulate, 1 decal, 2/3 highlight */
    uint8_t tcc;            /* texture alpha used */
    uint8_t linear;         /* bilinear filter */
    uint8_t clamp_s, clamp_t;
    uint8_t abe;            /* alpha blend on */
    uint8_t a, b, c, d;     /* GS blend (A - B) * C + D: 0 Cs, 1 Cd, 2 zero / FIX */
    uint8_t fix;            /* fixed alpha for c == 2 */
    uint8_t ate, atst, aref, afail;
    uint8_t zte, ztst, zmsk;
    uint8_t fge;
    uint32_t fogcol;        /* 0x00BBGGRR */
    uint8_t gouraud;
    uint8_t stereo;
    uint8_t mist;           /* the port's mist on this draw (the background is the mist colour) */         /* a 3D draw: shifted per eye with the 3D slider (2D stays on the screen) */
    int16_t sc_x0, sc_y0, sc_x1, sc_y1;   /* scissor, canvas pixels, inclusive */
} RState;

enum { RPRIM_POINT, RPRIM_LINE, RPRIM_TRI, RPRIM_SPRITE };

void rend_frame_begin(void);
/* n vertices: triangles (n % 3 == 0), lines (n % 2), sprites (pairs: corners) */
void rend_draw(const RState *s, int prim, const RVert *v, int n);
/* n / 3 triangles in one go */
void rend_draw_tris(const RState *s, const RVert *v, int n);
void rend_frame_end(void);

/* textures: RGBA8 pixels, 0xAABBGGRR, GS alpha range (0x80 = 1.0) */
int rend_tex_create(int w, int h, const uint32_t *rgba);   /* handle, or -1 */
void rend_tex_update(int handle, const uint32_t *rgba);
/* replace a w x h rectangle at (x, y): rgba has row stride `stride` and
 * the rectangle starts at rgba[y * stride + x] */
/* the bottom screen (320 x 240): a flat triangle, colour 0xAABBGGRR (0x80 = full) */
void rend_bot_tri(float x0, float y0, float x1, float y1, float x2, float y2, uint32_t rgba);
int rend_tex_patch(int handle, const uint32_t *rgba, int stride, int x, int y, int w, int h);
void rend_tex_free(int handle);
/* the GPU work of each frame on a thread of its own (core 2), while the
 * game goes on; rend_wait_idle: the frame handed over is done */
void rend_start_thread(void);
void rend_wait_idle(void);
int rend_shutdown(void);              /* 0: the render thread is stuck */
unsigned rend_tex_bytes(int handle);   /* its real GPU size (16- or 32-bit texels) */
int rend_mem_low(void);                /* little GPU texture memory left */

/* off-screen targets: a GS frame buffer (FBP) the game draws into and later
 * reads as a texture.  rend_target_handle gives the texture handle for it
 * (made on first use; its size in *tex_w x *tex_h; -1 = none left);
 * rend_use_target sends the following draws there (-1: the screen).
 * Sampling a target: u = x / tex_w, v = 1 - y / tex_h. */
int rend_target_handle(unsigned fbp, int w, int *tex_w, int *tex_h);
void rend_use_target(int handle);

/* the models' fast path (port/ps2/vu_strip.c -> gs.c): the GS state for a
 * batch of model triangles, worked out once (gs_tris_begin), and how each
 * vertex is turned into an RGpuVtx */
typedef struct {
    float ox, oy;           /* GS window offset, subtracted */
    int gouraud;            /* 0: a triangle takes its third vertex's colour */
    int fog_game;           /* the game's fog on: the vertex fog factor counts */
    float mist_d, mist_start;   /* the port's mist: fog *= exp(-d * (w - start)) */
    int tme;
} GsXf;
/* 1: draw with *s (rend_tris_ptr); 0: nothing to draw (a left-out buffer,
 * frame skip); -1: take the slow path (gs_draw_tris; a frame dump) */
int gs_tris_begin(unsigned prim, RState *s, GsXf *xf);
void gs_tris_count(int ntris, int tme);
RGpuVtx *rend_tris_ptr(const RState *s, int n);
void rend_tris_commit(int n);

/* the status strip on the bottom screen (port/ps2/radar.c -> ui_3ds.c) */
typedef struct {
    int valid;              /* the game is in play */
    int hp, max;
    int cond;               /* 0 fine, 1 caution, 2 caution (low), 3 danger, 4 poison */
    int has_weapon;
    int count;              /* rounds or percent, when show_count */
    int show_count, percent, infinite;
    char name[24];
    int stage, room;
} PortHud;
void port_hud(PortHud *h);

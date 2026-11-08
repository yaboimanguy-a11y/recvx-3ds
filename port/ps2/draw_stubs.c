/* draw_stubs.c: C versions of the drawing-side functions the game wrote in
 * EE / VU assembly (guarded out of its sources with RECVX_PORT), and the
 * frame hook.
 *
 * DMA waits are no-ops (the port's DMA is synchronous); loadImage starts a
 * GIF chain; Ps2SwapDBuff ends the frame (draws the ordering table, shows
 * it, runs two vblanks: the game runs at 30 Hz).  Models: vu_strip.c. */
#include "types.h"
#include "plat.h"

extern void (*VsyncFunc)();
extern void (*EorFunc)();
extern unsigned int Ps2_vcount;
extern unsigned int Ps2_dbuff;
extern sceGsDBuffDc Db;
void Ps2DrawOTag(void);
void Ps2ClearOT(void);
void PS2_swap(void);
void gs_dma_chain_gif(void *tag);
void gs_frame_end(void);
void plat_sleep_ms(int ms);

void SyncPath(void) {}
void D2_SyncTag(void) {}
void loadImage(void *tags) { gs_dma_chain_gif(tags); }

static int quit_requested;
int g_skip_draw;                    /* this frame runs without drawing (frame skip) */
int g_frame_skip_active;            /* skipping is on now (on, or auto when slow) */
int plat_frame_skip(void);
int port_in_cutscene(void);
int prof_enter(int b);
void prof_leave(int prev);

extern u_int ps2_stat_frames;
void Ps2SwapDBuff(void)
{
    ps2_stat_frames++;
    Ps2DrawOTag();
    Ps2ClearOT();
    EorFunc();
    PS2_swap();
    sceGsSwapDBuffDc(&Db, Ps2_dbuff);
    Ps2_dbuff = (Ps2_dbuff + 1) & 1;
    gs_frame_end();
    if (!plat_frame())
        quit_requested = 1;
    /* the game runs every other vblank (30 Hz) at most.
     * Frame skip (settings, plat_frame_skip: 0 off, 1 on, 2 auto):
     *   on: the game keeps 30 Hz (cutscenes stay with their voices); a
     *     frame that ends late has the next one run without drawing, up to
     *     3 in a row (at least every 4th frame is drawn).
     *   auto (the default): only in cutscenes, where the animation must
     *     keep up with the voices: every frame is drawn while the drawn
     *     frames take under ~38 ms on average; slower than that frame skip
     *     turns itself on (at most 2 frames in a
     *     row, so it stays smoother), and off again once the drawn frames
     *     are back under ~30 ms.
     *   off (and auto in play): every frame drawn, 30 Hz at most; a
     *     frame that took longer goes straight on */
    {
        static unsigned last_us, avg_us, avg_draw, need = 2, t_next;
        static int run, auto_on;                /* skipped frames in a row; auto: skipping */
        unsigned t0 = plat_us(), took;
        int mode = plat_frame_skip(), skip;
        took = last_us ? t0 - last_us : 33333;     /* the frame's own work */
        avg_us = avg_us ? (avg_us * 7 + took) / 8 : took;
        if (!g_skip_draw)                           /* a drawn frame's cost */
            avg_draw = avg_draw ? (avg_draw * 7 + took) / 8 : took;
        run = g_skip_draw ? run + 1 : 0;
        g_skip_draw = 0;
        if (mode == 2) {
            /* cutscenes only: in play every frame is drawn */
            if (!port_in_cutscene())
                auto_on = 0;
            else if (!auto_on && avg_draw > 38000)
                auto_on = 1;
            else if (auto_on && avg_draw < 30000)
                auto_on = 0;
        } else {
            auto_on = 0;
        }
        skip = mode == 1 || auto_on;
        g_frame_skip_active = skip;
        if (skip) {
            int behind;
            if (!t_next)
                t_next = t0;
            t_next += 33333;
            behind = (int)(t0 - t_next);
            if (behind > 100000) {              /* far behind (loading): start again */
                t_next = t0;
                behind = 0;
            }
            if (behind > 4000 && run < (mode == 1 ? 3 : 2))
                g_skip_draw = 1;
            if (behind <= 0) {
                int pv = prof_enter(10 /* PB_WAIT */);
                while (*(volatile unsigned int *)&Ps2_vcount < 2)
                    plat_sleep_ms(1);
                prof_leave(pv);
            }
            if (behind <= 0)
                t_next = plat_us();
            need = 2;
        } else {
            t_next = 0;
            /* 30 Hz at most; a frame that took longer goes straight on
             * (holding slow stretches at 20 Hz wasted the difference: the
             * frames are often 28-36 ms now) */
            need = 2;
            int pv = prof_enter(10 /* PB_WAIT */);
            while (*(volatile unsigned int *)&Ps2_vcount < need)
                plat_sleep_ms(1);
            prof_leave(pv);
        }
        prof_wait_us += plat_us() - t0;
        last_us = plat_us();
    }
    Ps2_vcount = 0;
    VsyncFunc();
    if (quit_requested)
        plat_quit();
}

/* ---- the game's own triangle scissoring ------------------------------------
 * The models (port/ps2/vu_strip.c) hand clipping to the 3DS GPU, which clips
 * against the camera itself; the remaining C users of the PS2 scissor code
 * (vu1DrawTriangleStripTransDouble1P, compiled from the game) only reach it
 * when these report a clipped triangle, which they never do. */
#define STUB(name) void name() { static int n; if (!n++) plat_log("draw: %s not drawn\n", #name); }
STUB(DrawScissorPolygonOpaque)
STUB(DrawScissorPolygonTrans1P)
STUB(DrawScissorPolygonOpaque2)
void InitNodeArraySet(void *s) { (void)s; }
int _Clip_ViewVolume(float *clip, float local_clip[4], float *vertex) { (void)clip; (void)local_clip; (void)vertex; return 0; }
void PushTriangleNodeArray(void *s, void *nod) { (void)s; (void)nod; }
void ResetNodeArraySet(void *s) { (void)s; }
void ScissorTriangle(void *s, void *plane) { (void)s; (void)plane; }
int _Clip_Screen(float *clip) { (void)clip; return 0; }
void InitNodeArraySet2(void) {}
unsigned int _Clip_ViewVolume2(void *vec) { (void)vec; return 0; }
unsigned int _Get_ClipViewVolume2(void) { return 0; }
int _Get_ClipVolumePlane(void) { return 0; }
void _Check_ClipViewAll(void *vec) { (void)vec; }
void _Set_NodeArray(void *s, void *p) { (void)s; (void)p; }
int _ClipInter(int m1, int m2, int xyz, float s, int w0, int w1, int count)
{ (void)m1; (void)m2; (void)xyz; (void)s; (void)w0; (void)w1; (void)count; return 0; }

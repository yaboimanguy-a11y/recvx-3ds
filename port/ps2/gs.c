/* gs.c: the PS2 Graphics Synthesizer, high level: 4 MB of GS memory with
 * the hardware's swizzled layouts, image transfers, GIF packets (PACKED,
 * REGLIST, IMAGE), the drawing registers and primitive assembly.  Drawing
 * goes to the GPU backend (rend.h); textures are decoded from GS memory
 * into RGBA and cached until the memory under them changes.
 *
 * Also here: DMA source chains (GIF and VIF1 channels), VIF codes
 * (UNPACK into VU1 memory, DIRECT to the GIF, MSCAL to vu1_mscal()) and the
 * libgraph calls (sceGs*).
 *
 * PSMT8 / PSMT4 addressing is measured from the game's own converters
 * (Conv8to32 / Conv4to32, ps2_pxlconv.c), which turn a linear 8/4-bit
 * image into the PSMCT32 words that read back as that image: the port
 * runs them once on probe images to learn where each texel lives. */
#include <math.h>
#include "types.h"
#include "plat.h"
#include "rend.h"

int Conv8to32(int width, int height, u_char *p_input, u_char *p_output);
int Conv4to32(int width, int height, u_char *p_input, u_char *p_output);

/* ---- GS memory -------------------------------------------------------- */
#define VRAM_WORDS (1024 * 1024)
static u_int vram[VRAM_WORDS];
static u_int page_gen[512];          /* bumped when a page (2048 words) is written */
static u_int gen_counter = 1;
/* per-frame counters for the stats log line */
static struct { u_int bytype[8]; u_int gif_tags, prims, sprites, tris, tex_prims, uploads, vif_mscal, dma_chains, tex_decodes, uploads_skipped, fb_reads, tris3d; } st;
/* upload bytes over a log period by format: 32, 16, 8, 4 bit, other */
static u_int up_bytes[5], up_count[5];

static const u_char bt32[4][8] = {
    { 0, 1, 4, 5, 16, 17, 20, 21 }, { 2, 3, 6, 7, 18, 19, 22, 23 },
    { 8, 9, 12, 13, 24, 25, 28, 29 }, { 10, 11, 14, 15, 26, 27, 30, 31 } };
static const u_char cw32[16] = { 0, 1, 4, 5, 8, 9, 12, 13, 2, 3, 6, 7, 10, 11, 14, 15 };
static const u_char bt16[8][4] = {
    { 0, 2, 8, 10 }, { 1, 3, 9, 11 }, { 4, 6, 12, 14 }, { 5, 7, 13, 15 },
    { 16, 18, 24, 26 }, { 17, 19, 25, 27 }, { 20, 22, 28, 30 }, { 21, 23, 29, 31 } };
static const u_char bt16s[8][4] = {
    { 0, 2, 16, 18 }, { 1, 3, 17, 19 }, { 8, 10, 24, 26 }, { 9, 11, 25, 27 },
    { 4, 6, 20, 22 }, { 5, 7, 21, 23 }, { 12, 14, 28, 30 }, { 13, 15, 29, 31 } };
static const u_char cw16[32] = {
    0, 2, 8, 10, 16, 18, 24, 26, 1, 3, 9, 11, 17, 19, 25, 27,
    4, 6, 12, 14, 20, 22, 28, 30, 5, 7, 13, 15, 21, 23, 29, 31 };

/* word address of a PSMCT32 pixel */
static inline u_int a32(u_int bp, u_int bw, u_int x, u_int y)
{
    u_int page = (y >> 5) * (bw ? bw : 1) + (x >> 6);
    u_int block = bp + page * 32 + bt32[(y >> 3) & 3][(x >> 3) & 7];
    return ((block * 64) + ((y >> 1) & 3) * 16 + cw32[((y & 1) << 3) | (x & 7)]) & (VRAM_WORDS - 1);
}

/* PSMCT32: word address of the first of 8 pixels (x a multiple of 8) on
 * row y; pixel j of the run is at that + cw32[((y & 1) << 3) | j] */
static inline u_int a32_run(u_int bp, u_int bw, u_int x, u_int y)
{
    u_int page = (y >> 5) * (bw ? bw : 1) + (x >> 6);
    u_int block = bp + page * 32 + bt32[(y >> 3) & 3][(x >> 3) & 7];
    return ((block * 64) + ((y >> 1) & 3) * 16) & (VRAM_WORDS - 1);
}

/* halfword address of a PSMCT16 / 16S pixel */
static inline u_int a16(u_int bp, u_int bw, u_int x, u_int y, int s)
{
    u_int page = (y >> 6) * (bw ? bw : 1) + (x >> 6);
    u_int b = s ? bt16s[(y >> 3) & 7][(x >> 4) & 3] : bt16[(y >> 3) & 7][(x >> 4) & 3];
    u_int block = bp + page * 32 + b;
    return ((block * 128) + ((y >> 1) & 3) * 32 + cw16[((y & 1) << 4) | (x & 15)]) & (VRAM_WORDS * 2 - 1);
}

/* PSMT8: byte offset in its page for (x, y) in a 128 x 64 page; PSMT4:
 * nibble offset in a 128 x 128 page (measured at init) */
static u_short t8map[64][128];
static u_short t4map[128][128];

static inline u_int a8(u_int bp, u_int bw, u_int x, u_int y)
{
    u_int ppr = (bw ? bw : 2) >> 1;
    u_int page = (y >> 6) * (ppr ? ppr : 1) + (x >> 7);
    return ((bp * 64 + page * 2048) * 4 + t8map[y & 63][x & 127]) & (VRAM_WORDS * 4 - 1);
}

static inline u_int a4(u_int bp, u_int bw, u_int x, u_int y)
{
    u_int ppr = (bw ? bw : 2) >> 1;
    u_int page = (y >> 7) * (ppr ? ppr : 1) + (x >> 7);
    return ((bp * 64 + page * 2048) * 8 + t4map[y & 127][x & 127]) & (VRAM_WORDS * 8 - 1);
}

#define VR8 ((u_char *)vram)
#define VR16 ((u_short *)vram)

static void measure_swizzle(void)
{
    static u_char in[128 * 128], out[128 * 128];
    int x, y, k;
    /* PSMT8 page 128 x 64 -> PSMCT32 64 x 32: find each texel's byte */
    memset(t8map, 0, sizeof t8map);
    for (k = 0; k < 2; k++) {          /* texel index (13 bits) in two passes */
        for (y = 0; y < 64; y++)
            for (x = 0; x < 128; x++)
                in[y * 128 + x] = (u_char)(((y * 128 + x) >> (k * 7)) & 0x7F) | (k << 7);
        memset(out, 0, 128 * 64);
        Conv8to32(128, 64, in, out);
        /* out = 64 x 32 CT32 words, linear; place them through a32 */
        for (y = 0; y < 32; y++)
            for (x = 0; x < 64; x++) {
                u_int w = a32(0, 1, x, y);
                int b;
                for (b = 0; b < 4; b++) {
                    u_char v = out[(y * 64 + x) * 4 + b];
                    int idx = v & 0x7F;
                    /* pass 0 gives the low 7 bits, pass 1 the high 6 */
                    (void)idx;
                    if (k == 0)
                        ((u_int *)in)[0] = ((u_int *)in)[0];
                    {
                        static u_short lo[8192];
                        static int have;
                        u_int byte = w * 4 + b;
                        if (k == 0) {
                            lo[byte & 8191] = (u_short)idx;
                            have = 1;
                        } else if (have) {
                            u_int t = (u_int)idx << 7 | lo[byte & 8191];
                            if (t < 8192)
                                t8map[t >> 7][t & 127] = (u_short)byte;
                        }
                    }
                }
            }
    }
    /* PSMT4 page 128 x 128 -> PSMCT32 64 x 32: 14-bit texel index in four
     * 4-bit passes (each nibble carries 4 bits) */
    {
        static u_short acc[16384];
        int pass;
        memset(acc, 0, sizeof acc);
        for (pass = 0; pass < 4; pass++) {
            for (y = 0; y < 128; y++)
                for (x = 0; x < 128; x += 2) {
                    int t0 = y * 128 + x, t1 = t0 + 1;
                    in[(y * 128 + x) / 2] = (u_char)(((t0 >> (pass * 4)) & 15) | (((t1 >> (pass * 4)) & 15) << 4));
                }
            memset(out, 0, 128 * 64);
            Conv4to32(128, 128, in, out);
            for (y = 0; y < 32; y++)
                for (x = 0; x < 64; x++) {
                    u_int w = a32(0, 1, x, y);
                    int n;
                    for (n = 0; n < 8; n++) {
                        u_int nib = w * 8 + n;           /* nibble slot in GS memory */
                        u_int v = (out[(y * 64 + x) * 4 + (n >> 1)] >> ((n & 1) * 4)) & 15;
                        acc[nib & 16383] |= (u_short)(v << (pass * 4));
                    }
                }
        }
        for (k = 0; k < 16384; k++) {
            u_int t = acc[k];
            if (t < 16384)
                t4map[t >> 7][t & 127] = (u_short)k;
        }
    }
}

/* ---- pixel access by format --------------------------------------------- */
static inline u_int rd_px(u_int psm, u_int bp, u_int bw, u_int x, u_int y)
{
    switch (psm) {
    case SCE_GS_PSMCT32: case SCE_GS_PSMCT24: case SCE_GS_PSMZ32: case SCE_GS_PSMZ24:
        return vram[a32(bp, bw, x, y)];
    case SCE_GS_PSMCT16: case SCE_GS_PSMZ16:
        return VR16[a16(bp, bw, x, y, 0)];
    case SCE_GS_PSMCT16S: case SCE_GS_PSMZ16S:
        return VR16[a16(bp, bw, x, y, 1)];
    case SCE_GS_PSMT8:
        return VR8[a8(bp, bw, x, y)];
    case SCE_GS_PSMT4: {
        u_int n = a4(bp, bw, x, y);
        return (VR8[n >> 1] >> ((n & 1) * 4)) & 15;
    }
    case SCE_GS_PSMT8H:
        return vram[a32(bp, bw, x, y)] >> 24;
    case SCE_GS_PSMT4HL:
        return (vram[a32(bp, bw, x, y)] >> 24) & 15;
    case SCE_GS_PSMT4HH:
        return vram[a32(bp, bw, x, y)] >> 28;
    }
    return 0;
}

static inline void touch(u_int word)
{
    page_gen[(word >> 11) & 511] = gen_counter;
}

/* write one pixel; a page is marked changed only when its contents really
 * change (the game re-uploads unchanged pictures every frame, and a changed
 * page means decoding the texture again) */
#define WR(lv, nv, word) do { u_int o_ = (lv); u_int n_ = (nv); if (o_ != n_) { (lv) = n_; touch(word); } } while (0)
static inline void wr_px(u_int psm, u_int bp, u_int bw, u_int x, u_int y, u_int v)
{
    u_int a;
    switch (psm) {
    case SCE_GS_PSMCT32: case SCE_GS_PSMZ32:
        a = a32(bp, bw, x, y); WR(vram[a], v, a); break;
    case SCE_GS_PSMCT24: case SCE_GS_PSMZ24:
        a = a32(bp, bw, x, y); WR(vram[a], (vram[a] & 0xFF000000) | (v & 0xFFFFFF), a); break;
    case SCE_GS_PSMCT16: case SCE_GS_PSMZ16:
        a = a16(bp, bw, x, y, 0);
        if (VR16[a] != (u_short)v) { VR16[a] = (u_short)v; touch(a >> 1); }
        break;
    case SCE_GS_PSMCT16S: case SCE_GS_PSMZ16S:
        a = a16(bp, bw, x, y, 1);
        if (VR16[a] != (u_short)v) { VR16[a] = (u_short)v; touch(a >> 1); }
        break;
    case SCE_GS_PSMT8:
        a = a8(bp, bw, x, y);
        if (VR8[a] != (u_char)v) { VR8[a] = (u_char)v; touch(a >> 2); }
        break;
    case SCE_GS_PSMT4: {
        u_char nb;
        a = a4(bp, bw, x, y);
        nb = (u_char)((VR8[a >> 1] & ((a & 1) ? 0x0F : 0xF0)) | ((v & 15) << ((a & 1) * 4)));
        if (VR8[a >> 1] != nb) { VR8[a >> 1] = nb; touch(a >> 3); }
    } break;
    case SCE_GS_PSMT8H:
        a = a32(bp, bw, x, y); WR(vram[a], (vram[a] & 0x00FFFFFF) | (v << 24), a); break;
    case SCE_GS_PSMT4HL:
        a = a32(bp, bw, x, y); WR(vram[a], (vram[a] & 0xF0FFFFFF) | ((v & 15) << 24), a); break;
    case SCE_GS_PSMT4HH:
        a = a32(bp, bw, x, y); WR(vram[a], (vram[a] & 0x0FFFFFFF) | ((v & 15) << 28), a); break;
    }
}

/* ---- registers --------------------------------------------------------------- */
typedef struct {
    u_long tex0, tex1, clamp, xyoffset, scissor, alpha, test, fba, frame, zbuf;
} Ctx;

static struct {
    Ctx ctx[2];
    u_long prim, prmode, rgbaq, st, uv, texa, fogcol, texclut;
    int prmodecont;
    float q;
    u_int fog;
    int pabe, dthe, colclamp;
    /* image transfer */
    u_long bitbltbuf, trxpos, trxreg;
    int trx_on, trx_x, trx_y;
    u_int trx_bits;         /* partial pixel bits for 24/16/8/4-bit */
    u_long trx_acc;
    /* the transfer's settings, decoded once at TRXDIR */
    u_int t_dbp, t_dbw, t_psm, t_sx, t_sy, t_w, t_h;
    /* vertex queue */
    RVert vq[4];
    int vn;
    int frame_open;
} gs;

static int bpp_of(u_int psm)
{
    switch (psm) {
    case SCE_GS_PSMCT24: case SCE_GS_PSMZ24: return 24;
    case SCE_GS_PSMCT16: case SCE_GS_PSMCT16S: case SCE_GS_PSMZ16: case SCE_GS_PSMZ16S: return 16;
    case SCE_GS_PSMT8: case SCE_GS_PSMT8H: return 8;
    case SCE_GS_PSMT4: case SCE_GS_PSMT4HL: case SCE_GS_PSMT4HH: return 4;
    }
    return 32;
}

#define F(v, lo, n) ((u_int)(((v) >> (lo)) & ((1ull << (n)) - 1)))

/* ---- exclusive profile: every microsecond of the game thread goes to one
 * bucket (nested work is charged to the inner one), logged per frame ---- */
enum { PB_GAME, PB_VTX, PB_STRIP, PB_STATE, PB_PARSE, PB_UPLOAD, PB_MAT, PB_TEX, PB_GSDRAW,
       PB_HANDOFF, PB_WAIT, PB_N };
static unsigned pb_acc[PB_N], pb_t, pb_cur, pb_calls[PB_N];
/* the timers run only with logging on: each reading of the clock is a
 * system call, a thousand or more a frame */
int prof_on;
#define PUS() (prof_on ? plat_us() : 0u)
int prof_enter(int b)
{
    unsigned now;
    if (!prof_on)
        return b;
    now = plat_us();
    int prev = (int)pb_cur;
    pb_acc[pb_cur] += now - pb_t;
    pb_t = now;
    pb_cur = (unsigned)b;
    pb_calls[b]++;
    return prev;
}
void prof_leave(int prev)
{
    unsigned now;
    if (!prof_on)
        return;
    now = plat_us();
    pb_acc[pb_cur] += now - pb_t;
    pb_t = now;
    pb_cur = (unsigned)prev;
}



/* an image put straight into a shown frame buffer (fb_blit, below) */
static int fb_dest(u_int dbp, u_int psm);
static void fb_blit(u_int dbp, u_int dbw, u_int psm, u_int x, u_int y, u_int w, u_int h);

/* host -> local transfer: one 64-bit chunk of image data */
static void trx_put64(u_long d)
{
    u_int dbp = F(gs.bitbltbuf, 32, 14), dbw = F(gs.bitbltbuf, 48, 6), psm = F(gs.bitbltbuf, 56, 6);
    u_int sx = F(gs.trxpos, 32, 11), sy = F(gs.trxpos, 48, 11);
    u_int w = F(gs.trxreg, 0, 12), h = F(gs.trxreg, 32, 12);
    int bpp = bpp_of(psm);
    int bits = 64;
    if (!gs.trx_on || w == 0)
        return;
    while (bits > 0 && gs.trx_on) {
        u_int v;
        if (bpp == 24) {
            /* 24-bit pixels span chunks: accumulate */
            gs.trx_acc |= (d & 0xFF) << gs.trx_bits;   /* byte at a time */
            d >>= 8;
            bits -= 8;
            gs.trx_bits += 8;
            if (gs.trx_bits < 24)
                continue;
            v = (u_int)gs.trx_acc;
            gs.trx_acc = 0;
            gs.trx_bits = 0;
        } else {
            v = (u_int)(d & ((bpp == 32) ? 0xFFFFFFFFu : ((1u << bpp) - 1)));
            d = bpp == 32 ? d >> 32 : d >> bpp;
            bits -= bpp;
        }
        wr_px(psm, dbp, dbw, sx + gs.trx_x, sy + gs.trx_y, v);
        if (++gs.trx_x >= (int)w) {
            gs.trx_x = 0;
            if (++gs.trx_y >= (int)h) {
                gs.trx_on = 0;
                gen_counter++;
                if (fb_dest(dbp, psm))
                    fb_blit(dbp, dbw, psm, sx, sy, w, h);
            }
        }
    }
}

/* host -> local transfer, fast path: n 32-bit words of image data for every
 * format except 24-bit (which uses trx_put64).  The settings come from
 * TRXDIR, and the pixel loop is chosen once per run, not per pixel. */
#define TRX_NEXT() do { if (++x >= w) { x = 0; if (++y >= h) { gs.trx_on = 0; gen_counter++; goto done; } } } while (0)
static void trx_run(const u_int *d, u_int n)
{
    u_int bp = gs.t_dbp, bw = gs.t_dbw, w = gs.t_w, h = gs.t_h, sx = gs.t_sx, sy = gs.t_sy;
    u_int x = (u_int)gs.trx_x, y = (u_int)gs.trx_y, i, a, k;
    if (!gs.trx_on)
        return;
    switch (gs.t_psm) {
    case SCE_GS_PSMCT32: case SCE_GS_PSMZ32:
        for (i = 0; i < n; ) {
            u_int px = sx + x, py = sy + y;
            if (!(px & 7) && x + 8 <= w && i + 8 <= n) {
                /* 8 pixels in one block row: one address computation */
                /* (the column layout pairs the pixels: 0-1, 2-3 ... sit
                 * side by side, so a run is four 2-word stores) */
                u_int base = a32_run(bp, bw, px, py);
                const u_char *cw = cw32 + ((py & 1) << 3);
                u_int *v = vram + base;
                const u_int *sd = d + i;
                v[cw[0]] = sd[0]; v[cw[0] + 1] = sd[1];
                v[cw[2]] = sd[2]; v[cw[2] + 1] = sd[3];
                v[cw[4]] = sd[4]; v[cw[4] + 1] = sd[5];
                v[cw[6]] = sd[6]; v[cw[6] + 1] = sd[7];
                touch(base);
                i += 8;
                x += 8;
                /* the rest of the row's runs: no address change inside a block row of 8 */
                while (x + 8 <= w && i + 8 <= n && !((sx + x) & 7)) {
                    base = a32_run(bp, bw, sx + x, py);
                    v = vram + base;
                    sd = d + i;
                    v[cw[0]] = sd[0]; v[cw[0] + 1] = sd[1];
                    v[cw[2]] = sd[2]; v[cw[2] + 1] = sd[3];
                    v[cw[4]] = sd[4]; v[cw[4] + 1] = sd[5];
                    v[cw[6]] = sd[6]; v[cw[6] + 1] = sd[7];
                    touch(base);
                    i += 8;
                    x += 8;
                }
                x--;
                TRX_NEXT();
                continue;
            }
            a = a32(bp, bw, px, py);
            if (vram[a] != d[i]) { vram[a] = d[i]; touch(a); }
            i++;
            TRX_NEXT();
        }
        break;
    case SCE_GS_PSMCT16: case SCE_GS_PSMZ16: case SCE_GS_PSMCT16S: case SCE_GS_PSMZ16S: {
        int s = gs.t_psm == SCE_GS_PSMCT16S || gs.t_psm == SCE_GS_PSMZ16S;
        for (i = 0; i < n; i++) {
            u_int v = d[i];
            for (k = 0; k < 2; k++, v >>= 16) {
                a = a16(bp, bw, sx + x, sy + y, s);
                if (VR16[a] != (u_short)v) { VR16[a] = (u_short)v; touch(a >> 1); }
                TRX_NEXT();
            }
        }
    } break;
    case SCE_GS_PSMT8: case SCE_GS_PSMT4: {
        /* a row at a time, up to the page edge (128 pixels): one page
         * address per run, the in-page layout from the measured tables */
        const u_char *src = (const u_char *)d;
        const int t8 = gs.t_psm == SCE_GS_PSMT8;
        u_int npx = n * (t8 ? 4u : 8u), j = 0, ppr = (bw ? bw : 2) >> 1;
        if (!ppr)
            ppr = 1;
        while (j < npx) {
            u_int px = sx + x, py = sy + y, run = w - x, edge = 128 - (px & 127), page;
            int changed = 0;
            if (run > edge)
                run = edge;
            if (run > npx - j)
                run = npx - j;
            if (t8) {
                const u_short *m = t8map[py & 63] + (px & 127);
                u_int base = (bp * 64 + ((py >> 6) * ppr + (px >> 7)) * 2048) * 4;
                for (k = 0; k < run; k++) {
                    u_char *b = &VR8[(base + m[k]) & (VRAM_WORDS * 4 - 1)];
                    u_char v = src[j + k];
                    if (*b != v) { *b = v; changed = 1; }
                }
                page = (base >> 13) & 511;
            } else {
                const u_short *m = t4map[py & 127] + (px & 127);
                u_int base = (bp * 64 + ((py >> 7) * ppr + (px >> 7)) * 2048) * 8;
                for (k = 0; k < run; k++) {
                    u_int an = (base + m[k]) & (VRAM_WORDS * 8 - 1), sn = j + k;
                    u_int v = (src[sn >> 1] >> ((sn & 1) * 4)) & 15;
                    u_char *b = &VR8[an >> 1];
                    u_char nb = (an & 1) ? (u_char)((*b & 0x0F) | (v << 4)) : (u_char)((*b & 0xF0) | v);
                    if (*b != nb) { *b = nb; changed = 1; }
                }
                page = (base >> 14) & 511;
            }
            if (changed) {
                page_gen[page] = gen_counter;
                if (bp & 31)            /* not page-aligned: the run can reach the next page */
                    page_gen[(page + 1) & 511] = gen_counter;
            }
            j += run;
            x += run;
            if (x >= w) {
                x = 0;
                if (++y >= h) {
                    gs.trx_on = 0;
                    gen_counter++;
                    goto done;
                }
            }
        }
    } break;
    default: {                       /* 8H / 4HL / 4HH: rare, per pixel */
        int bpp = bpp_of(gs.t_psm);
        for (i = 0; i < n; i++) {
            u_int v = d[i];
            for (k = 0; k < 32u / bpp; k++, v >>= bpp) {
                wr_px(gs.t_psm, bp, bw, sx + x, sy + y, v & ((1u << bpp) - 1));
                TRX_NEXT();
            }
        }
    } break;
    }
done:
    gs.trx_x = (int)x;
    gs.trx_y = (int)y;
}
#undef TRX_NEXT

/* an upload's fingerprint.  Small uploads (palettes, small pictures): a
 * checksum of every word.  Large ones are the game's textures, sent by DMA
 * reference straight from where they were loaded: where they are, their
 * size and 160 words spread over them (reading all ~4 MB a frame cost more
 * than anything else in a room). */
static void upload_sum(const u_int *d, u_int n, u_int *s1, u_int *s2)
{
    u_int a = 1, b = 0, i;
    if (n < 4096) {
        for (i = 0; i < n; i++) {
            a += d[i];
            b += a;
        }
    } else {
        /* a sample: the ends, and 48 words spread through it, mixed with
         * where it is and how long (each read here misses the cache) */
        u_int step = n / 48;
        for (i = 0; i < 16; i++) {
            a += d[i] ^ d[n - 1 - i];
            b += a;
        }
        for (i = 0; i < 48; i++) {
            a += d[i * step + (i & 7)];
            b += a;
        }
        b ^= (u_int)(size_t)d * 0x9E3779B1u ^ n * 0x85EBCA6Bu;
    }
    *s1 = a;
    *s2 = b;
}

/* ---- deferred uploads -------------------------------------------------------------
 * In a room the game sends ~4 MB of textures a frame: one texture slot (and a
 * palette slot) that every texture change is uploaded into again (Ps2TexLoad).
 * Writing that into the swizzled GS memory was most of the frame.  A whole
 * upload that covers whole GS pages is only noted here: which pages it owns,
 * where its data is (the game's own copy in memory: Ps2TexLoad sends it by
 * DMA reference, so it stays put) and its checksum.  The pages' identity for
 * the texture cache comes from that note (page_prov), so a texture already
 * decoded is found again without its data ever reaching GS memory.  Only
 * when something reads those pages (a texture decode, a palette load, a GS
 * copy or read-back, a partial write) is the data written ("materialized"). */
#define NPEND 512
typedef struct {
    const u_int *src;
    u_int nwords, sum1, sum2, id;
    u_int dbp, dbw, psm, sx, sy, w, h;
    u_int pw, ph, ppr;          /* page size in pixels, pages per buffer row */
    int owned;                  /* pages still waiting for this upload's data */
    int checked;
} Pend;
static Pend pend[NPEND];
static short page_owner[512];   /* pending upload that owns the page, or -1 */
static u_int page_prov[512];    /* the page's identity when written by a whole upload */
static u_int prov_gen[512];     /* page_gen + 1 while page_prov is valid */
static u_int pend_next;
static u_int st_deferred, st_mat_pages, st_src_changed;
static int defer_off;           /* uploads from a caller's buffer (LoadImage): written at once */

static inline u_int mix32(u_int h, u_int v)
{
    h ^= v;
    h *= 0x01000193u;
    h ^= h >> 15;
    return h;
}

static void pend_release(int k)
{
    if (k >= 0 && --pend[k].owned <= 0) {
        pend[k].owned = 0;
        pend[k].src = NULL;
    }
}

/* write page p's part of its pending upload into GS memory */
static void mat_page(u_int p)
{
    int k = page_owner[p], bpp;
    Pend *u;
    u_int rel, x0, y0, r, keep_gen;
    u_int s_dbp, s_dbw, s_psm, s_sx, s_sy, s_w, s_h;
    int s_on, s_x, s_y;
    if (k < 0)
        return;
    u = &pend[k];
    page_owner[p] = -1;
    if (!u->src) {
        pend_release(k);
        return;
    }
    bpp = bpp_of(u->psm);
    if (!u->checked) {
        /* the data must still be what was sent */
        u_int a, b;
        upload_sum(u->src, u->nwords, &a, &b);
        u->checked = 1;
        if (a != u->sum1 || b != u->sum2) {
            if (st_src_changed++ < 4)
                plat_log("gs: a texture's data changed in memory before it was used (%ux%u at %04X)\n",
                         u->w, u->h, u->dbp);
        }
    }
    rel = p - (u->dbp >> 5);
    x0 = (rel % u->ppr) * u->pw - u->sx;
    y0 = (rel / u->ppr) * u->ph - u->sy;
    s_dbp = gs.t_dbp; s_dbw = gs.t_dbw; s_psm = gs.t_psm; s_sx = gs.t_sx; s_sy = gs.t_sy;
    s_w = gs.t_w; s_h = gs.t_h; s_on = gs.trx_on; s_x = gs.trx_x; s_y = gs.trx_y;
    keep_gen = page_gen[p];
    for (r = 0; r < u->ph && y0 + r < u->h; r++) {     /* the last page row may be partial */
        const u_int *row = u->src + (((y0 + r) * u->w + x0) * (u_int)bpp) / 32u;
        gs.t_dbp = u->dbp; gs.t_dbw = u->dbw; gs.t_psm = u->psm;
        gs.t_sx = u->sx + x0; gs.t_sy = u->sy + y0 + r; gs.t_w = u->pw; gs.t_h = 1;
        gs.trx_on = 1; gs.trx_x = gs.trx_y = 0;
        trx_run(row, (u->pw * (u_int)bpp) / 32u);
    }
    gs.t_dbp = s_dbp; gs.t_dbw = s_dbw; gs.t_psm = s_psm; gs.t_sx = s_sx; gs.t_sy = s_sy;
    gs.t_w = s_w; gs.t_h = s_h; gs.trx_on = s_on; gs.trx_x = s_x; gs.trx_y = s_y;
    page_gen[p] = keep_gen;     /* the contents are what the page's identity says */
    st_mat_pages++;
    pend_release(k);
}

static void mat_range(u_int p0, u_int p1)
{
    u_int p;
    int pv = -1;
    if (p1 > 511)
        p1 = 511;
    for (p = p0; p <= p1; p++)
        if (page_owner[p] >= 0) {
            if (pv < 0)
                pv = prof_enter(PB_MAT);
            mat_page(p);
        }
    if (pv >= 0)
        prof_leave(pv);
}

static void mat_all(void) { mat_range(0, 511); }

/* 1: this whole upload was noted instead of written */
static int defer_upload(const u_int *d, u_int n, u_int s1, u_int s2)
{
    u_int psm = gs.t_psm, pw, ph, ppr, bpp, i, j, cols, rows, id, k, same = 1, base;
    Pend *u;
    switch (psm) {
    case SCE_GS_PSMCT32: pw = 64; ph = 32; ppr = gs.t_dbw; break;
    case SCE_GS_PSMCT16: case SCE_GS_PSMCT16S: pw = 64; ph = 64; ppr = gs.t_dbw; break;
    case SCE_GS_PSMT8: pw = 128; ph = 64; ppr = gs.t_dbw >> 1; break;
    case SCE_GS_PSMT4: pw = 128; ph = 128; ppr = gs.t_dbw >> 1; break;
    default: return 0;
    }
    if (defer_off)
        return 0;
    bpp = (u_int)bpp_of(psm);
    /* whole pages across; down, the last page row may be partial (the
     * room's 1024 x 511 texture sheet): its missing lines keep what GS
     * memory held, and only the sent lines are written out (mat_page) */
    if (!ppr || (gs.t_dbp & 31) || (gs.t_sx % pw) || (gs.t_sy % ph) || (gs.t_w % pw) || gs.t_h < ph)
        return 0;
    if ((u_long)n * 32u != (u_long)gs.t_w * gs.t_h * bpp || gs.t_sx + gs.t_w > ppr * pw)
        return 0;
    cols = gs.t_w / pw;
    rows = (gs.t_h + ph - 1) / ph;
    base = (gs.t_dbp >> 5) + (gs.t_sy / ph) * ppr + gs.t_sx / pw;
    if (base + (rows - 1) * ppr + cols - 1 > 511)
        return 0;
    id = mix32(mix32(mix32(mix32(0x9E3779B9u, s1), s2), n),
               mix32(mix32(gs.t_dbp | (gs.t_dbw << 14) | (psm << 20), gs.t_sx | (gs.t_sy << 16)),
                     gs.t_w | (gs.t_h << 16)));
    /* already what the pages hold: nothing changes */
    for (j = 0; j < rows && same; j++)
        for (i = 0; i < cols; i++) {
            u_int p = base + j * ppr + i;
            if (prov_gen[p] != page_gen[p] + 1u || page_prov[p] != mix32(id, j * cols + i)) {
                same = 0;
                break;
            }
        }
    if (same) {
        gs.trx_on = 0;
        st.uploads_skipped++;
        return 1;
    }
    /* a free note: one whose pages were all written over since */
    for (k = 0; k < NPEND; k++) {
        u_int c = (pend_next + k) % NPEND;
        if (pend[c].owned == 0) {
            k = c;
            break;
        }
    }
    if (k >= NPEND) {
        mat_all();
        k = pend_next % NPEND;
    }
    pend_next = k + 1;
    u = &pend[k];
    u->src = d;
    u->nwords = n;
    u->sum1 = s1;
    u->sum2 = s2;
    u->id = id;
    u->dbp = gs.t_dbp; u->dbw = gs.t_dbw; u->psm = psm;
    u->sx = gs.t_sx; u->sy = gs.t_sy; u->w = gs.t_w; u->h = gs.t_h;
    u->pw = pw; u->ph = ph; u->ppr = ppr;
    u->owned = 0;
    u->checked = 0;
    gen_counter++;
    for (j = 0; j < rows; j++)
        for (i = 0; i < cols; i++) {
            u_int p = base + j * ppr + i;
            if (page_owner[p] >= 0)
                pend_release(page_owner[p]);
            page_owner[p] = (short)k;
            u->owned++;
            page_gen[p] = gen_counter;
            page_prov[p] = mix32(id, j * cols + i);
            prov_gen[p] = gen_counter + 1u;
        }
    gen_counter++;
    gs.trx_on = 0;
    st_deferred++;
    return 1;
}

/* uploads the game repeats unchanged every frame: a whole image that is
 * identical (same place, size and checksum) to the last one sent there,
 * with nothing written to those pages since, is skipped */
#define UPC 16
static struct { u_long bitblt, pos, reg; u_int sum1, sum2, nwords, gen, p0, p1; } upc[UPC];
static u_int upc_next;

static void upload_pages(u_int *p0, u_int *p1)
{
    u_int pw = 64, ph = 32, tb = gs.t_dbw ? gs.t_dbw : 1, bwp, rows;
    switch (bpp_of(gs.t_psm)) {
    case 16: ph = 64; break;
    case 8: pw = 128; ph = 64; break;
    case 4: pw = 128; ph = 128; break;
    }
    if (gs.t_psm == SCE_GS_PSMT8H || gs.t_psm == SCE_GS_PSMT4HL || gs.t_psm == SCE_GS_PSMT4HH) {
        pw = 64; ph = 32;
    }
    bwp = (tb * 64 + pw - 1) / pw;
    rows = (gs.t_sy + gs.t_h + ph - 1) / ph;
    *p0 = (gs.t_dbp * 64) >> 11;
    *p1 = *p0 + bwp * rows - 1 + ((gs.t_dbp & 31) ? 1 : 0);
}

/* 1: this whole upload (n words at d) can be skipped; else remembers
 * nothing yet (upload_done records it once written) */
static int upload_same(const u_int *d, u_int n, u_int *s1, u_int *s2)
{
    u_int a, b, k, p0, p1, p, maxgen = 0;
    upload_sum(d, n, &a, &b);
    *s1 = a;
    *s2 = b;
    for (k = 0; k < UPC; k++) {
        if (upc[k].nwords != n || upc[k].sum1 != a || upc[k].sum2 != b || upc[k].bitblt != gs.bitbltbuf ||
            upc[k].pos != gs.trxpos || upc[k].reg != gs.trxreg)
            continue;
        upload_pages(&p0, &p1);
        for (p = p0; p <= p1 && p < 512; p++)
            if (page_gen[p] > maxgen)
                maxgen = page_gen[p];
        return maxgen < upc[k].gen;
    }
    return 0;
}

static void upload_done(u_int n, u_int s1, u_int s2)
{
    u_int k, slot = upc_next;
    for (k = 0; k < UPC; k++)
        if (upc[k].bitblt == gs.bitbltbuf && upc[k].pos == gs.trxpos && upc[k].reg == gs.trxreg) {
            slot = k;
            break;
        }
    if (slot == upc_next)
        upc_next = (upc_next + 1) % UPC;
    upc[slot].bitblt = gs.bitbltbuf;
    upc[slot].pos = gs.trxpos;
    upc[slot].reg = gs.trxreg;
    upc[slot].sum1 = s1;
    upc[slot].sum2 = s2;
    upc[slot].nwords = n;
    gen_counter++;
    upc[slot].gen = gen_counter;     /* every page written so far is older */
}

/* a whole upload in one piece (n words): skipped when unchanged */
static void trx_whole(const u_int *d, u_int n)
{
    u_int s1, s2;
    if (fb_dest(gs.t_dbp, gs.t_psm)) {
        /* a picture put straight on the screen (the intro's scrolling
         * text): written, and drawn on the GPU's canvas every time */
        u_int p0, p1, dbp = gs.t_dbp, dbw = gs.t_dbw, psm = gs.t_psm;
        u_int sx = gs.t_sx, sy = gs.t_sy, w = gs.t_w, h = gs.t_h;
        upload_pages(&p0, &p1);
        mat_range(p0, p1);
        trx_run(d, n);
        fb_blit(dbp, dbw, psm, sx, sy, w, h);
        return;
    }
    if (upload_same(d, n, &s1, &s2)) {
        gs.trx_on = 0;
        st.uploads_skipped++;
        return;
    }
    {
        int b = bpp_of(gs.t_psm), k = b == 32 ? 0 : b == 16 ? 1 : b == 8 ? 2 : b == 4 ? 3 : 4;
        up_bytes[k] += n * 4;
        up_count[k]++;
    }
    if (defer_upload(d, n, s1, s2)) {
        upload_done(n, s1, s2);
        return;
    }
    if (n >= 1024 && prof_on) {
        /* uploads written word by word (not deferred): which ones, for
         * making them deferrable */
        static u_int nlog;
        if (nlog < 40) {
            nlog++;
            plat_log("  upload written: psm %02X dbp %04X (page %s) dbw %u at %u,%u size %ux%u, %u words\n",
                     gs.t_psm, gs.t_dbp, (gs.t_dbp & 31) ? "offset" : "start", gs.t_dbw, gs.t_sx, gs.t_sy,
                     gs.t_w, gs.t_h, n);
        }
    }
    {
        u_int p0, p1;
        upload_pages(&p0, &p1);
        mat_range(p0, p1);
    }
    trx_run(d, n);
    upload_done(n, s1, s2);
}

/* local -> local copy */
static void trx_local_(void);
static void trx_local(void)
{
    int pv = prof_enter(PB_UPLOAD);
    trx_local_();
    prof_leave(pv);
}
static void trx_local_(void)
{
    u_int sbp = F(gs.bitbltbuf, 0, 14), sbw = F(gs.bitbltbuf, 16, 6), spsm = F(gs.bitbltbuf, 24, 6);
    u_int dbp = F(gs.bitbltbuf, 32, 14), dbw = F(gs.bitbltbuf, 48, 6), dpsm = F(gs.bitbltbuf, 56, 6);
    u_int ssx = F(gs.trxpos, 0, 11), ssy = F(gs.trxpos, 16, 11);
    u_int dsx = F(gs.trxpos, 32, 11), dsy = F(gs.trxpos, 48, 11);
    u_int w = F(gs.trxreg, 0, 12), h = F(gs.trxreg, 32, 12), x, y;
    mat_all();
    gen_counter++;                  /* newer than any texture checked so far */
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++)
            wr_px(dpsm, dbp, dbw, dsx + x, dsy + y, rd_px(spsm, sbp, sbw, ssx + x, ssy + y));
    gen_counter++;
    if (fb_dest(dbp, dpsm) && !fb_dest(sbp, spsm))
        fb_blit(dbp, dbw, dpsm, dsx, dsy, w, h);
}

/* ---- textures -------------------------------------------------------------------- */
/* The game streams textures: in a room it uploads ~50 pictures a frame,
 * several into the same few slots of GS memory, one after another.  So a
 * cache entry is known by its TEX0 (key0), palette (key1) AND the contents
 * of the GS memory pages it reads (ch): a slot that holds picture A, then B,
 * then A again finds A's decoded texture again instead of decoding it. */
#define TEXC 512
#define TEX_BUDGET (15u << 20)      /* real GPU bytes (16-bit textures count half); the
                                     * 32 MB linear heap also holds the vertex rings */      /* GPU texture bytes (linear memory: 32 MB in all) */
typedef struct {
    u_long key0, key1;
    u_int gen;              /* generation when last checked against GS memory */
    u_int ch;               /* hash of its pages' contents when decoded */
    u_int p0, p1;           /* page range */
    int handle, w, h;
    u_int last_use;
    u_int bytes;
    int hnext;              /* next entry with the same key hash (texc_head) */
} TexEnt;
static TexEnt texc[TEXC];
/* entries by their keys (TEX0 and palette): a lookup walks only the few
 * with the same hash, not all 512 (two full scans read ~57 KB a lookup) */
#define TEXH 256
static int texc_head[TEXH];
static inline u_int tex_hash(u_long key0, u_long key1)
{
    u_long h = key0 * 0x9E3779B97F4A7C15ull ^ key1 * 0xC2B2AE3D27D4EB4Full;
    return (u_int)(h >> 56) & (TEXH - 1);
}
static void texc_link(int i)
{
    u_int h = tex_hash(texc[i].key0, texc[i].key1);
    texc[i].hnext = texc_head[h];
    texc_head[h] = i;
}
static void texc_unlink(int i)
{
    int *pp = &texc_head[tex_hash(texc[i].key0, texc[i].key1)];
    while (*pp >= 0) {
        if (*pp == i) {
            *pp = texc[i].hnext;
            return;
        }
        pp = &texc[*pp].hnext;
    }
}
static u_int tex_bytes;
static u_int st_tex_new, st_tex_patch, st_tex_redo;   /* for the log */

/* a page's contents as a hash, recomputed only after the page changes */
static u_int page_hash[512], page_hashed[512];     /* page_hashed: page_gen + 1 when hashed */
static u_int page_content(u_int p)
{
    if (prov_gen[p] == page_gen[p] + 1u)
        return page_prov[p];
    if (page_owner[p] >= 0)
        mat_page(p);
    if (page_hashed[p] != page_gen[p] + 1u) {
        const u_int *wd = vram + (p << 11);
        u_int a = 1, b = 0, c = 0, i;
        for (i = 0; i < 2048; i += 4) {
            u_int x0 = wd[i], x1 = wd[i + 1], x2 = wd[i + 2], x3 = wd[i + 3];
            a += x0; b += a; a += x1; b += a; a += x2; b += a; a += x3; b += a;
            c = ((c << 5) | (c >> 27)) ^ (x0 + (x1 << 1)) ^ (x2 * 3u + (x3 << 3));
        }
        page_hash[p] = a ^ (b * 0x9E3779B1u) ^ (c * 0x85EBCA77u);
        page_hashed[p] = page_gen[p] + 1u;
    }
    return page_hash[p];
}
static u_int frame_no;
/* dump this frame's primitives to the log (set once a new screen has run a while) */
static int dump_now;
unsigned prof_gifdraw_us, gif_draws;    /* primitives from GS packets: drawing time, count */
static u_int gs_state_gen = 1;      /* bumped by every register write that is not vertex data */
extern int g_skip_draw;             /* frame skip: this frame is not drawn (draw_stubs.c) */
extern int plat_logging(void);
u_int diag_state(void);
unsigned prof_upload_us, prof_decode_us, prof_gpu_us, prof_wait_us;
static u_int texbuf[1024 * 1024];

/* ---- the GS CLUT buffer ----------------------------------------------------------
 * A TEX0 (or TEX2) write with CLD set copies the palette from GS memory into
 * the GS's own CLUT buffer; textures then read colours from that buffer at
 * CSA (16-colour units), not from memory.  Games load a 256-colour block
 * once and pick 16-colour slices of it with CSA, so this matters. */
static u_int clutbuf[512];          /* as 32-bit colours */
static u_int clut_serial;           /* bumped when the CLUT buffer changes */
static u_int cbp0 = 0xFFFFFFFF, cbp1 = 0xFFFFFFFF;

static u_int clut_px(u_int cbp, u_int cpsm, u_int x, u_int y)
{
    if (cpsm == SCE_GS_PSMCT32 || cpsm == SCE_GS_PSMCT24)
        return vram[a32(cbp, 1, x, y)];
    {
        u_int h = VR16[a16(cbp, 1, x, y, cpsm == SCE_GS_PSMCT16S)];
        return ((h & 0x1F) << 3) | ((h & 0x3E0) << 6) | ((h & 0x7C00) << 9) | ((h & 0x8000) ? 0x80000000u : 0);
    }
}

static void clut_reload(u_int psm, u_int cbp, u_int cpsm, u_int csa);
static void clut_load(u_long tex0)
{
    u_int psm = F(tex0, 20, 6), cbp = F(tex0, 37, 14), cpsm = F(tex0, 51, 4);
    u_int csa = F(tex0, 56, 5), cld = F(tex0, 61, 3), i;
    if (cld >= 1 && cld <= 5)
        mat_range((cbp * 64) >> 11, ((cbp * 64) >> 11) + 1);
    switch (cld) {
    case 0: return;
    case 1: break;
    case 2: cbp0 = cbp; break;
    case 3: cbp1 = cbp; break;
    case 4: if (cbp == cbp0) return; cbp0 = cbp; break;
    case 5: if (cbp == cbp1) return; cbp1 = cbp; break;
    default: return;
    }
    {
        /* the same load as the last one, from GS memory that has not
         * changed, with nothing else loaded since: the buffer holds it */
        static u_int ml_key = ~0u, ml_gen, ml_serial = ~0u;
        u_int pg = (cbp * 64) >> 11, g0 = page_gen[pg & 511], g1 = page_gen[(pg + 1) & 511];
        u_int gen = g0 > g1 ? g0 : g1;
        u_int key = cbp | (cpsm << 14) | ((psm == SCE_GS_PSMT8 || psm == SCE_GS_PSMT8H) ? 0x80000000u : (csa << 20));
        if (key == ml_key && gen == ml_gen && clut_serial == ml_serial)
            return;
        ml_key = key;
        ml_gen = gen;
        clut_reload(psm, cbp, cpsm, csa);
        ml_serial = clut_serial;
        return;
    }
}

static void clut_reload(u_int psm, u_int cbp, u_int cpsm, u_int csa)
{
    u_int i;
    if (psm == SCE_GS_PSMT8 || psm == SCE_GS_PSMT8H) {
        /* CSM1, 16 x 16: entries 8..15 and 16..23 of each 32 swapped */
        for (i = 0; i < 256; i++) {
            u_int p = (i & 0xE7) | ((i & 8) << 1) | ((i & 0x10) >> 1);
            u_int c = clut_px(cbp, cpsm, p & 15, p >> 4);
            if (clutbuf[i] != c) {
                clutbuf[i] = c;
                clut_serial++;
            }
        }
    } else if (psm == SCE_GS_PSMT4 || psm == SCE_GS_PSMT4HL || psm == SCE_GS_PSMT4HH) {
        /* 8 x 2 colours into the buffer at CSA */
        u_int base = (cpsm == SCE_GS_PSMCT32 || cpsm == SCE_GS_PSMCT24) ? (csa & 15) * 16 : csa * 16;
        for (i = 0; i < 16; i++) {
            u_int c = clut_px(cbp, cpsm, i & 7, i >> 3);
            if (clutbuf[(base + i) & 511] != c) {
                clutbuf[(base + i) & 511] = c;
                clut_serial++;
            }
        }
    }
}

static u_int clut_entry(u_long tex0, u_int i)
{
    u_int psm = F(tex0, 20, 6), cpsm = F(tex0, 51, 4), csa = F(tex0, 56, 5);
    if (psm == SCE_GS_PSMT8 || psm == SCE_GS_PSMT8H)
        return clutbuf[i & 255];
    return clutbuf[(((cpsm == SCE_GS_PSMCT32 || cpsm == SCE_GS_PSMCT24) ? (csa & 15) : csa) * 16 + i) & 511];
}

static u_int expand16(u_int h)
{
    u_int ta0 = F(gs.texa, 0, 8), ta1 = F(gs.texa, 32, 8), aem = F(gs.texa, 15, 1);
    u_int a = (h & 0x8000) ? ta1 : ((aem && (h & 0x7FFF) == 0) ? 0 : ta0);
    return ((h & 0x1F) << 3) | ((h & 0x3E0) << 6) | ((h & 0x7C00) << 9) | (a << 24);
}

/* decode the texels of [x0, x1) x [y0, y1) into texbuf (row stride w) */
static void decode_rect(u_long tex0, const u_int *clut, int ncl, int w, int x0, int y0, int x1, int y1)
{
    u_int tbp = F(tex0, 0, 14), tbw = F(tex0, 14, 6), psm = F(tex0, 20, 6), tcc = F(tex0, 34, 1);
    int x, y;
    if ((psm == SCE_GS_PSMCT32 || psm == SCE_GS_PSMZ32) && !(x0 & 7) && !((x1 - x0) & 7)) {
        u_int amask = tcc ? 0 : 0xFF000000u, aset = tcc ? 0 : 0x80000000u;
        for (y = y0; y < y1; y++) {
            const u_char *cw = cw32 + ((y & 1) << 3);
            u_int *row = texbuf + y * w;
            for (x = x0; x < x1; x += 8) {
                u_int base = a32_run(tbp, tbw, (u_int)x, (u_int)y), k;
                for (k = 0; k < 8; k++)
                    row[x + k] = (vram[base + cw[k]] & ~amask) | aset;
            }
        }
        return;
    }
    if ((psm == SCE_GS_PSMT8 && ncl == 256) || (psm == SCE_GS_PSMT4 && ncl == 16)) {
        /* palette formats, a page-wide run at a time (most of the game's
         * textures): the in-page layout comes from the measured tables */
        u_int pal[256], ppr = (tbw ? tbw : 2) >> 1, k;
        const int t8 = psm == SCE_GS_PSMT8, ph = t8 ? 64 : 128;
        for (k = 0; k < (u_int)ncl; k++)
            pal[k] = tcc ? clut[k] : ((clut[k] & 0xFFFFFF) | 0x80000000u);
        if (!ppr)
            ppr = 1;
        for (y = y0; y < y1; y++) {
            u_int *row = texbuf + y * w;
            u_int prow = ((u_int)y / (u_int)ph) * ppr;
            for (x = x0; x < x1; ) {
                int xe = (x | 127) + 1;
                u_int pagebase = tbp * 64 + (prow + ((u_int)x >> 7)) * 2048;
                if (xe > x1)
                    xe = x1;
                if (t8) {
                    const u_short *m = t8map[y & 63];
                    u_int base = pagebase * 4;
                    for (; x < xe; x++)
                        row[x] = pal[VR8[(base + m[x & 127]) & (VRAM_WORDS * 4 - 1)]];
                } else {
                    const u_short *m = t4map[y & 127];
                    u_int base = pagebase * 8;
                    for (; x < xe; x++) {
                        u_int n = (base + m[x & 127]) & (VRAM_WORDS * 8 - 1);
                        row[x] = pal[(VR8[n >> 1] >> ((n & 1) * 4)) & 15];
                    }
                }
            }
        }
        return;
    }
    for (y = y0; y < y1; y++)
        for (x = x0; x < x1; x++) {
            u_int v = rd_px(psm, tbp, tbw, x, y), c;
            if (ncl)
                c = clut[v & (ncl - 1)];
            else if (psm == SCE_GS_PSMCT16 || psm == SCE_GS_PSMCT16S)
                c = expand16(v);
            else if (psm == SCE_GS_PSMCT24)
                c = (v & 0xFFFFFF) | (F(gs.texa, 0, 8) << 24);
            else
                c = v;
            if (!tcc)
                c = (c & 0xFFFFFF) | 0x80000000u;
            texbuf[y * w + x] = c;
        }
}

static void tex_evict(int i)
{
    if (texc[i].w)
        texc_unlink(i);
    if (texc[i].handle >= 0)
        rend_tex_free(texc[i].handle);     /* freed a frame later: safe if drawn this frame */
    if (texc[i].w)
        tex_bytes -= texc[i].bytes;
    texc[i].handle = -1;
    texc[i].w = 0;
}

/* the least recently used entry (not one drawn this frame unless any = 1) */
static int tex_lru(int any)
{
    int i, best = -1;
    u_int oldest = 0xFFFFFFFF;
    for (i = 0; i < TEXC; i++)
        if (texc[i].w && (any || texc[i].last_use != frame_no) && texc[i].last_use < oldest) {
            oldest = texc[i].last_use;
            best = i;
        }
    return best;
}

/* a slot for a new texture of `need` bytes, keeping under the budget */
static int tex_slot(u_int need)
{
    int i, v;
    int low = 4;    /* little memory left: a few at a time (they are freed two frames later) */
    while ((tex_bytes + need > TEX_BUDGET || (low > 0 && rend_mem_low())) && (v = tex_lru(0)) >= 0) {
        if (tex_bytes + need <= TEX_BUDGET)
            low--;
        tex_evict(v);
    }
    for (i = 0; i < TEXC; i++)
        if (!texc[i].w)
            return i;
    v = tex_lru(0);
    if (v < 0)
        v = tex_lru(1);
    tex_evict(v);
    return v;
}

static int texture_lookup(u_long tex0, int *w_out, int *h_out)
{
    u_int tbp = F(tex0, 0, 14), tbw = F(tex0, 14, 6), psm = F(tex0, 20, 6);
    u_int tw = F(tex0, 26, 4), th = F(tex0, 30, 4);
    int w = 1 << (tw > 10 ? 10 : tw), h = 1 << (th > 10 ? 10 : th);
    /* everything that changes the texels: TBP..CSA (palette base, format,
     * layout and the 16-colour palette offset), without TFX and CLD */
    u_long key0 = tex0 & 0x1FFFFFFFFFFFFFFFull & ~(3ull << 35);
    u_long key1 = (psm == SCE_GS_PSMCT24 || psm == SCE_GS_PSMCT16 || psm == SCE_GS_PSMCT16S)
                  ? gs.texa : 0;
    u_int p0 = (tbp * 64) >> 11, p1, maxgen = 0, p, ch, curgen = 0;
    u_int pw = 64, ph = 32, bwp, rows, tb = tbw ? tbw : 1, bytes;
    int i, best = -1, cur = -1, ncl = 0;
    u_int clut[256];
    /* the pages the texture covers: page rows of its buffer width (TBW,
     * in 64-pixel units) down to its height */
    switch (bpp_of(psm)) {
    case 16: ph = 64; break;
    case 8: pw = 128; ph = 64; break;
    case 4: pw = 128; ph = 128; break;
    }
    if (psm == SCE_GS_PSMT8H || psm == SCE_GS_PSMT4HL || psm == SCE_GS_PSMT4HH) {
        pw = 64; ph = 32;              /* stored in 32-bit pages */
    }
    bwp = (tb * 64 + pw - 1) / pw;
    if (bwp == 0)
        bwp = 1;
    rows = ((u_int)h + ph - 1) / ph;
    p1 = p0 + bwp * rows - 1 + ((tbp & 31) ? 1 : 0);
    if (p1 > 511)
        p1 = 511;
    for (p = p0; p <= p1; p++)
        if (page_gen[p] > maxgen)
            maxgen = page_gen[p];
    if (psm == SCE_GS_PSMT8 || psm == SCE_GS_PSMT8H)
        ncl = 256;
    else if (psm == SCE_GS_PSMT4 || psm == SCE_GS_PSMT4HL || psm == SCE_GS_PSMT4HH)
        ncl = 16;
    if (ncl) {
        /* the colours come from the CLUT buffer: key on them.  The hash is
         * kept while the buffer and the palette fields stay the same */
        static u_int mh_serial = ~0u, mh_fields, mh_hash;
        u_int fields = (u_int)(tex0 >> 51) ^ (psm << 24);      /* CPSM, CSM, CSA; the format */
        u_int hsh;
        if (mh_serial == clut_serial && mh_fields == fields) {
            hsh = mh_hash;
        } else {
            hsh = 2166136261u;
            for (i = 0; i < ncl; i++)
                hsh = (hsh ^ clut_entry(tex0, (u_int)i)) * 16777619u;
            mh_serial = clut_serial;
            mh_fields = fields;
            mh_hash = hsh;
        }
        key1 = ((u_long)hsh << 32) | 1;
    }
    *w_out = w;
    *h_out = h;
    /* 1: an entry checked since its pages last changed */
    for (i = texc_head[tex_hash(key0, key1)]; i >= 0; i = texc[i].hnext) {
        TexEnt *e = &texc[i];
        if (!e->w || e->handle < 0 || e->key0 != key0 || e->key1 != key1)
            continue;
        if (e->gen >= maxgen) {
            e->last_use = frame_no;
            return e->handle;
        }
        if (e->gen >= curgen) {
            curgen = e->gen;
            cur = i;
        }
    }
    /* 2: the pages hold what they held when an entry was decoded */
    ch = 0x811C9DC5u ^ tbp;
    for (p = p0; p <= p1; p++)
        ch = (ch ^ page_content(p)) * 0x01000193u;
    for (i = texc_head[tex_hash(key0, key1)]; i >= 0; i = texc[i].hnext) {
        TexEnt *e = &texc[i];
        if (e->w && e->handle >= 0 && e->ch == ch && e->key0 == key0 && e->key1 == key1) {
            e->gen = gen_counter++;
            e->last_use = frame_no;
            return e->handle;
        }
    }
    st.tex_decodes++;
    for (i = 0; i < ncl; i++)             /* the colours, for decoding */
        clut[i] = clut_entry(tex0, (u_int)i);
    unsigned t_dec = PUS();
    /* 3: only some pages of the latest picture in this slot changed (an
     * animated texture): decode just those pages into it, unless it was
     * already drawn this frame (those draws must keep the old picture) */
    if (cur >= 0 && texc[cur].last_use != frame_no && !(tbp & 31) &&
        texc[cur].w == w && texc[cur].h == h) {
        u_int ndirty = 0, total = bwp * rows;
        for (p = p0; p <= p1; p++)
            if (page_gen[p] > texc[cur].gen)
                ndirty++;
        if (ndirty * 2 <= total) {
            mat_range(p0, p1);
            for (p = p0; p <= p1; p++) {
                u_int rel = p - p0;
                int x0 = (int)((rel % bwp) * pw), y0 = (int)((rel / bwp) * ph), x1, y1;
                if (page_gen[p] <= texc[cur].gen || x0 >= w || y0 >= h)
                    continue;
                x1 = x0 + (int)pw < w ? x0 + (int)pw : w;
                y1 = y0 + (int)ph < h ? y0 + (int)ph : h;
                decode_rect(tex0, clut, ncl, w, x0, y0, x1, y1);
                st_tex_patch++;
                if (rend_tex_patch(texc[cur].handle, texbuf, w, x0, y0, x1 - x0, y1 - y0) < 0) {
                    /* alpha the 16-bit texture cannot hold: the whole
                     * picture again, as RGBA8 (it stays so) */
                    decode_rect(tex0, clut, ncl, w, 0, 0, w, h);
                    rend_tex_update(texc[cur].handle, texbuf);
                    tex_bytes -= texc[cur].bytes;
                    texc[cur].bytes = rend_tex_bytes(texc[cur].handle);
                    tex_bytes += texc[cur].bytes;
                    st_tex_redo++;
                    break;
                }
            }
            prof_decode_us += PUS() - t_dec;
            texc[cur].ch = ch;
            texc[cur].gen = gen_counter++;
            texc[cur].last_use = frame_no;
            return texc[cur].handle;
        }
    }
    /* 4: a new entry */
    bytes = (u_int)w * (u_int)h * 4u;
    best = tex_slot(bytes);
    mat_range(p0, p1);
    decode_rect(tex0, clut, ncl, w, 0, 0, w, h);
    if (dump_now) {
        /* how bright the decoded picture is (a room that looks dark) */
        unsigned long long sr = 0, sg = 0, sb = 0, sa = 0;
        int k, np = w * h;
        for (k = 0; k < np; k++) {
            u_int c = texbuf[k];
            sr += c & 0xFF; sg += (c >> 8) & 0xFF; sb += (c >> 16) & 0xFF; sa += c >> 24;
        }
        plat_log("  tex %08X%08X %dx%d: average R %u G %u B %u A %u\n", (u_int)(tex0 >> 32), (u_int)tex0, w, h,
                 (u_int)(sr / np), (u_int)(sg / np), (u_int)(sb / np), (u_int)(sa / np));
    }
    texc[best].handle = rend_tex_create(w, h, texbuf);
    for (i = 0; texc[best].handle < 0 && i < 8; i++) {
        /* out of GPU memory: let older textures go and try again */
        int v = tex_lru(0), k;
        for (k = 0; k < 4 && v >= 0; k++, v = tex_lru(0))
            if (v != best)
                tex_evict(v);
        texc[best].handle = rend_tex_create(w, h, texbuf);
    }
    prof_decode_us += PUS() - t_dec;
    if (texc[best].handle < 0)
        return -1;
    texc[best].key0 = key0;
    texc[best].key1 = key1;
    texc[best].ch = ch;
    texc[best].gen = gen_counter++;
    texc[best].w = w;
    texc[best].h = h;
    texc[best].p0 = p0;
    texc[best].p1 = p1;
    bytes = rend_tex_bytes(texc[best].handle);     /* what it really takes */
    texc[best].bytes = bytes;
    texc[best].last_use = frame_no;
    tex_bytes += bytes;
    texc_link(best);
    st_tex_new++;
    return texc[best].handle;
}

/* most primitives use the texture the one before used, with nothing
 * written to GS memory or the CLUT buffer in between */
static int texture_for(u_long tex0, int *w_out, int *h_out)
{
    static u_long m_tex0 = ~0ull, m_texa;
    static u_int m_gen, m_clut, m_frame;
    static int m_handle = -1, m_w, m_h;
    if (m_handle >= 0 && tex0 == m_tex0 && gs.texa == m_texa && gen_counter == m_gen &&
        clut_serial == m_clut && frame_no == m_frame) {
        *w_out = m_w;
        *h_out = m_h;
        return m_handle;
    }
    {
        int pv = prof_enter(PB_TEX);
        m_handle = texture_lookup(tex0, &m_w, &m_h);
        prof_leave(pv);
    }
    m_tex0 = tex0;
    m_texa = gs.texa;
    m_gen = gen_counter;
    m_clut = clut_serial;
    m_frame = frame_no;
    *w_out = m_w;
    *h_out = m_h;
    return m_handle;
}

/* ---- primitives ------------------------------------------------------------- */
static int prim_type(void) { return F(gs.prim, 0, 3); }

static void state_for(RState *s, u_long prim, int *fst_out, int *tme_out, int *tw, int *th)
{
    int ctxt = F(prim, 9, 1);
    Ctx *c = &gs.ctx[ctxt];
    u_long tex0 = c->tex0;
    int tme = F(prim, 4, 1);
    memset(s, 0, sizeof *s);
    s->tex = -1;
    *tw = *th = 1;
    if (tme) {
        s->tex = texture_for(tex0, tw, th);
        s->tfx = F(tex0, 35, 2);
        s->tcc = F(tex0, 34, 1);
        s->linear = F(c->tex1, 5, 1);
        s->clamp_s = F(c->clamp, 0, 2) & 1;
        s->clamp_t = F(c->clamp, 2, 2) & 1;
    }
    s->abe = F(prim, 6, 1);
    s->a = F(c->alpha, 0, 2); s->b = F(c->alpha, 2, 2);
    s->c = F(c->alpha, 4, 2); s->d = F(c->alpha, 6, 2);
    s->fix = F(c->alpha, 32, 8);
    s->ate = F(c->test, 0, 1); s->atst = F(c->test, 1, 3);
    s->aref = F(c->test, 4, 8); s->afail = F(c->test, 12, 2);
    s->zte = F(c->test, 16, 1); s->ztst = F(c->test, 17, 2);
    s->zmsk = F(c->zbuf, 32, 1);
    s->fge = F(prim, 5, 1);
    s->fogcol = (u_int)(gs.fogcol & 0xFFFFFF);
    s->gouraud = F(prim, 3, 1);
    {
        int ox = F(c->xyoffset, 0, 16) >> 4, oy = F(c->xyoffset, 32, 16) >> 4;
        (void)ox; (void)oy;
        s->sc_x0 = F(c->scissor, 0, 11); s->sc_x1 = F(c->scissor, 16, 11);
        s->sc_y0 = F(c->scissor, 32, 11); s->sc_y1 = F(c->scissor, 48, 11);
    }
    *fst_out = F(prim, 8, 1);
    *tme_out = tme;
}

/* 1: a texture at the start of a frame buffer one of the contexts draws to */
/* The screen is drawn on the GPU (the canvas): the game's frame buffer and
 * the shown one.  Draws aimed at another buffer (FRAME) are the game
 * rendering into GS memory to use as a texture later - the shadows (a
 * buffer cleared to black, shadow shapes, then blended over the room) and
 * the scrolling intro text.  Such a buffer gets a GPU target of its own
 * (rend_target_handle); draws go there and a texture that reads it samples
 * it, until the game uploads a picture over that memory again. */
static u_int main_fbp;              /* the frame buffer libgraph draws into */
#define SHOW_FBP 150                /* the shown frame (PS2_swap copies into it; menus draw there) */
/* tris_cur / tris_prev: triangles aimed at the buffer this frame and the
 * frame before.  A buffer that gets triangles is a shadow pass (the
 * characters' shadows, drawn as triangles and then laid over the room):
 * left out, the room stays as it is.  A buffer of sprites only is a
 * picture the game composes (the intro's scrolling text): drawn on a GPU
 * target and read back as a texture, whatever else the frame has. */
static struct { u_int tbp, gen; int handle, tw, th; u_int tris_cur, tris_prev; } offs[4];
static u_int offs_n;

/* the target for this context's frame buffer: -1 = the screen, -2 = none */
static int frame_target(const Ctx *c, int tri)
{
    u_int fbp = F(c->frame, 0, 9), fbw = F(c->frame, 16, 6), k;
    {
        static u_char seen[512];
        if (!seen[fbp]) {
            seen[fbp] = 1;
            plat_log("gs: first draw into frame buffer %03X (FRAME %08X%08X, main %03X, 3d this frame %u)\n", fbp,
                     (u_int)(c->frame >> 32), (u_int)c->frame, main_fbp, st.tris3d);
        }
    }
    if (fbp == main_fbp || fbp == SHOW_FBP)
        return -1;
    for (k = 0; k < offs_n; k++)
        if (offs[k].tbp == fbp * 32u)
            break;
    if (k == offs_n) {
        if (offs_n == 4)
            return -2;
        memset(&offs[k], 0, sizeof offs[k]);
        offs[k].tbp = fbp * 32u;
        offs[k].handle = rend_target_handle(fbp, (int)(fbw ? fbw : 10) * 64, &offs[k].tw, &offs[k].th);
        plat_log("gs: frame buffer %03X is drawn off-screen and used as a texture (%s)\n", fbp,
                 offs[k].handle >= 0 ? "GPU target" : "no target left: left out");
        offs_n++;
    }
    offs[k].gen = gen_counter;
    if (tri)
        offs[k].tris_cur++;
    if (offs[k].tris_cur || offs[k].tris_prev)     /* a shadow pass */
        return -2;
    return offs[k].handle >= 0 ? offs[k].handle : -2;
}

/* models (vu_strip.c): the frame buffer their triangles would go to is
 * left out (a room's or cutscene's shadow pass), so they need not be
 * transformed at all */
u_int gs_models_dropped_n;
int gs_models_dropped(void)
{
    /* only a look: a buffer seen before and left out now.  frame_target
     * itself would take one of the two GPU targets for any buffer the
     * context happens to point at (the intro text needs one) */
    u_long p = gs.prmodecont ? 0 : gs.prmode;    /* gs_draw_tris' context */
    const Ctx *c = &gs.ctx[F(p, 9, 1)];
    u_int fbp = F(c->frame, 0, 9), k;
    if (fbp == main_fbp || fbp == SHOW_FBP)
        return 0;
    for (k = 0; k < offs_n; k++)
        if (offs[k].tbp == fbp * 32u) {
            offs[k].tris_cur++;                     /* models are triangles: a shadow pass */
            gs_models_dropped_n++;
            return 1;
        }
    return 0;
}

/* GS memory a shown frame buffer starts at: the game's frame buffer and
 * the shown one (640 x 480 each, colour formats) */
static int fb_dest(u_int dbp, u_int psm)
{
    if (psm != SCE_GS_PSMCT32 && psm != SCE_GS_PSMCT24 && psm != SCE_GS_PSMCT16 && psm != SCE_GS_PSMCT16S)
        return 0;
    return dbp == main_fbp * 32u || dbp == SHOW_FBP * 32u;
}

/* The game wrote pixels straight into a frame buffer (an image transfer or
 * a GS memory copy): on the PS2 they are on the screen.  This port draws
 * the screen on the GPU, so the rectangle is read back from GS memory and
 * drawn on the canvas where it went, in the frame's order of draws (what
 * the game draws over it afterwards stays on top). */
static u_int fb_blit_n;
static int fb_blit_h[32], fb_blit_nh;     /* this frame's pictures' textures */
static void fb_blit(u_int dbp, u_int dbw, u_int psm, u_int x, u_int y, u_int w, u_int h)
{
    RState s;
    RVert v[2];
    u_int i, j, cw, ch;
    int handle;
    if (g_skip_draw || x >= 640 || y >= 480 || !w || !h)
        return;
    cw = x + w > 640 ? 640 - x : w;
    ch = y + h > 480 ? 480 - y : h;
    if (cw > 1024 || ch > 1024)
        return;
    for (j = 0; j < ch; j++)
        for (i = 0; i < cw; i++) {
            u_int c = rd_px(psm, dbp, dbw, x + i, y + j);
            if (psm == SCE_GS_PSMCT16 || psm == SCE_GS_PSMCT16S)
                c = ((c & 0x1F) << 3) | (((c >> 5) & 0x1F) << 11) | (((c >> 10) & 0x1F) << 19);
            texbuf[j * cw + i] = (c & 0xFFFFFF) | 0x80000000u;     /* opaque (GS alpha 0x80) */
        }
    handle = rend_tex_create((int)cw, (int)ch, texbuf);
    if (handle < 0)
        return;
    memset(&s, 0, sizeof s);
    s.tex = handle;
    s.tfx = 1;                          /* decal: the picture as it is */
    s.clamp_s = s.clamp_t = 1;
    s.gouraud = 1;
    s.sc_x1 = 639;
    s.sc_y1 = 479;
    memset(v, 0, sizeof v);
    v[0].x = (float)x; v[0].y = (float)y; v[0].u = 0.0f; v[0].v = 0.0f;
    v[1].x = (float)(x + cw); v[1].y = (float)(y + ch); v[1].u = 1.0f; v[1].v = 1.0f;
    v[0].w = v[1].w = 1.0f;
    v[0].fog = v[1].fog = 1.0f;
    v[0].rgba = v[1].rgba = 0x80808080u;
    if (!gs.frame_open) {
        rend_frame_begin();
        gs.frame_open = 1;
    }
    rend_use_target(-1);
    rend_draw(&s, RPRIM_SPRITE, v, 2);
    /* freed when the frame has been handed over (the batch takes the
     * texture when it is flushed) */
    if (fb_blit_nh < 32)
        fb_blit_h[fb_blit_nh++] = handle;
    else
        rend_tex_free(handle);
    if (fb_blit_n++ < 8)
        plat_log("gs: a picture put straight into frame buffer %03X: %ux%u at %u,%u (drawn on the canvas)\n",
                 dbp / 32, w, h, x, y);
}

/* an off-screen target that this texture reads (nothing uploaded over it since) */
static int tex_target(u_long tex0)
{
    u_int tbp = F(tex0, 0, 14), psm = F(tex0, 20, 6), k;
    /* a palette texture is never a picture the GS drew: the game keeps
     * textures (the intro's text) in the shadow buffer's memory too */
    if (psm == SCE_GS_PSMT8 || psm == SCE_GS_PSMT4 || psm == SCE_GS_PSMT8H ||
        psm == SCE_GS_PSMT4HL || psm == SCE_GS_PSMT4HH)
        return -1;
    for (k = 0; k < offs_n; k++)
        if (tbp == offs[k].tbp && offs[k].handle >= 0 && !offs[k].tris_cur && !offs[k].tris_prev &&
            page_gen[(tbp * 64) >> 11] <= offs[k].gen)
            return (int)k;
    return -1;
}

/* the game's frame buffer read as a texture (PS2_swap's blend of the new
 * frame over the shown one): left out */
static int tex_reads_frame(u_long tex0)
{
    u_int tbp = F(tex0, 0, 14), psm = F(tex0, 20, 6), k;
    /* a palette texture is never a picture the GS drew: the game reuses
     * the shadow buffer's memory for room textures (the outdoor sky) */
    if (psm == SCE_GS_PSMT8 || psm == SCE_GS_PSMT4 || psm == SCE_GS_PSMT8H ||
        psm == SCE_GS_PSMT4HL || psm == SCE_GS_PSMT4HH)
        return 0;
    if (tbp == main_fbp * 32u)
        return 1;
    for (k = 0; k < 2; k++) {
        u_int fbp = F(gs.ctx[k].frame, 0, 9);
        if (fbp != SHOW_FBP && tbp == fbp * 32u && tex_target(tex0) < 0)
            return 1;
    }
    /* an off-screen buffer that was left out (a room's shadow pass) */
    if (tex_target(tex0) < 0)
        for (k = 0; k < offs_n; k++)
            if (tbp == offs[k].tbp && page_gen[(tbp * 64) >> 11] <= offs[k].gen)
                return 1;
    return 0;
}

static void vertex_kick(u_int x, u_int y, u_int z, u_int f, int drawing)
{
    u_long prim = gs.prmodecont ? gs.prim : ((gs.prim & 7) | (gs.prmode & ~7ull));
    Ctx *c = &gs.ctx[F(prim, 9, 1)];
    RVert *v = &gs.vq[gs.vn];
    float ox = (float)F(c->xyoffset, 0, 16), oy = (float)F(c->xyoffset, 32, 16);
    v->x = ((float)x - ox) / 16.0f;
    v->y = ((float)y - oy) / 16.0f;
    /* the game's z values stay below 65535 whatever the z buffer format
     * (its 2D and 3D code clamp them to 65534), so that range is the GPU's
     * 0..1: dividing a 32-bit format's range would leave a few hundred
     * steps of depth */
    v->z = (float)z * (1.0f / 65535.0f);
    if (v->z > 1.0f)
        v->z = 1.0f;
    v->w = 1.0f;
    v->rgba = (u_int)gs.rgbaq;
    v->fog = (float)f / 255.0f;
    if (F(prim, 8, 1)) {                       /* FST: UV in texels (12.4) */
        v->u = (float)F(gs.uv, 0, 14) / 16.0f;
        v->v = (float)F(gs.uv, 16, 14) / 16.0f;
        v->fog = (float)f / 255.0f;
        /* normalized later with the texture size */
        v->u = -v->u - 1.0f;                   /* mark as texel units */
    } else {
        float q = gs.q != 0.0f ? gs.q : 1.0f;
        union { u_int i; float f; } s, t;
        s.i = (u_int)gs.st;
        t.i = (u_int)(gs.st >> 32);
        v->u = s.f / q;
        v->v = t.f / q;
        /* q is 1/z for 3D: w = z makes the GPU map the texture with
         * perspective, as the GS does with STQ */
        if (q > 0.0f && q != 1.0f)
            v->w = 1.0f / q;
    }
    gs.vn++;
    if (!drawing) {
        /* XYZ3 / XYZF3: the vertex goes in the queue without a kick */
    }
    {
        int pt = prim_type(), need = 0, rp = 0;
        switch (pt) {
        case SCE_GS_PRIM_POINT: need = 1; rp = RPRIM_POINT; break;
        case SCE_GS_PRIM_LINE: case SCE_GS_PRIM_LINESTRIP: need = 2; rp = RPRIM_LINE; break;
        case SCE_GS_PRIM_TRI: case SCE_GS_PRIM_TRISTRIP: case SCE_GS_PRIM_TRIFAN: need = 3; rp = RPRIM_TRI; break;
        case SCE_GS_PRIM_SPRITE: need = 2; rp = RPRIM_SPRITE; break;
        default: gs.vn = 0; return;
        }
        if (gs.vn < need)
            return;
        int tgt = drawing ? frame_target(c, rp == RPRIM_TRI) : -1;
        if (drawing && offs_n && plat_logging()) {
            /* the off-screen buffers (the intro's text): every draw into
             * one or reading one, the first 400, for the log */
            static u_int ntrace;
            u_int fbp = F(c->frame, 0, 9);
            int into = fbp != main_fbp && fbp != SHOW_FBP;
            int from = F(prim, 4, 1) && tex_target(c->tex0) >= 0;
            u_int k, reads = 0;
            for (k = 0; k < offs_n && F(prim, 4, 1); k++)
                if (F(c->tex0, 0, 14) == offs[k].tbp)
                    reads = 1;
            if ((into || reads) && ntrace < 400) {
                ntrace++;
                plat_log("  offscreen f%u: %s fbp %03X prim %03X type %d tex0 %08X%08X %s alpha %X | (%.0f,%.0f uv %.3f,%.3f c%08X) (%.0f,%.0f uv %.3f,%.3f)\n",
                         frame_no, into ? (tgt >= 0 ? "INTO target" : tgt == -2 ? "into, LEFT OUT" : "into screen?") : "reads",
                         fbp, (u_int)(prim & 0x7FF), pt, (u_int)(c->tex0 >> 32), (u_int)c->tex0,
                         reads ? (from ? "sampled from target" : "READ LEFT OUT") : "", (u_int)c->alpha,
                         gs.vq[0].x, gs.vq[0].y, gs.vq[0].u, gs.vq[0].v, gs.vq[0].rgba,
                         gs.vq[need - 1].x, gs.vq[need - 1].y, gs.vq[need - 1].u, gs.vq[need - 1].v);
            }
        }
        if (drawing && dump_now && (tgt == -2 || (F(prim, 4, 1) && tex_reads_frame(c->tex0)))) {
            static u_int lo_frame = ~0u, lo_n;
            if (lo_frame != frame_no) {
                lo_frame = frame_no;
                lo_n = 0;
            }
            if (lo_n++ < 30) {
                int k;
                plat_log("  left out (%s): prim %03X fst %d frame %08X%08X tex0 %08X%08X alpha %X test %X |",
                         tgt == -2 ? "off-screen" : "reads frame", (u_int)(prim & 0x7FF), (int)F(prim, 8, 1),
                         (u_int)(c->frame >> 32), (u_int)c->frame, (u_int)(c->tex0 >> 32), (u_int)c->tex0,
                         (u_int)c->alpha, (u_int)c->test);
                for (k = 0; k < need; k++)
                    plat_log(" (%.1f,%.1f z%.3f uv %.3f,%.3f c%08X)", gs.vq[k].x, gs.vq[k].y, gs.vq[k].z,
                             gs.vq[k].u, gs.vq[k].v, gs.vq[k].rgba);
                plat_log("\n");
            }
        }
        if (drawing && tgt == -2) {
            st.fb_reads++;
        } else if (drawing && F(prim, 4, 1) && tex_reads_frame(c->tex0)) {
            /* the texture is a frame buffer (PS2_swap's 75% blend of the
             * new frame over the shown one): this port draws frames on
             * the GPU, not in GS memory, so it would blend in black */
            static int told;
            if (!told++)
                plat_log("gs: draws that read a frame buffer as a texture are skipped (TEX0 %08X%08X)\n",
                         (u_int)(c->tex0 >> 32), (u_int)c->tex0);
            st.fb_reads++;
        } else if (drawing && !g_skip_draw) {
            st.bytype[pt & 7]++;
            static u_int dump2d_frame = ~0u, dump2d_n;
            if (dump2d_frame != frame_no) {
                dump2d_frame = frame_no;
                dump2d_n = 0;
            }
            if (dump_now && dump2d_n++ < 40) {
                int k;
                plat_log("  prim %u: type %d prim %03X ctx %d tme %d fst %d abe %d |", st.prims, pt,
                         (u_int)(prim & 0x7FF), (int)F(prim, 9, 1), (int)F(prim, 4, 1), (int)F(prim, 8, 1),
                         (int)F(prim, 6, 1));
                for (k = 0; k < need; k++)
                    plat_log(" (%.1f,%.1f z%.3f uv %.3f,%.3f c%08X)", gs.vq[k].x, gs.vq[k].y, gs.vq[k].z,
                             gs.vq[k].u, gs.vq[k].v, gs.vq[k].rgba);
                plat_log(" ofs %X,%X\n", (u_int)F(c->xyoffset, 0, 16), (u_int)F(c->xyoffset, 32, 16));
                plat_log("    tex0 %08X%08X alpha %X test %X zbuf %X fba %X\n", (u_int)(c->tex0 >> 32), (u_int)c->tex0,
                         (u_int)c->alpha | (u_int)(F(c->alpha, 32, 8) << 8), (u_int)c->test, (u_int)c->zbuf,
                         (u_int)c->fba);
            }
            RState s;
            RVert out[3];
            int fst, tme, tw, th, i, rt;
            /* the state as the primitive before had it, while no register
             * that sets it was written and GS memory did not change */
            static u_int m_state = 0, m_gen;
            static u_long m_prim;
            static u_int m_frame = ~0u;
            static RState m_s;
            static int m_fst, m_tme, m_tw, m_th, m_rt;
            if (m_state == gs_state_gen && m_gen == gen_counter && m_prim == prim && m_frame == frame_no) {
                s = m_s; fst = m_fst; tme = m_tme; tw = m_tw; th = m_th; rt = m_rt;
            } else {
                state_for(&s, prim, &fst, &tme, &tw, &th);
                rt = tme ? tex_target(c->tex0) : -1;
                if (rt >= 0)
                    s.tex = offs[rt].handle;
                m_state = gs_state_gen; m_gen = gen_counter; m_prim = prim; m_frame = frame_no;
                m_s = s; m_fst = fst; m_tme = tme; m_tw = tw; m_th = th; m_rt = rt;
            }
            for (i = 0; i < need; i++) {
                out[i] = gs.vq[i];
                if (tme && out[i].u < 0.0f) {      /* texel units -> normalized */
                    out[i].u = (-out[i].u - 1.0f) / (float)tw;
                    out[i].v = out[i].v / (float)th;
                }
                if (rt >= 0) {                     /* a GPU target: its own layout */
                    out[i].u = out[i].u * (float)tw / (float)offs[rt].tw;
                    out[i].v = 1.0f - out[i].v * (float)th / (float)offs[rt].th;
                }
                if (!s.gouraud)
                    out[i].rgba = gs.vq[need - 1].rgba;
            }
            if (!gs.frame_open) {
                rend_frame_begin();
                gs.frame_open = 1;
            }
            /* a triangle with perspective depth (STQ, q != 1: the
             * see-through models - hair, glass - come this way) is part of
             * the 3D scene: shifted per eye like the models */
            if (tgt == -1 && rp == RPRIM_TRI && (out[0].w != 1.0f || out[1].w != 1.0f || out[2].w != 1.0f))
                s.stereo = 1;
            {
                unsigned tk = PUS();
                int pv = prof_enter(PB_GSDRAW);
                rend_use_target(tgt);
                rend_draw(&s, rp, out, need);
                prof_leave(pv);
                prof_gifdraw_us += PUS() - tk;
            }
            gif_draws++;
            st.prims++;
            if (rp == RPRIM_SPRITE) st.sprites++;
            if (rp == RPRIM_TRI) st.tris++;
            if (tme) st.tex_prims++;
        }
        /* advance the queue */
        switch (pt) {
        case SCE_GS_PRIM_LINESTRIP: gs.vq[0] = gs.vq[1]; gs.vn = 1; break;
        case SCE_GS_PRIM_TRISTRIP: gs.vq[0] = gs.vq[1]; gs.vq[1] = gs.vq[2]; gs.vn = 2; break;
        case SCE_GS_PRIM_TRIFAN: gs.vq[1] = gs.vq[2]; gs.vn = 2; break;
        default: gs.vn = 0; break;
        }
    }
}

/* triangles from the port's model code (vu_strip.c), already projected:
 * x, y in GS pixels, z the GS z value (not clamped), w the view depth (the
 * GPU clips against it and maps textures with perspective), u, v
 * normalized.  Drawn with the current GS state and this PRIM value, as the
 * VU1 microcode's GIF packets were. */
static u_int dump_zlo, dump_zhi, dump_wneg, dump_3d_out, dump_3d_out_logged;

/* night mist (a port option; the PS2's own fog fades to near black): 3D
 * surfaces fade into a dark blue-grey with view depth, and the empty
 * background (the sky outdoors) is that colour.  0 off, 1 light, 2 heavy */
#define MIST_COL 0x00362E28u        /* 0x00BBGGRR */
static int mist_level = 1;
int g_port_door;                    /* the door / stairs scene plays: no mist */
void gs_set_mist(int level)
{
    extern void rend_set_mist(uint32_t rgb, int on);
    mist_level = level;
    rend_set_mist(MIST_COL, level > 0);
}

static void apply_mist(RState *s, RVert *v, int n)
{
    static const float dens[3] = { 0.0f, 0.0065f, 0.014f };
    const float start = 25.0f, d = dens[mist_level];
    u_int fc = s->fogcol;
    /* the game's fog colour when it has one (not near black), else the mist's */
    int keep = s->fge && ((fc & 0xFF) + ((fc >> 8) & 0xFF) + ((fc >> 16) & 0xFF)) > 0x60;
    int i;
    for (i = 0; i < n; i++) {
        float f = s->fge ? v[i].fog : 1.0f, x = v[i].w - start;
        if (x > 0.0f)
            f *= expf(-d * x);
        v[i].fog = f;
    }
    if (!keep)
        s->fogcol = MIST_COL;
    s->fge = 1;
    s->mist = 1;
}
void gs_draw_tris(u_int prim, RVert *v, int n)
{
    u_long p = gs.prmodecont ? prim : ((prim & 7) | (gs.prmode & ~7ull));
    Ctx *c = &gs.ctx[F(p, 9, 1)];
    float ox = (float)F(c->xyoffset, 0, 16) / 16.0f, oy = (float)F(c->xyoffset, 32, 16) / 16.0f;
    RState s;
    int fst, tme, tw, th, i;
    int tgt;
    if (n < 3 || g_skip_draw)
        return;
    tgt = frame_target(c, 1);
    if (tgt == -2) {
        dump_3d_out += (u_int)n / 3;
        if (dump_now && dump_3d_out_logged++ < 8)
            plat_log("  3d left out (off-screen): %d tris frame %08X%08X tex0 %08X%08X | (%.1f,%.1f z%.1f w%.1f)\n",
                     n / 3, (u_int)(c->frame >> 32), (u_int)c->frame, (u_int)(c->tex0 >> 32), (u_int)c->tex0,
                     v[0].x, v[0].y, v[0].z, v[0].w);
        return;
    }
    state_for(&s, p, &fst, &tme, &tw, &th);
    for (i = 0; i < n; i++) {
        v[i].x -= ox;
        v[i].y -= oy;
        v[i].z *= 1.0f / 65535.0f;
    }
    if (!s.gouraud)
        for (i = 0; i + 2 < n; i += 3)
            v[i].rgba = v[i + 1].rgba = v[i + 2].rgba;
    if (dump_now)
        for (i = 0; i < n; i++) {           /* depth the 3DS would have clipped */
            if (v[i].z < -1.0f) dump_zlo++;
            else if (v[i].z > 1.0f) dump_zhi++;
            if (v[i].w < 0.0f) dump_wneg++;
        }
    if (dump_now && st.prims < 400) {
        static u_int last_frame = ~0u, logged;
        if (last_frame != frame_no) {
            last_frame = frame_no;
            logged = 0;
        }
        if (logged++ < 24)
            plat_log("  3d: %d tris prim %03X tex0 %08X%08X alpha %X test %X | (%.1f,%.1f z%.3f w%.1f uv %.3f,%.3f c%08X f%.2f)\n",
                     n / 3, (u_int)(p & 0x7FF), (u_int)(c->tex0 >> 32), (u_int)c->tex0,
                     (u_int)c->alpha, (u_int)c->test, v[0].x, v[0].y, v[0].z, v[0].w, v[0].u, v[0].v,
                     v[0].rgba, v[0].fog);
    }
    if (!gs.frame_open) {
        rend_frame_begin();
        gs.frame_open = 1;
    }
    s.stereo = tgt == -1;       /* the 3D scene on screen: drawn per eye */
    if (mist_level && tgt == -1 && !g_port_door)
        apply_mist(&s, v, n);
    rend_use_target(tgt);
    rend_draw_tris(&s, v, n);
    st.prims += (u_int)n / 3;
    st.tris += (u_int)n / 3;
    st.bytype[SCE_GS_PRIM_TRISTRIP] += (u_int)n / 3;
    st.tris3d += (u_int)n / 3;
    if (tme)
        st.tex_prims += (u_int)n / 3;
}

/* the models' fast path (vu_strip.c): what gs_draw_tris does, once for a
 * batch; the vertices are then made straight into the GPU's format */
static int gs_tris_begin_(unsigned prim, RState *s, GsXf *xf);
int gs_tris_begin(unsigned prim, RState *s, GsXf *xf)
{
    int pv = prof_enter(PB_STATE), r = gs_tris_begin_(prim, s, xf);
    prof_leave(pv);
    return r;
}
static int gs_tris_begin_(unsigned prim, RState *s, GsXf *xf)
{
    u_long p = gs.prmodecont ? prim : ((prim & 7) | (gs.prmode & ~7ull));
    Ctx *c = &gs.ctx[F(p, 9, 1)];
    int fst, tme, tw, th, tgt;
    if (g_skip_draw)
        return 0;
    if (dump_now)
        return -1;
    tgt = frame_target(c, 1);
    if (tgt == -2)
        return 0;
    state_for(s, p, &fst, &tme, &tw, &th);
    xf->ox = (float)F(c->xyoffset, 0, 16) / 16.0f;
    xf->oy = (float)F(c->xyoffset, 32, 16) / 16.0f;
    xf->gouraud = s->gouraud;
    xf->fog_game = s->fge;
    xf->mist_d = 0.0f;
    xf->mist_start = 25.0f;
    xf->tme = tme;
    if (!gs.frame_open) {
        rend_frame_begin();
        gs.frame_open = 1;
    }
    s->stereo = tgt == -1;
    if (mist_level && tgt == -1 && !g_port_door) {        /* as apply_mist */
        static const float dens[3] = { 0.0f, 0.0065f, 0.014f };
        u_int fc = s->fogcol;
        int keep = s->fge && ((fc & 0xFF) + ((fc >> 8) & 0xFF) + ((fc >> 16) & 0xFF)) > 0x60;
        xf->mist_d = dens[mist_level];
        if (!keep)
            s->fogcol = MIST_COL;
        s->fge = 1;
        s->mist = 1;
    }
    rend_use_target(tgt);
    return 1;
}

void gs_tris_count(int ntris, int tme)
{
    st.prims += (u_int)ntris;
    st.tris += (u_int)ntris;
    st.bytype[SCE_GS_PRIM_TRISTRIP] += (u_int)ntris;
    st.tris3d += (u_int)ntris;
    if (tme)
        st.tex_prims += (u_int)ntris;
}

/* ---- register writes ------------------------------------------------------------ */
static void gs_reg(u_int reg, u_long v)
{
    /* RGBAQ, ST, UV, XYZ*, FOG are vertex data; anything else may change
     * the state a primitive is drawn with (vertex_kick's memo) */
    if (reg > 0x05 && reg != SCE_GS_FOG && reg != SCE_GS_XYZF3 && reg != SCE_GS_XYZ3)
        gs_state_gen++;
    else if (reg == SCE_GS_PRIM)
        gs_state_gen++;
    switch (reg) {
    case SCE_GS_PRIM: gs.prim = v; gs.vn = 0; break;
    case SCE_GS_RGBAQ: gs.rgbaq = v; { union { u_int i; float f; } q; q.i = (u_int)(v >> 32); gs.q = q.f; } break;
    case SCE_GS_ST: gs.st = v; break;
    case SCE_GS_UV: gs.uv = v; break;
    case SCE_GS_XYZF2: vertex_kick(F(v, 0, 16), F(v, 16, 16), F(v, 32, 24), F(v, 56, 8), 1); break;
    case SCE_GS_XYZ2: vertex_kick(F(v, 0, 16), F(v, 16, 16), (u_int)(v >> 32), gs.fog, 1); break;
    case SCE_GS_XYZF3: vertex_kick(F(v, 0, 16), F(v, 16, 16), F(v, 32, 24), F(v, 56, 8), 0); break;
    case SCE_GS_XYZ3: vertex_kick(F(v, 0, 16), F(v, 16, 16), (u_int)(v >> 32), gs.fog, 0); break;
    case SCE_GS_FOG: gs.fog = F(v, 56, 8); break;
    case SCE_GS_TEX0_1: case SCE_GS_TEX0_2:
        gs.ctx[reg - SCE_GS_TEX0_1].tex0 = v;
        clut_load(v);
        break;
    case 0x16: case 0x17: {         /* TEX2: the palette fields and PSM of TEX0 */
        const u_long m = (0x3Full << 20) | (0x7FFFFFFFull << 37);
        u_long *t = &gs.ctx[reg - 0x16].tex0;
        *t = (*t & ~m) | (v & m);
        clut_load(v);
    } break;
    case SCE_GS_CLAMP_1: case SCE_GS_CLAMP_2: gs.ctx[reg - SCE_GS_CLAMP_1].clamp = v; break;
    case SCE_GS_TEX1_1: case SCE_GS_TEX1_2: gs.ctx[reg - SCE_GS_TEX1_1].tex1 = v; break;
    case SCE_GS_XYOFFSET_1: case SCE_GS_XYOFFSET_2: gs.ctx[reg - SCE_GS_XYOFFSET_1].xyoffset = v; break;
    case SCE_GS_PRMODECONT: gs.prmodecont = (int)(v & 1); break;
    case SCE_GS_PRMODE: gs.prmode = v; break;
    case SCE_GS_TEXCLUT: gs.texclut = v; break;
    case SCE_GS_TEXA: gs.texa = v; break;
    case SCE_GS_FOGCOL: gs.fogcol = v; break;
    case SCE_GS_SCISSOR_1: case SCE_GS_SCISSOR_2: gs.ctx[reg - SCE_GS_SCISSOR_1].scissor = v; break;
    case SCE_GS_ALPHA_1: case SCE_GS_ALPHA_2: gs.ctx[reg - SCE_GS_ALPHA_1].alpha = v; break;
    case SCE_GS_TEST_1: case SCE_GS_TEST_2: gs.ctx[reg - SCE_GS_TEST_1].test = v; break;
    case SCE_GS_FBA_1: case SCE_GS_FBA_2: gs.ctx[reg - SCE_GS_FBA_1].fba = v; break;
    case SCE_GS_FRAME_1: case SCE_GS_FRAME_2: gs.ctx[reg - SCE_GS_FRAME_1].frame = v; break;
    case SCE_GS_ZBUF_1: case SCE_GS_ZBUF_2: gs.ctx[reg - SCE_GS_ZBUF_1].zbuf = v; break;
    case SCE_GS_PABE: gs.pabe = (int)(v & 1); break;
    case SCE_GS_DTHE: gs.dthe = (int)(v & 1); break;
    case SCE_GS_COLCLAMP: gs.colclamp = (int)(v & 1); break;
    case SCE_GS_BITBLTBUF: gs.bitbltbuf = v; break;
    case SCE_GS_TRXPOS: gs.trxpos = v; break;
    case SCE_GS_TRXREG: gs.trxreg = v; break;
    case SCE_GS_TRXDIR:
        gs.trx_x = gs.trx_y = 0;
        gs.trx_bits = 0;
        gs.trx_acc = 0;
        if ((v & 3) == 0) {
            gen_counter++;
            gs.trx_on = 1;
            gs.t_dbp = F(gs.bitbltbuf, 32, 14);
            gs.t_dbw = F(gs.bitbltbuf, 48, 6);
            gs.t_psm = F(gs.bitbltbuf, 56, 6);
            gs.t_sx = F(gs.trxpos, 32, 11);
            gs.t_sy = F(gs.trxpos, 48, 11);
            gs.t_w = F(gs.trxreg, 0, 12);
            gs.t_h = F(gs.trxreg, 32, 12);
            if (gs.t_w == 0 || gs.t_h == 0)
                gs.trx_on = 0;
            st.uploads++;
        }
        else if ((v & 3) == 2)
            trx_local();
        break;
    default: break;
    }
}

/* ---- GIF packets ----------------------------------------------------------------
 * The GIF keeps its place in a packet across DMA transfers: a tag can
 * announce IMAGE data (or vertices) that arrive in the next DMA block.  So
 * the parser is a stream: gif_feed() continues the current tag. */
static struct {
    int active;             /* inside a tag */
    u_int nloop, flg, nreg;
    u_long regs;
    u_int k, r;             /* loop and register position */
    u_int w;                /* REGLIST: 64-bit word position in the quadword */
} gif;

static void gif_packed(u_int desc, const u_int *d)
{
    switch (desc) {
    case 0x0: gs_reg(SCE_GS_PRIM, d[0] & 0x7FF); break;
    case 0x1: gs.rgbaq = (u_long)(d[0] & 0xFF) | ((u_long)(d[1] & 0xFF) << 8) |
                         ((u_long)(d[2] & 0xFF) << 16) | ((u_long)(d[3] & 0xFF) << 24) |
                         ((u_long)(*(u_int *)&gs.q) << 32);
              break;
    case 0x2: gs.st = (u_long)d[0] | ((u_long)d[1] << 32);
              { union { u_int i; float f; } qq; qq.i = d[2]; gs.q = qq.f; }
              break;
    case 0x3: gs.uv = (u_long)(d[0] & 0x3FFF) | ((u_long)(d[1] & 0x3FFF) << 16); break;
    case 0x4: vertex_kick(d[0] & 0xFFFF, d[1] & 0xFFFF, (d[2] >> 4) & 0xFFFFFF,
                          (d[3] >> 4) & 0xFF, !((d[3] >> 15) & 1));
              break;
    case 0x5: vertex_kick(d[0] & 0xFFFF, d[1] & 0xFFFF, d[2], gs.fog, !((d[3] >> 15) & 1)); break;
    case 0xA: gs.fog = (d[3] >> 4) & 0xFF; break;
    case 0xE: gs_reg(d[2] & 0xFF, (u_long)d[0] | ((u_long)d[1] << 32)); break;
    case 0xF: break;
    default: gs_reg(desc == 0xC ? SCE_GS_XYZF3 : desc == 0xD ? SCE_GS_XYZ3 : desc,
                    (u_long)d[0] | ((u_long)d[1] << 32));
        break;
    }
}

/* feed n quadwords to the GIF; returns how many were used (all of them) */
u_int gif_packet(const u_int *q, u_int n_qw)
{
    u_int i = 0;
    while (i < n_qw) {
        if (!gif.active) {
            const u_int *t = q + i * 4;
            u_long tag = (u_long)t[0] | ((u_long)t[1] << 32);
            i++;
            st.gif_tags++;
            gif.nloop = F(tag, 0, 15);
            gif.flg = F(tag, 58, 2);
            gif.nreg = F(tag, 60, 4);
            if (gif.nreg == 0)
                gif.nreg = 16;
            gif.regs = (u_long)t[2] | ((u_long)t[3] << 32);
            gif.k = gif.r = gif.w = 0;
            if (F(tag, 46, 1) && gif.flg != SCE_GIF_IMAGE)
                gs_reg(SCE_GS_PRIM, F(tag, 47, 11));
            gif.active = gif.nloop != 0;
            continue;
        }
        if (gif.flg == SCE_GIF_PACKED) {
            gif_packed((u_int)(gif.regs >> (gif.r * 4)) & 15, q + i * 4);
            i++;
            if (++gif.r == gif.nreg) {
                gif.r = 0;
                if (++gif.k == gif.nloop)
                    gif.active = 0;
            }
        } else if (gif.flg == SCE_GIF_REGLIST) {
            const u_int *d = q + i * 4 + gif.w * 2;
            u_int desc = (u_int)(gif.regs >> (gif.r * 4)) & 15;
            if (desc != 0xE && desc != 0xF)
                gs_reg(desc, (u_long)d[0] | ((u_long)d[1] << 32));
            gif.w ^= 1;
            if (gif.w == 0)
                i++;
            if (++gif.r == gif.nreg) {
                gif.r = 0;
                if (++gif.k == gif.nloop) {
                    gif.active = 0;
                    if (gif.w) {            /* an odd count pads the quadword */
                        gif.w = 0;
                        i++;
                    }
                }
            }
        } else {                            /* IMAGE (and the disabled mode 3) */
            /* the whole run of image quadwords in this block at once */
            u_int m = n_qw - i, j;
            unsigned t0 = PUS();
            int pvu = prof_enter(PB_UPLOAD);
            if (m > gif.nloop - gif.k)
                m = gif.nloop - gif.k;
            if (gs.trx_on && gs.trx_x == 0 && gs.trx_y == 0 &&
                !(gif.k == 0 && m == gif.nloop && bpp_of(gs.t_psm) != 24)) {
                u_int p0, p1;
                upload_pages(&p0, &p1);
                mat_range(p0, p1);
            }
            if (bpp_of(gs.t_psm) == 24) {
                for (j = 0; j < m; j++) {
                    const u_int *d = q + (i + j) * 4;
                    trx_put64((u_long)d[0] | ((u_long)d[1] << 32));
                    trx_put64((u_long)d[2] | ((u_long)d[3] << 32));
                }
            } else if (gif.k == 0 && m == gif.nloop && gs.trx_on && gs.trx_x == 0 && gs.trx_y == 0)
                trx_whole(q + i * 4, m * 4);
            else
                trx_run(q + i * 4, m * 4);
            i += m;
            gif.k += m;
            if (gif.k == gif.nloop)
                gif.active = 0;
            prof_upload_us += PUS() - t0;
            prof_leave(pvu);
        }
    }
    return i;
}

/* ---- DMA chains, VIF1 ------------------------------------------------------------- */
static u_int vu1mem[1024 * 4];          /* 16 KB of VU1 data memory */
static u_int vif_row[4], vif_col[4], vif_mask, vif_cl = 1, vif_wl = 1, vif_mode;
static u_int vif_base, vif_ofst, vif_tops, vif_itop, vif_dbf;

u_int *vu1_data(void) { return vu1mem; }
u_int vu1_tops(void) { return vif_tops; }
u_int vu1_itop(void) { return vif_itop; }

/* the VU1 micro programs are done in C by the port (vu1_hle.c) */
__attribute__((weak)) void vu1_mscal(u_int addr)
{
    static int logged;
    if (logged < 20) {
        plat_log("vu1: mscal %04X (not drawn yet)\n", addr);
        logged++;
    }
}

static void *dma_ptr(u_int a)
{
    if (a & 0x80000000u) {
        u_int lo = a & 0x7FFFFFFF;
        if (lo < 0x4000 || (lo & 0xF0000000u) == 0x70000000u)
            return ps2_scratchpad + (a & 0x3FF0);
        return (void *)(size_t)lo;
    }
    if ((a & 0xF0000000u) == 0x70000000u)
        return ps2_scratchpad + (a & 0x3FF0);
    return (void *)(size_t)a;
}

static void vif_unpack(u_int code, const u_int *src, u_int *used_words)
{
    u_int num = F(code, 16, 8), addr = F(code, 0, 10), cmd = F(code, 24, 7);
    u_int vn = (cmd >> 2) & 3, vl = cmd & 3, usn = F(code, 14, 1), flg = F(code, 15, 1);
    u_int masked = (cmd >> 4) & 1;
    u_int n = num ? num : 256, k, comp, words = 0;
    u_int bits_per = vl == 0 ? 32 : vl == 1 ? 16 : vl == 2 ? 8 : 16;
    const u_char *b = (const u_char *)src;
    u_int bitpos = 0;
    u_int dst = addr + (flg ? vif_tops : 0);
    u_int cyc = 0;
    (void)masked;
    for (k = 0; k < n; k++) {
        u_int out[4] = { 0, 0, 0, 0x7FFFFFFF };
        u_int ncomp = vn + 1;
        if (vn == 3 && vl == 3) {          /* V4-5: 16-bit RGBA5551 */
            u_int h = b[bitpos / 8] | (b[bitpos / 8 + 1] << 8);
            bitpos += 16;
            out[0] = (h & 0x1F) << 3; out[1] = ((h >> 5) & 0x1F) << 3;
            out[2] = ((h >> 10) & 0x1F) << 3; out[3] = (h >> 15) ? 0x80 : 0;
        } else {
            for (comp = 0; comp < ncomp; comp++) {
                u_int v;
                if (bits_per == 32) v = b[bitpos / 8] | (b[bitpos / 8 + 1] << 8) | (b[bitpos / 8 + 2] << 16) | ((u_int)b[bitpos / 8 + 3] << 24);
                else if (bits_per == 16) { v = b[bitpos / 8] | (b[bitpos / 8 + 1] << 8); if (!usn && (v & 0x8000)) v |= 0xFFFF0000u; }
                else { v = b[bitpos / 8]; if (!usn && (v & 0x80)) v |= 0xFFFFFF00u; }
                bitpos += bits_per;
                out[comp] = v;
            }
            if (ncomp == 1)
                out[1] = out[2] = out[3] = out[0];
        }
        if (vif_mode == 1)
            for (comp = 0; comp < 4; comp++) out[comp] += vif_row[comp];
        {
            u_int a = (dst + (vif_cl < vif_wl ? k : (k / vif_wl) * vif_cl + (k % vif_wl))) & 1023;
            for (comp = 0; comp < 4; comp++)
                if (comp < ncomp || vn == 3 || ncomp == 1)
                    vu1mem[a * 4 + comp] = out[comp];
                else if (comp == 3 && ncomp < 4)
                    ;  /* w keeps the previous value */
        }
        cyc++;
    }
    words = (bitpos + 31) / 32;
    *used_words = words;
}

/* process VIF codes in words[0..n); returns words used */
static u_int vif1_words(const u_int *w, u_int n)
{
    u_int i = 0;
    while (i < n) {
        u_int code = w[i++], cmd = F(code, 24, 7), imm = code & 0xFFFF;
        switch (cmd) {
        case 0x00: case 0x07: case 0x10: case 0x11: case 0x13: case 0x06: break;
        case 0x01: vif_cl = imm & 0xFF; vif_wl = (imm >> 8) & 0xFF; if (!vif_wl) vif_wl = 1; if (!vif_cl) vif_cl = 1; break;
        case 0x02: vif_ofst = imm & 0x3FF; vif_dbf = 0; vif_tops = vif_base; break;
        case 0x03: vif_base = imm & 0x3FF; break;
        case 0x04: vif_itop = imm & 0x3FF; break;
        case 0x05: vif_mode = imm & 3; break;
        case 0x14: case 0x15: case 0x17:
            vu1_mscal(cmd == 0x17 ? 0xFFFF : imm);
            st.vif_mscal++;
            vif_dbf ^= 1;
            vif_tops = vif_dbf ? vif_base + vif_ofst : vif_base;
            break;
        case 0x20: vif_mask = w[i]; i += 1; break;
        case 0x30: memcpy(vif_row, w + i, 16); i += 4; break;
        case 0x31: memcpy(vif_col, w + i, 16); i += 4; break;
        case 0x4A: { u_int num = F(code, 16, 8); i += (num ? num : 256) * 2; } break;
        case 0x50: case 0x51: {
            u_int qw = imm ? imm : 65536;
            /* DIRECT data is quadword aligned */
            while (i & 3) i++;
            gif_packet(w + i, qw);
            i += qw * 4;
        } break;
        default:
            if ((cmd & 0x60) == 0x60) {
                u_int used;
                vif_unpack(code, w + i, &used);
                i += used;
            } else {
                static int logged;
                if (logged++ < 4)
                    plat_log("vif1: unknown code %08X (rest of packet skipped)\n", code);
                return n;
            }
        }
    }
    return i;
}

/* walk a source chain; vif: tags carry VIF codes in their upper half */
static void dump_qw(const char *what, const u_int *q, int n)
{
    int i;
    for (i = 0; i < n; i++)
        plat_log("    %s+%02d: %08X %08X %08X %08X\n", what, i, q[i * 4 + 3], q[i * 4 + 2], q[i * 4 + 1], q[i * 4 + 0]);
}

static void dma_chain(const void *start, int vif)
{
    st.dma_chains++;
    if (frame_no == 60 && st.dma_chains <= 6) {
        plat_log("  dma chain %u (%s) at %p:\n", st.dma_chains, vif ? "vif1" : "gif", start);
        dump_qw("tag", start, 10);
    }
    const u_int *tag = start;
    const u_int *ret_stack[2];
    int sp = 0, guard = 0;
    while (tag && guard++ < 100000) {
        u_int qwc = tag[0] & 0xFFFF, id = (tag[0] >> 28) & 7, addr = tag[1];
        const u_int *data, *next;
        if (vif)
            vif1_words(tag + 2, 2);
        switch (id) {
        case 0: data = dma_ptr(addr); next = NULL; break;                /* refe */
        case 1: data = tag + 4; next = tag + 4 + qwc * 4; break;          /* cnt */
        case 2: data = tag + 4; next = dma_ptr(addr); break;              /* next */
        case 3: case 4: data = dma_ptr(addr); next = tag + 4; break;      /* ref / refs */
        case 5: data = tag + 4;                                           /* call */
            if (sp < 2) ret_stack[sp++] = tag + 4 + qwc * 4;
            next = dma_ptr(addr); break;
        case 6: data = tag + 4; next = sp ? ret_stack[--sp] : NULL; break; /* ret */
        default: data = tag + 4; next = NULL; break;                      /* end */
        }
        if (qwc && !data) {             /* a tag with no data: the rest of the chain is unusable */
            static int told;
            if (!told++)
                plat_log("gs: a DMA tag points at nothing (id %u, %u quadwords): chain dropped\n", id, qwc);
            break;
        }
        if (qwc) {
            if (vif)
                vif1_words(data, qwc * 4);
            else
                gif_packet(data, qwc);
        }
        tag = next;
    }
}

/* the game passes channel register addresses (D1_CHCR etc.) as channels */
void sceDmaSend(sceDmaChan *d, void *tag)
{
    u_int a = (u_int)(size_t)d;
    void *t = dma_ptr((u_int)(size_t)tag);
    if (d == (sceDmaChan *)D1_CHCR || a == 0x10009000u)
        dma_chain(t, 1);
    else if (a == 0x10008000u)
        ;                   /* VIF0: VU0 micro programs (done in C) */
    else
        dma_chain(t, 0);    /* GIF */
}

void sceDmaSendN(sceDmaChan *d, void *addr, int qwc)
{
    if (d == (sceDmaChan *)D1_CHCR || (u_int)(size_t)d == 0x10009000u)
        vif1_words(dma_ptr((u_int)(size_t)addr), qwc * 4);
    else
        gif_packet(dma_ptr((u_int)(size_t)addr), qwc);
}

int sceDmaSync(sceDmaChan *d, int mode, int timeout) { (void)d; (void)mode; (void)timeout; return 0; }
int sceDmaReset(int mode) { (void)mode; return 0; }
sceDmaChan *sceDmaGetChan(int id)
{
    static const u_int base[] = { 0x10008000, 0x10009000, 0x1000A000, 0x1000B000, 0x1000B400 };
    return (sceDmaChan *)(size_t)(id >= 0 && id < 5 ? base[id] : 0x1000A000);
}

/* path 2/3 chains started by writing D2_TADR / D2_CHCR (loadImage) */
unsigned prof_dma_us;
void gs_dma_chain_gif(void *tag)
{
    unsigned t0 = PUS();
    int pv = prof_enter(PB_PARSE);
    dma_chain(dma_ptr((u_int)(size_t)tag), 0);
    prof_leave(pv);
    prof_dma_us += PUS() - t0;
}

/* ---- libgraph --------------------------------------------------------------- */
static int gs_inited;

void gs_init(void)
{
    if (gs_inited)
        return;
    gs_inited = 1;
    measure_swizzle();
    gs.prmodecont = 1;
    gs.ctx[0].xyoffset = gs.ctx[1].xyoffset = ((u_long)((2048 - 320) << 4) << 32) | ((2048 - 320) << 4);
    gs.ctx[0].xyoffset = gs.ctx[1].xyoffset = ((u_long)((2048 - 240) << 4) << 32) | ((2048 - 320) << 4);
    gs.ctx[0].scissor = gs.ctx[1].scissor = ((u_long)479 << 48) | (639 << 16);
    {
        int i;
        for (i = 0; i < TEXC; i++)
            texc[i].handle = -1;
        for (i = 0; i < TEXH; i++)
            texc_head[i] = -1;
        for (i = 0; i < 512; i++)
            page_owner[i] = -1;
    }
}

void sceGsResetGraph(short mode, short inter, short omode, short ffmd) { (void)mode; (void)inter; (void)omode; (void)ffmd; gs_init(); }
void sceGsResetPath(void) {}
int sceGsSyncPath(int mode, unsigned short timeout) { (void)mode; (void)timeout; return 0; }
int sceGsSyncV(int mode) { (void)mode; return 0; }

void sceGsSetDefDBuffDc(sceGsDBuffDc *db, short psm, short w, short h, short ztest, short zpsm, short clear)
{
    (void)psm; (void)ztest; (void)zpsm; (void)clear;
    memset(db, 0, sizeof *db);
    db->draw01.xyoffset1.OFX = db->draw11.xyoffset1.OFX = (2048 - w / 2) << 4;
    db->draw01.xyoffset1.OFY = db->draw11.xyoffset1.OFY = (2048 - h / 2) << 4;
    db->draw01.scissor1.SCAX1 = db->draw11.scissor1.SCAX1 = w - 1;
    db->draw01.scissor1.SCAY1 = db->draw11.scissor1.SCAY1 = h - 1;
    db->draw01.test1.ZTE = db->draw11.test1.ZTE = 1;
    db->draw01.test1.ZTST = db->draw11.test1.ZTST = ztest;
    db->draw02.xyoffset2 = *(sceGsXyoffset *)&db->draw01.xyoffset1;
    db->draw12.xyoffset2 = *(sceGsXyoffset *)&db->draw11.xyoffset1;
    db->draw02.scissor2 = *(sceGsScissor *)&db->draw01.scissor1;
    db->draw12.scissor2 = *(sceGsScissor *)&db->draw11.scissor1;
    gs_init();
}

/* load both contexts' drawing environment (what libgraph sends) */
static void put_env1(const sceGsDrawEnv1 *e, int c)
{
    gs.ctx[c].frame = *(const u_long *)&e->frame1;
    if (c == 0)
        main_fbp = F(gs.ctx[0].frame, 0, 9);
    gs.ctx[c].zbuf = *(const u_long *)&e->zbuf1;
    gs.ctx[c].xyoffset = *(const u_long *)&e->xyoffset1;
    gs.ctx[c].scissor = *(const u_long *)&e->scissor1;
    gs.ctx[c].test = *(const u_long *)&e->test1;
}

int sceGsSwapDBuffDc(sceGsDBuffDc *db, int id)
{
    put_env1(id ? &db->draw11 : &db->draw01, 0);
    put_env1((const sceGsDrawEnv1 *)(id ? (void *)&db->draw12 : (void *)&db->draw02), 1);
    return 0;
}

int sceGsSetDefLoadImage(sceGsLoadImage *lp, short dbp, short dbw, short dpsm, short x, short y, short w, short h)
{
    memset(lp, 0, sizeof *lp);
    lp->bitbltbuf = SCE_GS_SET_BITBLTBUF(0, 0, 0, dbp, dbw, dpsm);
    lp->trxpos = SCE_GS_SET_TRXPOS(0, 0, x, y, 0);
    lp->trxreg = SCE_GS_SET_TRXREG(w, h);
    return 0;
}

int sceGsExecLoadImage(sceGsLoadImage *lp, u_long128 *src)
{
    u_int w = F(lp->trxreg, 0, 12), h = F(lp->trxreg, 32, 12);
    u_int psm = F(lp->bitbltbuf, 56, 6);
    u_int bytes = w * h * bpp_of(psm) / 8, k;
    const u_int *s = (const u_int *)src;
    gs_reg(SCE_GS_BITBLTBUF, lp->bitbltbuf);
    gs_reg(SCE_GS_TRXPOS, lp->trxpos);
    gs_reg(SCE_GS_TRXREG, lp->trxreg);
    gs_reg(SCE_GS_TRXDIR, 0);
    {
        unsigned t0 = PUS();
        if (bpp_of(psm) == 24) {
            for (k = 0; k < (bytes + 7) / 8; k++)
                trx_put64((u_long)s[k * 2] | ((u_long)s[k * 2 + 1] << 32));
        } else {
            defer_off = 1;
            trx_whole(s, (bytes + 3) / 4);
            defer_off = 0;
        }
        prof_upload_us += PUS() - t0;
    }
    return 0;
}

int sceGsSetDefStoreImage(sceGsStoreImage *sp, short sbp, short sbw, short spsm, short x, short y, short w, short h)
{
    memset(sp, 0, sizeof *sp);
    sp->bitbltbuf = SCE_GS_SET_BITBLTBUF(sbp, sbw, spsm, 0, 0, 0);
    sp->trxpos = SCE_GS_SET_TRXPOS(x, y, 0, 0, 0);
    sp->trxreg = SCE_GS_SET_TRXREG(w, h);
    return 0;
}

int sceGsExecStoreImage(sceGsStoreImage *sp, u_long128 *dst)
{
    u_int sbp = F(sp->bitbltbuf, 0, 14), sbw = F(sp->bitbltbuf, 16, 6), psm = F(sp->bitbltbuf, 24, 6);
    u_int sx = F(sp->trxpos, 0, 11), sy = F(sp->trxpos, 16, 11);
    u_int w = F(sp->trxreg, 0, 12), h = F(sp->trxreg, 32, 12), x, y;
    u_int *d = (u_int *)dst;
    mat_all();
    if (bpp_of(psm) != 32)
        return 0;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++)
            *d++ = rd_px(psm, sbp, sbw, sx + x, sy + y);
    return 0;
}

/* end of a game frame: flush to the screen */
void gs_frame_end(void)
{
    prof_on = plat_logging();
    if (g_skip_draw) {          /* frame skip: the screens keep the last frame */
        gs.frame_open = 0;
    } else {
        if (!gs.frame_open)
            rend_frame_begin();
        gs.frame_open = 0;
        int pvh = prof_enter(PB_HANDOFF);
        if (st.tris3d) {        /* a 3D frame: the room radar on the bottom screen */
            extern void radar_draw(void);
            radar_draw();
        }
        {
            unsigned t0 = PUS();
            rend_frame_end();
            prof_gpu_us += PUS() - t0;
        }
        prof_leave(pvh);
    }
    while (fb_blit_nh > 0)              /* the pictures put on the screen this frame */
        rend_tex_free(fb_blit_h[--fb_blit_nh]);
    frame_no++;
    {
        /* a frame's primitives go to the log 40 frames into each new
         * screen (task / front-end step), for the first 12 screens */
        static u_int last_state = 0xFFFFFFFF, since, ndumps;
        /* a screen with 3D counts as a new screen (rooms keep the same
         * task flags as the menus) */
        u_int stt = diag_state() ^ (st.tris3d ? 0x80000000u : 0);
        if (dump_now) {
            plat_log("  3d vertices past the far depth %u, nearer than the GS range %u, behind the camera %u; 3d triangles left out (off-screen) %u\n",
                     dump_zlo, dump_zhi, dump_wneg, dump_3d_out);
            plat_log("  (end of primitive dump)\n");
        }
        dump_zlo = dump_zhi = dump_wneg = dump_3d_out = dump_3d_out_logged = 0;
        dump_now = 0;
        extern volatile int g_dump_req;     /* L + tap on the bottom screen */
        if (stt != last_state) {
            last_state = stt;
            since = 0;
        } else
            ++since;
        /* L + tap, or (logging on) 40 frames into each new screen, the
         * first 16: nothing is evicted, the game runs on as it was */
        if (g_dump_req || (since == 40 && ndumps < 16 && plat_logging())) {
            if (!g_dump_req)
                ndumps++;
            g_dump_req = 0;
            if (!plat_logging())
                goto no_dump;
            dump_now = 1;
            plat_log("primitive dump, screen %08X, frame %u:\n", stt, frame_no);
            {
                extern void vu_dump_begin(void);
                extern void cnk_skip_log(void);
                cnk_skip_log();
                int i;
                vu_dump_begin();
                (void)i;
            }
        }
    no_dump:;
    }
    {
        static unsigned t_last, nskip;
        unsigned now = plat_ms();
        nskip += g_skip_draw != 0;
        if (frame_no % 60 == 1) {
            float span = t_last ? (float)(now - t_last) : 0.0f;
            plat_log("[speed] %.1f frames/s (%.1f ms a frame over the last 60), %u of them not drawn (frame skip)\n",
                     t_last ? 60000.0f / span : 0.0f, t_last ? span / 60.0f : 0.0f, nskip);
            nskip = 0;
            {
                extern unsigned rend_linear_free_kb(void);
                plat_log("[textures] last 60 frames: %u made, %u page patches, %u made again as 32-bit; "
                         "cache %u KB, GPU memory free %u KB\n",
                         st_tex_new, st_tex_patch, st_tex_redo, tex_bytes >> 10, rend_linear_free_kb());
            }
            st_tex_new = st_tex_patch = st_tex_redo = 0;
            plat_log("[time] per frame: image uploads %.1f ms, texture building %.1f ms, "
                     "GPU + present %.1f ms, waiting for vblank %.1f ms, the rest (game code) %.1f ms\n",
                     prof_upload_us / 60000.0f, prof_decode_us / 60000.0f, prof_gpu_us / 60000.0f,
                     prof_wait_us / 60000.0f,
                     t_last ? span / 60.0f - (prof_upload_us + prof_decode_us + prof_gpu_us + prof_wait_us) / 60000.0f : 0.0f);
            prof_upload_us = prof_decode_us = prof_gpu_us = prof_wait_us = 0;
            t_last = now;
        }
    }
    if (frame_no % 60 == 1)
        plat_log("[gs] frame %u: dma %u, gif tags %u, prims %u (tri %u, sprite %u, textured %u), "
                 "uploads %u (%u unchanged, skipped), tex decodes %u (cache %u KB), off-screen / frame-buffer draws left out %u, "
                 "vu1 calls %u; by type %u %u %u %u %u %u %u\n",
                 frame_no, st.dma_chains, st.gif_tags, st.prims, st.tris, st.sprites, st.tex_prims,
                 st.uploads, st.uploads_skipped, st.tex_decodes, tex_bytes >> 10, st.fb_reads, st.vif_mscal, st.bytype[0], st.bytype[1], st.bytype[2],
                 st.bytype[3], st.bytype[4], st.bytype[5], st.bytype[6]);
    if (frame_no % 60 == 1) {
        extern void diag_line(void);
        extern void vu_strip_stats(unsigned *models, unsigned *tris);
        unsigned models, tris;
        extern unsigned prof_vtx_us, prof_strip_us, prof_dma_us;
        extern void vu_light_log(void);
        static int nlight;
        vu_strip_stats(&models, &tris);
        plat_log("[3d] over the last 60 frames: %u model strip lists, %u triangles; per frame: vertices %.1f ms, "
                 "strips + drawing %.1f ms, GS packets %.1f ms (of which: packet primitives %u, drawing them %.1f ms); "
                 "models for a left-out buffer, skipped: %u\n",
                 models, tris, prof_vtx_us / 60000.0f, prof_strip_us / 60000.0f, prof_dma_us / 60000.0f,
                 gif_draws / 60, prof_gifdraw_us / 60000.0f, gs_models_dropped_n);
        gif_draws = prof_gifdraw_us = 0;
        {
            static const char *nm[PB_N] = { "game", "vertices", "strips", "render state", "packet parsing",
                                            "uploads", "upload write-out", "textures", "packet drawing",
                                            "hand-off", "waiting" };
            char line[400];
            int b, k = snprintf(line, sizeof line, "[prof] ms per frame, each part once:");
            prof_leave(prof_enter((int)pb_cur));        /* bring the current part up to now */
            for (b = 0; b < PB_N; b++)
                k += snprintf(line + k, sizeof line - k, " %s %.1f%s", nm[b], pb_acc[b] / 60000.0f,
                              b + 1 < PB_N ? "," : "");
            plat_log("%s\n", line);
            k = snprintf(line, sizeof line, "[prof] calls per frame:");
            for (b = 1; b < PB_N - 2; b++)
                k += snprintf(line + k, sizeof line - k, " %s %u%s", nm[b], pb_calls[b] / 60, b + 1 < PB_N - 2 ? "," : "");
            plat_log("%s\n", line);
            memset(pb_acc, 0, sizeof pb_acc);
            memset(pb_calls, 0, sizeof pb_calls);
        }
        gs_models_dropped_n = 0;
        prof_vtx_us = prof_strip_us = prof_dma_us = 0;
        plat_log("[upload] per frame by format: 32-bit %u x %u KB, 16-bit %u x %u KB, 8-bit %u x %u KB, "
                 "4-bit %u x %u KB, other %u x %u KB\n",
                 up_count[0] / 60, up_bytes[0] / 61440, up_count[1] / 60, up_bytes[1] / 61440,
                 up_count[2] / 60, up_bytes[2] / 61440, up_count[3] / 60, up_bytes[3] / 61440,
                 up_count[4] / 60, up_bytes[4] / 61440);
        memset(up_bytes, 0, sizeof up_bytes);
        memset(up_count, 0, sizeof up_count);
        plat_log("[upload] noted instead of written: %u; pages written out later: %u\n", st_deferred, st_mat_pages);
        st_deferred = st_mat_pages = 0;
        {
            extern unsigned adx_out_frames(void);
            unsigned af = adx_out_frames();
            {
                extern unsigned plat_audio_dry(void);
                plat_log("[audio] ADX samples sent to the speaker in the last 60 frames: %u, streams ran dry %u times\n",
                         af, plat_audio_dry());
            }
            {
                extern void snd_log_stats(void);
                snd_log_stats();
            }
        }
        if (tris && (nlight++ % 5) == 0)
            vu_light_log();
        diag_line();
    }
    {
        u_int k;
        for (k = 0; k < offs_n; k++) {     /* this frame's triangles become the frame before's */
            offs[k].tris_prev = offs[k].tris_cur;
            offs[k].tris_cur = 0;
        }
    }
    memset(&st, 0, sizeof st);
}

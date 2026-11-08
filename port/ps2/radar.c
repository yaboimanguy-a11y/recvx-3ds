/* The room radar on the 3DS bottom screen: a top-down view of the room the
 * player is in, drawn every frame from the game's own data:
 *   floors (rom->flrp) and walls (rom->walp + sys->mwalp, the collision the
 *   game moves characters against), the player (plp: position, facing ay)
 *   and the enemies (ene[], as bhCheckEnemies picks the live ones).
 * Only the walls and floors at the player's height are drawn (rooms with
 * more than one storey).
 *
 * The view turns with the game's camera (cam.mtx, world -> view): right on
 * the radar is right on the top screen and up is into the picture, so the
 * arrow moves the way Claire does on screen.  The room is fitted into the
 * screen. */
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "types.h"
#include "main.h"
#include "ps2_NaMatrix.h"
#include "rend.h"
#include "item.h"
#include "plat.h"

#define SW 320.0f
#define SH 214.0f      /* above the settings footer (ui_3ds.c) */
#define TOP 26.0f      /* below the status strip */
#define MARGIN 12.0f

/* colours: 0xAABBGGRR, 0x80 = full */
#define COL_FLOOR  0x80201A14u
#define COL_WALL   0x80443F3Au
#define COL_EDGE   0x80605A54u
#define COL_ENEMY  0x80181C78u
#define COL_PLAYER 0x80407830u

extern int g_port_door;

/* world x, z -> the camera's screen plane (r = right, d = down), then pixels */
static float ax_r, az_r, ax_d, az_d;
static float sc, ox, oy;

static inline void rot(float x, float z, float *r, float *d)
{
    *r = ax_r * x + az_r * z;
    *d = ax_d * x + az_d * z;
}

static inline void pix(float x, float z, float *px, float *py)
{
    float r, d;
    rot(x, z, &r, &d);
    *px = ox + r * sc;
    *py = oy + d * sc;
}

static void camera_basis(void)
{
    NJS_VECTOR ex = { 1.0f, 0.0f, 0.0f }, ez = { 0.0f, 0.0f, 1.0f }, vx, vz;
    float l;
    ax_r = 1.0f; az_r = 0.0f; ax_d = 0.0f; az_d = 1.0f;
    if (!cam.mtx)
        return;
    njCalcVector((NJS_MATRIX *)cam.mtx, &ex, &vx);
    njCalcVector((NJS_MATRIX *)cam.mtx, &ez, &vz);
    /* right: view x; down the screen: toward the camera, view +z (the
     * camera looks along -z); a camera looking straight down: view -y */
    ax_r = vx.x; az_r = vz.x;
    ax_d = vx.z; az_d = vz.z;
    if (ax_d * ax_d + az_d * az_d < 0.04f) {
        ax_d = -vx.y;
        az_d = -vz.y;
    }
    l = sqrtf(ax_r * ax_r + az_r * az_r);
    if (l > 1e-4f) { ax_r /= l; az_r /= l; }
    l = sqrtf(ax_d * ax_d + az_d * az_d);
    if (l > 1e-4f) { ax_d /= l; az_d /= l; }
}

/* a quad given by four world corners (x, z) */
static void wquad(const float *c, uint32_t col)
{
    float p[8];
    int i;
    for (i = 0; i < 4; i++)
        pix(c[i * 2], c[i * 2 + 1], &p[i * 2], &p[i * 2 + 1]);
    rend_bot_tri(p[0], p[1], p[2], p[3], p[4], p[5], col);
    rend_bot_tri(p[0], p[1], p[4], p[5], p[6], p[7], col);
}

static void wbox(float x0, float z0, float x1, float z1, uint32_t col)
{
    float c[8] = { x0, z0, x1, z0, x1, z1, x0, z1 };
    wquad(c, col);
}

/* the shape of a collision entry; grow: world units added around a box */
static void draw_atr(const ATR_WORK *hp, uint32_t col, float grow)
{
    switch (hp->type) {
    case 2: case 3: {                       /* a round pillar: centre, radius w */
        int i;
        float cx, cy, r = (hp->w + grow) * sc;
        pix(hp->px, hp->pz, &cx, &cy);
        for (i = 0; i < 12; i++) {
            float a0 = (float)i * 0.5235988f, a1 = (float)(i + 1) * 0.5235988f;
            rend_bot_tri(cx, cy, cx + r * cosf(a0), cy + r * sinf(a0), cx + r * cosf(a1), cy + r * sinf(a1), col);
        }
        break;
    }
    case 4: case 5: {                       /* a right triangle: the corner at px, pz */
        float p0x, p0y, p1x, p1y, p2x, p2y;
        if (grow)
            break;
        pix(hp->px, hp->pz, &p0x, &p0y);
        pix(hp->px + hp->w, hp->pz, &p1x, &p1y);
        pix(hp->px, hp->pz + hp->d, &p2x, &p2y);
        rend_bot_tri(p0x, p0y, p1x, p1y, p2x, p2y, col);
        break;
    }
    default: {                              /* a box */
        float x0 = hp->px, x1 = hp->px + hp->w, z0 = hp->pz, z1 = hp->pz + hp->d, t;
        if (x0 > x1) { t = x0; x0 = x1; x1 = t; }
        if (z0 > z1) { t = z0; z0 = z1; z1 = t; }
        wbox(x0 - grow, z0 - grow, x1 + grow, z1 + grow, col);
        break;
    }
    }
}

/* the entry's extent on the radar's axes */
static void extent(const ATR_WORK *hp, float *r0, float *d0, float *r1, float *d1)
{
    float xs[4], zs[4];
    int i, n = 4;
    if (hp->type == 2 || hp->type == 3) {
        float r, d;
        rot(hp->px, hp->pz, &r, &d);
        if (r - hp->w < *r0) *r0 = r - hp->w;
        if (r + hp->w > *r1) *r1 = r + hp->w;
        if (d - hp->w < *d0) *d0 = d - hp->w;
        if (d + hp->w > *d1) *d1 = d + hp->w;
        return;
    }
    xs[0] = hp->px; zs[0] = hp->pz;
    xs[1] = hp->px + hp->w; zs[1] = hp->pz;
    xs[2] = hp->px; zs[2] = hp->pz + hp->d;
    xs[3] = hp->px + hp->w; zs[3] = hp->pz + hp->d;
    for (i = 0; i < n; i++) {
        float r, d;
        rot(xs[i], zs[i], &r, &d);
        if (r < *r0) *r0 = r;
        if (r > *r1) *r1 = r;
        if (d < *d0) *d0 = d;
        if (d > *d1) *d1 = d;
    }
}

/* at the player's height (the game's own wall test: [py, py + h] against
 * the character's [y, y + ah]) */
static int at_height(const ATR_WORK *hp, float y, float ah)
{
    float h = hp->h ? hp->h : rom->h;
    return (y + ah) >= hp->py && y <= hp->py + h;
}

static const ATR_WORK *wall_at(int i)
{
    return i < rom->wal_n ? &rom->walp[i] : &sys->mwalp[i - rom->wal_n];
}

/* ---- markers: doors, items, item boxes, typewriters ---------------------- */
#define COL_DOOR 0x807F6437u
#define COL_ITEM 0x801E697Fu
#define COL_IBOX 0x803C732Du
#define COL_TYPE 0x807F7F7Fu

/* a line 2 pixels thick */
static void px_line(float x0, float y0, float x1, float y1, uint32_t c)
{
    float dx = x1 - x0, dy = y1 - y0, l = sqrtf(dx * dx + dy * dy), nx, ny;
    if (l < 0.01f)
        return;
    nx = -dy / l;
    ny = dx / l;
    rend_bot_tri(x0 + nx, y0 + ny, x1 + nx, y1 + ny, x0 - nx, y0 - ny, c);
    rend_bot_tri(x1 + nx, y1 + ny, x1 - nx, y1 - ny, x0 - nx, y0 - ny, c);
}

static void box_outline(const ATR_WORK *e, uint32_t c)
{
    float p[8];
    pix(e->px, e->pz, &p[0], &p[1]);
    pix(e->px + e->w, e->pz, &p[2], &p[3]);
    pix(e->px + e->w, e->pz + e->d, &p[4], &p[5]);
    pix(e->px, e->pz + e->d, &p[6], &p[7]);
    px_line(p[0], p[1], p[2], p[3], c);
    px_line(p[2], p[3], p[4], p[5], c);
    px_line(p[4], p[5], p[6], p[7], c);
    px_line(p[6], p[7], p[0], p[1], c);
}

static void dot_square(float x, float z, float r, uint32_t c)
{
    float px, py;
    pix(x, z, &px, &py);
    rend_bot_tri(px - r, py - r, px + r, py - r, px - r, py + r, c);
    rend_bot_tri(px + r, py - r, px + r, py + r, px - r, py + r, c);
}

static void dot_diamond(float x, float z, float r, uint32_t c)
{
    float px, py;
    pix(x, z, &px, &py);
    rend_bot_tri(px, py - r, px + r, py, px - r, py, c);
    rend_bot_tri(px + r, py, px, py + r, px - r, py, c);
}

static const ATR_WORK *etc_at(int i)
{
    return i < rom->etc_n ? &rom->etcp[i] : &sys->metcp[i - rom->etc_n];
}

/* typewriters are examine spots only the room's script knows: learned when
 * one is used (the script sets cb_flg 0x200000 with the spot in etc_idx),
 * and kept in typewriters.txt */
#define MAX_TW 128
extern int plat_tw_load(short *pairs, int max);
extern void plat_tw_save(const short *pairs, int n);
static short tw[MAX_TW * 2];        /* room id (stage * 100 + room), spot */
static int tw_n = -1;

static void tw_learn(void)
{
    static int was;
    int on = (sys->cb_flg & 0x200000) != 0;
    if (tw_n < 0)
        tw_n = plat_tw_load(tw, MAX_TW);
    if (on && !was) {
        short id = (short)(sys->stg_no * 100 + sys->rom_no), idx = (short)sys->etc_idx;
        int i, known = 0;
        for (i = 0; i < tw_n; i++)
            if (tw[i * 2] == id && tw[i * 2 + 1] == idx)
                known = 1;
        if (!known && tw_n < MAX_TW) {
            tw[tw_n * 2] = id;
            tw[tw_n * 2 + 1] = idx;
            tw_n++;
            plat_tw_save(tw, tw_n);
        }
    }
    was = on;
}

static void draw_markers(const BH_PWORK *p)
{
    int i, n = rom->etc_n + sys->metc_n;
    short id = (short)(sys->stg_no * 100 + sys->rom_no);
    tw_learn();
    for (i = 0; i < n; i++) {
        const ATR_WORK *e = etc_at(i);
        if (!(e->flg & 0x1) || e->flr_no != p->flr_no)
            continue;
        if (e->type == 0)
            box_outline(e, COL_DOOR);
        else if (e->type == 4 && (e->attr & 0x2) && e->prm0 == 0xFF)
            dot_square(e->px + e->w * 0.5f, e->pz + e->d * 0.5f, 3.0f, COL_IBOX);
    }
    for (i = 0; i < tw_n; i++) {
        int k = tw[i * 2 + 1];
        if (tw[i * 2] == id && k >= 0 && k < n) {
            const ATR_WORK *e = etc_at(k);
            dot_square(e->px + e->w * 0.5f, e->pz + e->d * 0.5f, 3.0f, COL_TYPE);
        }
    }
    if (sys->itwp)
        for (i = 0; i < rom->itm_n; i++) {
            const O_WRK *o = &sys->itwp[i];
            if ((o->flg & 0x1) && !(o->stflg & 0x1000000) && o->flr_no == p->flr_no)
                dot_diamond(o->px, o->pz, 4.0f, COL_ITEM);
        }
}

void radar_draw(void)
{
    BH_PWORK *p = plp;
    float y, ah, r0 = 1e30f, d0 = 1e30f, r1 = -1e30f, d1 = -1e30f, pr, pd;
    int i, nw, any = 0;
    if (g_port_door || !sys || !rom || !p || !(p->flg & 0x1) || (!rom->walp && !rom->flrp))
        return;
    camera_basis();
    y = p->py;
    ah = p->ah > 0.0f ? p->ah : 1800.0f;
    nw = rom->wal_n + sys->mwal_n;

    /* the room's extent at this height, on the radar's axes */
    for (i = 0; i < rom->flr_n; i++)
        if (at_height(&rom->flrp[i], y, ah)) {
            extent(&rom->flrp[i], &r0, &d0, &r1, &d1);
            any = 1;
        }
    for (i = 0; i < nw; i++) {
        const ATR_WORK *hp = wall_at(i);
        if ((hp->flg & 0x1) && at_height(hp, y, ah)) {
            extent(hp, &r0, &d0, &r1, &d1);
            any = 1;
        }
    }
    rot(p->px, p->pz, &pr, &pd);
    if (pr < r0) r0 = pr;
    if (pr > r1) r1 = pr;
    if (pd < d0) d0 = pd;
    if (pd > d1) d1 = pd;
    if (!any || r1 - r0 < 1.0f || d1 - d0 < 1.0f) {
        r0 = pr - 4000.0f; r1 = pr + 4000.0f;
        d0 = pd - 3000.0f; d1 = pd + 3000.0f;
    }
    {
        float s1 = (SW - 2 * MARGIN) / (r1 - r0), s2 = (SH - TOP - 2 * MARGIN) / (d1 - d0);
        sc = s1 < s2 ? s1 : s2;
        ox = SW * 0.5f - (r0 + r1) * 0.5f * sc;
        oy = (TOP + SH) * 0.5f - (d0 + d1) * 0.5f * sc;
    }

    /* floors, then walls (a lighter edge under each, then the fill) */
    for (i = 0; i < rom->flr_n; i++)
        if (at_height(&rom->flrp[i], y, ah))
            draw_atr(&rom->flrp[i], COL_FLOOR, 0.0f);
    for (i = 0; i < nw; i++) {
        const ATR_WORK *hp = wall_at(i);
        if (!(hp->flg & 0x1) || !at_height(hp, y, ah))
            continue;
        draw_atr(hp, COL_EDGE, 1.0f / sc);
        draw_atr(hp, COL_WALL, 0.0f);
    }

    draw_markers(p);

    /* enemies: live ones (bhCheckEnemies' test), as dots */
    for (i = 0; i < sys->ewk_n && i < 128; i++) {
        const BH_PWORK *e = &ene[i];
        float ex, ey;
        if (!(e->flg & 0x1) || !(e->flg & 0x8) || (e->stflg & 0x1000000) || e == p)
            continue;
        pix(e->px, e->pz, &ex, &ey);
        rend_bot_tri(ex - 3, ey - 3, ex + 3, ey - 3, ex - 3, ey + 3, COL_ENEMY);
        rend_bot_tri(ex + 3, ey - 3, ex + 3, ey + 3, ex - 3, ey + 3, COL_ENEMY);
    }

    /* the player: an arrow along the facing */
    {
        float a = (float)p->ay * (6.2831853f / 65536.0f);
        float fr, fd, cx, cy, len = 9.0f, wid = 5.0f;
        rot(-sinf(a), -cosf(a), &fr, &fd);      /* the game's facing is -(sin ay, cos ay) */
        pix(p->px, p->pz, &cx, &cy);
        rend_bot_tri(cx + fr * len, cy + fd * len,
                     cx - fr * len * 0.6f - fd * wid, cy - fd * len * 0.6f + fr * wid,
                     cx - fr * len * 0.6f + fd * wid, cy - fd * len * 0.6f - fr * wid, COL_PLAYER);
    }
}

/* a cutscene (event scene) is playing: the event system has the player
 * (sys->cb_flg & 7; bhSysCallEvent keeps the pause menu away and hands
 * control back when these clear).  Frame skip AUTO works only then. */
int port_in_cutscene(void)
{
    return sys && (sys->cb_flg & 0x7) != 0;
}

/* widescreen (settings): the game's projection gets 0.8 of its x scale
 * (njSetAspect, hooked by port/tools/hostify.py), so 25% more of the scene
 * fits across the 640-wide canvas, which is then shown across the whole
 * 400-pixel top screen (5:3 against 4:3): a wider view, not a stretched
 * one.  The clip volume the game culls with is made from the same aspect,
 * so what comes into view at the sides is drawn. */
extern int plat_widescreen(void);
void njSetAspect(float ax, float ay);
static float aspect_x, aspect_y;
static int aspect_have;
float port_aspect_hook(float ax, float ay)
{
    aspect_x = ax;
    aspect_y = ay;
    aspect_have = 1;
    return plat_widescreen() ? ax * 0.8f : ax;
}

/* the setting changed: the game's last aspect again, with or without */
void port_aspect_refresh(void)
{
    if (aspect_have)
        njSetAspect(aspect_x, aspect_y);
}

/* ---- for the input helpers and the status strip --------------------------- */

/* the player can act now (the game's own gate, bhCheckSubTask) */
int port_player_has_control(void)
{
    if (!sys || !plp || g_port_door)
        return 0;
    return (sys->sp_flg & 0x21) == 0x21 && !(sys->cb_flg & 0x7) && !(sys->st_flg & 0x1C04000C) &&
           !(sys->ss_flg & 0x80000000) && (sys->ts_flg & 0x1000) && plp->mode0 == 1;
}

/* Doors: Fast (settings): the door scene takes 3 steps a frame */
extern int plat_fast_doors(void);
int port_door_speed(void)
{
    return plat_fast_doors() ? 2 : 0;
}

/* the item's name from the game's messages: font codes are ASCII - 0x20 */
static void item_name(int id, char *out, int n)
{
    const unsigned short *dp;
    int k = 0;
    out[0] = 0;
    if (!sys->mes_ip || id < 0 || id > 200)
        return;
    dp = (const unsigned short *)((const char *)sys->mes_ip + sys->mes_ip[id + 1]);
    while (k < n - 1) {
        unsigned c = *dp++;
        if (c == 0xFFFF)
            break;
        if (c == 0xFF01)
            out[k++] = ' ';
        else if (c < 0x5F)
            out[k++] = (char)(c + 0x20);
        else if (c >= 0xFF00)
            continue;                           /* a control code */
        else
            out[k++] = '?';
    }
    out[k] = 0;
    {
        static int logged[256];
        if (id < 256 && !logged[id]) {
            logged[id] = 1;
            plat_log("hud: item %d is \"%s\"\n", id, out);
        }
    }
}

void port_hud(PortHud *h)
{
    const unsigned int *pip;
    unsigned slot, w;
    int id;
    memset(h, 0, sizeof *h);
    if (!sys || !plp || !rom || !(plp->flg & 0x1) || g_port_door)
        return;
    h->valid = 1;
    h->hp = plp->hp;
    h->max = sys->gm_mode == 2 ? 320 : 160;
    h->cond = plp->hp >= 120 ? 0 : plp->hp >= 60 ? 1 : plp->hp >= 30 ? 2 : 3;
    if (plp->stflg & 0x280000)
        h->cond = 4;
    h->stage = sys->stg_no;
    h->room = sys->rom_no;
    pip = &sys->itm[sys->ply_id * 16];
    slot = pip[0];
    if (!slot || slot >= 16)
        return;
    w = pip[slot];
    id = (int)((w >> 16) & 0xFF);
    if (!id || id >= 156)
        return;
    h->has_weapon = 1;
    h->count = (int)(w & 0xFFFF);
    h->infinite = (w & 0x08000000) != 0;
    if (itemdata[id].type & 0x3800) {
        h->show_count = 1;
        if ((itemdata[id].type & 0x1800) == 0x1000) {
            int mx = itemdata[id].max / 100;
            h->percent = 1;
            h->count = mx ? h->count / mx : h->count;
            if (h->count == 0 && (w & 0xFFFF))
                h->count = 1;
        }
    }
    {
        int nid = id;
        if (id == 0x48 && (w & 0x80000000u))
            nid = 158;
        else if (id == 0x52 && !(w & 0xFFFF))
            nid = 149;
        item_name(nid, h->name, sizeof h->name);
    }
}

/* ---- Save State / Load State (settings) -----------------------------------
 * One slot, built on the game's own retry checkpoint (bhPushGameData /
 * bhPopGameData): the block of SYS_WORK a save holds, with the player's
 * place, health and items, kept in a file.  Saving does not touch the
 * game's checkpoint.  Loading goes as a game over's Continue does: fade,
 * stop the sound, then restart the room from the block; the room's enemies
 * and objects start as they do on entering it.  Both run between the
 * game's tasks (port_qs_service, called from njUserMain). */
extern void bhPopGameData(void);
extern void Ps2ClearOT(void);
extern void bhSetScreenFade(unsigned int argb, float frames);
extern void RequestAllStopSoundEx(int a, int b, int frames);
extern int plat_qs_write(const void *p, unsigned n);
extern int plat_qs_read(void *p, unsigned n);
extern void plat_qs_message(const char *s);
extern void plat_qs_loading(void);
#define QS_MAGIC 0x31565852u    /* "RXV1" */
#define QS_BLOCK ((unsigned)((char *)&sys->save_end - (char *)&sys->version))
#define QS_MAX 4096u
static unsigned char qs_buf[QS_MAX + 8];
static int qs_req;              /* 1 save, 2 load, 3 loading: fading out */
static int qs_wait;

void port_qs_request(int load)
{
    if (!qs_req)
        qs_req = load ? 2 : 1;
}

static void qs_save(void)
{
    unsigned n = QS_BLOCK;
    char msg[48];
    if (n > QS_MAX)
        return;
    /* as bhPushGameData, without its copy to the game's checkpoint */
    sys->version = 10;
    sys->flr_no = plp->flr_no;
    sys->ply_stflg[sys->ply_id] = plp->stflg & 0x78280000;
    sys->ply_pos.x = plp->px;
    sys->ply_pos.y = plp->py;
    sys->ply_pos.z = plp->pz;
    sys->ply_ang = plp->ay;
    sys->ply_wno[sys->ply_id] = plp->wpnr_no;
    sys->ply_hp[sys->ply_id] = plp->hp;
    ((unsigned *)qs_buf)[0] = QS_MAGIC;
    ((unsigned *)qs_buf)[1] = n;
    memcpy(qs_buf + 8, &sys->version, n);
    if (plat_qs_write(qs_buf, n + 8)) {
        snprintf(msg, sizeof msg, "State saved (room %d-%02d)", sys->stg_no, sys->rom_no);
        plat_qs_message(msg);
    } else {
        plat_qs_message("Could not write the state to the SD card");
    }
}

void port_qs_service(void)
{
    if (!qs_req)
        return;
    if (qs_req == 3) {
        /* the fade is done (as the game over's Continue waits for it) */
        if (!(sys->cb_flg & 0x2) || ++qs_wait > 45) {
            short retries = sys->retry_ct;
            memcpy(freemem, qs_buf + 8, QS_BLOCK);
            qs_req = 0;
            /* bhExitGame draws one last frame: a game over gets there with
             * the scene stopped, here this frame's draw list is still full */
            Ps2ClearOT();
            bhPopGameData();
            sys->retry_ct = retries;    /* not a Continue: the count stays */
        }
        return;
    }
    if (!port_player_has_control()) {
        plat_qs_message(qs_req == 1 ? "You can only save during play" : "You can only load during play");
        qs_req = 0;
        return;
    }
    if (qs_req == 1) {
        qs_save();
        qs_req = 0;
        return;
    }
    {
        unsigned n = QS_BLOCK;
        if (n > QS_MAX || !plat_qs_read(qs_buf, n + 8) ||
            ((unsigned *)qs_buf)[0] != QS_MAGIC || ((unsigned *)qs_buf)[1] != n) {
            plat_qs_message("No saved state");
            qs_req = 0;
            return;
        }
        bhSetScreenFade(0xFF000000u, 10.0f);
        RequestAllStopSoundEx(1, 1, 10);
        qs_wait = 0;
        qs_req = 3;
        plat_qs_loading();              /* back to the radar while the room loads */
    }
}

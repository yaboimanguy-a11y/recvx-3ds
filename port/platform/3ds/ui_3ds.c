/*
 * The 3DS bottom screen (citro2d), laid out as the MVC2 port's: the room
 * radar (port/ps2/radar.c hands its shapes over with rend_bot_tri) above a
 * footer with the settings cog at the left.  Touch the cog: the settings
 * screen (frame skip, 4:3 / widescreen, logging, check for updates), kept
 * in settings.txt in
 * the app's folder; touch it again to go back.
 *
 * Drawn once per shown frame between the game's draws and C3D_FrameEnd
 * (rend_frame_end in port/render/3ds/rend_c3d.c).
 */
#include <3ds.h>
#include <citro2d.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ui_3ds.h"
#include "update_3ds.h"
#include "rend.h"

#define W 320.f
#define HEAD_H 32.f
#define FOOT_Y 214.f
#define Z 0.5f
#define GEAR_W 40.f

#define RGB(r, g, b) C2D_Color32(r, g, b, 255)
#define RGBA(r, g, b, a) C2D_Color32(r, g, b, a)

static C3D_RenderTarget *bottom;
static C2D_TextBuf tbuf;
static int ui_ok, settings_open;
static u32 c_bg0, c_bg1, c_white, c_dim, c_gold, c_head0, c_head1, c_red;

#ifdef RECVX_O3DS
UiSettings g_ui = { 1, 0, 0, 0 };       /* frame skip on: it cannot keep up without */
#else
UiSettings g_ui = { 2, 0, 0, 0 };
#endif
static char app_dir[128];

/* ---------------- settings.txt ---------------- */
static void settings_load(void)
{
    char path[200], line[96];
    FILE *f;
    int version = 0;
    snprintf(path, sizeof path, "%s/settings.txt", app_dir);
    f = fopen(path, "r");
    if (!f)
        return;
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        int v;
        if (!eq)
            continue;
        *eq = 0;
        v = atoi(eq + 1);
        if (!strcmp(line, "frame_skip")) g_ui.frame_skip = v < 0 || v > 2 ? 2 : v;
        else if (!strcmp(line, "widescreen")) g_ui.stretch = v != 0;
        else if (!strcmp(line, "logging")) g_ui.logging = v != 0;
        else if (!strcmp(line, "fast_doors")) g_ui.fast_doors = v != 0;
        else if (!strcmp(line, "version")) version = v;
    }
    fclose(f);
    if (version < 2 && g_ui.frame_skip == 1)
        g_ui.frame_skip = 2;            /* build 49's default "on" becomes auto */
}

static void settings_save(void)
{
    char path[200];
    FILE *f;
    snprintf(path, sizeof path, "%s/settings.txt", app_dir);
    f = fopen(path, "w");
    if (!f)
        return;
    fprintf(f, "version=2\nframe_skip=%d\nwidescreen=%d\nlogging=%d\nfast_doors=%d\n",
            g_ui.frame_skip, g_ui.stretch, g_ui.logging, g_ui.fast_doors);
    fclose(f);
}

void ui_load_settings(const char *dir)
{
    snprintf(app_dir, sizeof app_dir, "%s", dir);
    settings_load();
}

/* ---------------- drawing ---------------- */
static void colours(void)
{
    c_bg0 = RGB(18, 22, 44);
    c_bg1 = RGB(8, 10, 24);
    c_white = RGB(240, 240, 248);
    c_dim = RGB(150, 156, 178);
    c_gold = RGB(255, 204, 64);
    c_head0 = RGB(120, 36, 160);
    c_head1 = RGB(30, 70, 180);
    c_red = RGB(214, 70, 62);
}

static float text_w(const char *s, float sc)
{
    C2D_Text t;
    float w, h;
    C2D_TextParse(&t, tbuf, s);
    C2D_TextGetDimensions(&t, sc, sc, &w, &h);
    return w;
}

static void text(const char *s, float x, float y, float sc, u32 col, int right)
{
    C2D_Text t;
    float w, h;
    C2D_TextParse(&t, tbuf, s);
    C2D_TextOptimize(&t);
    C2D_TextGetDimensions(&t, sc, sc, &w, &h);
    C2D_DrawText(&t, C2D_WithColor, right ? x - w : x, y, Z, sc, sc, col);
}

static void draw_header(const char *title)
{
    C2D_DrawRectangle(0, 0, Z, W, HEAD_H, c_head0, c_head1, c_head0, c_head1);
    C2D_DrawRectSolid(0, HEAD_H - 2, Z, W, 2, RGBA(255, 255, 255, 60));
    text(title, 10.f, 5.f, 0.68f, c_white, 0);
}

/* a cog: teeth round a disc with a hole */
static void draw_gear(float cx, float cy, float r, u32 col, u32 hole)
{
    int i;
    for (i = 0; i < 8; i++) {
        float a = (float)i * 0.785398f;
        C2D_DrawCircleSolid(cx + cosf(a) * r * 0.86f, cy + sinf(a) * r * 0.86f, Z, r * 0.27f, col);
    }
    C2D_DrawCircleSolid(cx, cy, Z, r * 0.78f, col);
    C2D_DrawCircleSolid(cx, cy, Z, r * 0.32f, hole);
}

/* the footer's shortcuts, right of the cog */
#define FB_X0 (GEAR_W + 2.f)
#define FB_W ((W - FB_X0 - 4.f) / 3.f)
static const char *const fb_label[3] = { "Items", "Map", "Turn" };

static void draw_footer(void)
{
    u32 bar = RGB(14, 10, 26), btn = settings_open ? RGB(120, 36, 160) : RGB(46, 44, 72);
    C2D_DrawRectSolid(0, FOOT_Y, Z, W, 240 - FOOT_Y, bar);
    C2D_DrawRectSolid(0, FOOT_Y, Z, W, 1, RGBA(255, 255, 255, 40));
    C2D_DrawRectSolid(4, FOOT_Y + 3, Z, GEAR_W - 8, 240 - FOOT_Y - 6, btn);
    if (settings_open) {
        text("tap the cog to go back", GEAR_W + 2, FOOT_Y + 8, 0.36f, c_dim, 0);
    } else {
        int i;
        for (i = 0; i < 3; i++) {
            float x = FB_X0 + i * FB_W;
            C2D_DrawRectSolid(x + 1, FOOT_Y + 3, Z, FB_W - 2, 240 - FOOT_Y - 6, RGB(46, 44, 72));
            text(fb_label[i], x + FB_W / 2 - text_w(fb_label[i], 0.46f) / 2, FOOT_Y + 5, 0.46f, c_white, 0);
        }
    }
    /* circles last: they switch citro2d into its circle mode */
    draw_gear(GEAR_W / 2, FOOT_Y + 13, 8.f, c_white, btn);
}

/* ---------------- the status strip ---------------- */
static PortHud dhud;                    /* for the frame being drawn (ui_radar_commit) */

static void draw_hud(void)
{
    static const struct { const char *t; u8 r, g, b; } cd[5] = {
        { "FINE", 60, 170, 90 }, { "CAUTION", 173, 173, 0 }, { "CAUTION", 214, 66, 0 },
        { "DANGER", 173, 0, 0 }, { "POISON", 189, 0, 140 } };
    const PortHud *h = &dhud;
    char line[48], rm[16];
    int c = h->cond < 0 || h->cond > 4 ? 0 : h->cond;
    float pw = 66.f;
    C2D_DrawRectSolid(0, 0, Z, W, 24, RGB(14, 10, 26));
    C2D_DrawRectSolid(0, 24, Z, W, 1, RGBA(255, 255, 255, 40));
    C2D_DrawRectSolid(4, 3, Z, pw, 18, RGB(cd[c].r, cd[c].g, cd[c].b));
    text(cd[c].t, 4 + pw / 2 - text_w(cd[c].t, 0.44f) / 2, 4, 0.44f, c_white, 0);
    if (h->has_weapon) {
        if (!h->show_count)
            snprintf(line, sizeof line, "%s", h->name);
        else if (h->infinite)
            snprintf(line, sizeof line, "%s  \xE2\x88\x9E", h->name);
        else if (h->percent)
            snprintf(line, sizeof line, "%s  %d%%", h->name, h->count);
        else
            snprintf(line, sizeof line, "%s  %d", h->name, h->count);
        text(line, W - 6, 4, 0.46f, c_white, 1);
    } else {
        text("No weapon", W - 6, 4, 0.46f, c_dim, 1);
    }
    snprintf(rm, sizeof rm, "Room %d-%02d", h->stage, h->room);
    text(rm, 6, 27, 0.36f, c_dim, 0);
}

/* ---------------- settings screen ---------------- */
#define SET_Y0 (HEAD_H + 4.f)
#define SET_ROW 23.f
#define SET_H 21.f
enum { SET_SAVE, SET_LOAD, SET_SKIP, SET_PICTURE, SET_DOORS, SET_LOG, SET_UPDATE, SET_N };

static void set_row(int i, const char *label, const char *value, u32 vcol)
{
    float y = SET_Y0 + i * SET_ROW, vw = text_w(value, 0.44f) + 16;
    C2D_DrawRectSolid(8, y, Z, W - 16, SET_H, RGBA(255, 255, 255, 22));
    text(label, 18, y + 1, 0.48f, c_white, 0);
    C2D_DrawRectSolid(W - 16 - vw, y + 2, Z, vw, SET_H - 4, vcol);
    text(value, W - 16 - vw / 2 - text_w(value, 0.44f) / 2, y + 2, 0.44f, c_white, 0);
}

static char msg_line[64];
static volatile int msg_frames;

void ui_close_settings(void) { settings_open = 0; }

void ui_message(const char *s)
{
    snprintf(msg_line, sizeof msg_line, "%s", s);
    msg_frames = 120;
}

static void draw_settings(void)
{
    u32 on = RGB(60, 150, 90), off = RGB(90, 90, 110), blue = RGB(70, 80, 140);
    char line[96], v[24];
    int st = update_state(line, sizeof line, NULL);
    const char *ub = st == UPD_AVAILABLE ? "Install" : st == UPD_BUSY ? "Wait..." : "Check";
    draw_header("Settings");
    snprintf(v, sizeof v, "v%s", update_version());
    text(v, W - 10, 9.f, 0.46f, c_white, 1);
    set_row(SET_SAVE, "Save State", "Save", blue);
    set_row(SET_LOAD, "Load State", "Load", blue);
    set_row(SET_SKIP, "Frame skip", g_ui.frame_skip == 2 ? "AUTO" : g_ui.frame_skip ? "ON" : "OFF",
            g_ui.frame_skip == 2 ? blue : g_ui.frame_skip ? on : off);
    set_row(SET_PICTURE, "Picture", g_ui.stretch ? "Widescreen" : "4:3", blue);
    set_row(SET_DOORS, "Door scenes", g_ui.fast_doors ? "Fast" : "Normal", g_ui.fast_doors ? on : blue);
    set_row(SET_LOG, "Logging (recvx_log.txt)", g_ui.logging ? "ON" : "OFF", g_ui.logging ? on : off);
    set_row(SET_UPDATE, "Check for updates", ub, st == UPD_AVAILABLE ? c_red : blue);
    {
        float y = SET_Y0 + SET_N * SET_ROW + 1;
        u32 lc = st == UPD_ERROR ? c_red : st == UPD_DONE || st == UPD_AVAILABLE ? c_gold : c_dim;
        if (msg_frames > 0) {           /* a state saved or loaded, or why not */
            msg_frames--;
            text(msg_line, 14, y, 0.38f, c_gold, 0);
        } else {
            text(line, 14, y, 0.38f, lc, 0);
        }
    }
}

/* ---------------- the radar's shapes ---------------- */
#define MAX_TRI 2048
typedef struct { float x[3], y[3]; u32 c; } Tri;
/* filled by the game thread (radar_draw), drawn by the render thread:
 * two sets, swapped when a frame is handed over (ui_radar_commit) */
static Tri trisbuf[2][MAX_TRI];
static Tri *tris = trisbuf[0];      /* being filled */
static int ntri, radar_on;
static const Tri *dtris = trisbuf[1];   /* being drawn */
static int dntri, dradar_on;

static PortHud hud_next;
void ui_radar_commit(void)
{
    port_hud(&hud_next);
    dtris = tris;
    dntri = ntri;
    dradar_on = radar_on;
    tris = tris == trisbuf[0] ? trisbuf[1] : trisbuf[0];
    ntri = 0;
    radar_on = 0;
}

void ui_radar_tri(float x0, float y0, float x1, float y1, float x2, float y2, unsigned rgba)
{
    Tri *t;
    unsigned r = (rgba & 0xFF) * 2, g = ((rgba >> 8) & 0xFF) * 2, b = ((rgba >> 16) & 0xFF) * 2;
    if (ntri == MAX_TRI)
        return;
    t = &tris[ntri++];
    t->x[0] = x0; t->y[0] = y0;
    t->x[1] = x1; t->y[1] = y1;
    t->x[2] = x2; t->y[2] = y2;
    t->c = RGB(r > 255 ? 255 : r, g > 255 ? 255 : g, b > 255 ? 255 : b);
    radar_on = 1;
}

static void draw_radar(void)
{
    int i;
    for (i = 0; i < dntri; i++) {
        const Tri *t = &dtris[i];
        C2D_DrawTriangle(t->x[0], t->y[0], t->c, t->x[1], t->y[1], t->c, t->x[2], t->y[2], t->c, Z);
    }
}

/* ---------------- public ---------------- */
int ui_init(void)
{
    gfxSetScreenFormat(GFX_BOTTOM, GSP_BGR8_OES);
    gfxSetDoubleBuffering(GFX_BOTTOM, true);
    if (!C2D_Init(C2D_DEFAULT_MAX_OBJECTS))
        return -1;
    bottom = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    tbuf = C2D_TextBufNew(2048);
    if (!bottom || !tbuf)
        return -2;
    colours();
    ui_ok = 1;
    return 0;
}

void ui_render(void)
{
    if (!ui_ok)
        return;
    C2D_TextBufClear(tbuf);
    C2D_Prepare();
    /* citro2d assumes citro3d's defaults; the game's renderer leaves its own
     * blending, alpha test and scissor set */
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
                   GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    C2D_TargetClear(bottom, c_bg1);
    C2D_SceneBegin(bottom);
    if (settings_open) {
        C2D_DrawRectangle(0, 0, 0.1f, W, 240, c_bg0, c_bg0, c_bg1, c_bg1);
        draw_settings();
    } else if (dradar_on) {
        C2D_DrawRectangle(0, 0, 0.1f, W, FOOT_Y, c_bg1, c_bg1, c_bg1, c_bg1);
        draw_radar();
        dhud = hud_next;
        if (dhud.valid)
            draw_hud();
    } else {
        C2D_DrawRectangle(0, 0, 0.1f, W, 240, c_bg0, c_bg0, c_bg1, c_bg1);
        text("The room radar shows here during play.", 22, 96, 0.44f, c_dim, 0);
    }
    draw_footer();
    C2D_Flush();
}

/* a touch went down at x, y: the cog, or a settings row */
int ui_touch(int x, int y)
{
    if (!ui_ok)
        return UI_NONE;
    if (y >= FOOT_Y) {
        if (x < GEAR_W) {
            settings_open = !settings_open;
        } else if (!settings_open) {
            int b = (int)((x - FB_X0) / FB_W);
            if (b >= 0 && b < 3)
                return b == 0 ? UI_ITEMS : b == 1 ? UI_MAP : UI_TURN;
        } else if (x < GEAR_W + 60) {
            settings_open = 0;
        }
        return UI_NONE;
    }
    if (settings_open) {
        int i = (int)((y - SET_Y0) / SET_ROW);
        if (y < SET_Y0 || i >= SET_N || y > SET_Y0 + i * SET_ROW + SET_H)
            return UI_NONE;
        if (i == SET_SAVE)
            return UI_SAVE;
        if (i == SET_LOAD)
            return UI_LOAD;
        if (i == SET_UPDATE) {
            int st = update_state(NULL, 0, NULL);
            if (st == UPD_AVAILABLE)
                update_install();
            else if (st != UPD_BUSY)
                update_check(app_dir);
            return UI_NONE;
        }
        if (i == SET_SKIP) g_ui.frame_skip = g_ui.frame_skip == 2 ? 1 : g_ui.frame_skip == 1 ? 0 : 2;  /* auto, on, off */
        else if (i == SET_PICTURE) g_ui.stretch = !g_ui.stretch;
        else if (i == SET_LOG) g_ui.logging = !g_ui.logging;
        else if (i == SET_DOORS) g_ui.fast_doors = !g_ui.fast_doors;
        settings_save();
        return i == SET_SKIP ? UI_SKIP : i == SET_PICTURE ? UI_PICTURE : i == SET_LOG ? UI_LOG : UI_NONE;
    }
    return UI_NONE;
}

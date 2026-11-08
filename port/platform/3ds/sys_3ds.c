/* sys_3ds.c: the platform side of the port on the 3DS (port/ps2/plat.h):
 * log, threads, controller, frame pacing, clock, app folder. */
#include <3ds.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "plat.h"
#include "ui_3ds.h"
#include "rend.h"

#define APP_DIR "sdmc:/3ds/recvx"

static FILE *g_log;
static int g_console = 1;
static int g_frames;

const char *plat_app_dir(void) { return APP_DIR; }

/* the log file, when logging is on (settings; off by default) */
void sys3ds_open_log(void)
{
    if (g_ui.logging && !g_log)
        g_log = fopen(APP_DIR "/recvx_log.txt", "w");
    else if (!g_ui.logging && g_log) {
        fclose(g_log);
        g_log = NULL;
    }
}

int plat_frame_skip(void) { return g_ui.frame_skip; }
int plat_logging(void) { return g_log != NULL; }
int plat_widescreen(void) { return g_ui.stretch; }
int plat_fast_doors(void) { return g_ui.fast_doors; }

/* Save State / Load State: the one slot's file (port/ps2/radar.c) */
int plat_qs_write(const void *p, unsigned n)
{
    FILE *f = fopen(APP_DIR "/savestate.bin.new", "wb");
    size_t w;
    if (!f)
        return 0;
    w = fwrite(p, 1, n, f);
    if (fclose(f) || w != n) {
        remove(APP_DIR "/savestate.bin.new");
        return 0;
    }
    remove(APP_DIR "/savestate.bin");
    return rename(APP_DIR "/savestate.bin.new", APP_DIR "/savestate.bin") == 0;
}

int plat_qs_read(void *p, unsigned n)
{
    FILE *f = fopen(APP_DIR "/savestate.bin", "rb");
    size_t r;
    if (!f)
        return 0;
    r = fread(p, 1, n, f);
    fclose(f);
    return r == n;
}

void plat_qs_message(const char *s) { ui_message(s); }
void plat_qs_loading(void) { ui_close_settings(); }

/* typewriters the player has used (port/ps2/radar.c): room id, spot */
int plat_tw_load(short *pairs, int max)
{
    FILE *f = fopen(APP_DIR "/typewriters.txt", "r");
    int n = 0, a, b;
    if (!f)
        return 0;
    while (n < max && fscanf(f, "%d %d", &a, &b) == 2) {
        pairs[n * 2] = (short)a;
        pairs[n * 2 + 1] = (short)b;
        n++;
    }
    fclose(f);
    return n;
}

void plat_tw_save(const short *pairs, int n)
{
    FILE *f = fopen(APP_DIR "/typewriters.txt", "w");
    int i;
    if (!f)
        return;
    for (i = 0; i < n; i++)
        fprintf(f, "%d %d\n", pairs[i * 2], pairs[i * 2 + 1]);
    fclose(f);
}

void sys3ds_console(int on) { g_console = on; }

/* the log: a message repeated many times in a row is written once with a
 * count; the file is flushed at most every ~0.5 s (and at exit) so a
 * chatty game does not crawl on SD writes */
static char g_last[256];
static unsigned g_repeat;
static u64 g_last_flush;
static LightLock g_log_lock;
static int g_log_lock_init;

static void log_write(const char *s)
{
    u64 now = svcGetSystemTick();
    if (g_log) {
        fputs(s, g_log);
        if (now - g_last_flush > SYSCLOCK_ARM11 / 2) {
            fflush(g_log);
            g_last_flush = now;
        }
    }
    if (g_console)
        fputs(s, stdout);
}

void plat_log(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    if (!g_log && !g_console)       /* logging off: nothing to format (the game prints a lot) */
        return;
    if (!g_log_lock_init) {
        LightLock_Init(&g_log_lock);
        g_log_lock_init = 1;
    }
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    LightLock_Lock(&g_log_lock);
    if (strncmp(buf, g_last, sizeof g_last - 1) == 0) {
        g_repeat++;
    } else {
        if (g_repeat) {
            char r[64];
            snprintf(r, sizeof r, " (repeated %u more times)\n", g_repeat);
            log_write(r);
            g_repeat = 0;
        }
        snprintf(g_last, sizeof g_last, "%s", buf);
        log_write(buf);
    }
    LightLock_Unlock(&g_log_lock);
}

void plat_log_flush(void)
{
    if (g_log)
        fflush(g_log);
}

/* ---- threads (run one at a time; see port/ps2/kernel.c) ---------------------- */
struct PlatSem { LightSemaphore s; };

PlatSem *plat_sem_new(int initial)
{
    PlatSem *p = malloc(sizeof *p);
    if (p)
        LightSemaphore_Init(&p->s, initial, 0x7FFF);
    return p;
}

void plat_sem_post(PlatSem *p) { LightSemaphore_Release(&p->s, 1); }
void plat_sem_wait(PlatSem *p) { LightSemaphore_Acquire(&p->s, 1); }

static s32 g_main_prio = 0x30;

/* game threads: same core as the game, above it in priority, so a woken
 * thread preempts the game like on the PS2 (port/ps2/kernel.c hands the
 * baton) */
int plat_thread_new(void (*fn)(void *), void *arg, int stack_size)
{
    Thread t = threadCreate(fn, arg, (size_t)stack_size, g_main_prio - 2, -2, true);
    return t ? 0 : -1;
}

void plat_log_flush(void);
static void (*g_vblank_fn)(void);
static int g_scan_ticks;
static volatile int g_quit;
static u64 g_quit_tick;

static volatile int g_vbl_stop;
static Thread g_vbl_thread;

/* called before the app exits: the vblank thread must not call into the
 * game once it is shutting down */
void sys3ds_stop_vblank(void)
{
    if (!g_vbl_thread)
        return;
    g_vbl_stop = 1;
    threadJoin(g_vbl_thread, 500000000ull);
    g_vbl_thread = NULL;
}

static void vblank_thread(void *arg)
{
    const u64 period = SYSCLOCK_ARM11 / 60;   /* NTSC: ~59.94; close enough */
    u64 next = svcGetSystemTick() + period;
    (void)arg;
    while (!g_vbl_stop) {
        u64 now = svcGetSystemTick();
        if (now < next)
            svcSleepThread((s64)((next - now) * 1000000000ull / SYSCLOCK_ARM11));
        next += period;
        if (svcGetSystemTick() > next + period * 4)
            next = svcGetSystemTick() + period;
        if (g_vbl_stop)
            break;
        g_vblank_fn();
        /* quit even when the game is stuck: L + R + START + SELECT */
        {
            u32 k = hidKeysHeld();
            if ((k & (KEY_L | KEY_R | KEY_START | KEY_SELECT)) == (KEY_L | KEY_R | KEY_START | KEY_SELECT)) {
                /* the game thread quits cleanly at its next frame; if it is
                 * stuck, exit from here after 2 s */
                if (!g_quit) {
                    plat_log("quit (L+R+START+SELECT)\n");
                    plat_log_flush();
                    g_quit = 1;
                    g_quit_tick = svcGetSystemTick();
                }
            }
            if (g_quit && svcGetSystemTick() - g_quit_tick > SYSCLOCK_ARM11 * 2) {
                plat_log("quit: the game did not stop, forcing it\n");
                plat_log_flush();
                svcExitProcess();
            }
            if (++g_scan_ticks >= 2) {      /* keep the pad fresh when the game is busy */
                g_scan_ticks = 0;
                hidScanInput();
            }
        }
    }
}

void plat_start_vblank(void (*fn)(void))
{
    svcGetThreadPriority(&g_main_prio, CUR_THREAD_HANDLE);
    g_vblank_fn = fn;
    g_vbl_thread = threadCreate(vblank_thread, NULL, 64 * 1024, g_main_prio - 4, -2, false);
}

void plat_sleep_ms(int ms) { svcSleepThread((s64)ms * 1000000); }

int plat_vprintf(const char *fmt, va_list ap)
{
    char buf[512];
    int n;
    if (!g_log && !g_console)
        return 0;
    n = vsnprintf(buf, sizeof buf, fmt, ap);
    plat_log("%s", buf);
    return n;
}

/* ---- controller ------------------------------------------------------------- */
static unsigned g_buttons;
static unsigned char g_sticks[4] = { 128, 128, 128, 128 };

/* brightness: tap the bottom screen, left half darker, right half
 * brighter; kept in display.txt.  Levels 0..5 are a brightness curve on the
 * finished picture (port/render/3ds/rend_c3d.c); 6..8 use a milder curve
 * and give dark 3D surfaces a minimum of light and thin the fog
 * (port/ps2/vu_strip.c), for rooms that are lit for a TV. */
void rend_set_brightness(int level);
void vu_set_assist(int level);
#define BRIGHT_MAX 8
static int g_bright = 3;

static void bright_apply(void)
{
    /* 6..8: a milder curve (menus and text stay clean) plus the 3D assist */
    static const int curve[BRIGHT_MAX + 1] = { 0, 1, 2, 3, 4, 5, 3, 3, 4 };
    rend_set_brightness(curve[g_bright]);
    vu_set_assist(g_bright > 5 ? g_bright - 5 : 0);
}

void gs_set_mist(int level);

void sys3ds_load_brightness(void)
{
    gs_set_mist(1);                     /* the night mist, light */
    g_bright = 3;                       /* fixed (the tap control is gone) */
    bright_apply();
}

volatile int g_dump_req;

/* ---- presses made for the player: a mask for each coming frame ---------- */
int port_player_has_control(void);
#define PQ_N 8
static unsigned pq[PQ_N];
static int pq_h, pq_n;

static void press(const unsigned *m, int n)
{
    int i;
    if (pq_n)                               /* one thing at a time */
        return;
    for (i = 0; i < n && i < PQ_N; i++)
        pq[i] = m[i];
    pq_h = 0;
    pq_n = n;
}

static void press_turn(void)
{
    /* back held, then a new press of run: the game's quick turn */
    static const unsigned m[4] = { PAD_DOWN, PAD_DOWN | PAD_CIRCLE, PAD_DOWN | PAD_CIRCLE, 0 };
    press(m, 4);
}

static void bright_touch(void)
{
    static int was_down;
    int down = (hidKeysHeld() & KEY_TOUCH) != 0;
    if (!down || was_down) {
        was_down = down;
        return;
    }
    was_down = 1;
    if (hidKeysHeld() & KEY_L) {        /* L + tap: a primitive dump to the log */
        g_dump_req = 1;
        return;
    }
    {
        touchPosition tp;
        hidTouchRead(&tp);
        int what = ui_touch(tp.px, tp.py);
        if (what == UI_LOG) {
            sys3ds_open_log();
            if (g_ui.logging)
                plat_log("log on (settings), built %s %s\n", __DATE__, __TIME__);
        } else if (what == UI_PICTURE) {
            extern void port_aspect_refresh(void);
            port_aspect_refresh();          /* the wider (or 4:3) view at once */
        } else if (what == UI_SAVE || what == UI_LOAD) {
            extern void port_qs_request(int load);
            port_qs_request(what == UI_LOAD);
        } else if ((what == UI_ITEMS || what == UI_MAP || what == UI_TURN) && port_player_has_control()) {
            static const unsigned items[3] = { PAD_START, PAD_START, 0 };
            static const unsigned map[3] = { PAD_TRIANGLE, PAD_TRIANGLE, 0 };
            if (what == UI_ITEMS)
                press(items, 3);
            else if (what == UI_MAP)
                press(map, 3);
            else
                press_turn();
        }
    }
}

static void read_input(void)
{
    u32 k;
    circlePosition cp, cs;
    unsigned b = 0;
    hidScanInput();
    k = hidKeysHeld();
    bright_touch();
    if (k & KEY_DUP) b |= PAD_UP;
    if (k & KEY_DDOWN) b |= PAD_DOWN;
    if (k & KEY_DLEFT) b |= PAD_LEFT;
    if (k & KEY_DRIGHT) b |= PAD_RIGHT;
    if (k & KEY_START) b |= PAD_START;
    if (k & KEY_SELECT) b |= PAD_SELECT;
    if (k & KEY_Y) b |= PAD_SQUARE;
    if (k & KEY_X) b |= PAD_TRIANGLE;
    /* by meaning, not position: the US game confirms with Cross and backs
     * out with Circle, and on a 3DS A confirms and B goes back */
    if (k & KEY_A) b |= PAD_CROSS;
    if (k & KEY_B) b |= PAD_CIRCLE;
    if (k & KEY_L) b |= PAD_L1;
    if (k & KEY_R) b |= PAD_R1;
    if (k & KEY_ZL) b |= PAD_L2;
    if (k & KEY_ZR) b |= PAD_R2;
    hidCircleRead(&cp);
    irrstCstickRead(&cs);
    if (pq_n) {                             /* presses made for the player */
        b |= pq[pq_h++];
        pq_n--;
    }
    /* Circle Pad = left stick, C-stick = right stick (0..255, y down) */
    {
        int lx = 128 + cp.dx * 127 / 156, ly = 128 - cp.dy * 127 / 156;
        int rx = 128 + cs.dx * 127 / 156, ry = 128 - cs.dy * 127 / 156;
#define CL(v) (unsigned char)((v) < 0 ? 0 : (v) > 255 ? 255 : (v))
        g_sticks[0] = CL(rx);
        g_sticks[1] = CL(ry);
        g_sticks[2] = CL(lx);
        g_sticks[3] = CL(ly);
#undef CL
    }
    g_buttons = b;
}

void plat_pad(unsigned *buttons, unsigned char sticks[4])
{
    *buttons = g_buttons;
    memcpy(sticks, g_sticks, 4);
}

/* ---- frame ------------------------------------------------------------------ */
/* the system's events (HOME menu, sleep) and the pad, once a frame.  The
 * renderer calls it between frames, while its thread has nothing on the
 * GPU (rend_frame_end); a frame not drawn (frame skip) polls here after
 * waiting for that thread */
static int g_polled, g_poll_ok = 1;
/* the app is closing (HOME menu "close", or L + R + START + SELECT): the
 * renderer hands no more frames to its thread (the GPU is no longer ours) */
int plat_quitting(void)
{
    return !g_poll_ok || g_quit;
}

void plat_frame_poll(void)
{
    g_poll_ok = aptMainLoop();
    if (g_poll_ok)
        read_input();
    g_polled = 1;
}

int plat_frame(void)
{
    if (!g_polled) {
        rend_wait_idle();
        plat_frame_poll();
    }
    g_polled = 0;
    if (!g_poll_ok)
        return 0;
    if (g_quit || (hidKeysHeld() & (KEY_L | KEY_R | KEY_START | KEY_SELECT)) == (KEY_L | KEY_R | KEY_START | KEY_SELECT))
        return 0;
    /* pacing: the game waits for two vblanks (port/ps2/draw_stubs.c) */
    ++g_frames;
    return 1;
}

void plat_clock(int *year, int *month, int *day, int *hour, int *min, int *sec)
{
    time_t t = time(NULL);
    struct tm *tm = gmtime(&t);
    *year = tm->tm_year + 1900;
    *month = tm->tm_mon + 1;
    *day = tm->tm_mday;
    *hour = tm->tm_hour;
    *min = tm->tm_min;
    *sec = tm->tm_sec;
}

void sys3ds_poll_input(void) { read_input(); }

/* milliseconds since boot (frame timing in the log) */
unsigned plat_ms(void) { return (unsigned)(svcGetSystemTick() / (SYSCLOCK_ARM11 / 1000)); }

/* microseconds since boot (profiling) */
unsigned plat_us(void) { return (unsigned)(svcGetSystemTick() / (SYSCLOCK_ARM11 / 1000000)); }

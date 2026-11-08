/* main_3ds.c: Resident Evil Code: Veronica X on the New Nintendo 3DS.
 *
 * Finds the user's disc image in sdmc:/3ds/recvx/, sets up the screen and
 * the CPU like the PS2 (float rounding), then runs the game's own main().
 * The game's frame ends in Ps2SwapDBuff (port/ps2/draw_stubs.c), which
 * draws, shows the frame and reads input through sys_3ds.c. */
#include <3ds.h>
#include <citro3d.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "plat.h"
#include "ui_3ds.h"
#include "disc.h"

u32 __stacksize__ = 1024 * 1024;
u32 __ctru_linear_heap_size = 32u << 20;

int recvx_ps2_main(int argc, char **argv);
int rend_init(void);
void rend_start_thread(void);
int rend_shutdown(void);
void plat_log_flush(void);
void sys3ds_open_log(void);
void sys3ds_load_brightness(void);
void sys3ds_stop_vblank(void);
void gs_init(void);
void ps2_start_vblank(void);
void sys3ds_console(int on);

static int has_ext(const char *n, const char *ext)
{
    size_t l = strlen(n), e = strlen(ext);
    return l > e && strcasecmp(n + l - e, ext) == 0;
}

/* the disc image: a .iso first, then a .bin (raw sectors are read directly) */
static int find_disc(char *out, size_t n)
{
    static const char *exts[] = { ".iso", ".bin", ".img" };
    unsigned k;
    for (k = 0; k < sizeof exts / sizeof exts[0]; k++) {
        DIR *d = opendir(plat_app_dir());
        struct dirent *e;
        if (!d)
            return -1;
        while ((e = readdir(d)) != NULL)
            if (has_ext(e->d_name, exts[k])) {
                snprintf(out, n, "%s/%s", plat_app_dir(), e->d_name);
                if (disc_open(out) == 0) {
                    closedir(d);
                    return 0;
                }
                plat_log("not a usable disc image: %s\n", e->d_name);
            }
        closedir(d);
    }
    return -1;
}

static void wait_exit(void)
{
    printf("\nPress START to exit.\n");
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START)
            break;
        gspWaitForVBlank();
    }
    gfxExit();
    exit(0);
}

int main(void)
{
    char path[512];
    bool n3ds = false;
    int r;

    gfxInitDefault();
    consoleInit(GFX_BOTTOM, NULL);
    APT_CheckNew3DS(&n3ds);
    osSetSpeedupEnable(true);           /* New 3DS: 804 MHz + L2 cache */
    irrstInit();
    mkdir(plat_app_dir(), 0777);
    ui_load_settings(plat_app_dir());   /* settings.txt (the cog on the bottom screen) */
    sys3ds_open_log();
    plat_log("Resident Evil CVX (native port, test build)\n");
    plat_log("%s\n", n3ds ? "New 3DS" : "WARNING: not a New 3DS - this will be too slow");
    plat_log("memory: application %u KB (%u KB free), linear heap free %u KB\n",
             (unsigned)(osGetMemRegionSize(MEMREGION_APPLICATION) >> 10),
             (unsigned)(osGetMemRegionFree(MEMREGION_APPLICATION) >> 10), (unsigned)(linearSpaceFree() >> 10));

    if (find_disc(path, sizeof path) != 0) {
        printf("\nNo disc image found.\n\nPut your own dump of the US disc\n(SLUS-20184) in %s/\nas a .iso (or .bin).\n", plat_app_dir());
        wait_exit();
    }
    plat_log("disc: %s\n", path);
    {
        unsigned lsn, size;
        if (disc_find("SLUS_201.84", &lsn, &size) != 0) {
            printf("\nThat image is not the US disc\n(no SLUS_201.84 on it).\n");
            wait_exit();
        }
    }

    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE * 4);
    r = rend_init();
    if (r != 0) {
        plat_log("renderer init failed (%d)\n", r);
        wait_exit();
    }
    gs_init();
    plat_audio_init();
    sys3ds_console(0);
    if (ui_init() != 0)                 /* the bottom screen: radar and settings */
        plat_log("bottom screen init failed\n");
    rend_start_thread();
    sys3ds_load_brightness();

    /* PS2 FPU semantics: round toward zero, flush denormals, default NaN */
    {
        u32 fpscr;
        __asm__ volatile("vmrs %0, fpscr" : "=r"(fpscr));
        fpscr = (fpscr & ~0x03C00000u) | 0x03C00000u;
        __asm__ volatile("vmsr fpscr, %0" : : "r"(fpscr));
    }

    ps2_start_vblank();
    plat_log("starting the game (log continues in recvx_log.txt)\n");
    sys3ds_console(0);          /* the text console is slow; the log file keeps everything */
    recvx_ps2_main(0, NULL);
    plat_log("the game returned\n");
    sys3ds_stop_vblank();
    plat_audio_exit();
    if (!rend_shutdown())
        svcExitProcess();
    C3D_Fini();
    gfxExit();
    return 0;
}

/* called on the game thread at a frame boundary */
void plat_quit(void)
{
    plat_log("quitting\n");
    plat_log_flush();
    sys3ds_stop_vblank();
    plat_audio_exit();
    if (!rend_shutdown()) {             /* the GPU thread is stuck: leave at once */
        plat_log("quitting: renderer thread stuck, exiting directly\n");
        plat_log_flush();
        svcExitProcess();
    }
    C3D_Fini();
    gfxExit();
    exit(0);
}

/* plat.h: what the PS2 layer (port/ps2, game-side) needs from the platform
 * (port/platform/3ds).  Plain C types only: the two sides are built with
 * different enum sizes. */
#pragma once
#include <stdarg.h>

/* logging (to the log file and, early on, the screen) */
void plat_log(const char *fmt, ...);

/* threads that run one at a time (the PS2 kernel's model, port/ps2/kernel.c) */
typedef struct PlatSem PlatSem;
PlatSem *plat_sem_new(int initial);
void plat_sem_post(PlatSem *s);
void plat_sem_wait(PlatSem *s);
int plat_thread_new(void (*fn)(void *), void *arg, int stack_size);   /* 0 = ok */
/* call fn 60 times a second from a thread that preempts the game */
void plat_start_vblank(void (*fn)(void));
/* the game's printf */
int plat_vprintf(const char *fmt, va_list ap);

/* the folder the app runs from (save files, logs), no trailing slash */
const char *plat_app_dir(void);

/* controller: PS2 button bits (active high, PAD_* below) and sticks 0..255 */
enum { PAD_SELECT = 0x0001, PAD_L3 = 0x0002, PAD_R3 = 0x0004, PAD_START = 0x0008,
       PAD_UP = 0x0010, PAD_RIGHT = 0x0020, PAD_DOWN = 0x0040, PAD_LEFT = 0x0080,
       PAD_L2 = 0x0100, PAD_R2 = 0x0200, PAD_L1 = 0x0400, PAD_R1 = 0x0800,
       PAD_TRIANGLE = 0x1000, PAD_CIRCLE = 0x2000, PAD_CROSS = 0x4000, PAD_SQUARE = 0x8000 };
void plat_pad(unsigned *buttons, unsigned char sticks[4]);   /* rx, ry, lx, ly */

/* end of a game frame: show it, read input, keep 60 Hz.  0 = the user quit */
int plat_frame(void);

/* leave the app cleanly (from the game thread) */
void plat_quit(void) __attribute__((noreturn));

/* milliseconds since boot */
unsigned plat_ms(void);
unsigned plat_us(void);

/* time accounting for the [time] log line (microseconds) */
extern unsigned prof_upload_us, prof_decode_us, prof_gpu_us, prof_wait_us;

/* local time for the game clock */
void plat_clock(int *year, int *month, int *day, int *hour, int *min, int *sec);

/* sound (port/platform/3ds/audio_3ds.c): streams of 16-bit PCM the game side
 * pushes and the platform's own audio thread plays.  plat_stream_new
 * returns -1 when there is no sound; the other calls then do nothing. */
int plat_audio_init(void);                  /* 0 = sound available */
void plat_audio_exit(void);
int plat_stream_new(void);
void plat_stream_free(int s);
void plat_stream_format(int s, int nch, int rate);
void plat_stream_gain(int s, float left, float right);
void plat_stream_play(int s, int on);       /* off also drops what is queued */
unsigned plat_stream_room(int s);           /* frames that still fit */
unsigned plat_stream_queued(int s);         /* frames not yet heard */
void plat_stream_push(int s, const short *l, const short *r, unsigned n);  /* r NULL: mono */
unsigned plat_stream_drop(int s, unsigned n);

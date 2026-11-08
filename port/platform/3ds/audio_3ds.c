/* audio_3ds.c: sound output on the 3DS (port/ps2/plat.h, plat_stream_*).
 *
 * The game side (port/ps2/adx_out.c) pushes 16-bit PCM into per-stream ring
 * buffers; an audio thread of its own (on the New 3DS's spare core when it
 * can get it) moves the rings into DSP wave buffers (ndsp), one DSP channel
 * per stream.  The DSP resamples each stream to the output rate and mixes.
 *
 * ndsp needs the DSP firmware dump, sdmc:/3ds/dspfirm.cdc (made by the DSP1
 * homebrew); without it the game runs silent. */
#include <3ds.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "plat.h"

#define NSTREAM 8
#define RING_FRAMES 32768u              /* power of two */
#define CAP_FRAMES 16384u               /* what the game may queue: twice the PS2's IOP
                                         * buffer, so a slow frame (a heavy cutscene)
                                         * does not run the voices dry */
#define NWB 6
#define WB_FRAMES 1024u

typedef struct {
    int used;
    int nch, rate;
    int playing;
    int fmt_dirty, gain_dirty, clear_req;
    float gl, gr;
    LightLock lock;
    short *ring;                        /* RING_FRAMES x 2 shorts (right unused for mono) */
    unsigned rd, wr;                    /* frame counts; index = count & (RING_FRAMES - 1) */
    int had_data;                       /* for the ran-dry count */
    ndspWaveBuf wb[NWB];
    short *wbdata;                      /* linear memory: NWB x WB_FRAMES x 2 shorts */
} Stream;

static Stream S[NSTREAM];
static int g_ok;
static volatile unsigned g_dry;

/* times a playing stream ran dry since the last call (the log) */
unsigned plat_audio_dry(void)
{
    unsigned n = g_dry;
    g_dry = 0;
    return n;
}

/* the sound effects synth (port/ps2/snd_synth.c): rendered here on the
 * audio thread into a DSP channel of its own, 32 kHz stereo */
void snd_render(short *out, unsigned n);
#define SYN_CH NSTREAM
#define SYN_RATE 32000
#define SYN_NWB 3
#define SYN_FRAMES 512u
static ndspWaveBuf syn_wb[SYN_NWB];
static short *syn_data;

static void synth_service(void)
{
    int k;
    for (k = 0; k < SYN_NWB; k++) {
        ndspWaveBuf *w = &syn_wb[k];
        short *dst = syn_data + (size_t)k * SYN_FRAMES * 2;
        if (w->status == NDSP_WBUF_QUEUED || w->status == NDSP_WBUF_PLAYING)
            continue;
        snd_render(dst, SYN_FRAMES);
        memset(w, 0, sizeof *w);
        w->data_vaddr = dst;
        w->nsamples = SYN_FRAMES;
        DSP_FlushDataCache(dst, SYN_FRAMES * 4);
        ndspChnWaveBufAdd(SYN_CH, w);
    }
}
static volatile int g_run;
static Thread g_thr;

/* frames still in the DSP's queue for this stream */
static unsigned dsp_frames(const Stream *s)
{
    unsigned n = 0;
    int k;
    for (k = 0; k < NWB; k++)
        if (s->wb[k].status == NDSP_WBUF_QUEUED || s->wb[k].status == NDSP_WBUF_PLAYING)
            n += s->wb[k].nsamples;
    return n;
}

static void stream_service(int c)
{
    Stream *s = &S[c];
    int k;
    LightLock_Lock(&s->lock);
    if (s->clear_req) {
        ndspChnWaveBufClear(c);
        memset(s->wb, 0, sizeof s->wb);
        s->clear_req = 0;
    }
    if (s->fmt_dirty) {
        ndspChnWaveBufClear(c);
        memset(s->wb, 0, sizeof s->wb);
        ndspChnSetInterp(c, NDSP_INTERP_LINEAR);
        ndspChnSetRate(c, (float)s->rate);
        ndspChnSetFormat(c, s->nch == 2 ? NDSP_FORMAT_STEREO_PCM16 : NDSP_FORMAT_MONO_PCM16);
        s->fmt_dirty = 0;
        s->gain_dirty = 1;
    }
    if (s->gain_dirty) {
        float mix[12];
        memset(mix, 0, sizeof mix);
        mix[0] = s->gl;
        mix[1] = s->gr;
        ndspChnSetMix(c, mix);
        s->gain_dirty = 0;
    }
    if (s->playing) {
        /* ran dry: the DSP has nothing left while the stream plays and had
         * something before (a stream's natural end counts once too) */
        if (s->wr == s->rd && !dsp_frames(s)) {
            if (s->had_data) {
                g_dry++;
                s->had_data = 0;
            }
        } else
            s->had_data = 1;
        for (k = 0; k < NWB; k++) {
            ndspWaveBuf *w = &s->wb[k];
            unsigned avail = s->wr - s->rd, n, i;
            short *dst;
            if (w->status == NDSP_WBUF_QUEUED || w->status == NDSP_WBUF_PLAYING)
                continue;
            if (!avail)
                break;
            n = avail < WB_FRAMES ? avail : WB_FRAMES;
            dst = s->wbdata + (size_t)k * WB_FRAMES * 2;
            if (s->nch == 2) {
                for (i = 0; i < n; i++) {
                    const short *f = s->ring + (((s->rd + i) & (RING_FRAMES - 1)) << 1);
                    dst[2 * i] = f[0];
                    dst[2 * i + 1] = f[1];
                }
            } else {
                for (i = 0; i < n; i++)
                    dst[i] = s->ring[((s->rd + i) & (RING_FRAMES - 1)) << 1];
            }
            s->rd += n;
            memset(w, 0, sizeof *w);
            w->data_vaddr = dst;
            w->nsamples = n;
            DSP_FlushDataCache(dst, n * (unsigned)s->nch * 2u);
            ndspChnWaveBufAdd(c, w);
        }
    }
    LightLock_Unlock(&s->lock);
}

static void audio_thread(void *arg)
{
    (void)arg;
    while (g_run) {
        int c;
        for (c = 0; c < NSTREAM; c++)
            if (S[c].used)
                stream_service(c);
        if (syn_data)
            synth_service();
        svcSleepThread(4000000);        /* 4 ms: a wave buffer is 16+ ms */
    }
}

int plat_audio_init(void)
{
    int c;
    s32 prio = 0x30;
    if (R_FAILED(ndspInit())) {
        plat_log("audio: no sound (the DSP firmware is missing: run DSP1 once to make "
                 "sdmc:/3ds/dspfirm.cdc)\n");
        return -1;
    }
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspSetOutputCount(1);
    ndspSetMasterVol(1.0f);
    for (c = 0; c < NSTREAM; c++) {
        LightLock_Init(&S[c].lock);
        S[c].ring = malloc(RING_FRAMES * 2 * sizeof(short));
        S[c].wbdata = linearAlloc(NWB * WB_FRAMES * 2 * sizeof(short));
        if (!S[c].ring || !S[c].wbdata) {
            plat_log("audio: out of memory\n");
            ndspExit();
            return -1;
        }
    }
    syn_data = linearAlloc(SYN_NWB * SYN_FRAMES * 2 * sizeof(short));
    if (syn_data) {
        float mix[12];
        memset(syn_wb, 0, sizeof syn_wb);
        ndspChnReset(SYN_CH);
        ndspChnSetInterp(SYN_CH, NDSP_INTERP_LINEAR);
        ndspChnSetRate(SYN_CH, (float)SYN_RATE);
        ndspChnSetFormat(SYN_CH, NDSP_FORMAT_STEREO_PCM16);
        memset(mix, 0, sizeof mix);
        mix[0] = mix[1] = 1.0f;
        ndspChnSetMix(SYN_CH, mix);
    }
    g_ok = 1;
    g_run = 1;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    /* its own core: core 2 (New 3DS), else the system core 1 (with some of
     * its time given to the app), else the game's core */
    g_thr = threadCreate(audio_thread, NULL, 32 * 1024, prio - 3, 2, false);
    if (g_thr) {
        plat_log("audio: on (thread on core 2)\n");
    } else {
        APT_SetAppCpuTimeLimit(30);
        g_thr = threadCreate(audio_thread, NULL, 32 * 1024, prio - 3, 1, false);
        if (g_thr)
            plat_log("audio: on (thread on core 1)\n");
        else {
            g_thr = threadCreate(audio_thread, NULL, 32 * 1024, prio - 3, -2, false);
            plat_log("audio: on (thread on the game's core)\n");
        }
    }
    if (!g_thr) {
        g_ok = 0;
        ndspExit();
        return -1;
    }
    return 0;
}

void plat_audio_exit(void)
{
    if (!g_ok)
        return;
    g_run = 0;
    threadJoin(g_thr, U64_MAX);
    threadFree(g_thr);
    ndspExit();
    g_ok = 0;
}

int plat_stream_new(void)
{
    int c;
    if (!g_ok)
        return -1;
    for (c = 0; c < NSTREAM; c++) {
        Stream *s = &S[c];
        if (s->used)
            continue;
        LightLock_Lock(&s->lock);
        s->used = 1;
        s->nch = 1;
        s->rate = 44100;
        s->playing = 0;
        s->rd = s->wr = 0;
        s->gl = s->gr = 1.0f;
        s->fmt_dirty = 1;
        s->gain_dirty = 1;
        s->clear_req = 1;
        LightLock_Unlock(&s->lock);
        return c;
    }
    return -1;
}

void plat_stream_free(int c)
{
    Stream *s;
    if (c < 0 || c >= NSTREAM)
        return;
    s = &S[c];
    LightLock_Lock(&s->lock);
    s->playing = 0;
    s->rd = s->wr = 0;
    s->clear_req = 1;
    LightLock_Unlock(&s->lock);
    /* the audio thread clears the DSP side, then the slot can be reused */
    stream_service(c);
    s->used = 0;
}

void plat_stream_format(int c, int nch, int rate)
{
    Stream *s;
    if (c < 0 || c >= NSTREAM)
        return;
    s = &S[c];
    if (nch < 1) nch = 1;
    if (nch > 2) nch = 2;
    if (rate < 1000) rate = 1000;
    if (rate > 96000) rate = 96000;
    LightLock_Lock(&s->lock);
    if (s->nch != nch || s->rate != rate) {
        s->nch = nch;
        s->rate = rate;
        s->fmt_dirty = 1;
    }
    LightLock_Unlock(&s->lock);
}

void plat_stream_gain(int c, float left, float right)
{
    Stream *s;
    if (c < 0 || c >= NSTREAM)
        return;
    s = &S[c];
    LightLock_Lock(&s->lock);
    if (s->gl != left || s->gr != right) {
        s->gl = left;
        s->gr = right;
        s->gain_dirty = 1;
    }
    LightLock_Unlock(&s->lock);
}

void plat_stream_play(int c, int on)
{
    Stream *s;
    if (c < 0 || c >= NSTREAM)
        return;
    s = &S[c];
    LightLock_Lock(&s->lock);
    if (!on && s->playing) {
        /* stopping drops what is queued (the PS2 resets its IOP buffers) */
        s->rd = s->wr;
        s->clear_req = 1;
    }
    s->playing = on;
    LightLock_Unlock(&s->lock);
}

unsigned plat_stream_room(int c)
{
    unsigned used;
    Stream *s;
    if (c < 0 || c >= NSTREAM)
        return 0;
    s = &S[c];
    LightLock_Lock(&s->lock);
    used = (s->wr - s->rd) + dsp_frames(s);
    LightLock_Unlock(&s->lock);
    return used >= CAP_FRAMES ? 0 : CAP_FRAMES - used;
}

unsigned plat_stream_queued(int c)
{
    unsigned n;
    Stream *s;
    if (c < 0 || c >= NSTREAM)
        return 0;
    s = &S[c];
    LightLock_Lock(&s->lock);
    n = (s->wr - s->rd) + (s->clear_req ? 0 : dsp_frames(s));
    LightLock_Unlock(&s->lock);
    return n;
}

void plat_stream_push(int c, const short *l, const short *r, unsigned n)
{
    Stream *s;
    unsigned i;
    if (c < 0 || c >= NSTREAM || !n)
        return;
    s = &S[c];
    LightLock_Lock(&s->lock);
    if (n > RING_FRAMES - (s->wr - s->rd))
        n = RING_FRAMES - (s->wr - s->rd);
    for (i = 0; i < n; i++) {
        short *f = s->ring + (((s->wr + i) & (RING_FRAMES - 1)) << 1);
        f[0] = l[i];
        f[1] = r ? r[i] : l[i];
    }
    s->wr += n;
    LightLock_Unlock(&s->lock);
}

unsigned plat_stream_drop(int c, unsigned n)
{
    Stream *s;
    unsigned have;
    if (c < 0 || c >= NSTREAM)
        return 0;
    s = &S[c];
    LightLock_Lock(&s->lock);
    have = s->wr - s->rd;
    if (n > have)
        n = have;
    s->rd += n;
    LightLock_Unlock(&s->lock);
    return n;
}

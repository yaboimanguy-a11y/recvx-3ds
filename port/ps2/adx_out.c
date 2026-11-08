/* adx_out.c: the ADX player's audio renderer (ADXRNA_*), in place of
 * adx_rnap.c / ps2_rna.c.
 *
 * On the PS2 the CRI library decodes ADX on the EE into one stream joint
 * (SJ) per channel; PS2RNA then copied the PCM over SIF DMA into a 16 KB
 * buffer per channel on the IOP, which played it.  ADXT watches that
 * buffer: how much is queued (GetNumData) and how much room is left
 * (GetNumRoom) decide when a stream starts and when it has finished.
 *
 * Here the ADX server (60 Hz, adx_mps2.c's thread) moves the decoded PCM
 * straight into a platform sound stream (plat_stream_*), which the
 * platform's own audio thread plays; queued / room count that stream. */
#include "cri/mwlib/ee/lib/libadxe/adx_rnap.h"
#include "plat.h"

#include <math.h>
#include <string.h>

#define MAX_RNA 8

typedef struct {
    int used;
    int maxnch, nch;
    SJ sjo[2];
    int trnsw, plysw;
    int sfreq, vol, pan[2], bps;
    int stream;
    /* no sound (no DSP firmware): the PCM is used up at the playing rate,
     * so ADXT's start / end timing still works */
    unsigned vq;
    unsigned vlast;
} RnaObj;

#define NOSND_CAP 8192u

static unsigned q_frames(RnaObj *r)
{
    if (r->stream >= 0)
        return plat_stream_queued(r->stream);
    if (r->plysw) {
        unsigned now = plat_ms(), used = (unsigned)((unsigned long long)(now - r->vlast) * (unsigned)r->sfreq / 1000u);
        if (used) {
            r->vq = used >= r->vq ? 0 : r->vq - used;
            r->vlast = now;
        }
    } else {
        r->vlast = plat_ms();
    }
    return r->vq;
}

static unsigned room_frames(RnaObj *r)
{
    unsigned q;
    if (r->stream >= 0)
        return plat_stream_room(r->stream);
    q = q_frames(r);
    return q >= NOSND_CAP ? 0 : NOSND_CAP - q;
}

static RnaObj rna_obj[MAX_RNA];
static unsigned rna_pushed;             /* frames, for the log */

static void set_gain(RnaObj *r)
{
    /* volume in 0.1 dB (0 .. -999); pan -15 (left) .. 15 (right) */
    float g = r->vol <= -999 ? 0.0f : powf(10.0f, (float)r->vol / 200.0f);
    float gl = g, gr = g;
    if (r->nch == 1) {
        int p = r->pan[0];
        if (p > 0)
            gl = g * (float)(15 - p) / 15.0f;
        else if (p < 0)
            gr = g * (float)(15 + p) / 15.0f;
    }
    plat_stream_gain(r->stream, gl, gr);
}

void ADXRNA_Init(void) {}
void ADXRNA_Finish(void) {}

ADXRNA ADXRNA_Create(SJ *sj, Sint32 maxnch)
{
    int i;
    for (i = 0; i < MAX_RNA; i++) {
        RnaObj *r = &rna_obj[i];
        if (r->used)
            continue;
        memset(r, 0, sizeof *r);
        r->maxnch = r->nch = maxnch > 2 ? 2 : (maxnch < 1 ? 1 : maxnch);
        r->sjo[0] = sj[0];
        r->sjo[1] = r->maxnch > 1 ? sj[1] : NULL;
        r->sfreq = 44100;
        r->bps = 16;
        r->stream = plat_stream_new();
        r->used = 1;
        plat_stream_format(r->stream, r->nch, r->sfreq);
        set_gain(r);
        return (ADXRNA)r;
    }
    return NULL;
}

void ADXRNA_Destroy(ADXRNA rna)
{
    RnaObj *r = rna;
    if (!r)
        return;
    plat_stream_play(r->stream, 0);
    plat_stream_free(r->stream);
    r->used = 0;
}

void ADXRNA_Start(ADXRNA rna) { (void)rna; }

void ADXRNA_Stop(ADXRNA rna)
{
    ADXRNA_SetTransSw(rna, 0);
    ADXRNA_SetPlaySw(rna, 0);
}

void ADXRNA_SetTransSw(ADXRNA rna, Sint32 sw)
{
    RnaObj *r = rna;
    if (r)
        r->trnsw = sw;
}

void ADXRNA_SetPlaySw(ADXRNA rna, Sint32 sw)
{
    RnaObj *r = rna;
    if (!r)
        return;
    if (r->stream < 0) {
        if (!sw)
            r->vq = 0;
        else if (!r->plysw)
            r->vlast = plat_ms();
    }
    r->plysw = sw;
    plat_stream_play(r->stream, sw);
}

void ADXRNA_GetTime(ADXRNA rna, Sint32 *ncount, Sint32 *tscale)
{
    (void)rna;
    *ncount = 0;                        /* as PS2RNA: ADXT times by vsyncs */
    *tscale = 48000;
}

Sint32 ADXRNA_GetNumData(ADXRNA rna)
{
    RnaObj *r = rna;
    return r ? (Sint32)q_frames(r) : 0;
}

Sint32 ADXRNA_GetNumRoom(ADXRNA rna)
{
    RnaObj *r = rna;
    return r ? (Sint32)room_frames(r) : 0;
}

Sint32 ADXRNA_GetStat(ADXRNA rna) { (void)rna; return 0; }

/* the decoded PCM, decoder SJs -> platform stream (while transfer is on) */
static void move_pcm(RnaObj *r)
{
    int ch, nch = r->nch > r->maxnch ? r->maxnch : r->nch;
    unsigned n = room_frames(r);
    for (ch = 0; ch < nch; ch++) {
        unsigned have = (unsigned)SJ_GetNumData(r->sjo[ch], 1) / 2u;
        if (have < n)
            n = have;
    }
    while (n > 0) {
        SJCK ck[2];
        unsigned m = n;
        for (ch = 0; ch < nch; ch++) {
            SJ_GetChunk(r->sjo[ch], 1, (Sint32)(m * 2), &ck[ch]);
            if ((unsigned)ck[ch].len / 2u < m)
                m = (unsigned)ck[ch].len / 2u;
        }
        /* the channels' rings can wrap at different places: keep the same
         * number of samples from each, give the rest back */
        for (ch = 0; ch < nch; ch++) {
            if ((unsigned)ck[ch].len > m * 2) {
                SJCK a, b;
                SJ_SplitChunk(&ck[ch], (Sint32)(m * 2), &a, &b);
                SJ_UngetChunk(r->sjo[ch], 1, &b);
                ck[ch] = a;
            }
        }
        if (m == 0) {
            for (ch = 0; ch < nch; ch++)
                SJ_UngetChunk(r->sjo[ch], 1, &ck[ch]);
            break;
        }
        if (r->stream >= 0)
            plat_stream_push(r->stream, (const short *)ck[0].data,
                             nch > 1 ? (const short *)ck[1].data : NULL, m);
        else
            r->vq += m;
        rna_pushed += m;
        for (ch = 0; ch < nch; ch++)
            SJ_PutChunk(r->sjo[ch], 0, &ck[ch]);
        n -= m;
    }
}

void ADXRNA_ExecServer(void)
{
    int i;
    for (i = 0; i < MAX_RNA; i++) {
        RnaObj *r = &rna_obj[i];
        if (r->used && r->trnsw)
            move_pcm(r);
    }
}

unsigned adx_out_frames(void)
{
    unsigned n = rna_pushed;
    rna_pushed = 0;
    return n;
}

void ADXRNA_SetStartSmpl(ADXRNA rna, Sint32 nsmpl) { (void)rna; (void)nsmpl; }

void ADXRNA_SetNumChan(ADXRNA rna, Sint32 nch)
{
    RnaObj *r = rna;
    if (!r)
        return;
    r->nch = nch > 2 ? 2 : (nch < 1 ? 1 : nch);
    plat_stream_format(r->stream, r->nch, r->sfreq);
    set_gain(r);
}

void ADXRNA_SetSfreq(ADXRNA rna, Sint32 sfreq)
{
    RnaObj *r = rna;
    if (!r)
        return;
    r->sfreq = sfreq;
    plat_stream_format(r->stream, r->nch, r->sfreq);
}

void ADXRNA_SetOutVol(ADXRNA rna, Sint32 vol)
{
    RnaObj *r = rna;
    if (!r)
        return;
    if (vol > 0) vol = 0;
    if (vol < -999) vol = -999;
    r->vol = vol;
    set_gain(r);
}

void ADXRNA_SetOutPan(ADXRNA rna, Sint32 chno, Sint32 pan)
{
    RnaObj *r = rna;
    if (!r || chno < 0 || chno > 1)
        return;
    if (pan < -15) pan = -15;
    if (pan > 15) pan = 15;
    r->pan[chno] = pan;
    set_gain(r);
}

void ADXRNA_SetBitPerSmpl(ADXRNA rna, Sint32 bps)
{
    RnaObj *r = rna;
    if (r)
        r->bps = bps;
}

Sint32 ADXRNA_GetSfreq(ADXRNA rna) { RnaObj *r = rna; return r ? r->sfreq : 0; }
Sint32 ADXRNA_GetOutVol(ADXRNA rna) { RnaObj *r = rna; return r ? r->vol : 0; }
Sint32 ADXRNA_GetOutPan(ADXRNA rna, Sint32 chno) { RnaObj *r = rna; return (r && chno >= 0 && chno < 2) ? r->pan[chno] : 0; }
Sint32 ADXRNA_GetBitPerSmpl(ADXRNA rna) { RnaObj *r = rna; return r ? r->bps : 16; }

Sint32 ADXRNA_DiscardData(ADXRNA rna, Sint32 nsmpl)
{
    RnaObj *r = rna;
    if (!r || nsmpl <= 0)
        return 0;
    if (r->stream < 0) {
        unsigned n = (unsigned)nsmpl < r->vq ? (unsigned)nsmpl : r->vq;
        r->vq -= n;
        return (Sint32)n;
    }
    return (Sint32)plat_stream_drop(r->stream, (unsigned)nsmpl);
}

void ADXRNA_ClearBuf(ADXRNA rna) { (void)rna; }
void ADXRNA_SetPcmType(ADXRNA rna, Sint32 type) { (void)rna; (void)type; }
void ADXRNA_SetTotalNumSmpl(ADXRNA rna, Sint32 nsmpl) { (void)rna; (void)nsmpl; }
void ADXRNA_SetWavFname(ADXRNA rna, const Sint8 *fname) { (void)rna; (void)fname; }
Sint32 ADXRNA_SetStmHdInfo(ADXRNA rna, void *snddat) { (void)rna; (void)snddat; return 0; }

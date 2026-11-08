/* snd_synth.c: the game's sound effects and MIDI sequences, in place of the
 * IOP sound driver TSNDDRV (Tamsoft, on Sony's modhsyn / modmidi).
 *
 * What the game (ps2_sg_sd.c, ps2_snddrv.c) downloads, each kind numbered
 * on its own:
 *  - effect ("shot") banks 0..4: an HD (Sony "SCEI" instruments: program ->
 *    split -> sample set -> sample -> VAG) by SdrHDDataSet2(n), its samples
 *    (PS ADPCM) by SdrBDDataTrans(SE_BANK[n] = 0, 4, 5, 6, 7);
 *  - MIDI instrument banks 0..3: SdrHDDataSet(n), samples by
 *    SdrBDDataTrans(MIDI_BANK[n] = n);
 *  - MIDI sequence banks 0..3: SdrSQDataSet(n), MIDI blocks.
 * Requests: an effect is SdrSeReq(bank << 16 | effect << 8 | slot), from
 * sdShotPlay: program `effect` of that effect bank, at its sample's own
 * pitch, bent by `pitch` (8192 = none), in voice slot `slot`.  A sequence
 * is SdrBgmReq(port, bank, vol, block): MIDI block `block` of sequence bank
 * `bank` on instrument bank `bank`, looping (ambient sounds).
 *
 * Here: hooks at the top of the game's Sdr* functions (hostify) keep copies
 * of the downloads and queue the requests; snd_render, on the platform's
 * audio thread, plays them and mixes one 32 kHz stereo stream. */
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "plat.h"

void *iop_ptr(unsigned a);
extern int iop_data_buff;

#define NTRACK 16
#define NVOICE 40
#define RATE 32000
#define BD_DBUF 204800          /* the second half of the download buffer (ps2_sg_sd.c) */

typedef struct { uint8_t *d; unsigned n, cap; } Blob;

/* an instrument set: an HD and the samples it plays, decoded on first use */
typedef struct {
    Blob hd;
    int bd;
    short **pcm;
    unsigned *len, *loop, nvag;
} Inst;

#define NMIDI 4
#define NSHOT 5
static Inst inst[NMIDI + NSHOT];            /* 0..3 MIDI banks, 4..8 effect banks */
static Blob sq[NMIDI];
static Blob bdat[8];
static const int shot_bd[NSHOT] = { 0, 4, 5, 6, 7 };
static PlatSem *g_lock;

static void lock(void) { plat_sem_wait(g_lock); }
static void unlock(void) { plat_sem_post(g_lock); }

static uint32_t rd32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static unsigned rd16(const uint8_t *p) { return p[0] | (p[1] << 8); }

/* a chunk ("IECSxxxx") by its tag, or NULL */
static const uint8_t *find_chunk(const uint8_t *d, unsigned size, const char tag[8])
{
    unsigned off = 0;
    while (off + 16 <= size) {
        unsigned len = rd32(d + off + 8);
        if (memcmp(d + off, tag, 8) == 0)
            return d + off;
        if (len < 16 || off + len > size)
            break;
        off += len;
    }
    return NULL;
}

/* ---- voices (declared early: dropping samples stops the voices using them) */
typedef struct {
    int active;
    int inst;                   /* whose samples */
    int track;                  /* MIDI: owner track, chan, note; -1 for an effect */
    int chan, note;
    int se_bank, se_slot;       /* effect: its bank and slot */
    const short *pcm;
    unsigned len, loop;
    uint32_t pos, step;         /* 16.16 */
    float gl, gr, base_vol;
    float env, rel;
    int released;
    float rate, pitch, bend_range;
} Voice;
static Voice voice[NVOICE];

static void drop_pcm(Inst *in)
{
    unsigned i;
    int k = (int)(in - inst);
    for (i = 0; i < NVOICE; i++)
        if (voice[i].active && voice[i].inst == k)
            voice[i].active = 0;
    for (i = 0; i < in->nvag; i++)
        free(in->pcm[i]);
    free(in->pcm);
    free(in->len);
    free(in->loop);
    in->pcm = NULL;
    in->len = in->loop = NULL;
    in->nvag = 0;
}

/* HD: Head chunk: header size, body size, then the Prog / Sset / Smpl / Vagi
 * chunks' offsets (from the HD's start) */
static const uint8_t *hd_part(const Inst *in, int which, unsigned *count)
{
    const uint8_t *h, *c;
    unsigned off;
    if (!in->hd.d)
        return NULL;
    h = find_chunk(in->hd.d, in->hd.n, "IECSdaeH");
    if (!h)
        return NULL;
    off = rd32(h + 20 + which * 4);        /* 0 prog, 1 sset, 2 smpl, 3 vagi */
    if (off == 0xFFFFFFFFu || off + 16 > in->hd.n)
        return NULL;
    c = in->hd.d + off;
    *count = rd32(c + 12) + 1;
    return c;
}

static const uint8_t *hd_entry(const Inst *in, int which, unsigned i)
{
    unsigned n;
    const uint8_t *c = hd_part(in, which, &n);
    uint32_t o;
    if (!c || i >= n)
        return NULL;
    o = rd32(c + 16 + i * 4);
    if (o == 0xFFFFFFFFu || (unsigned)(c - in->hd.d) + o >= in->hd.n)
        return NULL;
    return c + o;
}

/* PS ADPCM (VAG) -> 16-bit PCM up to the end flag; loop start noted */
static void decode_vag(Inst *in, unsigned v)
{
    static const int f0[5] = { 0, 60, 115, 98, 122 }, f1[5] = { 0, 0, -52, -55, -60 };
    const Blob *bd = &bdat[in->bd];
    const uint8_t *e = hd_entry(in, 3, v), *src;
    unsigned off, max, n = 0, blocks, k, loop = ~0u;
    int h1 = 0, h2 = 0;
    short *out;
    if (!e)
        return;
    off = rd32(e);
    if (off >= bd->n)
        return;
    src = bd->d + off;
    max = (bd->n - off) / 16;
    for (blocks = 0; blocks < max; blocks++)
        if (src[blocks * 16 + 1] & 1) {
            blocks++;
            break;
        }
    if (!blocks)
        return;
    out = malloc((size_t)blocks * 28 * sizeof(short));
    if (!out)
        return;
    for (k = 0; k < blocks; k++) {
        const uint8_t *p = src + k * 16;
        int shift = p[0] & 15, filt = (p[0] >> 4) % 5, i;
        if (p[1] & 4)
            loop = n;
        if (shift > 12)
            shift = 9;
        for (i = 0; i < 28; i++) {
            int nib = (p[2 + i / 2] >> ((i & 1) * 4)) & 15, s;
            s = (int)(int16_t)(nib << 12) >> shift;
            s += (h1 * f0[filt] + h2 * f1[filt] + 32) >> 6;
            if (s > 32767) s = 32767;
            if (s < -32768) s = -32768;
            h2 = h1;
            h1 = s;
            out[n++] = (short)s;
        }
    }
    /* it loops only when its last block says so */
    if ((src[(blocks - 1) * 16 + 1] & 3) != 3)
        loop = ~0u;
    else if (loop == ~0u)
        loop = 0;
    in->pcm[v] = out;
    in->len[v] = n;
    in->loop[v] = loop;
}

static const short *sample_pcm(Inst *in, unsigned v, unsigned *len, unsigned *loop)
{
    if (!in->pcm) {
        unsigned n = 0;
        if (!hd_part(in, 3, &n) || !n)
            return NULL;
        in->pcm = calloc(n, sizeof *in->pcm);
        in->len = calloc(n, sizeof *in->len);
        in->loop = calloc(n, sizeof *in->loop);
        if (!in->pcm || !in->len || !in->loop) {
            drop_pcm(in);
            return NULL;
        }
        in->nvag = n;
    }
    if (v >= in->nvag)
        return NULL;
    if (!in->pcm[v])
        decode_vag(in, v);
    *len = in->len[v];
    *loop = in->loop[v];
    return in->pcm[v];
}

/* ---- MIDI tracks ---------------------------------------------------------- */
typedef struct {
    int program, vol, expr, pan, bend;
} Chan;

typedef struct {
    int active;
    int port, inst;
    const uint8_t *start, *p, *end, *loop;
    unsigned res;
    double us_per_tick, wait_us;
    int status;
    Chan ch[16];
    float vol;
} Track;
static Track track[NTRACK];

static float master = 1.0f;
static volatile unsigned short se_busy[8];   /* the driver's status: effect slots playing per bank */
static volatile unsigned short midi_busy;    /* sequence ports playing */
static volatile unsigned short se_pending[8]; /* requested, not yet started by the audio thread */
static unsigned st_req, st_play, st_miss;

static inline uint32_t step_of(float rate, float semis)
{
    return (uint32_t)(rate * powf(2.0f, semis / 12.0f) / RATE * 65536.0f);
}

static void set_pan(Voice *v, int pan, float vol)
{
    float l = pan <= 64 ? 1.0f : (127 - pan) / 63.0f, r = pan >= 64 ? 1.0f : pan / 64.0f;
    v->gl = vol * l;
    v->gr = vol * r;
}

/* program -> split (by note; note < 0: an effect, the first split at its
 * sample's own pitch) -> sample set -> sample (by velocity); starts a voice.
 * bend01: -1..1 of the split's bend range; pan 0..127 */
static Voice *play_note(int k, int program, int note, int vel, float vol, int pan, float bend01)
{
    Inst *in = &inst[k];
    const uint8_t *prog = hd_entry(in, 0, (unsigned)program), *split = NULL, *sset, *smpl = NULL, *vag;
    unsigned nsplit, ssz, i, ns, len = 0, loop = 0, vidx;
    const short *pcm;
    int best = -1, transpose, p;
    float detune, range;
    Voice *v;
    if (!prog)
        return NULL;
    nsplit = prog[4];
    ssz = prog[5];
    if (note < 0 && nsplit) {
        split = prog + rd32(prog);
    } else {
        for (i = 0; i < nsplit; i++) {
            const uint8_t *s = prog + rd32(prog) + i * ssz;
            if (note >= s[2] && note <= s[4]) {
                split = s;
                break;
            }
        }
    }
    if (!split)
        return NULL;
    sset = hd_entry(in, 1, rd16(split));
    if (!sset)
        return NULL;
    ns = sset[3];
    for (i = 0; i < ns; i++) {
        const uint8_t *sm = hd_entry(in, 2, rd16(sset + 4 + i * 2));
        if (!sm)
            continue;
        if (!smpl)
            smpl = sm;
        if (vel >= sm[2] && vel <= sm[4]) {
            smpl = sm;
            break;
        }
    }
    if (!smpl)
        return NULL;
    vidx = rd16(smpl);
    pcm = sample_pcm(in, vidx, &len, &loop);
    if (!pcm || !len)
        return NULL;
    if (note < 0)
        note = smpl[11];
    for (i = 0; i < NVOICE; i++)
        if (!voice[i].active) {
            best = (int)i;
            break;
        }
    if (best < 0) {                     /* steal the quietest */
        float q = 1e9f;
        for (i = 0; i < NVOICE; i++)
            if (voice[i].env * (voice[i].gl + voice[i].gr) < q) {
                q = voice[i].env * (voice[i].gl + voice[i].gr);
                best = (int)i;
            }
    }
    v = &voice[best];
    vag = hd_entry(in, 3, vidx);
    memset(v, 0, sizeof *v);
    v->active = 1;
    v->inst = k;
    v->track = -1;
    v->se_bank = v->se_slot = -1;
    v->note = note;
    v->pcm = pcm;
    v->len = len;
    v->loop = loop;
    v->rate = vag ? (float)rd16(vag + 4) : 32000.0f;
    transpose = (int8_t)prog[10] + (int8_t)split[18];
    detune = ((int8_t)prog[11] + (int8_t)split[19] + (int8_t)smpl[12]) / 128.0f;
    v->pitch = (float)(note - smpl[11] + transpose) + detune;
    range = (float)(rd16(split + 8) >> 8);
    v->bend_range = range > 0.0f ? range : 2.0f;
    v->step = step_of(v->rate, v->pitch + bend01 * v->bend_range);
    v->env = 1.0f;
    v->rel = expf(-1.0f / (RATE * (0.004f * powf(2.0f, (rd16(smpl + 20) & 31) / 3.0f))));
    v->base_vol = (vel / 127.0f) * (smpl[16] / 127.0f) * (prog[6] / 127.0f) * (split[16] / 127.0f) * 0.5f;
    p = pan + (smpl[13] - 64) + (prog[7] - 64) + (split[17] - 64);
    set_pan(v, p < 0 ? 0 : (p > 127 ? 127 : p), v->base_vol * vol * master);
    return v;
}

static unsigned vlq(const uint8_t **pp, const uint8_t *end)
{
    unsigned v = 0;
    while (*pp < end) {
        uint8_t c = *(*pp)++;
        v = (v << 7) | (c & 0x7F);
        if (!(c & 0x80))
            break;
    }
    return v;
}

static void release_track(int t, int chan, int note)
{
    int i;
    for (i = 0; i < NVOICE; i++)
        if (voice[i].active && voice[i].track == t && (chan < 0 || voice[i].chan == chan) &&
            (note < 0 || voice[i].note == note))
            voice[i].released = 1;
}

static void track_stop(int t)
{
    if (!track[t].active)
        return;
    release_track(t, -1, -1);
    track[t].active = 0;
}

/* the events due now; 0 at the end of the block */
static int track_run(int t)
{
    Track *tr = &track[t];
    while (tr->wait_us <= 0.0) {
        uint8_t st, d1, d2;
        int c;
        if (tr->p >= tr->end)
            return 0;
        if (*tr->p & 0x80)
            tr->status = *tr->p++;
        st = (uint8_t)tr->status;
        c = st & 15;
        switch (st & 0xF0) {
        case 0x80: case 0x90:
            d1 = *tr->p++;
            d2 = 0;
            if (d1 & 0x80)              /* Sony: a note with the top bit has no velocity byte */
                d1 &= 0x7F;
            else
                d2 = *tr->p++;
            if ((st & 0xF0) == 0x90 && d2) {
                Chan *ch = &tr->ch[c];
                Voice *v = play_note(tr->inst, ch->program, d1, d2,
                                     (ch->vol / 127.0f) * (ch->expr / 127.0f) * tr->vol, ch->pan,
                                     ch->bend / 8192.0f);
                if (v) {
                    v->track = t;
                    v->chan = c;
                }
            } else {
                release_track(t, c, d1);
            }
            break;
        case 0xA0:
            tr->p += 2;
            break;
        case 0xB0:
            d1 = *tr->p++;
            d2 = *tr->p++;
            if (d1 == 7) tr->ch[c].vol = d2;
            else if (d1 == 10) tr->ch[c].pan = d2;
            else if (d1 == 11) tr->ch[c].expr = d2;
            else if (d1 == 99 && d2 == 20) tr->loop = tr->p;               /* loop start */
            else if (d1 == 99 && d2 == 30 && tr->loop) tr->p = tr->loop;   /* loop end */
            else if (d1 == 120 || d1 == 123) release_track(t, c, -1);
            break;
        case 0xC0:
            tr->ch[c].program = *tr->p++;
            break;
        case 0xD0:
            tr->p++;
            break;
        case 0xE0:
            d1 = *tr->p++;
            d2 = *tr->p++;
            tr->ch[c].bend = ((d2 << 7) | d1) - 8192;
            {
                int i;
                for (i = 0; i < NVOICE; i++)
                    if (voice[i].active && voice[i].track == t && voice[i].chan == c)
                        voice[i].step = step_of(voice[i].rate,
                                                voice[i].pitch + tr->ch[c].bend / 8192.0f * voice[i].bend_range);
            }
            break;
        case 0xF0:
            if (st == 0xFF) {
                uint8_t type = *tr->p++;
                unsigned len = vlq(&tr->p, tr->end);
                if (type == 0x2F)
                    return 0;
                if (type == 0x51 && len == 3) {
                    unsigned us = (tr->p[0] << 16) | (tr->p[1] << 8) | tr->p[2];
                    tr->us_per_tick = (double)us / (tr->res ? tr->res : 480);
                }
                tr->p += len;
            } else {
                unsigned len = vlq(&tr->p, tr->end);
                tr->p += len;
            }
            break;
        }
        if (tr->p >= tr->end)
            return 0;
        tr->wait_us += vlq(&tr->p, tr->end) * tr->us_per_tick;
    }
    return 1;
}

static int track_start(int port, int bank, int block, float vol)
{
    const Blob *b;
    const uint8_t *m, *blk;
    unsigned n, off, i, t, end;
    int k;
    if (bank < 0 || bank >= NMIDI)
        return -1;
    b = &sq[bank];
    if (!b->d)
        return -1;
    m = find_chunk(b->d, b->n, "IECSidiM");
    if (!m)
        return -1;
    n = rd32(m + 12) + 1;
    if (block < 0 || (unsigned)block >= n)
        return -1;
    off = rd32(m + 16 + block * 4);
    if (off == 0xFFFFFFFFu || (unsigned)(m - b->d) + off + 6 > b->n)
        return -1;
    blk = m + off;
    end = rd32(m + 8);
    for (i = 0; i < n; i++) {
        unsigned o = rd32(m + 16 + i * 4);
        if (o != 0xFFFFFFFFu && o > off && o < end)
            end = o;
    }
    for (t = 0; t < NTRACK; t++)
        if (!track[t].active)
            break;
    if (t == NTRACK)
        return -1;
    /* its instruments: the MIDI bank of that number, else effect bank 0 */
    k = inst[bank].hd.d ? bank : NMIDI;
    memset(&track[t], 0, sizeof track[t]);
    track[t].active = 1;
    track[t].port = port;
    track[t].inst = k;
    track[t].start = blk + rd32(blk);
    track[t].p = track[t].start;
    track[t].end = m + end;
    track[t].res = rd16(blk + 4);
    track[t].us_per_tick = 500000.0 / (track[t].res ? track[t].res : 480);
    track[t].vol = vol;
    for (i = 0; i < 16; i++) {
        track[t].ch[i].vol = 100;
        track[t].ch[i].expr = 127;
        track[t].ch[i].pan = 64;
    }
    track[t].wait_us = vlq(&track[t].p, track[t].end) * track[t].us_per_tick;
    return (int)t;
}

/* ---- effects ------------------------------------------------------------------ */
static void effect_stop(int bank, int slot)
{
    int i;
    for (i = 0; i < NVOICE; i++)
        if (voice[i].active && voice[i].se_bank == bank && (slot < 0 || voice[i].se_slot == slot))
            voice[i].released = 1;
}

static float se_vol_of(int vol) { return vol < 0 ? 1.0f : vol / 127.0f; }

static void effect_play(int bank, int effect, int slot, int vol, int pan, int pitch)
{
    Voice *v;
    effect_stop(bank, slot);            /* a slot plays one effect at a time */
    if (bank < 0 || bank >= NSHOT || !inst[NMIDI + bank].hd.d) {
        st_miss++;
        return;
    }
    v = play_note(NMIDI + bank, effect, -1, 127, se_vol_of(vol), pan >= 0 ? pan : 64,
                  pitch >= 0 ? (pitch - 8192) / 8192.0f : 0.0f);
    if (!v) {
        st_miss++;
        return;
    }
    v->se_bank = bank;
    v->se_slot = slot;
    st_play++;
}

static void effect_change(int bank, int slot, int vol, int pan, int pitch)
{
    int i;
    for (i = 0; i < NVOICE; i++) {
        Voice *v = &voice[i];
        if (!v->active || v->se_bank != bank || v->se_slot != slot)
            continue;
        if (vol >= 0 || pan >= 0) {
            float g = v->base_vol * se_vol_of(vol) * master;
            set_pan(v, pan >= 0 ? pan : 64, g);
        }
        if (pitch >= 0)
            v->step = step_of(v->rate, v->pitch + (pitch - 8192) / 8192.0f * v->bend_range);
    }
}

/* ---- requests from the game thread, run by the audio thread ----------------- */
enum { Q_SE, Q_SE_CHG, Q_SE_CANCEL, Q_SE_ALLSTOP, Q_BGM, Q_BGM_STOP, Q_MVOL };
typedef struct { int op, a, b, c, vol, pan, pitch; } Req;
#define NQ 256
static Req q[NQ];
static volatile unsigned q_w, q_r;

static void push(Req r)
{
    if (q_w - q_r >= NQ)
        return;
    q[q_w % NQ] = r;
    __sync_synchronize();
    q_w++;
}

static void apply(const Req *r)
{
    int t;
    switch (r->op) {
    case Q_SE:
        effect_play(r->a, r->b, r->c, r->vol, r->pan, r->pitch);
        if (r->a < 8 && r->c < 16)
            se_pending[r->a] &= (unsigned short)~(1 << r->c);
        break;
    case Q_SE_CHG:
        effect_change(r->a, r->c, r->vol, r->pan, r->pitch);
        break;
    case Q_SE_CANCEL:
        effect_stop(r->a, r->c);
        break;
    case Q_SE_ALLSTOP:
        for (t = 0; t < NSHOT; t++)
            effect_stop(t, -1);
        break;
    case Q_BGM:
        for (t = 0; t < NTRACK; t++)
            if (track[t].active && track[t].port == r->a)
                track_stop(t);
        track_start(r->a, r->b, r->c, r->vol / 127.0f);
        break;
    case Q_BGM_STOP:
        for (t = 0; t < NTRACK; t++)
            if (track[t].active && track[t].port == r->a)
                track_stop(t);
        break;
    case Q_MVOL:
        master = r->vol / 16383.0f;
        if (master > 1.0f)
            master = 1.0f;
        break;
    }
}

/* ---- the hooks (top of the game's Sdr* functions, see hostify.py) ------------- */
static int ready(void)
{
    if (!g_lock)
        g_lock = plat_sem_new(1);
    return g_lock != NULL;
}

static void blob_set(Blob *b, unsigned off, const void *src, unsigned size)
{
    if (off + size > b->cap) {
        unsigned cap = off + size + 16384;
        uint8_t *n = realloc(b->d, cap);
        if (!n)
            return;
        b->d = n;
        b->cap = cap;
    }
    memcpy(b->d + off, src, size);
    if (off + size > b->n)
        b->n = off + size;
}

static void hd_set(int k, int size)
{
    Inst *in = &inst[k];
    if (size <= 0 || size > 0x100000)
        return;
    lock();
    drop_pcm(in);
    in->hd.n = 0;
    blob_set(&in->hd, 0, iop_ptr((unsigned)iop_data_buff), (unsigned)size);
    unlock();
}

/* MIDI instrument bank n */
void snd_hook_hd(int bank, int size)
{
    if (!ready() || bank < 0 || bank >= NMIDI)
        return;
    inst[bank].bd = bank;
    hd_set(bank, size);
    plat_log("sound: MIDI bank %d instruments (%d bytes)\n", bank, size);
}

/* effect bank n */
void snd_hook_hd2(int bank, int size)
{
    if (!ready() || bank < 0 || bank >= NSHOT)
        return;
    inst[NMIDI + bank].bd = shot_bd[bank];
    hd_set(NMIDI + bank, size);
    plat_log("sound: effect bank %d instruments (%d bytes)\n", bank, size);
}

/* the end of a bank's samples (SdrBDDataSet / 2): nothing to do here */
void snd_hook_bd_done(int bank) { (void)bank; }

/* samples, in pieces: the piece at offset 0 starts a new set */
void snd_hook_bd_trans(int id, int adrs, int size, int flag)
{
    int k;
    if (!ready())
        return;
    id &= 0x7F;
    if (id >= 8 || size <= 0 || adrs < 0)
        return;
    lock();
    for (k = 0; k < NMIDI + NSHOT; k++)     /* decoded samples of this set are stale */
        if (inst[k].bd == id)
            drop_pcm(&inst[k]);
    if (adrs == 0)
        bdat[id].n = 0;
    blob_set(&bdat[id], (unsigned)adrs, iop_ptr((unsigned)iop_data_buff + (flag ? BD_DBUF : 0)), (unsigned)size);
    unlock();
}

/* MIDI sequence bank n */
void snd_hook_sq(int bank, int size)
{
    int t;
    if (!ready() || bank < 0 || bank >= NMIDI || size <= 0 || size > 0x100000)
        return;
    lock();
    for (t = 0; t < NTRACK; t++)            /* tracks point into the old data */
        if (track[t].active && sq[bank].d && track[t].start >= sq[bank].d &&
            track[t].start < sq[bank].d + sq[bank].n)
            track_stop(t);
    sq[bank].n = 0;
    blob_set(&sq[bank], 0, iop_ptr((unsigned)iop_data_buff), (unsigned)size);
    unlock();
    plat_log("sound: MIDI sequence bank %d (%d bytes)\n", bank, size);
}

void snd_hook_se(int req, int vol, int pan, int pitch)
{
    Req r = { Q_SE, (req >> 16) & 0xFF, (req >> 8) & 0xFF, req & 0xFF, vol, pan, pitch };
    static int logged;
    if (!ready())
        return;
    if (logged++ < 30)
        plat_log("sound: effect bank %d number %d slot %d vol %d pan %d pitch %d%s\n", r.a, r.b, r.c, vol, pan,
                 pitch, (r.a < NSHOT && inst[NMIDI + r.a].hd.d) ? "" : " (bank not loaded)");
    if (r.a < 8 && r.c < 16)
        se_pending[r.a] |= (unsigned short)(1 << r.c);
    push(r);
    st_req++;
}

void snd_hook_se_chg(int req, int vol, int pan, int pitch)
{
    Req r = { Q_SE_CHG, (req >> 16) & 0xFF, (req >> 8) & 0xFF, req & 0xFF, vol, pan, pitch };
    if (ready())
        push(r);
}

void snd_hook_se_cancel(int req)
{
    Req r = { Q_SE_CANCEL, (req >> 16) & 0xFF, (req >> 8) & 0xFF, req & 0xFF, -1, -1, -1 };
    if (ready())
        push(r);
}

void snd_hook_se_allstop(void)
{
    Req r = { Q_SE_ALLSTOP, 0, 0, 0, -1, -1, -1 };
    if (ready())
        push(r);
}

void snd_hook_bgm(int port, int bank, int vol, int block)
{
    Req r = { Q_BGM, port, bank, block, vol, -1, -1 };
    if (!ready())
        return;
    plat_log("sound: sequence port %d bank %d block %d vol %d\n", port, bank, block, vol);
    push(r);
}

void snd_hook_bgm_stop(int port)
{
    Req r = { Q_BGM_STOP, port, 0, 0, -1, -1, -1 };
    if (ready())
        push(r);
}

void snd_hook_mvol(int mvol)
{
    Req r = { Q_MVOL, 0, 0, 0, mvol, -1, -1 };
    if (ready())
        push(r);
}

/* the driver's status for the game (ps2_stubs.c) */
void snd_status_bits(unsigned short se[6], unsigned short *midi)
{
    int i;
    for (i = 0; i < 6; i++)
        se[i] = se_busy[i] | se_pending[i];
    *midi = midi_busy;
}

void snd_log_stats(void)
{
    int i, nv = 0;
    for (i = 0; i < NVOICE; i++)
        nv += voice[i].active;
    plat_log("[sound] effect requests %u, played %u, not found %u, voices now %d\n", st_req, st_play, st_miss, nv);
    st_req = st_play = st_miss = 0;
}

/* ---- the mixer (audio thread): n frames of 32 kHz stereo ---------------------- */
void snd_render(short *out, unsigned n)
{
    static float mix[2 * 256];
    unsigned done = 0;
    if (!g_lock) {
        memset(out, 0, n * 4);
        return;
    }
    lock();
    while (q_r != q_w) {
        apply(&q[q_r % NQ]);
        q_r++;
    }
    while (done < n) {
        unsigned m = n - done > 256 ? 256 : n - done, i, k;
        int t;
        unsigned short busy[8] = { 0 }, mb = 0;
        for (t = 0; t < NTRACK; t++) {
            if (!track[t].active)
                continue;
            if (!track_run(t)) {                /* sequences loop */
                track[t].p = track[t].start;
                track[t].wait_us = vlq(&track[t].p, track[t].end) * track[t].us_per_tick;
            }
            track[t].wait_us -= m * (1000000.0 / RATE);
            if (track[t].port < 16)
                mb |= (unsigned short)(1 << track[t].port);
        }
        memset(mix, 0, m * 2 * sizeof(float));
        for (k = 0; k < NVOICE; k++) {
            Voice *v = &voice[k];
            if (!v->active)
                continue;
            for (i = 0; i < m; i++) {
                unsigned idx = v->pos >> 16;
                float s;
                if (idx >= v->len) {
                    if (v->loop != ~0u && v->loop < v->len) {
                        v->pos = (v->loop << 16) | (v->pos & 0xFFFF);
                        idx = v->loop;
                    } else {
                        v->active = 0;
                        break;
                    }
                }
                s = v->pcm[idx] * v->env;
                mix[2 * i] += s * v->gl;
                mix[2 * i + 1] += s * v->gr;
                v->pos += v->step;
                if (v->released) {
                    v->env *= v->rel;
                    if (v->env < 0.001f) {
                        v->active = 0;
                        break;
                    }
                }
            }
            if (v->active && v->se_bank >= 0 && v->se_bank < 8 && v->se_slot >= 0 && v->se_slot < 16)
                busy[v->se_bank] |= (unsigned short)(1 << v->se_slot);
        }
        for (i = 0; i < 6; i++)
            se_busy[i] = busy[i];
        midi_busy = mb;
        for (i = 0; i < m * 2; i++) {
            float s = mix[i];
            out[done * 2 + i] = (short)(s > 32767.0f ? 32767 : (s < -32768.0f ? -32768 : s));
        }
        done += m;
    }
    unlock();
}

/* ps2_stubs.c: SIF / IOP calls, the VU0 macro library and other PS2 SDK
 * functions the game calls.  The IOP (sound driver, CRI streaming) is not
 * emulated: RPCs answer with zeroes and complete at once; iop_rpc() is the
 * hook where sound support plugs in. */
#include "types.h"
#include "plat.h"

volatile u_int ps2_hwreg[64];
char _gp[4];
unsigned char ps2_scratchpad[16384] __attribute__((aligned(64)));

/* ---- IOP memory and RPC -----------------------------------------------------
 * The IOP's 2 MB of RAM is emulated as iop_ram: IOP addresses (below
 * 0x200000) the game gets from the sound driver or the IOP heap are offsets
 * into it; DMA between EE and IOP copies through iop_ptr(). */
u_int ps2_stat_rpc, ps2_stat_cd;   /* counters for the heartbeat log */

#define IOP_RAM_SIZE 0x200000u
static unsigned char iop_ram[IOP_RAM_SIZE] __attribute__((aligned(64)));
static u_int iop_heap_top = 0x180000;

/* the sound driver's memory map (what TSNDDRV reports through state 18/19) */
#define IOP_SND_HEADERS 0x020000u
#define IOP_SND_SEQS 0x0C0000u
#define IOP_SND_DATA 0x140000u
#define IOP_SND_TABLE 0x1FF000u
#define IOP_SND_STATUS 0x1FF100u  /* the driver's status block (not in the data buffer) */

void *iop_ptr(u_int a)
{
    if (a < IOP_RAM_SIZE)
        return iop_ram + a;
    return (void *)(size_t)a;
}

static void iop_init(void)
{
    static int done;
    u_int *t;
    if (done)
        return;
    done = 1;
    t = (u_int *)(iop_ram + IOP_SND_TABLE);
    t[0] = IOP_SND_HEADERS;
    t[1] = IOP_SND_SEQS;
    t[2] = IOP_SND_DATA;
}

int sceSifInitIopHeap(void) { iop_init(); return 0; }

void *sceSifAllocIopHeap(unsigned int size)
{
    u_int a;
    iop_init();
    size = (size + 63) & ~63u;
    if (iop_heap_top + size > IOP_SND_TABLE) {
        plat_log("iop heap: out of memory (%u)\n", size);
        return NULL;
    }
    a = iop_heap_top;
    iop_heap_top += size;
    return (void *)(size_t)a;
}

int sceSifFreeIopHeap(void *addr) { (void)addr; return 0; }
int sceSifLoadModule(const char *name, int argl, const char *args) { (void)argl; (void)args; plat_log("iop: module %s\n", name); return 1; }
int sceSifRebootIop(const char *img) { (void)img; return 1; }
int sceSifSyncIop(void) { return 1; }
void sceSifInitRpc(unsigned int mode) { (void)mode; iop_init(); }

int sceSifBindRpc(sceSifClientData *cd, unsigned int request, unsigned int mode)
{
    (void)mode;
    cd->command = request;
    cd->serve = (void *)cd;     /* bound */
    return 0;
}

/* RPC servers on the IOP: the sound driver (TSNDDRV: 0 = commands,
 * 1 = state queries) and CRI's streaming (0x7D000000).  Sound itself is
 * produced by the port's mixer (when present); these keep the game's side
 * of the protocol happy. */
__attribute__((weak)) int iop_rpc(unsigned server, unsigned fno, void *send, int ssize,
                                  void *recv, int rsize)
{
    static int logged;
    u_int *r = recv;
    (void)send; (void)ssize;
    iop_init();
    if (recv && rsize > 0)
        memset(recv, 0, rsize);
    if (server == 1 && r && rsize >= 4) {
        switch (fno) {
        case 18: r[0] = IOP_SND_STATUS; return 0;    /* driver ready: its status block */
        case 19: r[0] = IOP_SND_TABLE; return 0;     /* address of the memory map */
        default: r[0] = 0; return 0;
        }
    }
    if (server == 0)
        return 0;                                    /* sound commands: queued */
    if (server == 0x7D000000u) {
        /* CRI's IOP side (DTX transport; SJRMT ring buffers, SJX, PS2RNA
         * PCM output).  Creates get a handle; the rest answer 0 until the
         * port's own ADX player replaces this layer. */
        static u_int next_handle = 0x1000;
        if (fno == 2) {                              /* DTX create */
            if (r && rsize >= 4)
                r[0] = next_handle += 0x10;
            return 0;
        }
        if (fno >= 1024) {
            u_int f = fno - 1024;
            if (f == 0 || f == 8 || f == 32 || f == 33 || f == 34) {   /* SJX / RNA / SJRMT create */
                if (r && rsize >= 4)
                    r[0] = next_handle += 0x10;
            }
            return 0;
        }
        return 0;
    }
    if (logged < 40) {
        plat_log("iop rpc %08X fn %u (send %d recv %d)\n", server, fno, ssize, rsize);
        logged++;
    }
    return 0;
}

int sceSifCallRpc(sceSifClientData *cd, unsigned int request, unsigned int mode, void *send,
                  int ssize, void *receive, int rsize, sceSifEndFunc end, void *param)
{
    (void)mode;
    ps2_stat_rpc++;
    iop_rpc(cd->command, request, send, ssize, receive, rsize);
    if (end)
        end(param);
    return 0;
}

int sceSifCheckStatRpc(void *cd) { (void)cd; return 0; }

unsigned int sceSifSetDma(sceSifDmaData *d, int n)
{
    int i;
    for (i = 0; i < n; i++)
        if (d[i].data && d[i].size)
            memmove(iop_ptr(d[i].addr), iop_ptr(d[i].data), d[i].size);
    return 1;
}

unsigned int isceSifSetDma(sceSifDmaData *d, int n) { return sceSifSetDma(d, n); }
int sceSifDmaStat(unsigned int id) { (void)id; return -1; }

/* the sound driver's status block (SND_STATUS, ps2_snddrv.h), which the game
 * reads from IOP memory: nothing playing, and each downloaded bank header's
 * checksum as the driver would compute it (the port's copy is exact, so it
 * equals the game's own MIDI_HD_CHECK / SE_HD_CHECK) */
extern short MIDI_HD_CHECK[4];
extern short SE_HD_CHECK[5];
void snd_status_bits(unsigned short se[6], unsigned short *midi);
static void snd_status(void)
{
    unsigned char *st = iop_ram + IOP_SND_STATUS;
    unsigned short se[6], midi;
    memset(st, 0, 0x42);
    snd_status_bits(se, &midi);
    memcpy(st, se, sizeof se);
    memcpy(st + 0x0C, &midi, 2);
    memcpy(st + 0x1E, MIDI_HD_CHECK, sizeof(short) * 4);
    memcpy(st + 0x26, SE_HD_CHECK, sizeof(short) * 5);
}

int sceSifGetOtherData(void *rd, void *src, void *dest, int size, unsigned int mode)
{
    (void)rd; (void)mode;
    ps2_stat_rpc++;
    if ((u_int)(size_t)src == IOP_SND_STATUS)
        snd_status();
    if (size > 0)
        memmove(iop_ptr((u_int)(size_t)dest), iop_ptr((u_int)(size_t)src), size);
    return 0;
}

void sceSifSetRpcQueue(sceSifQueueData *q, int thid) { (void)q; (void)thid; }
void sceSifRegisterRpc(sceSifServeData *sd, unsigned int request, sceSifRpcFunc f, void *buf,
                       sceSifRpcFunc cf, void *cbuf, sceSifQueueData *q)
{ (void)sd; (void)request; (void)f; (void)buf; (void)cf; (void)cbuf; (void)q; }
void sceSifRpcLoop(sceSifQueueData *q) { (void)q; }

int sceSdRemoteInit(void) { return 0; }
int sceSdRemote(int arg, ...) { (void)arg; return 0; }
int sceSdrChangeThreadPriority(int a, int b) { (void)a; (void)b; return 0; }
int sceSSyn_SetOutputMode(int mode) { (void)mode; return 0; }

/* ---- VU0 macro library ------------------------------------------------------- */
void sceVu0UnitMatrix(sceVu0FMATRIX m)
{
    int i, j;
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++)
            m[i][j] = i == j ? 1.0f : 0.0f;
}

void sceVu0AddVector(sceVu0FVECTOR a, sceVu0FVECTOR b, sceVu0FVECTOR c)
{ int i; for (i = 0; i < 4; i++) a[i] = b[i] + c[i]; }
void sceVu0SubVector(sceVu0FVECTOR a, sceVu0FVECTOR b, sceVu0FVECTOR c)
{ int i; for (i = 0; i < 4; i++) a[i] = b[i] - c[i]; }
void sceVu0ScaleVector(sceVu0FVECTOR a, sceVu0FVECTOR b, float s)
{ int i; for (i = 0; i < 4; i++) a[i] = b[i] * s; }
float sceVu0InnerProduct(sceVu0FVECTOR a, sceVu0FVECTOR b)
{ return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

void sceVu0OuterProduct(sceVu0FVECTOR o, sceVu0FVECTOR a, sceVu0FVECTOR b)
{
    float x = a[1] * b[2] - b[1] * a[2];
    float y = a[2] * b[0] - b[2] * a[0];
    float z = a[0] * b[1] - b[0] * a[1];
    o[0] = x; o[1] = y; o[2] = z; o[3] = 0.0f;
}

void sceVu0Normalize(sceVu0FVECTOR o, sceVu0FVECTOR v)
{
    float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    float q = l > 0.0f ? 1.0f / l : 0.0f;
    o[0] = v[0] * q; o[1] = v[1] * q; o[2] = v[2] * q; o[3] = v[3];
}

void sceVu0FTOI0Vector(sceVu0IVECTOR o, sceVu0FVECTOR v) { int i; for (i = 0; i < 4; i++) o[i] = (int)v[i]; }
void sceVu0FTOI4Vector(sceVu0IVECTOR o, sceVu0FVECTOR v) { int i; for (i = 0; i < 4; i++) o[i] = (int)(v[i] * 16.0f); }
void sceVu0ITOF12Vector(sceVu0FVECTOR o, sceVu0IVECTOR v) { int i; for (i = 0; i < 4; i++) o[i] = (float)v[i] / 4096.0f; }

/* VU0/VU1 micro programs uploaded by the game: the port does that work in C */
void ps2_vu0sub0(void) {}
void ps2_vu1sub0(void) {}
void ps2_vu1sub1(void) {}

/* ---- movies (Sofdec): skipped ----------------------------------------------- */
int sceMpegDemuxPssRing(sceMpeg *mp, u_char *start, int size, u_char *base, int ringsize)
{ (void)mp; (void)start; (void)base; (void)ringsize; return size; }


/* ---- VU0 water-wave effect (micro program): not drawn yet ------------------ */
void bhObj005(void *op) { (void)op; }

/* ---- the game's printf ----------------------------------------------------- */
#undef printf
int recvx_printf(const char *fmt, ...)
{
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = plat_vprintf(fmt, ap);
    va_end(ap);
    return n;
}

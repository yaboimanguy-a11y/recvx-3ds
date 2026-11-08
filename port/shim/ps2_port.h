/* ps2_port.h: the PS2 SDK names the game uses, for the native port.
 *
 * Written for this port from the PS2's documented hardware layout (GS
 * registers, GIF tags, VIF codes); the functions are implemented (or
 * stubbed) in port/ps2/.  Every <libXXX.h> shim includes this one file.
 *
 * Sizes that the game's own structs depend on are kept: u_long is the
 * EE's 64-bit long, u_long128 a 16-byte aligned quadword. */
#pragma once

#include <stddef.h>
#include <string.h>
#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- basic EE types ---------------------------------------------------- */
typedef unsigned char u_char;
typedef unsigned short u_short;
typedef unsigned int u_int;
/* EE long is 64-bit; a macro so it never clashes with the C library's
 * BSD u_long */
typedef unsigned long long ps2_u_long;
#define u_long ps2_u_long
typedef struct { unsigned int w[4]; } __attribute__((aligned(16))) u_long128;
typedef struct { int w[4]; } __attribute__((aligned(16))) long128;

typedef float sceVu0FVECTOR[4] __attribute__((aligned(16)));
typedef int sceVu0IVECTOR[4] __attribute__((aligned(16)));
typedef float sceVu0FMATRIX[4][4] __attribute__((aligned(16)));
typedef int sceVu0IMATRIX[4][4] __attribute__((aligned(16)));

/* ---- EE kernel ---------------------------------------------------------- */
#define WRITEBACK_DCACHE 0
#define INVALIDATE_DCACHE 1
#define INVALIDATE_ICACHE 2
#define INVALIDATE_CACHE 3
void FlushCache(int op);
void SyncDCache(void *start, void *end);
void InvalidDCache(void *start, void *end);
int DI(void);
int EI(void);
int iSignalSema(int sema);
int SignalSema(int sema);
int WaitSema(int sema);
int PollSema(int sema);
int GetThreadId(void);
int ChangeThreadPriority(int thid, int prio);
int RotateThreadReadyQueue(int prio);
int AddIntcHandler(int cause, int (*handler)(int), int next);
int EnableIntc(int cause);
int DisableIntc(int cause);
void ExitHandler(void);
void *GetGP(void);

/* ---- file io (sifdev) -------------------------------------------------- */
#define SCE_RDONLY 0x0001
#define SCE_WRONLY 0x0002
#define SCE_RDWR 0x0003
#define SCE_CREAT 0x0200
#define SCE_TRUNC 0x0400
#define SCE_SEEK_SET 0
#define SCE_SEEK_CUR 1
#define SCE_SEEK_END 2
#define SCE_FS_EXECUTING 1
int sceOpen(const char *name, int flags, ...);
int sceClose(int fd);
int sceRead(int fd, void *buf, int n);
int sceWrite(int fd, const void *buf, int n);
int sceLseek(int fd, int off, int whence);
int sceIoctl(int fd, int req, void *arg);
int sceFsReset(void);

/* ---- SIF / IOP --------------------------------------------------------- */
typedef struct sceSifClientData {
    struct { unsigned int pad[4]; } rpcd;
    unsigned int command;
    void *buff;
    void *cbuff;
    void (*end_function)(void *);
    void *end_param;
    void *serve;
    int sema;
    int pad2;
} sceSifClientData;
typedef struct sceSifServeData sceSifServeData;
typedef struct sceSifQueueData { int pad[16]; } sceSifQueueData;
typedef void (*sceSifEndFunc)(void *);
typedef void *(*sceSifRpcFunc)(unsigned int, void *, int);
typedef struct sceSifReceiveData { unsigned int src, dest; int size; } sceSifReceiveData;
typedef struct sceSifDmaData { unsigned int data, addr, size, mode; } sceSifDmaData;
struct sceSifServeData { int pad[16]; };
#define SIF_RPCM_NOWAIT 0x01
void sceSifInitRpc(unsigned int mode);
int sceSifBindRpc(sceSifClientData *cd, unsigned int request, unsigned int mode);
int sceSifCallRpc(sceSifClientData *cd, unsigned int request, unsigned int mode, void *send,
                  int ssize, void *receive, int rsize, sceSifEndFunc end, void *param);
int sceSifCheckStatRpc(void *cd);
void sceSifSetRpcQueue(sceSifQueueData *q, int thid);
void sceSifRegisterRpc(sceSifServeData *sd, unsigned int request, sceSifRpcFunc f, void *buf,
                       sceSifRpcFunc cf, void *cbuf, sceSifQueueData *q);
void sceSifRpcLoop(sceSifQueueData *q);
int sceSifGetOtherData(void *rd, void *src, void *dest, int size, unsigned int mode);
unsigned int sceSifSetDma(sceSifDmaData *sdd, int len);
int sceSifDmaStat(unsigned int id);
unsigned int isceSifSetDma(sceSifDmaData *sdd, int len);
int sceSifInitIopHeap(void);
void *sceSifAllocIopHeap(unsigned int size);
int sceSifFreeIopHeap(void *addr);
int sceSifLoadModule(const char *name, int argl, const char *args);
int sceSifRebootIop(const char *img);
int sceSifSyncIop(void);
void sceSifDmaData_unused(void);

/* ---- DMA ---------------------------------------------------------------- */
typedef struct sceDmaChan {
    volatile unsigned int chcr; unsigned int p0[3];
    volatile void *madr; unsigned int p1[3];
    volatile unsigned int qwc; unsigned int p2[3];
    volatile void *tadr; unsigned int p3[3];
    volatile void *as0; unsigned int p4[3];
    volatile void *as1; unsigned int p5[3];
    unsigned int p6[4]; unsigned int p7[4];
    volatile void *sadr; unsigned int p8[3];
} sceDmaChan;
#define SCE_DMA_VIF0 0
#define SCE_DMA_VIF1 1
#define SCE_DMA_GIF 2
#define SCE_DMA_fromIPU 3
#define SCE_DMA_toIPU 4
sceDmaChan *sceDmaGetChan(int id);
int sceDmaReset(int mode);
void sceDmaSend(sceDmaChan *d, void *tag);
void sceDmaSendN(sceDmaChan *d, void *addr, int size);
int sceDmaSync(sceDmaChan *d, int mode, int timeout);

/* ---- VU0 macro library -------------------------------------------------- */
void sceVu0UnitMatrix(sceVu0FMATRIX m);
void sceVu0CopyMatrix(sceVu0FMATRIX d, sceVu0FMATRIX s);
void sceVu0MulMatrix(sceVu0FMATRIX d, sceVu0FMATRIX a, sceVu0FMATRIX b);
void sceVu0ApplyMatrix(sceVu0FVECTOR v0, sceVu0FMATRIX m, sceVu0FVECTOR v1);
void sceVu0AddVector(sceVu0FVECTOR v0, sceVu0FVECTOR v1, sceVu0FVECTOR v2);
void sceVu0SubVector(sceVu0FVECTOR v0, sceVu0FVECTOR v1, sceVu0FVECTOR v2);
void sceVu0ScaleVector(sceVu0FVECTOR v0, sceVu0FVECTOR v1, float s);
float sceVu0InnerProduct(sceVu0FVECTOR v0, sceVu0FVECTOR v1);
void sceVu0OuterProduct(sceVu0FVECTOR v0, sceVu0FVECTOR v1, sceVu0FVECTOR v2);
void sceVu0Normalize(sceVu0FVECTOR v0, sceVu0FVECTOR v1);
void sceVu0ITOF4Vector(sceVu0FVECTOR v0, sceVu0IVECTOR v1);
void sceVu0ITOF12Vector(sceVu0FVECTOR v0, sceVu0IVECTOR v1);
void sceVu0FTOI0Vector(sceVu0IVECTOR v0, sceVu0FVECTOR v1);
void sceVu0FTOI4Vector(sceVu0IVECTOR v0, sceVu0FVECTOR v1);
void sceVpu0Reset(void);

/* ---- GS: registers (address field of A+D) ----------------------------- */
enum {
    SCE_GS_PRIM = 0x00, SCE_GS_RGBAQ = 0x01, SCE_GS_ST = 0x02, SCE_GS_UV = 0x03,
    SCE_GS_XYZF2 = 0x04, SCE_GS_XYZ2 = 0x05, SCE_GS_TEX0_1 = 0x06, SCE_GS_TEX0_2 = 0x07,
    SCE_GS_CLAMP_1 = 0x08, SCE_GS_CLAMP_2 = 0x09, SCE_GS_FOG = 0x0a, SCE_GS_XYZF3 = 0x0c,
    SCE_GS_XYZ3 = 0x0d, SCE_GS_TEX1_1 = 0x14, SCE_GS_TEX1_2 = 0x15, SCE_GS_TEX2_1 = 0x16,
    SCE_GS_TEX2_2 = 0x17, SCE_GS_XYOFFSET_1 = 0x18, SCE_GS_XYOFFSET_2 = 0x19,
    SCE_GS_PRMODECONT = 0x1a, SCE_GS_PRMODE = 0x1b, SCE_GS_TEXCLUT = 0x1c,
    SCE_GS_SCANMSK = 0x22, SCE_GS_TEXA = 0x3b, SCE_GS_FOGCOL = 0x3d, SCE_GS_TEXFLUSH = 0x3f,
    SCE_GS_SCISSOR_1 = 0x40, SCE_GS_SCISSOR_2 = 0x41, SCE_GS_ALPHA_1 = 0x42,
    SCE_GS_ALPHA_2 = 0x43, SCE_GS_DIMX = 0x44, SCE_GS_DTHE = 0x45, SCE_GS_COLCLAMP = 0x46,
    SCE_GS_TEST_1 = 0x47, SCE_GS_TEST_2 = 0x48, SCE_GS_PABE = 0x49, SCE_GS_FBA_1 = 0x4a,
    SCE_GS_FBA_2 = 0x4b, SCE_GS_FRAME_1 = 0x4c, SCE_GS_FRAME_2 = 0x4d, SCE_GS_ZBUF_1 = 0x4e,
    SCE_GS_ZBUF_2 = 0x4f, SCE_GS_BITBLTBUF = 0x50, SCE_GS_TRXPOS = 0x51,
    SCE_GS_TRXREG = 0x52, SCE_GS_TRXDIR = 0x53, SCE_GS_HWREG = 0x54,
    SCE_GS_SIGNAL = 0x60, SCE_GS_FINISH = 0x61, SCE_GS_LABEL = 0x62,
};
/* register descriptors in a GIF tag's REGS field */
enum {
    SCE_GIF_PRIM = 0x0, SCE_GIF_RGBAQ = 0x1, SCE_GIF_ST = 0x2, SCE_GIF_UV = 0x3,
    SCE_GIF_XYZF2 = 0x4, SCE_GIF_XYZ2 = 0x5, SCE_GIF_TEX0_1 = 0x6, SCE_GIF_TEX0_2 = 0x7,
    SCE_GIF_CLAMP_1 = 0x8, SCE_GIF_CLAMP_2 = 0x9, SCE_GIF_FOG = 0xa, SCE_GIF_XYZF3 = 0xc,
    SCE_GIF_XYZ3 = 0xd, SCE_GIF_PACKED_AD = 0xe, SCE_GIF_NOP = 0xf,
};
enum { SCE_GIF_PACKED = 0, SCE_GIF_REGLIST = 1, SCE_GIF_IMAGE = 2 };

#define SCE_GS_FALSE 0
#define SCE_GS_TRUE 1
#define SCE_GS_NOINTERLACE 0
#define SCE_GS_INTERLACE 1
#define SCE_GS_FIELD 1
#define SCE_GS_FRAME 0
#define SCE_GS_NTSC 2
#define SCE_GS_PAL 3
/* primitives */
#define SCE_GS_PRIM_POINT 0
#define SCE_GS_PRIM_LINE 1
#define SCE_GS_PRIM_LINESTRIP 2
#define SCE_GS_PRIM_TRI 3
#define SCE_GS_PRIM_TRISTRIP 4
#define SCE_GS_PRIM_TRIFAN 5
#define SCE_GS_PRIM_SPRITE 6
/* pixel storage formats */
#define SCE_GS_PSMCT32 0x00
#define SCE_GS_PSMCT24 0x01
#define SCE_GS_PSMCT16 0x02
#define SCE_GS_PSMCT16S 0x0a
#define SCE_GS_PSMT8 0x13
#define SCE_GS_PSMT4 0x14
#define SCE_GS_PSMT8H 0x1b
#define SCE_GS_PSMT4HL 0x24
#define SCE_GS_PSMT4HH 0x2c
#define SCE_GS_PSMZ32 0x30
#define SCE_GS_PSMZ24 0x31
#define SCE_GS_PSMZ16 0x32
#define SCE_GS_PSMZ16S 0x3a
/* TEST */
#define SCE_GS_ALPHA_NEVER 0
#define SCE_GS_ALPHA_ALWAYS 1
#define SCE_GS_ALPHA_LESS 2
#define SCE_GS_ALPHA_LEQUAL 3
#define SCE_GS_ALPHA_EQUAL 4
#define SCE_GS_ALPHA_GEQUAL 5
#define SCE_GS_ALPHA_GREATER 6
#define SCE_GS_ALPHA_NOTEQUAL 7
#define SCE_GS_AFAIL_KEEP 0
#define SCE_GS_AFAIL_FB_ONLY 1
#define SCE_GS_AFAIL_ZB_ONLY 2
#define SCE_GS_AFAIL_RGB_ONLY 3
#define SCE_GS_DEPTH_NEVER 0
#define SCE_GS_DEPTH_ALWAYS 1
#define SCE_GS_DEPTH_GEQUAL 2
#define SCE_GS_DEPTH_GREATER 3
#define SCE_GS_ZNOUSE 0
#define SCE_GS_ZALWAYS 1
#define SCE_GS_ZGEQUAL 2
#define SCE_GS_ZGREATER 3
/* ALPHA blend inputs */
#define SCE_GS_ALPHA_CS 0
#define SCE_GS_ALPHA_CD 1
#define SCE_GS_ALPHA_ZERO 2
#define SCE_GS_ALPHA_AS 0
#define SCE_GS_ALPHA_AD 1
#define SCE_GS_ALPHA_FIX 2
/* texture function, wrap, filter */
#define SCE_GS_MODULATE 0
#define SCE_GS_DECAL 1
#define SCE_GS_HIGHLIGHT 2
#define SCE_GS_HIGHLIGHT2 3
#define SCE_GS_REPEAT 0
#define SCE_GS_CLAMP 1
#define SCE_GS_REGION_CLAMP 2
#define SCE_GS_REGION_REPEAT 3
#define SCE_GS_NEAREST 0
#define SCE_GS_LINEAR 1

#define GS_B_(v, lo) ((u_long)(v) << (lo))
#define SCE_GS_SET_PRIM(prim, iip, tme, fge, abe, aa1, fst, ctxt, fix) \
    (GS_B_(prim, 0) | GS_B_(iip, 3) | GS_B_(tme, 4) | GS_B_(fge, 5) | GS_B_(abe, 6) | \
     GS_B_(aa1, 7) | GS_B_(fst, 8) | GS_B_(ctxt, 9) | GS_B_(fix, 10))
#define SCE_GS_SET_RGBAQ(r, g, b, a, q) \
    (GS_B_(r, 0) | GS_B_(g, 8) | GS_B_(b, 16) | GS_B_(a, 24) | GS_B_(q, 32))
#define SCE_GS_SET_ST(s, t) (GS_B_(s, 0) | GS_B_(t, 32))
#define SCE_GS_SET_UV(u, v) (GS_B_(u, 0) | GS_B_(v, 16))
#define SCE_GS_SET_XYZF(x, y, z, f) (GS_B_(x, 0) | GS_B_(y, 16) | GS_B_(z, 32) | GS_B_(f, 56))
#define SCE_GS_SET_XYZF2 SCE_GS_SET_XYZF
#define SCE_GS_SET_XYZF3 SCE_GS_SET_XYZF
#define SCE_GS_SET_XYZ(x, y, z) (GS_B_(x, 0) | GS_B_(y, 16) | GS_B_(z, 32))
#define SCE_GS_SET_XYZ2 SCE_GS_SET_XYZ
#define SCE_GS_SET_XYZ3 SCE_GS_SET_XYZ
#define SCE_GS_SET_TEX0(tbp, tbw, psm, tw, th, tcc, tfx, cbp, cpsm, csm, csa, cld) \
    (GS_B_(tbp, 0) | GS_B_(tbw, 14) | GS_B_(psm, 20) | GS_B_(tw, 26) | GS_B_(th, 30) | \
     GS_B_(tcc, 34) | GS_B_(tfx, 35) | GS_B_(cbp, 37) | GS_B_(cpsm, 51) | GS_B_(csm, 55) | \
     GS_B_(csa, 56) | GS_B_(cld, 61))
#define SCE_GS_SET_TEX0_1 SCE_GS_SET_TEX0
#define SCE_GS_SET_TEX0_2 SCE_GS_SET_TEX0
#define SCE_GS_SET_TEX1(lcm, mxl, mmag, mmin, mtba, l, k) \
    (GS_B_(lcm, 0) | GS_B_(mxl, 2) | GS_B_(mmag, 5) | GS_B_(mmin, 6) | GS_B_(mtba, 9) | \
     GS_B_(l, 19) | GS_B_(k, 32))
#define SCE_GS_SET_TEX1_1 SCE_GS_SET_TEX1
#define SCE_GS_SET_TEX1_2 SCE_GS_SET_TEX1
#define SCE_GS_SET_CLAMP(wms, wmt, minu, maxu, minv, maxv) \
    (GS_B_(wms, 0) | GS_B_(wmt, 2) | GS_B_(minu, 4) | GS_B_(maxu, 14) | GS_B_(minv, 24) | \
     GS_B_(maxv, 34))
#define SCE_GS_SET_CLAMP_1 SCE_GS_SET_CLAMP
#define SCE_GS_SET_CLAMP_2 SCE_GS_SET_CLAMP
#define SCE_GS_SET_TEST(ate, atst, aref, afail, date, datm, zte, ztst) \
    (GS_B_(ate, 0) | GS_B_(atst, 1) | GS_B_(aref, 4) | GS_B_(afail, 12) | GS_B_(date, 14) | \
     GS_B_(datm, 15) | GS_B_(zte, 16) | GS_B_(ztst, 17))
#define SCE_GS_SET_TEST_1 SCE_GS_SET_TEST
#define SCE_GS_SET_TEST_2 SCE_GS_SET_TEST
#define SCE_GS_SET_ALPHA(a, b, c, d, fix) \
    (GS_B_(a, 0) | GS_B_(b, 2) | GS_B_(c, 4) | GS_B_(d, 6) | GS_B_(fix, 32))
#define SCE_GS_SET_ALPHA_1 SCE_GS_SET_ALPHA
#define SCE_GS_SET_ALPHA_2 SCE_GS_SET_ALPHA
#define SCE_GS_SET_FRAME(fbp, fbw, psm, fbmsk) \
    (GS_B_(fbp, 0) | GS_B_(fbw, 16) | GS_B_(psm, 24) | GS_B_(fbmsk, 32))
#define SCE_GS_SET_FRAME_1 SCE_GS_SET_FRAME
#define SCE_GS_SET_FRAME_2 SCE_GS_SET_FRAME
#define SCE_GS_SET_ZBUF(zbp, psm, zmsk) (GS_B_(zbp, 0) | GS_B_(psm, 24) | GS_B_(zmsk, 32))
#define SCE_GS_SET_ZBUF_1 SCE_GS_SET_ZBUF
#define SCE_GS_SET_ZBUF_2 SCE_GS_SET_ZBUF
#define SCE_GS_SET_SCISSOR(x0, x1, y0, y1) \
    (GS_B_(x0, 0) | GS_B_(x1, 16) | GS_B_(y0, 32) | GS_B_(y1, 48))
#define SCE_GS_SET_SCISSOR_1 SCE_GS_SET_SCISSOR
#define SCE_GS_SET_SCISSOR_2 SCE_GS_SET_SCISSOR
#define SCE_GS_SET_XYOFFSET(x, y) (GS_B_(x, 0) | GS_B_(y, 32))
#define SCE_GS_SET_XYOFFSET_1 SCE_GS_SET_XYOFFSET
#define SCE_GS_SET_XYOFFSET_2 SCE_GS_SET_XYOFFSET
#define SCE_GS_SET_TEXA(ta0, aem, ta1) (GS_B_(ta0, 0) | GS_B_(aem, 15) | GS_B_(ta1, 32))
#define SCE_GS_SET_FOGCOL(r, g, b) (GS_B_(r, 0) | GS_B_(g, 8) | GS_B_(b, 16))
#define SCE_GS_SET_FOG(f) GS_B_(f, 56)
#define SCE_GS_SET_PABE(v) ((u_long)(v))
#define SCE_GS_SET_FBA(v) ((u_long)(v))
#define SCE_GS_SET_FBA_1 SCE_GS_SET_FBA
#define SCE_GS_SET_FBA_2 SCE_GS_SET_FBA
#define SCE_GS_SET_TEXFLUSH(v) ((u_long)(v))
#define SCE_GS_SET_COLCLAMP(v) ((u_long)(v))
#define SCE_GS_SET_DTHE(v) ((u_long)(v))
#define SCE_GS_SET_PRMODECONT(v) ((u_long)(v))
#define SCE_GS_SET_TEXCLUT(cbw, cou, cov) (GS_B_(cbw, 0) | GS_B_(cou, 6) | GS_B_(cov, 12))
#define SCE_GS_SET_BITBLTBUF(sbp, sbw, spsm, dbp, dbw, dpsm) \
    (GS_B_(sbp, 0) | GS_B_(sbw, 16) | GS_B_(spsm, 24) | GS_B_(dbp, 32) | GS_B_(dbw, 48) | \
     GS_B_(dpsm, 56))
#define SCE_GS_SET_TRXPOS(ssax, ssay, dsax, dsay, dir) \
    (GS_B_(ssax, 0) | GS_B_(ssay, 16) | GS_B_(dsax, 32) | GS_B_(dsay, 48) | GS_B_(dir, 59))
#define SCE_GS_SET_TRXREG(rrw, rrh) (GS_B_(rrw, 0) | GS_B_(rrh, 32))
#define SCE_GS_SET_TRXDIR(xdir) ((u_long)(xdir))
#define SCE_GIF_SET_TAG(nloop, eop, pre, prim, flg, nreg) \
    (GS_B_(nloop, 0) | GS_B_(eop, 15) | GS_B_(pre, 46) | GS_B_(prim, 47) | GS_B_(flg, 58) | \
     GS_B_(nreg, 60))

/* GS register images as the game stores them */
typedef struct { u_long TBP0:14, TBW:6, PSM:6, TW:4, TH:4, TCC:1, TFX:2, CBP:14, CPSM:4,
                 CSM:1, CSA:5, CLD:3; } sceGsTex0;
typedef struct { u_long LCM:1, pad01:1, MXL:3, MMAG:1, MMIN:3, MTBA:1, pad10:9, L:2,
                 pad21:11, K:12, pad44:20; } sceGsTex1;
typedef struct { u_long PRIM:3, IIP:1, TME:1, FGE:1, ABE:1, AA1:1, FST:1, CTXT:1, FIX:1,
                 pad11:53; } sceGsPrim;
typedef struct { u_long ATE:1, ATST:3, AREF:8, AFAIL:2, DATE:1, DATM:1, ZTE:1, ZTST:2,
                 pad19:45; } sceGsTest;
typedef struct { u_long A:2, B:2, C:2, D:2, pad8:24, FIX:8, pad40:24; } sceGsAlpha;
typedef struct { u_long FBP:9, pad9:7, FBW:6, pad22:2, PSM:6, pad30:2, FBMSK:32; } sceGsFrame;
typedef struct { u_long ZBP:9, pad9:15, PSM:4, pad28:4, ZMSK:1, pad33:31; } sceGsZbuf;
typedef struct { u_long OFX:16, pad16:16, OFY:16, pad48:16; } sceGsXyoffset;
typedef struct { u_long SCAX0:11, pad11:5, SCAX1:11, pad27:5, SCAY0:11, pad43:5, SCAY1:11,
                 pad59:5; } sceGsScissor;
typedef struct { u_long WMS:2, WMT:2, MINU:10, MAXU:10, MINV:10, MAXV:10, pad44:20; } sceGsClamp;
typedef struct { u_long PRMODECONT:1, pad1:63; } sceGsPrmodecont;
typedef struct { u_long CLAMP:1, pad1:63; } sceGsColclamp;
typedef struct { u_long DTHE:1, pad1:63; } sceGsDthe;

typedef struct {
    sceGsFrame frame1; u_long frame1addr;
    sceGsZbuf zbuf1; u_long zbuf1addr;
    sceGsXyoffset xyoffset1; u_long xyoffset1addr;
    sceGsScissor scissor1; u_long scissor1addr;
    sceGsPrmodecont prmodecont; u_long prmodecontaddr;
    sceGsColclamp colclamp; u_long colclampaddr;
    sceGsDthe dthe; u_long dtheaddr;
    sceGsTest test1; u_long test1addr;
} sceGsDrawEnv1;
typedef struct {
    sceGsFrame frame2; u_long frame2addr;
    sceGsZbuf zbuf2; u_long zbuf2addr;
    sceGsXyoffset xyoffset2; u_long xyoffset2addr;
    sceGsScissor scissor2; u_long scissor2addr;
    sceGsPrmodecont prmodecont; u_long prmodecontaddr;
    sceGsColclamp colclamp; u_long colclampaddr;
    sceGsDthe dthe; u_long dtheaddr;
    sceGsTest test2; u_long test2addr;
} sceGsDrawEnv2;
typedef struct { u_long EN1:1, EN2:1, CRTMD:3, MMOD:1, AMOD:1, SLBG:1, ALP:8, pad16:48; } sceGsPmode;
typedef struct { u_long INT:1, FFMD:1, DPMS:2, pad4:60; } sceGsSmode2;
typedef struct { u_long FBP:9, FBW:6, PSM:5, pad20:12, DBX:11, DBY:11, pad54:10; } sceGsDispfb;
typedef struct { u_long DX:12, DY:11, MAGH:4, MAGV:2, pad29:3, DW:12, DH:11, pad55:9; } sceGsDisplay;
typedef struct { u_long R:8, G:8, B:8, pad24:40; } sceGsBgcolor;
typedef struct { sceGsPmode pmode; sceGsSmode2 smode2; sceGsDispfb dispfb; sceGsDisplay display;
                 sceGsBgcolor bgcolor; } sceGsDispEnv;
typedef struct { u_long R:8, G:8, B:8, A:8, Q:32; } sceGsRgbaq;
typedef struct {
    sceGsTest testa; u_long testaaddr;
    sceGsPrim prim; u_long primaddr;
    sceGsRgbaq rgbaq; u_long rgbaqaddr;
    u_long xyz2a, xyz2aaddr, xyz2b, xyz2baddr;
    sceGsTest testb; u_long testbaddr;
} sceGsClear;
typedef struct {
    sceGsDispEnv disp[2];
    u_long128 giftag0;
    sceGsDrawEnv1 draw01;
    sceGsDrawEnv2 draw02;
    sceGsClear clear0;
    u_long128 giftag1;
    sceGsDrawEnv1 draw11;
    sceGsDrawEnv2 draw12;
    sceGsClear clear1;
} sceGsDBuffDc;
typedef struct { sceGsDispEnv disp[2]; u_long128 giftag0; sceGsDrawEnv1 draw0;
                 u_long128 giftag1; sceGsDrawEnv1 draw1; } sceGsDBuff;
typedef struct { u_long128 vifcode; u_long128 giftag; u_long bitbltbuf, bitbltbufaddr,
                 trxpos, trxposaddr, trxreg, trxregaddr, trxdir, trxdiraddr;
                 u_long128 giftag1; } sceGsLoadImage;
typedef struct { u_long128 vifcode[2]; u_long128 giftag; u_long bitbltbuf, bitbltbufaddr,
                 trxpos, trxposaddr, trxreg, trxregaddr, finish, finishaddr, trxdir,
                 trxdiraddr; } sceGsStoreImage;

int sceGsSyncV(int mode);
int sceGsSyncPath(int mode, unsigned short timeout);
int sceGsSyncVCallback(int (*cb)(int));
void sceGsResetGraph(short mode, short inter, short omode, short ffmd);
void sceGsResetPath(void);
void sceGsSetDefDBuffDc(sceGsDBuffDc *db, short psm, short w, short h, short ztest, short zpsm,
                        short clear);
int sceGsSwapDBuffDc(sceGsDBuffDc *db, int id);
int sceGsSetDefLoadImage(sceGsLoadImage *lp, short dbp, short dbw, short dpsm, short x, short y,
                         short w, short h);
int sceGsSetDefStoreImage(sceGsStoreImage *sp, short sbp, short sbw, short spsm, short x,
                          short y, short w, short h);
int sceGsExecLoadImage(sceGsLoadImage *lp, u_long128 *src);
int sceGsExecStoreImage(sceGsStoreImage *sp, u_long128 *dst);

/* ---- VIF codes ---------------------------------------------------------- */
#define SCE_VIF1_SET_NOP(irq) ((u_int)(irq) << 31)
#define SCE_VIF1_SET_OFFSET(off, irq) ((u_int)(off) | (0x02u << 24) | ((u_int)(irq) << 31))
#define SCE_VIF1_SET_BASE(base, irq) ((u_int)(base) | (0x03u << 24) | ((u_int)(irq) << 31))
#define SCE_VIF1_SET_ITOP(v, irq) ((u_int)(v) | (0x04u << 24) | ((u_int)(irq) << 31))
#define SCE_VIF1_SET_STCYCL(wl, cl, irq) \
    ((u_int)(cl) | ((u_int)(wl) << 8) | (0x01u << 24) | ((u_int)(irq) << 31))
#define SCE_VIF1_SET_FLUSH(irq) ((0x11u << 24) | ((u_int)(irq) << 31))
#define SCE_VIF1_SET_FLUSHA(irq) ((0x13u << 24) | ((u_int)(irq) << 31))
#define SCE_VIF1_SET_MSCAL(addr, irq) ((u_int)(addr) | (0x14u << 24) | ((u_int)(irq) << 31))
#define SCE_VIF1_SET_MSCNT(irq) ((0x17u << 24) | ((u_int)(irq) << 31))
#define SCE_VIF1_SET_DIRECT(n, irq) ((u_int)(n) | (0x50u << 24) | ((u_int)(irq) << 31))
#define SCE_VIF1_SET_UNPACK(addr, num, fmt, irq) \
    ((u_int)(addr) | ((u_int)(num) << 16) | ((u_int)(0x60 | (fmt)) << 24) | ((u_int)(irq) << 31))
#define SCE_VIF0_SET_NOP SCE_VIF1_SET_NOP
#define SCE_VIF0_SET_UNPACK SCE_VIF1_SET_UNPACK
#define SCE_VIF0_SET_MSCAL SCE_VIF1_SET_MSCAL
#define SCE_UPF_S_32 0x00
#define SCE_UPF_S_16 0x01
#define SCE_UPF_S_8 0x02
#define SCE_UPF_V2_32 0x04
#define SCE_UPF_V2_16 0x05
#define SCE_UPF_V2_8 0x06
#define SCE_UPF_V3_32 0x08
#define SCE_UPF_V3_16 0x09
#define SCE_UPF_V3_8 0x0a
#define SCE_UPF_V4_32 0x0c
#define SCE_UPF_V4_16 0x0d
#define SCE_UPF_V4_8 0x0e
#define SCE_UPF_V4_5 0x0f

/* ---- CD/DVD ------------------------------------------------------------- */
typedef struct { u_int lsn; u_int size; char name[16]; u_char date[8]; } sceCdlFILE;
typedef struct { u_char trycount, spindlctrl, datapattern, pad; } sceCdRMode;
typedef struct { u_char stat, second, minute, hour, pad, day, month, year; } sceCdCLOCK;
#define SCE_CdSTREAM 0
#define SCE_CdDVD 2
#define SCE_CdSpinNom 0x01
#define SCE_CdSpinStm 0x00
#define SCE_CdSecS2048 0
int sceCdInit(int mode);
int sceCdMmode(int media);
int sceCdSync(int mode);
int sceCdSearchFile(sceCdlFILE *fp, const char *name);
int sceCdRead(u_int lsn, u_int sectors, void *buf, sceCdRMode *mode);
int sceCdGetError(void);
int sceCdDiskReady(int mode);
int sceCdBreak(void);
int sceCdPause(void);
int sceCdReadClock(sceCdCLOCK *clock);
int sceCdStInit(u_int bufmax, u_int bankmax, u_int iop_bufaddr);
int sceCdStStart(u_int lsn, sceCdRMode *mode);
int sceCdStRead(u_int sectors, u_int *buf, u_int mode, u_int *err);
int sceCdStStop(void);
#define STMBLK 0

/* ---- pad ---------------------------------------------------------------- */
#define scePadDmaBufferMax 16
#define InfoModeCurID 1
#define InfoModeCurExID 2
#define InfoModeCurExOffs 3
#define InfoModeIdTable 4
#define scePadStateDiscon 0
#define scePadStateFindPad 1
#define scePadStateFindCTP1 2
#define scePadStateExecCmd 5
#define scePadStateStable 6
#define scePadStateError 7
#define scePadReqStateComplete 0
#define scePadReqStateFaild 1
#define scePadReqStateBusy 2
int scePadInit(int mode);
int scePadPortOpen(int port, int slot, u_long128 *addr);
int scePadPortClose(int port, int slot);
int scePadRead(int port, int slot, unsigned char *rdata);
int scePadGetState(int port, int slot);
int scePadGetReqState(int port, int slot);
int scePadInfoMode(int port, int slot, int term, int offs);
int scePadSetMainMode(int port, int slot, int offs, int lock);
int scePadInfoPressMode(int port, int slot);
int scePadEnterPressMode(int port, int slot);
int scePadSetActDirect(int port, int slot, const unsigned char *data);
int scePadSetActAlign(int port, int slot, const unsigned char *data);
int scePadInfoAct(int port, int slot, int actno, int term);

/* ---- memory card -------------------------------------------------------- */
typedef struct { struct { u_char resv2, sec, min, hour, day, month; u_short year; } _create;
                 struct { u_char resv2, sec, min, hour, day, month; u_short year; } _modify;
                 u_int FileSizeByte; u_short AttrFile; u_short Reserve1; u_int Reserve2;
                 u_int PdaAplNo; u_char EntryName[32]; } sceMcTblGetDir;
int sceMcInit(void);
int sceMcOpen(int port, int slot, const char *name, int mode);
int sceMcClose(int fd);
int sceMcRead(int fd, void *buf, int size);
int sceMcWrite(int fd, const void *buf, int size);
int sceMcMkdir(int port, int slot, const char *name);
int sceMcChdir(int port, int slot, const char *path, char *pwd);
int sceMcGetDir(int port, int slot, const char *name, unsigned mode, int maxent,
                sceMcTblGetDir *table);
int sceMcGetInfo(int port, int slot, int *type, int *free, int *format);
int sceMcFormat(int port, int slot);
int sceMcDelete(int port, int slot, const char *name);
int sceMcSync(int mode, int *cmd, int *result);
#define sceMcInitError (-1)

/* ---- MPEG / IPU (movies) ------------------------------------------------ */
typedef struct sceMpeg {
    int width, height, frameCount;
    long long pts, dts;
    u_long flags;
    long long pts2nd, dts2nd;
    u_long flags2nd;
    void *sys;
} sceMpeg;
typedef enum { sceMpegCbError, sceMpegCbNodata, sceMpegCbStopDMA, sceMpegCbRestartDMA,
               sceMpegCbBackground, sceMpegCbTimeStamp, sceMpegCbStr } sceMpegCbType;
typedef struct { sceMpegCbType type; } sceMpegCbData0;
typedef struct { sceMpegCbType type; int errMessage; } sceMpegCbDataError;
typedef struct { sceMpegCbType type; long long pts, dts; } sceMpegCbDataTimeStamp;
typedef struct { sceMpegCbType type; u_char *header; u_char *data; u_int len;
                 long long pts, dts; } sceMpegCbDataStr;
typedef union { sceMpegCbType type; sceMpegCbData0 cb0; sceMpegCbDataError error;
                sceMpegCbDataTimeStamp ts; sceMpegCbDataStr str; } sceMpegCbData;
typedef int (*sceMpegCallback)(sceMpeg *mp, sceMpegCbData *cbdata, void *anyData);
typedef enum { sceMpegStrM2V = 0, sceMpegStrIPU = 1, sceMpegStrPCM = 2, sceMpegStrADPCM = 3,
               sceMpegStrDATA = 4 } sceMpegStrType;
typedef struct { u_long128 *data; u_int size; } sceIpuRGB32_;
typedef struct { u_char c[16 * 16 * 4]; } sceIpuRGB32;
int sceMpegInit(void);
int sceMpegCreate(sceMpeg *mp, u_char *work, int size);
int sceMpegDelete(sceMpeg *mp);
int sceMpegReset(sceMpeg *mp);
int sceMpegIsEnd(sceMpeg *mp);
int sceMpegGetPicture(sceMpeg *mp, sceIpuRGB32 *rgb32, int mbcount);
sceMpegCallback sceMpegAddCallback(sceMpeg *mp, sceMpegCbType type, sceMpegCallback f,
                                   void *data);
sceMpegCallback sceMpegAddStrCallback(sceMpeg *mp, sceMpegStrType type, int ch,
                                      sceMpegCallback f, void *data);
int sceMpegDemuxPssRing(sceMpeg *mp, u_char *start, int size, u_char *base, int ringsize);
int sceIpuInit(void);
int sceIpuIsBusy(void);
void sceIpuBCLR(int bp);

/* ---- sound (libsdr / CSL soft synth) ------------------------------------ */
int sceSdRemoteInit(void);
int sceSdRemote(int arg, ...);
int sceSdrChangeThreadPriority(int a, int b);
int sceSSyn_SetOutputMode(int mode);

/* ---- EE hardware registers ------------------------------------------------
 * The game pokes DMA / timer / IPU registers directly.  In the port they are
 * plain words in ps2_hwreg[] (port/ps2/ps2_stubs.c): writes go nowhere and
 * busy-wait loops on them see 0 (idle) and fall through. */
extern volatile u_int ps2_hwreg[64];
#define PS2_HWREG(n) (&ps2_hwreg[n])
#define D_CTRL PS2_HWREG(0)
#define D_ENABLER PS2_HWREG(1)
#define D_ENABLEW PS2_HWREG(2)
#define D1_CHCR PS2_HWREG(3)
#define D3_CHCR PS2_HWREG(4)
#define D3_MADR PS2_HWREG(5)
#define D3_QWC PS2_HWREG(6)
#define D4_CHCR PS2_HWREG(7)
#define D4_MADR PS2_HWREG(8)
#define D4_QWC PS2_HWREG(9)
#define D4_TADR PS2_HWREG(10)
#define T0_MODE PS2_HWREG(11)
#define T0_COUNT PS2_HWREG(12)
#define IPU_CTRL PS2_HWREG(13)
#define IPU_CMD PS2_HWREG(14)
#define IPU_IN_FIFO PS2_HWREG(15)

/* ---- kernel threads / semaphores --------------------------------------- */
struct ThreadParam {
    int status; void (*entry)(void *); void *stack; int stackSize; void *gpReg;
    int initPriority; int currentPriority; u_int attr; u_int option;
    int waitType; int waitId; int wakeupCount;
};
struct SemaParam { int currentCount; int maxCount; int initCount; int numWaitThreads;
                   u_int attr; u_int option; };
int CreateThread(struct ThreadParam *p);
int StartThread(int thid, void *arg);
int DeleteThread(int thid);
int TerminateThread(int thid);
int SleepThread(void);
int WakeupThread(int thid);
int iWakeupThread(int thid);
int SuspendThread(int thid);
int ResumeThread(int thid);
int CreateSema(struct SemaParam *p);
int DeleteSema(int sema);
extern char _gp[];

/* ---- CD/DVD constants ------------------------------------------------------ */
#define SCECdINIT 0x00
#define SCECdINoD 0x01
#define SCECdEXIT 0x05
#define SCECdCD 1
#define SCECdDVD 2
#define SCECdErNO 0x00
#define SCECdErEOM 0x32
#define SCECdErTRMOPN 0x31
#define SCECdErREAD 0x30
#define SCECdErPRM 0x22
#define SCECdErILI 0x21
#define SCECdErIPI 0x20
#define SCECdErCUD 0x14
#define SCECdErNORDY 0x13
#define SCECdErNODISC 0x12
#define SCECdErOPENS 0x11
#define SCECdErCMD 0x10
#define SCECdErABRT 0x01

/* ---- libsdr (SPU2) constants: the port mixes sound itself ------------------ */
#define SD_INIT_HOT 1
#define SD_INIT_COLD 0
#define SD_TRANS_MODE_WRITE 0
#define SD_TRANS_MODE_READ 1
#define SD_TRANS_MODE_STOP 2
#define SD_TRANS_MODE_WRITE_FROM 3
#define SD_BLOCK_LOOP 0x10
#define SD_BLOCK_ONESHOT 0
#define SD_P_BVOLL 0x0f80
#define SD_P_BVOLR 0x1080
#define SD_P_MVOLL 0x0980
#define SD_P_MVOLR 0x0a80
enum { rSdInit = 0x8000, rSdSetParam, rSdGetParam, rSdSetSwitch, rSdGetSwitch, rSdSetAddr,
       rSdGetAddr, rSdSetCoreAttr, rSdGetCoreAttr, rSdNote2Pitch, rSdPitch2Note,
       rSdProcBatch, rSdProcBatchEx, rSdVoiceTrans, rSdBlockTrans, rSdVoiceTransStatus,
       rSdBlockTransStatus, rSdSetTransCallback, rSdSetIRQCallback, rSdSetEffectAttr,
       rSdGetEffectAttr, rSdClearEffectWorkArea, rSdSetTransIntrHandler,
       rSdSetSpu2IntrHandler };

/* ---- IPU --------------------------------------------------------------------- */
typedef struct { u_int d3madr, d3qwc, d3chcr, d4madr, d4tadr, d4qwc, d4chcr, ipubp, ipuctrl; } sceIpuDmaEnv;

#ifdef __cplusplus
}
#endif

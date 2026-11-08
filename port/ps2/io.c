/* io.c: disc (sceCd*, cdrom0: files), controller (scePad*) and memory card
 * (sceMc*) for the port.  Disc reads are synchronous; the "async" calls
 * report completion on the next sync/poll. */
#include "types.h"
#include "plat.h"
#include "disc.h"

#include <dirent.h>
#include <sys/stat.h>

/* ---- CD/DVD ------------------------------------------------------------------ */
static int cd_error;

int sceCdInit(int mode) { (void)mode; return 1; }
int sceCdMmode(int media) { (void)media; return 1; }
int sceCdSync(int mode) { (void)mode; return 0; }
int sceCdGetError(void) { return cd_error; }
int sceCdDiskReady(int mode) { (void)mode; return 2; }
int sceCdBreak(void) { return 1; }
int sceCdPause(void) { return 1; }

int sceCdSearchFile(sceCdlFILE *fp, const char *name)
{
    unsigned lsn, size;
    const char *base;
    if (disc_find(name, &lsn, &size) != 0) {
        plat_log("cd: not found: %s\n", name);
        return 0;
    }
    fp->lsn = lsn;
    fp->size = size;
    base = strrchr(name, '\\');
    base = base ? base + 1 : name;
    memset(fp->name, 0, sizeof fp->name);
    strncpy(fp->name, base, sizeof fp->name - 1);
    return 1;
}

extern u_int ps2_stat_cd;
int sceCdRead(u_int lsn, u_int sectors, void *buf, sceCdRMode *mode)
{
    (void)mode;
    ps2_stat_cd++;
    cd_error = disc_read(lsn, sectors, buf) == 0 ? SCECdErNO : SCECdErREAD;
    if (cd_error)
        plat_log("cd: read error lsn %u x%u\n", lsn, sectors);
    return 1;
}

int sceCdReadClock(sceCdCLOCK *c)
{
    int y, mo, d, h, mi, s;
#define BCD(v) (u_char)((((v) / 10) << 4) | ((v) % 10))
    plat_clock(&y, &mo, &d, &h, &mi, &s);
    c->stat = 0;
    c->second = BCD(s);
    c->minute = BCD(mi);
    c->hour = BCD(h);
    c->day = BCD(d);
    c->month = BCD(mo);
    c->year = BCD(y % 100);
    return 1;
#undef BCD
}

/* streaming (movies): a cursor over the disc */
static u_int st_lsn;
static int st_on;
int sceCdStInit(u_int bufmax, u_int bankmax, u_int iop) { (void)bufmax; (void)bankmax; (void)iop; return 1; }
int sceCdStStart(u_int lsn, sceCdRMode *mode) { (void)mode; st_lsn = lsn; st_on = 1; return 1; }
int sceCdStStop(void) { st_on = 0; return 1; }
int sceCdStRead(u_int sectors, u_int *buf, u_int mode, u_int *err)
{
    (void)mode;
    if (!st_on || disc_read(st_lsn, sectors, buf) != 0) {
        if (err)
            *err = SCECdErREAD;
        return 0;
    }
    st_lsn += sectors;
    if (err)
        *err = 0;
    return (int)sectors;
}

/* ---- cdrom0: files through sceOpen ----------------------------------------- */
#define MAX_FD 16
static struct { int used; u_int lsn, size, pos; } fds[MAX_FD];

int sceOpen(const char *name, int flags, ...)
{
    unsigned lsn, size;
    int i;
    (void)flags;
    if (disc_find(name, &lsn, &size) != 0) {
        plat_log("sceOpen: not found: %s\n", name);
        return -1;
    }
    for (i = 1; i < MAX_FD; i++)
        if (!fds[i].used) {
            fds[i].used = 1;
            fds[i].lsn = lsn;
            fds[i].size = size;
            fds[i].pos = 0;
            return i;
        }
    return -1;
}

int sceClose(int fd) { if (fd > 0 && fd < MAX_FD) fds[fd].used = 0; return 0; }

int sceLseek(int fd, int off, int whence)
{
    if (fd <= 0 || fd >= MAX_FD || !fds[fd].used)
        return -1;
    if (whence == SCE_SEEK_SET)
        fds[fd].pos = off;
    else if (whence == SCE_SEEK_CUR)
        fds[fd].pos += off;
    else
        fds[fd].pos = fds[fd].size + off;
    return (int)fds[fd].pos;
}

int sceRead(int fd, void *buf, int n)
{
    static unsigned char sec[2048];
    unsigned char *out = buf;
    int done = 0;
    if (fd <= 0 || fd >= MAX_FD || !fds[fd].used)
        return -1;
    if (fds[fd].pos + (u_int)n > fds[fd].size)
        n = (int)(fds[fd].size - fds[fd].pos);
    while (done < n) {
        u_int p = fds[fd].pos + done, s = p / 2048, o = p % 2048;
        u_int k = 2048 - o;
        if (k > (u_int)(n - done))
            k = n - done;
        if (o == 0 && k == 2048 && (u_int)(n - done) >= 2048) {
            u_int whole = (u_int)(n - done) / 2048;
            if (disc_read(fds[fd].lsn + s, whole, out + done) != 0)
                break;
            done += whole * 2048;
            continue;
        }
        if (disc_read(fds[fd].lsn + s, 1, sec) != 0)
            break;
        memcpy(out + done, sec + o, k);
        done += k;
    }
    fds[fd].pos += done;
    return done;
}

int sceWrite(int fd, const void *buf, int n) { (void)fd; (void)buf; return n; }
/* SCE_FS_EXECUTING: is an asynchronous request still running?  Port I/O
 * finishes at once, so never (CRI's htci.c polls this). */
int sceIoctl(int fd, int req, void *arg)
{
    (void)fd;
    if (req == SCE_FS_EXECUTING && arg)
        *(int *)arg = 0;
    return 0;
}
int sceFsReset(void) { return 0; }

/* ---- controller ---------------------------------------------------------------- */
int scePadInit(int mode) { (void)mode; return 1; }
int scePadPortOpen(int port, int slot, u_long128 *addr) { (void)slot; (void)addr; return port == 0 ? 1 : 0; }
int scePadPortClose(int port, int slot) { (void)port; (void)slot; return 1; }
int scePadGetState(int port, int slot) { (void)slot; return port == 0 ? scePadStateStable : scePadStateDiscon; }
int scePadGetReqState(int port, int slot) { (void)port; (void)slot; return scePadReqStateComplete; }
int scePadInfoMode(int port, int slot, int term, int offs)
{
    (void)port; (void)slot;
    if (term == InfoModeCurID)
        return 7;               /* DualShock 2 (analog) */
    if (term == InfoModeCurExID)
        return 7;
    if (term == InfoModeIdTable)
        return offs == -1 ? 2 : (offs == 0 ? 4 : 7);
    return 0;
}
int scePadSetMainMode(int port, int slot, int offs, int lock) { (void)port; (void)slot; (void)offs; (void)lock; return 1; }
/* DualShock 2: pressure-sensitive buttons supported (the game waits for it) */
int scePadInfoPressMode(int port, int slot) { (void)port; (void)slot; return 1; }
int scePadEnterPressMode(int port, int slot) { (void)port; (void)slot; return 1; }
int scePadSetActDirect(int port, int slot, const unsigned char *d) { (void)port; (void)slot; (void)d; return 1; }
int scePadSetActAlign(int port, int slot, const unsigned char *d) { (void)port; (void)slot; (void)d; return 1; }
int scePadInfoAct(int port, int slot, int actno, int term) { (void)port; (void)slot; (void)actno; (void)term; return 0; }

unsigned int pad_reads;
int scePadRead(int port, int slot, unsigned char *r)
{
    pad_reads++;
    unsigned b = 0;
    unsigned char st[4] = { 128, 128, 128, 128 };
    (void)slot;
    memset(r, 0, 32);
    if (port != 0)
        return 0;
    plat_pad(&b, st);
    r[0] = 0;
    r[1] = 0x79;                 /* DualShock 2 with pressures: 9 words */
    r[2] = (unsigned char)~(b & 0xFF);
    r[3] = (unsigned char)~(b >> 8);
    r[4] = st[0];
    r[5] = st[1];
    r[6] = st[2];
    r[7] = st[3];
    {   /* pressures: right left up down triangle circle cross square L1 R1 L2 R2 */
        static const unsigned short order[12] = { PAD_RIGHT, PAD_LEFT, PAD_UP, PAD_DOWN, PAD_TRIANGLE,
            PAD_CIRCLE, PAD_CROSS, PAD_SQUARE, PAD_L1, PAD_R1, PAD_L2, PAD_R2 };
        int k;
        for (k = 0; k < 12; k++)
            r[8 + k] = (b & order[k]) ? 0xFF : 0x00;
    }
    {
        static unsigned last;
        if (b != last) {
            plat_log("pad: %04X\n", b);
            last = b;
        }
    }
    return 32;
}

/* ---- memory card: files under <app>/mc<port>/ ---------------------------------- */
/* Each call completes at once; its result waits for that thread's
 * sceMcSync.  Results are kept per game thread: the card-watch thread polls
 * both slots all the time, and with one shared result its "no card in slot
 * 2" answers replaced the save screen's results ("couldn't secure space",
 * then a broken save). */
#define MC_THREADS 16
extern int ps2_cur_thread;
static int mc_cmd[MC_THREADS], mc_result[MC_THREADS], mc_pending[MC_THREADS];
static char mc_cwd[2][256] = { "/", "/" };
static FILE *mc_fp[MAX_FD];

static int mc_tid(void)
{
    int t = ps2_cur_thread;
    return (t >= 0 && t < MC_THREADS) ? t : 0;
}

static void mc_done(int cmd, int result)
{
    static int logged;
    int t = mc_tid();
    if (cmd != 1 && logged++ < 200)
        plat_log("mc: cmd %d -> %d (thread %d)\n", cmd, result, t);
    mc_cmd[t] = cmd;
    mc_result[t] = result;
    mc_pending[t] = 1;
}

static void mc_path(int port, const char *name, char *out, size_t n)
{
    char rel[512];
    if (name[0] == '/')
        snprintf(rel, sizeof rel, "%s", name);
    else
        snprintf(rel, sizeof rel, "%s%s%s", mc_cwd[port & 1],
                 mc_cwd[port & 1][strlen(mc_cwd[port & 1]) - 1] == '/' ? "" : "/", name);
    snprintf(out, n, "%s/mc%d%s", plat_app_dir(), port & 1, rel);
}

static void mc_ensure_root(int port)
{
    char p[512];
    snprintf(p, sizeof p, "%s/mc%d", plat_app_dir(), port & 1);
    mkdir(p, 0777);
}

int sceMcInit(void) { return 0; }

int sceMcSync(int mode, int *cmd, int *result)
{
    int t = mc_tid();
    (void)mode;
    if (!mc_pending[t])
        return -1;
    mc_pending[t] = 0;
    if (cmd)
        *cmd = mc_cmd[t];
    if (result)
        *result = mc_result[t];
    return 1;
}

int sceMcGetInfo(int port, int slot, int *type, int *free, int *format)
{
    (void)slot;
    if (port != 0) {           /* only slot 1 has a card */
        if (type) *type = 0;
        mc_done(1, -10);
        return 0;
    }
    mc_ensure_root(port);
    if (type) *type = 2;       /* PS2 memory card */
    if (free) *free = 8000;    /* KB */
    if (format) *format = 1;
    mc_done(1, 0);
    return 0;
}

int sceMcFormat(int port, int slot) { (void)slot; mc_ensure_root(port); mc_done(0x10, 0); return 0; }

int sceMcMkdir(int port, int slot, const char *name)
{
    char p[512];
    (void)slot;
    mc_ensure_root(port);
    mc_path(port, name, p, sizeof p);
    mc_done(0x0B, mkdir(p, 0777) == 0 ? 0 : -4);
    return 0;
}

int sceMcChdir(int port, int slot, const char *path, char *pwd)
{
    char p[512];
    struct stat st;
    (void)slot;
    if (pwd)
        strcpy(pwd, mc_cwd[port & 1]);
    mc_path(port, path, p, sizeof p);
    if (stat(p, &st) == 0 && S_ISDIR(st.st_mode)) {
        if (path[0] == '/')
            snprintf(mc_cwd[port & 1], sizeof mc_cwd[0], "%s", path);
        else if (strcmp(path, "..") == 0)
            strcpy(mc_cwd[port & 1], "/");
        else
            snprintf(mc_cwd[port & 1], sizeof mc_cwd[0], "/%s", path);
        mc_done(0x0C, 0);
    } else {
        mc_done(0x0C, -4);
    }
    return 0;
}

/* name may end in a wildcard ("*"); entries are files in that folder */
int sceMcGetDir(int port, int slot, const char *name, unsigned mode, int maxent, sceMcTblGetDir *t)
{
    plat_log("mc: getdir %d %s (cwd %s)\n", port, name, mc_cwd[port & 1]);
    char p[512], dir[512], pat[128];
    const char *slash;
    DIR *d;
    struct dirent *e;
    int n = 0;
    (void)slot; (void)mode;
    mc_path(port, name, p, sizeof p);
    slash = strrchr(p, '/');
    snprintf(dir, sizeof dir, "%.*s", (int)(slash - p), p);
    snprintf(pat, sizeof pat, "%s", slash + 1);
    d = opendir(dir);
    if (!d) {
        mc_done(0x0D, 0);
        return 0;
    }
    /* a folder listing starts with "." and "..", as on a PS2 card (the
     * save check counts them: files + 2) */
    if (strcmp(pat, "*") == 0) {
        static const char *dots[2] = { ".", ".." };
        int k;
        for (k = 0; k < 2 && n < maxent; k++, n++) {
            memset(&t[n], 0, sizeof t[n]);
            t[n].AttrFile = 0x8427;
            snprintf((char *)t[n].EntryName, sizeof t[n].EntryName, "%s", dots[k]);
            t[n]._create.year = t[n]._modify.year = 2001;
            t[n]._create.month = t[n]._modify.month = 1;
            t[n]._create.day = t[n]._modify.day = 1;
        }
    }
    while ((e = readdir(d)) != NULL && n < maxent) {
        int plen = (int)strlen(pat);
        int wild = plen > 0 && pat[plen - 1] == '*';
        char fp[768];
        struct stat st;
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        if (wild ? strncmp(e->d_name, pat, plen - 1) != 0 : strcmp(e->d_name, pat) != 0)
            continue;
        snprintf(fp, sizeof fp, "%s/%s", dir, e->d_name);
        memset(&t[n], 0, sizeof t[n]);
        if (stat(fp, &st) == 0) {
            t[n].FileSizeByte = (u_int)st.st_size;
            t[n].AttrFile = S_ISDIR(st.st_mode) ? 0x8427 : 0x8497;
        }
        snprintf((char *)t[n].EntryName, sizeof t[n].EntryName, "%s", e->d_name);
        t[n]._create.year = t[n]._modify.year = 2001;
        t[n]._create.month = t[n]._modify.month = 1;
        t[n]._create.day = t[n]._modify.day = 1;
        n++;
    }
    closedir(d);
    mc_done(0x0D, n);
    return 0;
}

int sceMcOpen(int port, int slot, const char *name, int mode)
{
    plat_log("mc: open %d %s mode %X\n", port, name, mode);
    char p[512];
    const char *m = "rb";
    int i;
    (void)slot;
    mc_ensure_root(port);
    mc_path(port, name, p, sizeof p);
    if (mode & SCE_CREAT)
        m = (mode & SCE_TRUNC) || !(mode & 1) ? "w+b" : "r+b";
    else if ((mode & 3) != SCE_RDONLY)
        m = "r+b";
    /* card files are numbered from 0, as on the PS2 (the game never keeps
     * the number sceMcOpen returns: it reads and writes file 0) */
    for (i = 0; i < MAX_FD; i++)
        if (!mc_fp[i]) {
            FILE *f = fopen(p, m);
            if (!f && (mode & SCE_CREAT))
                f = fopen(p, "w+b");
            mc_fp[i] = f;
            mc_done(2, f ? i : -4);
            return 0;
        }
    mc_done(2, -7);
    return 0;
}

int sceMcClose(int fd)
{
    if (fd >= 0 && fd < MAX_FD && mc_fp[fd]) {
        fclose(mc_fp[fd]);
        mc_fp[fd] = NULL;
    }
    mc_done(3, 0);
    return 0;
}

int sceMcRead(int fd, void *buf, int size)
{
    int n = (fd >= 0 && fd < MAX_FD && mc_fp[fd]) ? (int)fread(buf, 1, size, mc_fp[fd]) : -4;
    mc_done(5, n);
    return 0;
}

int sceMcWrite(int fd, const void *buf, int size)
{
    int n = (fd >= 0 && fd < MAX_FD && mc_fp[fd]) ? (int)fwrite(buf, 1, size, mc_fp[fd]) : -4;
    mc_done(6, n);
    return 0;
}

int sceMcDelete(int port, int slot, const char *name)
{
    char p[512];
    (void)slot;
    mc_path(port, name, p, sizeof p);
    mc_done(0x0E, remove(p) == 0 ? 0 : -4);
    return 0;
}

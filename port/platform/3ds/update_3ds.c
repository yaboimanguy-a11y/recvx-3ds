/*
 * In-app updates from the project's GitHub releases (settings screen, "Check
 * for updates") - the MVC2 port's updater (mvc2-ps2-decomp
 * port/platform/3ds/update_3ds.c), for this app.
 *
 * A background thread (off the game's core) asks the GitHub API for the
 * latest release of RECVX_UPDATE_REPO, compares its tag (v0.2.0) with this
 * build's version and, when asked to install, downloads the release's .zip
 * and writes the files under 3ds/recvx/ in it over the ones in the app's
 * folder (each written as .new first, checked, then swapped in).  Nothing
 * else is touched: the disc image, saves, settings and logs stay.  The
 * running program is already in memory, so the new one starts next time.
 *
 * HTTPS through libcurl + mbedTLS (devkitPro portlibs, 3ds-curl), checked
 * against cacert.pem in the app's folder (the SD card folder carries it).
 * Without the portlibs at build time (RECVX_HAVE_CURL unset) the button
 * only says so.
 */
#include <3ds.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "update_3ds.h"

#ifndef RECVX_VERSION
#define RECVX_VERSION "0.1.0"
#endif
#ifndef RECVX_UPDATE_REPO
#define RECVX_UPDATE_REPO "yaboimanguy-a11y/recvx-3ds"
#endif

const char *update_version(void) { return RECVX_VERSION; }

static LightLock lk;
static int lk_init;
static volatile int state = UPD_IDLE;
static char msg[96];
static int msg_serial, msg_seen = -1;
static Thread thr;
static char dir[256];
static char tag[32], zip_url[512];
static int want_install;

static void set(int st, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void set(int st, const char *fmt, ...)
{
    va_list ap;
    LightLock_Lock(&lk);
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    state = st;
    msg_serial++;
    LightLock_Unlock(&lk);
}

int update_state(char *out, size_t n, int *changed)
{
    int st;
    if (!lk_init) {
        if (out && n)
            snprintf(out, n, "Updates: the newest release from GitHub (needs Wi-Fi)");
        if (changed)
            *changed = 0;
        return UPD_IDLE;
    }
    LightLock_Lock(&lk);
    st = state;
    if (out && n)
        snprintf(out, n, "%s", msg);
    if (changed)
        *changed = msg_serial != msg_seen;
    msg_seen = msg_serial;
    LightLock_Unlock(&lk);
    return st;
}

#ifdef RECVX_HAVE_CURL
#include <curl/curl.h>
#include "inflate.h"

/* the network (soc) for the updater's thread */
static u32 *soc_buf;
static int soc_up(void)
{
    if (!soc_buf)
        soc_buf = (u32 *)memalign(0x1000, 0x100000);
    return soc_buf && R_SUCCEEDED(socInit(soc_buf, 0x100000)) ? 0 : -1;
}

static void curl_global(void)
{
    static int done;
    if (!done) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        done = 1;
    }
}

typedef struct Buf { uint8_t *p; size_t n, cap; } Buf;

static size_t on_data(char *d, size_t sz, size_t nm, void *u)
{
    Buf *b = (Buf *)u;
    size_t k = sz * nm;
    if (b->n + k + 1 > b->cap) {
        size_t c = b->cap ? b->cap * 2 : 65536;
        uint8_t *q;
        while (c < b->n + k + 1)
            c *= 2;
        if (c > (48u << 20))
            return 0;               /* nothing this big belongs to a release */
        q = (uint8_t *)realloc(b->p, c);
        if (!q)
            return 0;
        b->p = q;
        b->cap = c;
    }
    memcpy(b->p + b->n, d, k);
    b->n += k;
    b->p[b->n] = 0;
    return k;
}

static int on_progress(void *u, curl_off_t total, curl_off_t now, curl_off_t ut, curl_off_t un)
{
    static int last = -1;
    (void)u; (void)ut; (void)un;
    if (total > 0) {
        int pc = (int)(now * 100 / total);
        if (pc / 5 != last / 5) {
            last = pc;
            set(UPD_BUSY, "Downloading %s ... %d%%", tag, pc);
        }
    }
    return 0;
}

static int get(const char *url, Buf *b, int progress)
{
    char ca[300];
    struct stat st;
    CURL *c;
    CURLcode r;
    struct curl_slist *h;
    snprintf(ca, sizeof ca, "%s/cacert.pem", dir);
    if (stat(ca, &st) != 0)
        return -2;
    c = curl_easy_init();
    if (!c)
        return -1;
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "recvx-3ds-updater");
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_CAINFO, ca);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_data);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, b);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    if (progress) {
        curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, on_progress);
    }
    h = curl_slist_append(NULL, "Accept: application/vnd.github+json");
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    r = curl_easy_perform(c);
    curl_slist_free_all(h);
    curl_easy_cleanup(c);
    return r == CURLE_OK ? 0 : -1;
}

/* the string value of "key": "..." after p (no escapes in what we need) */
static int json_str(const char *p, const char *key, char *out, size_t n, const char **after)
{
    char k[64];
    const char *q;
    size_t i = 0;
    snprintf(k, sizeof k, "\"%s\"", key);
    q = strstr(p, k);
    if (!q)
        return -1;
    q += strlen(k);
    while (*q == ' ' || *q == ':' || *q == '\t' || *q == '\n' || *q == '\r')
        q++;
    if (*q != '"')
        return -1;
    q++;
    while (*q && *q != '"' && i + 1 < n)
        out[i++] = *q++;
    out[i] = 0;
    if (after)
        *after = q;
    return 0;
}

/* "v1.2.3" vs "1.2": <0, 0, >0 */
static int vcmp(const char *a, const char *b)
{
    while (*a && (*a < '0' || *a > '9'))
        a++;
    while (*b && (*b < '0' || *b > '9'))
        b++;
    for (;;) {
        long x = strtol(a, (char **)&a, 10), y = strtol(b, (char **)&b, 10);
        int ma, mb;
        if (x != y)
            return x < y ? -1 : 1;
        ma = *a == '.';
        mb = *b == '.';
        if (!ma && !mb)
            return 0;
        if (ma)
            a++;
        if (mb)
            b++;
    }
}

static void do_check(void)
{
    Buf b = { 0 };
    const char *p, *q, *u = NULL;
    char url[512];
    int r;
    set(UPD_BUSY, "Checking GitHub for updates ...");
    r = get("https://api.github.com/repos/" RECVX_UPDATE_REPO "/releases/latest", &b, 0);
    if (r == -2) { set(UPD_ERROR, "cacert.pem is missing from the app folder"); return; }
    if (r) { free(b.p); set(UPD_ERROR, "No release found yet (or no Wi-Fi)"); return; }
    p = (const char *)b.p;
    zip_url[0] = 0;
    if (json_str(p, "tag_name", tag, sizeof tag, NULL)) { free(b.p); set(UPD_ERROR, "No release found yet"); return; }
    q = p;
    while (!json_str(q, "browser_download_url", url, sizeof url, &u)) {      /* the first .zip */
        size_t l = strlen(url);
        if (l > 4 && !strcmp(url + l - 4, ".zip") && !zip_url[0])
            snprintf(zip_url, sizeof zip_url, "%s", url);
        q = u;
    }
    free(b.p);
    if (vcmp(tag, RECVX_VERSION) <= 0) { set(UPD_UPTODATE, "Up to date (v%s, latest %s)", RECVX_VERSION, tag); return; }
    if (!zip_url[0]) { set(UPD_ERROR, "Release %s has nothing to install", tag); return; }
    set(UPD_AVAILABLE, "Update %s available - tap to install", tag);
}

static uint32_t rd16(const uint8_t *p) { return p[0] | p[1] << 8; }
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* the release zip's 3ds/recvx/<file> entries into the app folder */
static int install_zip(const uint8_t *z, size_t n, int *nfiles)
{
    static const char pre[] = "3ds/recvx/";
    size_t e = n >= 22 ? n - 22 : 0;
    uint32_t cnt, cd;
    int pass;
    while (e > 0 && rd32(z + e) != 0x06054b50u)
        e--;
    if (rd32(z + e) != 0x06054b50u)
        return -1;
    cnt = rd16(z + e + 10);
    cd = rd32(z + e + 16);
    *nfiles = 0;
    /* pass 0: unpack every file as .new (and check it); pass 1: swap them in */
    for (pass = 0; pass < 2; pass++) {
        size_t o = cd;
        uint32_t i;
        for (i = 0; i < cnt; i++) {
            uint32_t meth, crc, csz, usz, nl, xl, cl, lo;
            char name[128], dst[400], tmp[410];
            const char *base;
            size_t at;
            uint8_t *out;
            long got;
            FILE *f;
            size_t w;
            if (o + 46 > n || rd32(z + o) != 0x02014b50u)
                return -1;
            meth = rd16(z + o + 10); crc = rd32(z + o + 16); csz = rd32(z + o + 20); usz = rd32(z + o + 24);
            nl = rd16(z + o + 28); xl = rd16(z + o + 30); cl = rd16(z + o + 32); lo = rd32(z + o + 42);
            if (nl >= sizeof name || o + 46 + nl > n)
                return -1;
            memcpy(name, z + o + 46, nl);
            name[nl] = 0;
            o += 46 + nl + xl + cl;
            base = name + sizeof pre - 1;
            if (strncmp(name, pre, sizeof pre - 1) || !*base || strchr(base, '/') || strstr(base, ".."))
                continue;
            snprintf(dst, sizeof dst, "%s/%s", dir, base);
            snprintf(tmp, sizeof tmp, "%s.new", dst);
            if (pass == 1) {
                remove(dst);
                if (rename(tmp, dst))
                    return -3;
                (*nfiles)++;
                continue;
            }
            if (lo + 30 > n || rd32(z + lo) != 0x04034b50u)
                return -1;
            at = lo + 30 + rd16(z + lo + 26) + rd16(z + lo + 28);
            if (at + csz > n || usz > (32u << 20))
                return -1;
            out = (uint8_t *)malloc(usz ? usz : 1);
            if (!out)
                return -2;
            if (meth == 0) { memcpy(out, z + at, usz); got = csz == usz ? (long)usz : -1; }
            else if (meth == 8) got = host_inflate(out, usz, z + at, csz);
            else got = -1;
            if (got != (long)usz || host_crc32(0, out, usz) != crc) { free(out); return -1; }
            f = fopen(tmp, "wb");
            w = f ? fwrite(out, 1, usz, f) : 0;
            if (f && fclose(f))
                w = 0;
            free(out);
            if (w != usz)
                return -3;
        }
    }
    return 0;
}

static void do_install(void)
{
    Buf b = { 0 };
    int nf = 0, r;
    set(UPD_BUSY, "Downloading %s ...", tag);
    if (get(zip_url, &b, 1)) { free(b.p); set(UPD_ERROR, "Download failed (is Wi-Fi on?)"); return; }
    set(UPD_BUSY, "Installing %s ...", tag);
    r = install_zip(b.p, b.n, &nf);
    free(b.p);
    if (r == -3) { set(UPD_ERROR, "Could not write to the SD card"); return; }
    if (r || !nf) { set(UPD_ERROR, "The download was damaged; try again"); return; }
    set(UPD_DONE, "Installed %s: quit and start the game again", tag);
}

static void thread_main(void *arg)
{
    (void)arg;
    if (soc_up()) {
        set(UPD_ERROR, "Network start failed (is Wi-Fi on?)");
        return;
    }
    curl_global();
    if (want_install)
        do_install();
    else
        do_check();
    socExit();
}

static void start(int install)
{
    s32 prio = 0x30;
    if (thr) {
        if (state == UPD_BUSY)
            return;
        threadJoin(thr, U64_MAX);
        threadFree(thr);
        thr = NULL;
    }
    want_install = install;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    state = UPD_BUSY;
    /* off the game's core: the system core (the audio thread has core 2) */
    thr = threadCreate(thread_main, NULL, 256 * 1024, prio + 1, 1, false);
    if (!thr)
        thr = threadCreate(thread_main, NULL, 256 * 1024, prio + 1, 2, false);
    if (!thr)
        thr = threadCreate(thread_main, NULL, 256 * 1024, prio + 1, -1, false);
    if (!thr)
        set(UPD_ERROR, "Could not start the updater");
}

void update_check(const char *app_dir)
{
    if (!lk_init) {
        LightLock_Init(&lk);
        lk_init = 1;
    }
    snprintf(dir, sizeof dir, "%s", app_dir);
    start(0);
}

void update_install(void)
{
    if (!lk_init || state != UPD_AVAILABLE)
        return;
    start(1);
}

void update_shutdown(void)
{
    if (thr) {
        threadJoin(thr, U64_MAX);
        threadFree(thr);
        thr = NULL;
    }
}

#else /* no libcurl at build time */

void update_check(const char *app_dir)
{
    (void)app_dir;
    if (!lk_init) {
        LightLock_Init(&lk);
        lk_init = 1;
    }
    set(UPD_ERROR, "This build has no updater (built without 3ds-curl)");
}
void update_install(void) {}
void update_shutdown(void) {}
#endif

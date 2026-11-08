/* disc.c: the user's disc image (ISO, or raw BIN at 2352 bytes a sector)
 * and its ISO 9660 file system.  Plain stdio; no game headers. */
#include "disc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static FILE *g_f;
static unsigned g_sector = 2048, g_offset = 0;
static unsigned g_root_lsn, g_root_size;
static char *g_iobuf;

static int raw_read(unsigned lsn, void *buf, unsigned n)
{
    unsigned i;
    unsigned char *b = buf;
    if (!g_f)
        return -1;
    if (g_sector == 2048) {
        if (fseek(g_f, (long)lsn * 2048, SEEK_SET) != 0)
            return -1;
        return fread(b, 2048, n, g_f) == n ? 0 : -1;
    }
    for (i = 0; i < n; i++) {
        if (fseek(g_f, (long)(lsn + i) * g_sector + g_offset, SEEK_SET) != 0)
            return -1;
        if (fread(b + i * 2048, 1, 2048, g_f) != 2048)
            return -1;
    }
    return 0;
}

int disc_open(const char *path)
{
    static const struct { unsigned sector, offset; } layouts[] = {
        { 2048, 0 }, { 2352, 24 }, { 2352, 16 }, { 2336, 8 },
    };
    unsigned char pvd[2048];
    unsigned i;
    disc_close();
    g_f = fopen(path, "rb");
    if (!g_f)
        return -1;
    if (!g_iobuf)
        g_iobuf = malloc(64 * 1024);
    if (g_iobuf)
        setvbuf(g_f, g_iobuf, _IOFBF, 64 * 1024);
    for (i = 0; i < sizeof layouts / sizeof layouts[0]; i++) {
        g_sector = layouts[i].sector;
        g_offset = layouts[i].offset;
        if (raw_read(16, pvd, 1) == 0 && memcmp(pvd + 1, "CD001", 5) == 0) {
            g_root_lsn = pvd[158] | pvd[159] << 8 | pvd[160] << 16 | (unsigned)pvd[161] << 24;
            g_root_size = pvd[166] | pvd[167] << 8 | pvd[168] << 16 | (unsigned)pvd[169] << 24;
            return 0;
        }
    }
    disc_close();
    return -2;
}

void disc_close(void)
{
    if (g_f)
        fclose(g_f);
    g_f = NULL;
}

int disc_is_open(void) { return g_f != NULL; }

int disc_read(unsigned lsn, unsigned count, void *buf)
{
    return raw_read(lsn, buf, count);
}

/* compare an ISO name (may end in ";1") with a path component */
static int name_eq(const unsigned char *iso, int n, const char *comp, int cn)
{
    int i;
    while (n > 0 && iso[n - 1] != ';' && 0)
        ;
    for (i = 0; i < n; i++)
        if (iso[i] == ';') {
            n = i;
            break;
        }
    if (n > 0 && iso[n - 1] == '.')   /* "NAME." for files without an extension */
        n--;
    while (cn > 0 && comp[cn - 1] == '.')
        cn--;
    if (n != cn)
        return 0;
    for (i = 0; i < n; i++)
        if (toupper(iso[i]) != toupper((unsigned char)comp[i]))
            return 0;
    return 1;
}

static int dir_find(unsigned lsn, unsigned size, const char *comp, int cn,
                    unsigned *out_lsn, unsigned *out_size, int *is_dir)
{
    unsigned char sec[2048];
    unsigned off, s;
    for (s = 0; s * 2048 < size; s++) {
        if (raw_read(lsn + s, sec, 1) != 0)
            return -1;
        for (off = 0; off < 2048;) {
            unsigned len = sec[off];
            if (len == 0)
                break;
            if (off + 33 <= 2048 && name_eq(sec + off + 33, sec[off + 32], comp, cn)) {
                const unsigned char *r = sec + off;
                *out_lsn = r[2] | r[3] << 8 | r[4] << 16 | (unsigned)r[5] << 24;
                *out_size = r[10] | r[11] << 8 | r[12] << 16 | (unsigned)r[13] << 24;
                *is_dir = (r[25] & 2) != 0;
                return 0;
            }
            off += len;
        }
    }
    return -1;
}

int disc_find(const char *path, unsigned *lsn, unsigned *size)
{
    unsigned l = g_root_lsn, sz = g_root_size;
    const char *p = path;
    int is_dir = 1;
    if (!g_f)
        return -1;
    /* skip a device prefix ("cdrom0:") */
    {
        const char *c = strchr(p, ':');
        if (c)
            p = c + 1;
    }
    for (;;) {
        const char *e;
        int n;
        while (*p == '\\' || *p == '/')
            p++;
        if (!*p)
            break;
        e = p;
        while (*e && *e != '\\' && *e != '/')
            e++;
        n = (int)(e - p);
        {   /* drop ";1" */
            int k;
            for (k = 0; k < n; k++)
                if (p[k] == ';') {
                    n = k;
                    break;
                }
        }
        if (!is_dir || dir_find(l, sz, p, n, &l, &sz, &is_dir) != 0)
            return -1;
        p = e;
    }
    *lsn = l;
    *size = sz;
    return 0;
}

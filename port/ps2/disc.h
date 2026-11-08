/* disc.h: the user's disc image, read by 2048-byte sector. */
#pragma once

int disc_open(const char *path);     /* 0 = ok; -1 no file; -2 not ISO 9660 */
void disc_close(void);
int disc_is_open(void);
int disc_read(unsigned lsn, unsigned count, void *buf);   /* 0 = ok */
/* path like "cdrom0:\\DIR\\FILE.EXT;1" or "/DIR/FILE.EXT": 0 = found */
int disc_find(const char *path, unsigned *lsn, unsigned *size);

/* port_pre.h: force-included before every game file in the port build. */
#pragma once
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdarg.h>
#define __gnuc_va_list va_list
/* the Dreamcast SDK (sg_xpt.h) declares its own enum bool */
#undef bool
#undef true
#undef false

/* MWCC built-ins the game calls by name */
#define _builtin_strcmp strcmp
#define _builtin_strcpy strcpy
#define _builtin_strlen strlen
#define _builtin_memcpy memcpy
#define _builtin_memset memset

/* EE address-segment tricks (uncached / uncached-accelerated mirrors):
 * the PS2 build ORs the segment into the pointer; the port leaves it. */
#ifdef RECVX_PORT
#define PS2_UNCACHED_ACCEL(p) ((void *)(p))
#define PS2_UNCACHED(p) ((void *)(p))
#else
#define PS2_UNCACHED_ACCEL(p) ((void *)((int)(p) | 0x30000000))
#define PS2_UNCACHED(p) ((void *)((int)(p) | 0x20000000))
#endif

/* the EE's 16 KB scratchpad RAM (WORKBASE in the game) */
extern unsigned char ps2_scratchpad[16384];
#define PS2_SCRATCHPAD_ADDR 0x70000000u

/* the game's printf goes to the port's log (port/platform/3ds/sys_3ds.c) */
#ifdef RECVX_PORT
int recvx_printf(const char *fmt, ...);
#define printf recvx_printf
#endif

/* trace.c: with -finstrument-functions (RECVX_TRACE), every game function
 * entry lands here; the last ones are kept so the hang heartbeat
 * (kernel.c) can say where the game is.  Addresses are looked up in
 * recvx.elf with addr2line. */
#include "plat.h"
#include <stdio.h>

#define NTRACE 16
static volatile unsigned trace_ring[NTRACE];
static volatile unsigned trace_pos;
volatile unsigned trace_count;

__attribute__((no_instrument_function))
void __cyg_profile_func_enter(void *fn, void *site)
{
    extern int ps2_cur_thread;
    (void)site;
    if (ps2_cur_thread != 1)        /* the game's main thread only */
        return;
    if (trace_ring[(trace_pos - 1) & (NTRACE - 1)] == (unsigned)(unsigned long)fn) {
        trace_count++;
        return;
    }
    trace_ring[trace_pos & (NTRACE - 1)] = (unsigned)(unsigned long)fn;
    trace_pos++;
    trace_count++;
}

__attribute__((no_instrument_function))
void __cyg_profile_func_exit(void *fn, void *site) { (void)fn; (void)site; }

__attribute__((no_instrument_function))
void trace_dump(void)
{
    char buf[256];
    int i, n = 0;
    unsigned p = trace_pos;
    for (i = NTRACE; i > 0; i--)
        n += snprintf(buf + n, sizeof buf - n, " %08X", trace_ring[(p - i) & (NTRACE - 1)]);
    plat_log("  last functions (oldest first):%s; calls %u\n", buf, trace_count);
}

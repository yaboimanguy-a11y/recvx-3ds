/* kernel.c: the EE kernel calls the game makes (threads, semaphores,
 * interrupts, caches).
 *
 * Threads follow the PS2's single-CPU model: exactly one runs at a time.
 * A woken thread (the ADX sound server, woken from the vblank callback)
 * runs until it sleeps again, then the waker continues - like a
 * higher-priority thread preempting on the PS2.  Each game thread is a real
 * platform thread; a baton (one semaphore per thread) makes sure only one
 * of them runs. */
#include "types.h"
#include "plat.h"

#define MAX_THREADS 16
enum { T_FREE, T_DORMANT, T_READY, T_RUNNING, T_SLEEP, T_DONE };

typedef struct {
    int state;
    void (*entry)(void *);
    void *arg;
    int prio;
    int wakeups;
    int suspended;
    int started;
    int resume_to;
    PlatSem *go;
} KThread;

static KThread th[MAX_THREADS];
int ps2_cur_thread = 1;        /* thread 1 is the game's main thread; 0 = interrupt */
#define cur ps2_cur_thread
static int kinit;

__attribute__((no_instrument_function)) static void kernel_init(void)
{
    if (kinit)
        return;
    kinit = 1;
    th[1].state = T_RUNNING;
    th[1].prio = 1;
    th[1].go = plat_sem_new(0);
    /* thread 0: interrupt context (the vblank thread, plat_start_vblank) */
    th[0].state = T_RUNNING;
    th[0].go = plat_sem_new(0);
}

static void thread_body(void *p)
{
    int id = (int)(size_t)p;
    plat_sem_wait(th[id].go);
    th[id].entry(th[id].arg);
    th[id].state = T_DONE;
    plat_sem_post(th[th[id].resume_to].go);
}

/* run thread id until it sleeps (or ends), then come back */
u_int ps2_stat_wakes;
static void switch_to(int id)
{
    ps2_stat_wakes++;
    int me = cur;
    th[id].resume_to = me;
    th[id].state = T_RUNNING;
    cur = id;
    plat_sem_post(th[id].go);
    plat_sem_wait(th[me].go);
    cur = me;
}

int CreateThread(struct ThreadParam *p)
{
    int i;
    kernel_init();
    for (i = 2; i < MAX_THREADS; i++)
        if (th[i].state == T_FREE) {
            memset(&th[i], 0, sizeof th[i]);
            th[i].state = T_DORMANT;
            th[i].entry = p->entry;
            th[i].prio = p->initPriority;
            th[i].go = plat_sem_new(0);
            return i;
        }
    plat_log("kernel: out of threads\n");
    return -1;
}

int StartThread(int id, void *arg)
{
    kernel_init();
    if (id < 2 || id >= MAX_THREADS || th[id].state != T_DORMANT)
        return -1;
    th[id].arg = arg;
    th[id].state = T_SLEEP;   /* runs on its first wakeup */
    if (plat_thread_new(thread_body, (void *)(size_t)id, 64 * 1024) != 0) {
        plat_log("kernel: thread %d not created\n", id);
        th[id].state = T_DONE;
        return -1;
    }
    th[id].started = 1;
    return id;
}

int DeleteThread(int id) { if (id > 1 && id < MAX_THREADS) th[id].state = T_FREE; return 0; }
int TerminateThread(int id) { if (id > 1 && id < MAX_THREADS) th[id].state = T_DONE; return 0; }
int GetThreadId(void) { kernel_init(); return cur; }
int SuspendThread(int id) { if (id > 0 && id < MAX_THREADS) th[id].suspended = 1; return 0; }
int ResumeThread(int id) { if (id > 0 && id < MAX_THREADS) th[id].suspended = 0; return 0; }
int ChangeThreadPriority(int id, int prio) { (void)id; (void)prio; return 0; }
int RotateThreadReadyQueue(int prio) { (void)prio; return 0; }

int ReferThreadStatus(int id, struct ThreadParam *p)
{
    kernel_init();
    if (id <= 0 || id >= MAX_THREADS)
        return -1;
    memset(p, 0, sizeof *p);
    p->currentPriority = p->initPriority = th[id].prio;
    p->status = th[id].state;
    return 0;
}

int WakeupThread(int id)
{
    kernel_init();
    if (id <= 0 || id >= MAX_THREADS)
        return -1;
    if (th[id].state == T_SLEEP && !th[id].suspended && id != cur)
        switch_to(id);
    else
        th[id].wakeups++;
    return 0;
}

int iWakeupThread(int id) { return WakeupThread(id); }

int SleepThread(void)
{
    int me;
    kernel_init();
    me = cur;
    if (th[me].wakeups > 0) {
        th[me].wakeups--;
        return 0;
    }
    if (me == 1)        /* the main thread never really sleeps here */
        return 0;
    th[me].state = T_SLEEP;
    plat_sem_post(th[th[me].resume_to].go);
    plat_sem_wait(th[me].go);
    return 0;
}

/* ---- semaphores (counters; no other thread could signal while we wait) ---- */
#define MAX_SEMA 64
static struct { int used, count, max; } sema[MAX_SEMA];

int CreateSema(struct SemaParam *p)
{
    int i;
    for (i = 1; i < MAX_SEMA; i++)
        if (!sema[i].used) {
            sema[i].used = 1;
            sema[i].count = p->initCount;
            sema[i].max = p->maxCount > 0 ? p->maxCount : 0x7fffffff;
            return i;
        }
    return -1;
}

int DeleteSema(int s) { if (s > 0 && s < MAX_SEMA) sema[s].used = 0; return s; }

int SignalSema(int s)
{
    if (s <= 0 || s >= MAX_SEMA)
        return -1;
    if (sema[s].count < sema[s].max)
        sema[s].count++;
    return s;
}

int iSignalSema(int s) { return SignalSema(s); }

int WaitSema(int s)
{
    if (s <= 0 || s >= MAX_SEMA)
        return -1;
    if (sema[s].count > 0)
        sema[s].count--;
    return s;
}

int PollSema(int s)
{
    if (s <= 0 || s >= MAX_SEMA || sema[s].count <= 0)
        return -1;
    sema[s].count--;
    return s;
}

/* ---- interrupts and vblank ------------------------------------------------ */
static int (*intc[16])(int);
static int (*syncv_cb)(int);

int AddIntcHandler(int cause, int (*h)(int), int next) { (void)next; if (cause >= 0 && cause < 16) intc[cause] = h; return cause; }
int EnableIntc(int cause) { (void)cause; return 0; }
int DisableIntc(int cause) { (void)cause; return 0; }
void ExitHandler(void) {}
int DI(void) { return 0; }
int EI(void) { return 0; }

int sceGsSyncVCallback(int (*cb)(int))
{
    int (*old)(int) = syncv_cb;
    syncv_cb = cb;
    return (int)(size_t)old;
}

/* one PS2 vblank: start (cause 2), the libgraph callback, end (cause 3).
 * Runs on the platform's 60 Hz thread, which preempts the game thread on
 * the same core (as the interrupt does on the PS2); a thread it wakes runs
 * until it sleeps, then the game continues. */
static void heartbeat(void);
__attribute__((no_instrument_function)) static void vblank_irq(void)
{
    int saved;
    kernel_init();
    saved = cur;
    cur = 0;
    if (intc[2])
        intc[2](2);
    if (syncv_cb)
        syncv_cb(0);
    if (intc[3])
        intc[3](3);
    cur = saved;
    heartbeat();
}

void ps2_vblank(void) { vblank_irq(); }

/* heartbeat: every 5 s without a new frame, say what the game is doing */
extern u_int ps2_stat_rpc, ps2_stat_cd;
u_int ps2_stat_frames;
__attribute__((no_instrument_function)) static void heartbeat(void)
{
    static u_int ticks, last_frames, last_rpc, last_cd;
    if (++ticks % 300)
        return;
    if (ps2_stat_frames != last_frames) {
        last_frames = ps2_stat_frames;
        return;
    }
    plat_log("[waiting] frames %u, disc reads +%u, IOP calls +%u\n",
             ps2_stat_frames, ps2_stat_cd - last_cd, ps2_stat_rpc - last_rpc);
    {
        extern void trace_dump(void) __attribute__((weak));
        if (trace_dump)
            trace_dump();
    }
    last_cd = ps2_stat_cd;
    last_rpc = ps2_stat_rpc;
}

void ps2_start_vblank(void)
{
    kernel_init();
    plat_start_vblank(vblank_irq);
}

typedef void (*AlarmCB)(int, unsigned short, void *);
int SetAlarm(unsigned short time, AlarmCB cb, void *arg)
{
    if (cb)
        cb(1, time, arg);
    return 1;
}

/* ---- caches, misc ---------------------------------------------------------- */
void FlushCache(int op) { (void)op; }
void SyncDCache(void *a, void *b) { (void)a; (void)b; }
void InvalidDCache(void *a, void *b) { (void)a; (void)b; }
void *GetGP(void) { return 0; }
void sceVpu0Reset(void) {}

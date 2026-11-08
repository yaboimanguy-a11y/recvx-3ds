/* movie_stub.c: the Sofdec movie player, skipped.  The game asks for a
 * player object; this one reports "playing" for a moment and then
 * "finished" (status 3), so every movie ends at once and the game goes on
 * exactly as after a real one.  Every other method does nothing. */
#include "types.h"
#include "mwwrap.h"
#include "ps2_sfd_mw.h"
#include "plat.h"

typedef __typeof__(*(MWPLY)0) MwPlyObj;
typedef __typeof__(*((MWPLY)0)->vtbl) MwPlyVtbl;

static MwPlyObj dummy_ply;
static MwPlyVtbl dummy_vtbl;
static int dummy_polls;

static int dummy_zero(void) { return 0; }

static MWE_PLY_STAT dummy_stat(MWPLY ply)
{
    (void)ply;
    
    /* the game waits for "playing" once (WaitPrePlayMovie), then plays
     * until "finished" (PlayMovieMain): two checks of "playing" (one in
     * PlayMwMain2, one in WaitPrePlayMovie), then finished.  The decoder
     * (mwPlyExecServer) is switched off in the port, so the playing checks
     * do nothing else. */
    return (MWE_PLY_STAT)(dummy_polls++ < 2 ? MWE_PLY_STAT_PLAYING : 3);
}

MWPLY ps2mwPlyCreateSofdec(MWS_PLY_CPRM_SFD *cprm, char *fname)
{
    void (**slot)(void) = (void (**)(void))&dummy_vtbl;
    unsigned i;
    (void)cprm;
    for (i = 0; i < sizeof dummy_vtbl / sizeof *slot; i++)
        slot[i] = (void (*)(void))dummy_zero;
    dummy_vtbl.GetStat = dummy_stat;
    dummy_ply.vtbl = &dummy_vtbl;
    dummy_polls = 0;
    plat_log("movie skipped: %s\n", fname ? fname : "?");
    return &dummy_ply;
}

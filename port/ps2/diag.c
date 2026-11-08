/* diag.c: a status line for the log (every 2 s from gs_frame_end): which
 * system task runs, the front-end (Adv) state, and the file reader. */
#include "types.h"
#include "main.h"
#include "adv.h"
#include "ps2_SystemLoadScreen.h"
#include "ps2_sg_pad.h"
#include "plat.h"

extern int FileReadStatus;
extern int ReadFileRequestFlag;
extern int GenAdxfSlot;
extern u_int ps2_stat_wakes;
extern ADV_WORK AdvWork;
extern SYSLOAD_SCREEN SysLoad;
extern PAD_STATUS Pad_status;
extern unsigned char Pad_rdata1[];
extern unsigned int pad_reads;

void diag_line(void)
{
    ADV_WORK *ap = (ADV_WORK *)&AdvWork;
    plat_log("[game] tasks %08X (stopped %08X), adv mode %d/%d, file read: request %d status %d, "
             "adx server runs %u\n",
             (u_int)sys->tk_flg, (u_int)sys->ts_flg, ap->Mode, ap->Mode2,
             ReadFileRequestFlag, FileReadStatus, ps2_stat_wakes);
    {
        MEMORYCARDSTATE *mc = SysLoad.pMcState;
        plat_log("[pad] handshake step %u flags %X, raw %02X %02X %02X %02X, pad reads %u\n",
                 Pad_status.routine_0, Pad_status.be_flag, Pad_rdata1[0], Pad_rdata1[1],
                 Pad_rdata1[2], Pad_rdata1[3], pad_reads);
        plat_log("[card] load screen state %u mes %d exit %u; card state %u sub %u sys %u err %u\n",
                 SysLoad.ulState, SysLoad.cMesFlag, SysLoad.usExitFlag,
                 mc ? mc->ulState : 0, mc ? mc->ulMcSubState : 0, mc ? mc->usMcSysState : 0,
                 mc ? mc->ulError : 0);
    }
}

/* a number that changes with the screen: system tasks and front-end step */
u_int diag_state(void)
{
    ADV_WORK *ap = (ADV_WORK *)&AdvWork;
    return ((u_int)sys->tk_flg & 0x00FFFFFF) | ((u_int)(ap->Mode & 0xFF) << 24);
}

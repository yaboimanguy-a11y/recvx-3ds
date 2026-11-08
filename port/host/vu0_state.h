/* vu0_state.h: the VU0 registers whose values the game's assembly passed
 * from one function to another; the port's C versions keep them here. */
#pragma once

typedef struct {
    float cn[16];     /* vf28..vf31: the "CN" matrix (njSetMatrixCN, njMulMatrixCN) */
    float vf18[4];    /* last transformed point */
    float vf11[4];    /* njSinCos: sin */
    float vf12[4];    /* njSinCos: cos */
    float clip[16];   /* vf24..vf27: clip matrix (_Make_ClipMatrix) */
} Vu0State;

extern Vu0State vu0;

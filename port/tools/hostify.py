#!/usr/bin/env python3
"""hostify.py OUT_DIR: copy the game's C sources and headers (src/, include/ps2,
include/JPN, include/cri) into OUT_DIR with the source-level fixes a 32-bit
host compiler needs.  The original files stay as they are (they still build
the PS2 executable with MWCC).

Transforms (outside comments and strings):
  1. EE `long` is 64-bit: a lone `long` becomes `long long`
     (`long long`, `long double` untouched).
  2. MWCC lvalue casts:
        (T*)X += n;            ->  X = (void *)((T*)X + n);
        *((T*)X)++             ->  *(*(T**)&X)++
        ((T*)X)++              ->  (*(T**)&X)++
Only files whose text changes are written again, so a rebuild is cheap."""
import os
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TREES = ["src", "include/ps2", "include/JPN", "include/cri"]

TOKEN = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'', re.S)


def code_parts(text):
    """yield (is_code, chunk)"""
    pos = 0
    for m in TOKEN.finditer(text):
        if m.start() > pos:
            yield True, text[pos:m.start()]
        yield False, m.group(0)
        pos = m.end()
    if pos < len(text):
        yield True, text[pos:]


LONG = re.compile(r"(?<![\w])long(?![\w])")
T = r"(?:unsigned\s+|signed\s+)?(?:long\s+long|long|int|char|short|float|u_long128|u_long|u_int|u_char|u_short|void)"
LV_ADD = re.compile(r"(?<![*\w)\]])\((" + T + r")\s*\*\)\s*(\w+(?:\.\w+|->\w+)*)\s*([+-])=\s*([^;]+);")
LV_DEREF_INC = re.compile(r"\*\(\((" + T + r")\s*\*\)\s*(\w+(?:\.\w+|->\w+)*)\)(\+\+|--)")
LV_INC = re.compile(r"\(\((" + T + r")\s*\*\)\s*(\w+(?:\.\w+|->\w+)*)\)(\+\+|--)")


def fix_long(code):
    out = []
    toks = re.split(r"(\W)", code)
    # work on word tokens with whitespace awareness
    words = [(i, t) for i, t in enumerate(toks) if t and re.match(r"\w", t)]
    longs = {i for i, t in words if t == "long"}
    idx = [i for i, _ in words]
    skip = set()
    for n, i in enumerate(idx):
        if i not in longs:
            continue
        prev = toks[idx[n - 1]] if n > 0 else ""
        nxt = toks[idx[n + 1]] if n + 1 < len(idx) else ""
        if prev == "long" or nxt in ("long", "double"):
            continue
        skip.add(i)
    for i, t in enumerate(toks):
        out.append("long long" if i in skip else t)
    return "".join(out)


def fix_lvalues(code):
    code = LV_ADD.sub(lambda m: f"{m.group(2)} = (void *)(({m.group(1)} *){m.group(2)} {m.group(3)} ({m.group(4)}));", code)
    code = LV_DEREF_INC.sub(lambda m: f"*(*({m.group(1)} **)&{m.group(2)}){m.group(3)}", code)
    code = LV_INC.sub(lambda m: f"(*({m.group(1)} **)&{m.group(2)}){m.group(3)}", code)
    return code


KEY_INC = re.compile(r"\*\s*(\w+)->(\w+)\+\+|\b(\w+)\s*[-+]?=(?!=)|([{}])")


def fix_array_inc(code):
    """MWCC took `*p->arr++` on an array member as reading arr[0], arr[1], ...
    in turn (checked against the retail code of bhEne03_DG02).  The index
    restarts when p is assigned and at every brace."""
    count = {}

    def sub(m):
        if m.group(4):                      # a block boundary: restart all
            count.clear()
            return m.group(0)
        if m.group(3):                      # p = / p += ...: restart p's counters
            for k in [k for k in count if k[0] == m.group(3)]:
                del count[k]
            return m.group(0)
        key = (m.group(1), m.group(2))
        if key not in ARRAY_MEMBERS:
            return m.group(0)
        n = count.get(key, 0)
        count[key] = n + 1
        return f"{m.group(1)}->{m.group(2)}[{n}]"
    return KEY_INC.sub(sub, code)


# (pointer variable, array member) pairs the game increments
ARRAY_MEMBERS = {("mkfP", "key"), ("mkaP", "key")}


LV_ASSIGN = re.compile(r"(?<![*\w)\]])\((" + T + r")\s*\*\)\s*(\w+)\s*=(?!=)\s*([^;]+);")
ZERO_Q = re.compile(r"\*\(u_long128\s*\*\)\s*([^=;]+?)\s*=\s*0\s*;")


CVT_OTZ = re.compile(r"asm volatile \(cvt\.w\.s f12, f12\);\s*asm volatile \(mfc1\s+otz, f12\);")


def fix_misc(code):
    code = re.sub(r"(?<!\w)__int128(?!\w)", "u_long128", code)
    # Ps2AddOT: float z -> int (the EE FPU truncates)
    code = CVT_OTZ.sub("otz = (unsigned int)(int)z;", code)
    # the scratchpad passed by its EE address
    code = code.replace("(void*)0x70000000", "(void*)ps2_scratchpad")
    code = LV_ASSIGN.sub(lambda m: f"{m.group(2)} = (void *)({m.group(3)});", code)
    code = ZERO_Q.sub(lambda m: f"memset({m.group(1)}, 0, 16);", code)
    return code


# one-off fixes: (file suffix, old, new)
FILE_FIXES = [
    # MWCC passed the struct's address where a pointer was expected
    ("sdfunc.c", "memcpy(Room_SoundEnv,", "memcpy(&Room_SoundEnv,"),
    # called before their static definition (MWCC allowed it; clang does not)
    ("dvci.c", "static void dvci_wait(void)", "void dvci_wait(void)"),
    ("effect.c", "static void bhDrawThunder()", "void bhDrawThunder()"),
    # the EE scratchpad (16 KB at 0x70000000) and uncached mirror: plain memory
    ("types.h", "#define WORKBASE (0x70000000)", "#define WORKBASE ((u_int)ps2_scratchpad)"),
    ("types.h", "#define UNCACHED(x) ((u_int)(x)|UNCACHED_BASE)", "#define UNCACHED(x) ((u_int)(x))"),
    # CRI's uncached-memory pointer bit: the port's memory has no such mirror
    ("dtx.h", "#define UNCBASE 0x20000000", "#define UNCBASE 0"),
    ("types.h", "#define UNCBASE 0x20000000", "#define UNCBASE 0"),
    # pointers cut to the EE's 24 bits: the port's memory is above 16 MB
    ("ps2_dummy.c", "loadImage((void*)((int)start_addr & 0xFFFFFF));", "loadImage(start_addr);"),
    ("ps2_MovieFunc.c", "((unsigned int)image & 0xFFFFFF) << 32", "((unsigned int)image) << 32"),
    # the draw-list packet buffer through the PS2's uncached mirror
    # (| 0x30000000): on the 3DS that address is other memory, so later
    # links (cut to 28 bits) found nothing and only one entry drew
    ("ps2_dummy.c", "Ps2_PP = (void*)((int)&Ps2_PBUFF | 0x30000000);", "Ps2_PP = (void*)((int)&Ps2_PBUFF);"),
    # models: port/ps2/vu_strip.c replaces the VU0 / VU1 code and needs the
    # vertex buffer and the model clip flag; the game's own C strip drawers
    # that went through the PS2 scissor code are replaced there too
    ("ps2_NinjaCnk.c", "static VU1_STRIP_BUF* pNaCnkVerBufTop;", "VU1_STRIP_BUF* pNaCnkVerBufTop;"),
    ("ps2_NinjaCnk.c", "static unsigned int Vu0ClipFlag;", "unsigned int Vu0ClipFlag;"),
    # widescreen: the projection's x scale (and the clip volume made from
    # it) narrowed by the port, so more of the scene fits (port/ps2/radar.c)
    ("ps2_NaView.c", "void    njSetAspect(Float ax, Float ay)\n{ \n", "void    njSetAspect(Float ax, Float ay)\n{ \n    { extern float port_aspect_hook(float, float); ax = port_aspect_hook(ax, ay); }\n"),
    # motion blending flips a joint's angles to the same rotation (x + 180,
    # 180 - y, z + 180); the original's unsequenced *p = .. *p++ writes give
    # the wrong slots with clang (limbs flying when an animation changes)
    ("Motion.c", "*srcP = 32768 + *srcP++;", "srcP[0] += 32768; srcP++;"),
    ("Motion.c", "*srcP = 32768 + *++srcP;", "srcP++; srcP[0] += 32768;"),
    ("playpch.c", "*srcP = 32768 + *srcP++;", "srcP[0] += 32768; srcP++;"),
    ("playpch.c", "*srcP = 32768 + *++srcP;", "srcP++; srcP[0] += 32768;"),
    # face mesh: the PS2 reads the index, then stores and steps (unsequenced in C)
    ("face_bh.c", "*nlist++ = (int)&dvp[*nlist * vofs];", "*nlist = (int)&dvp[*nlist * vofs]; nlist++;"),
    # the door / stairs scene: the port's mist is off while it plays (gs.c)
    # Doors: Fast (settings): bhControlDoor takes spd_mde + 1 steps a frame
    ("door.c", "    dwP->spd_mde = DMO_SPD_1;\n    dwP->spd_mde = DMO_SPD_1;\n", "    { extern int port_door_speed(void); dwP->spd_mde = port_door_speed(); }\n"),
    # Save State / Load State (settings): done between the game's tasks (port/ps2/radar.c)
    ("main.c", "    PS2_jikken(); \n", "    { extern void port_qs_service(void); port_qs_service(); }\n    PS2_jikken(); \n"),
    ("door.c", "void bhInitDoor()\n{\n", "void bhInitDoor()\n{\n    { extern int g_port_door; g_port_door = 1; }\n"),
    ("door.c", "void bhSetDoor()\n{\n", "void bhSetDoor()\n{\n    { extern int g_port_door; g_port_door = 1; }\n"),
    ("door.c", "void bhExitDoor()\n{\n", "void bhExitDoor()\n{\n    { extern int g_port_door; g_port_door = 0; }\n"),
    # chunk types the game has no drawer for (counted for the log)
    ("ps2_NinjaCnk.c", "CHUNK_HEAD* njCnkDefaultLong(CHUNK_HEAD* pCnk)\n{ \n", "CHUNK_HEAD* njCnkDefaultLong(CHUNK_HEAD* pCnk)\n{ \n    { extern void cnk_skip_note(int, int); cnk_skip_note(pCnk->ucType, pCnk->usSize); }\n"),
    ("ps2_Vu1Strip.c", "void vu1DrawTriangleStripOpaqueSingle(unsigned long long ulType", "void vu1DrawTriangleStripOpaqueSingle_ps2(unsigned long long ulType"),
    ("ps2_Vu1Strip.c", "void vu1DrawTriangleStripOpaqueDouble(unsigned long long ulType", "void vu1DrawTriangleStripOpaqueDouble_ps2(unsigned long long ulType"),
    ("ps2_Vu1Strip.c", "void vu1DrawTriangleStripTransSingle(unsigned long long ulType", "void vu1DrawTriangleStripTransSingle_ps2(unsigned long long ulType"),
    # movies are skipped (port/ps2/movie_stub.c): no decoder to run
    ("ps2_sfd_mw.c", "void mwPlyExecServer(void)\n{\n", "void mwPlyExecServer(void)\n{\n    return;   /* port: movies skipped */\n"),
    # the C library's index() (only printed), declared with other constness
    ("ps2_NaTextureFunction.c", "char* index(char*, int);", ""),
    # ADXT_StopWithoutLsc reaches into PS2RNA's IOP transfer objects; the
    # port's renderer (port/ps2/adx_out.c) has none: stopping transfer and
    # play (which drops what is queued) is the same thing there
    ("adx_tlk.c", "    rna = adxt->rna;\n    \n    nch = ADXT_GetNumChan(adxt);",
     "    ADXRNA_SetTransSw(adxt->rna, 0);\n    if (0) {   /* port: PS2RNA internals */\n    rna = adxt->rna;\n    \n    nch = ADXT_GetNumChan(adxt);"),
    ("adx_tlk.c", "    ADXCRS_Lock();\n    \n    ADXRNA_SetPlaySw(adxt->rna, 0);",
     "    }\n    ADXCRS_Lock();\n    \n    ADXRNA_SetPlaySw(adxt->rna, 0);"),
    # sound effects: the port's synth (port/ps2/snd_synth.c) sees each request
    # and each bank download (the IOP driver itself is not emulated)
    ("ps2_snddrv.c", "\nint SdrSeReq(int req, char vol, char pan, short pitch)\n{", "\nvoid snd_hook_se(int, int, int, int);\nvoid snd_hook_se_cancel(int);\nvoid snd_hook_se_chg(int, int, int, int);\nvoid snd_hook_se_allstop(void);\nvoid snd_hook_mvol(int);\nvoid snd_hook_bgm(int, int, int, int);\nvoid snd_hook_bgm_stop(int);\nvoid snd_hook_hd(int, int);\nvoid snd_hook_bd_done(int);\nvoid snd_hook_hd2(int, int);\nvoid snd_hook_bd_trans(int, int, int, int);\nvoid snd_hook_sq(int, int);\nint SdrSeReq(int req, char vol, char pan, short pitch)\n{\n    snd_hook_se(req, vol, pan, pitch);"),
    ("ps2_snddrv.c", "int SdrSeCancel(int req) \n{", "int SdrSeCancel(int req) \n{\n    snd_hook_se_cancel(req);"),
    ("ps2_snddrv.c", "int SdrSeChg(int req, char vol, char pan, short pitch)\n{", "int SdrSeChg(int req, char vol, char pan, short pitch)\n{\n    snd_hook_se_chg(req, vol, pan, pitch);"),
    ("ps2_snddrv.c", "int SdrSeAllStop()\n{", "int SdrSeAllStop()\n{\n    snd_hook_se_allstop();"),
    ("ps2_snddrv.c", "int SdrMasterVol(unsigned short mvol)\n{", "int SdrMasterVol(unsigned short mvol)\n{\n    snd_hook_mvol(mvol);"),
    ("ps2_snddrv.c", "int SdrBgmReq(unsigned char port, unsigned char bank, unsigned char vol, unsigned char block) \n{", "int SdrBgmReq(unsigned char port, unsigned char bank, unsigned char vol, unsigned char block) \n{\n    snd_hook_bgm(port, bank, vol, block);"),
    ("ps2_snddrv.c", "int SdrBgmStop(unsigned char port) \n{", "int SdrBgmStop(unsigned char port) \n{\n    snd_hook_bgm_stop(port);"),
    ("ps2_snddrv.c", "int SdrHDDataSet(int port, int size) \n{", "int SdrHDDataSet(int port, int size) \n{\n    snd_hook_hd(port, size);"),
    ("ps2_snddrv.c", "int SdrHDDataSet2(int port, int size)\n{", "int SdrHDDataSet2(int port, int size)\n{\n    snd_hook_hd2(port, size);"),
    ("ps2_snddrv.c", "int SdrBDDataSet(int port) \n{", "int SdrBDDataSet(int port) \n{\n    snd_hook_bd_done(port);"),
    ("ps2_snddrv.c", "int SdrBDDataSet2(int port) \n{", "int SdrBDDataSet2(int port) \n{\n    snd_hook_bd_done(port);"),
    ("ps2_snddrv.c", "int SdrBDDataTrans(int port, int adrs, int size, int flag) \n{", "int SdrBDDataTrans(int port, int adrs, int size, int flag) \n{\n    snd_hook_bd_trans(port, adrs, size, flag);"),
    ("ps2_snddrv.c", "int SdrSQDataSet(int port, int size) \n{", "int SdrSQDataSet(int port, int size) \n{\n    snd_hook_sq(port, size);"),
    # the EE cache prefetch hint: nothing to do on the host
    ("macros.h", '#define PREFETCH(_v) asm("pref 0x0,0(%0)": : "r"(_v) : "memory")',
     "#define PREFETCH(_v) ((void)0)"),
]


def transform(text, name=""):
    parts = []
    for is_code, chunk in code_parts(text):
        if is_code:
            chunk = fix_misc(fix_array_inc(fix_lvalues(fix_long(chunk))))
        parts.append(chunk)
    text = "".join(parts)
    for suffix, old, new in FILE_FIXES:
        if name.endswith(suffix):
            text = text.replace(old, new)
    return text


def main():
    out = Path(sys.argv[1])
    n = 0
    for tree in TREES:
        for p in (ROOT / tree).rglob("*"):
            if p.suffix not in (".c", ".h") or not p.is_file():
                continue
            dst = out / p.relative_to(ROOT)
            text = p.read_text(encoding="utf-8", errors="surrogateescape")
            new = transform(text, str(p))
            if dst.exists() and dst.read_text(encoding="utf-8", errors="surrogateescape") == new:
                continue
            dst.parent.mkdir(parents=True, exist_ok=True)
            dst.write_text(new, encoding="utf-8", errors="surrogateescape")
            n += 1
    print(f"hostify: {n} files written to {out}")


if __name__ == "__main__":
    main()

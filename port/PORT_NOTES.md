# RE: Code Veronica X native port: working notes

Targets: **New 3DS** (ARM32, citro3d) and a **Mac app** (WebAssembly in a
native app wrapper). macOS can't run 32-bit code, and the game's structs
assume 4-byte pointers, so wasm32 is the Mac's 32-bit host. Both build from
the same C.

## Status

| Step | State |
| --- | --- |
| Survey: game code is all C; only the CRI IOP middleware (`src/cri/ps2`) is still asm | done |
| `port/shim/`: replacement PS2 SDK headers (types, GS/GIF/VIF, CD, pad, MC, MPEG, hw regs, threads) | done |
| `port/tools/hostify.py`: build-time source fixes (64-bit EE `long`, MWCC lvalue casts, `*p->arr++`) | done |
| `port/tools/census.py`: compile every file for the 3DS with `-fsyntax-only` | done |
| C versions of asm: Ninja maths + matrices (48), skinning (njplus, 10 blocks), face animation (7 blocks), PRS decompressor, bit count, clip matrix | done |
| Compile errors | 191 failing files → **14** (134 errors), all in the draw / movie / sound-driver layers |
| Link, boot test, renderer, sound, input, saves | not started |

Disc: the user's US disc (SLUS_201.84, SHA-1 f6c26ed6…, matches the decomp's config) is read from
~/Downloads; `port/tools/isols.py` lists/extracts files.  `private/` (gitignored) holds the
extracted executable for checking the original code with llvm-objdump.

Run the census: `python3 port/tools/census.py [--target wasm] [--file] [--only NAME]`

### Still failing (all hardware-facing; next to replace)

| Layer | Files | Plan |
| --- | --- | --- |
| Drawing (VU1 strips, clipping, GS packets) | ps2_Vu1Strip, ps2_Vu1Scissor2, ps2_NinjaCnk, ps2_dummy, ps2_loadtim2, ps2_NaDraw2D | stub for the boot test, then the citro3d renderer |
| Movies (Sofdec / MPEG IPU) | ps2_MovieFunc, ps2_sfd_mw | stub (skip movies) |
| Sound driver (IOP RPC) | ps2_snddrv, sdfunc, ps2_sg_sd, cri lsc_err | host mixer |
| VU0 wave micro-program | objitm (bhObj005) | C from ps2_vu0.vsm |
| JPN-only save history | JPN/ps2_sg_uf | leave out (US build) |

## Inline-assembly functions (need C versions)

| File | Functions | Plan |
| --- | --- | --- |
| `ps2_NaMath.c`, `ps2_NaMatrix.c` | 48: Ninja sin/cos/sqrt, matrix and vector maths | plain C |
| `njplus.c`, `face_bh.c` | 11: skinning, face/jaw/eye animation | C from the asm |
| `ps2_Vu1Strip.c`, `ps2_Vu1Scissor2.c`, `ps2_NinjaCnk.c`, `ps2_dummy.c` | 46: VU1 strip/clip drawing, primitive packets | replaced by the port renderer |
| `ps2_loadtim2.c`, `ps2_NaSystem.c`, `ps2_NaView.c` | 5: DMA/GS sync, buffer swap, clip matrix | port renderer / C |
| `ps2_sg_gd.c` | 2: `gdFsInit`, `gdFsRead` (disc file system) | port disc reader (ISO) |
| `ps2_sfd_mw.c` | 1: Sofdec movie player | stub (movies skipped at first) |
| `expand.c`, `objitm.c`, `ps2_sg_sd.c`, `system.c` | 4: decompressor, misc | C from the asm |

## Not part of the port build

- `src/cri/ps2/**`: IOP-side CRI modules (sound/streaming on the PS2's I/O CPU).
- `src/users/oshimi/adx/sjr*`, `src/atick.c`, `src/tq.c`, `src/tsnddrv.c`: IOP sound driver.
  The port mixes sound on the host instead.

## Needed from the user

Their own dump of the US disc, **SLUS-20184** (ISO, or BIN/CUE).

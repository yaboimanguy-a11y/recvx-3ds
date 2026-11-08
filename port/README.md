# Resident Evil Code: Veronica X for the New 3DS

A native New Nintendo 3DS port built on the [recvx-decomp](https://github.com/AshfordFamily/recvx-decomp) reconstruction of the PS2 game. Downloads, the feature list and known issues are on the [releases page](https://github.com/yaboimanguy-a11y/recvx-3ds/releases).

No game data is in this repository or its releases. You need your own dump of the US PS2 disc (SLUS-20184).

## How it works

- `tools/hostify.py` copies the game's sources into the build folder and applies the fixes they need on ARM.
- `ps2/`: replacements for the PS2 side: GS emulation (`gs.c`), models (`vu_strip.c`), threads (`kernel.c`), disc and memory card (`io.c`), sound (`snd_synth.c`, `adx_out.c`), radar and status data (`radar.c`).
- `render/3ds/`: the citro3d renderer. `platform/3ds/`: app, input, audio, bottom screen, updater.
- `shim/`: replacement PS2 SDK headers, so no Sony SDK is needed.

## Building

You need [devkitPro](https://devkitpro.org/) with `3ds-dev` and `3ds-curl`, LLVM clang, CMake, Ninja, Python 3, and the repository's submodules.

```bash
git clone --recursive -b 3ds https://github.com/yaboimanguy-a11y/recvx-3ds.git
cd recvx-3ds
cmake -S port -B build/port-3ds -G Ninja -DCMAKE_TOOLCHAIN_FILE=port/cmake/3ds-clang.cmake
ninja -C build/port-3ds recvx_sdcard
```

The SD card folder is written to `build/port-3ds/sdcard/3ds/recvx/`.

For the original 3DS or 2DS, use its own build folder and add `-DRECVX_O3DS=ON`. That makes `recvx_o3ds.3dsx`, with less memory for textures, no 3D slider, no updater, and frame skip on by default. Expect it to run well below full speed.

 If clang isn't at the Homebrew path, pass `-DRECVX_3DS_CLANG=/path/to/clang`. `-DRECVX_CA_BUNDLE=` sets the certificate bundle copied in as `cacert.pem` (default `/etc/ssl/cert.pem`).

## AI disclosure

The port code in `port/`, and its changes to the game sources, were written with substantial AI assistance (Claude). This is disclosed in line with the decomp project's AI policy. This port is separate from the decomp's matching work: please report port problems here, not to the decomp team.

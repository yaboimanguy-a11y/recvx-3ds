#!/usr/bin/env python3
"""census.py [--target 3ds|wasm] [--only SUBSTR] [--show N]: compile every game C
file for a 32-bit host with -fsyntax-only and summarise the errors.

Error lines are grouped by message (identifiers kept), so the most common
blockers come first."""
import argparse
import collections
import concurrent.futures as cf
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CLANG = "/opt/homebrew/opt/llvm/bin/clang"
DKP = os.environ.get("DEVKITPRO", "/opt/devkitpro")

TARGETS = {
    "3ds": ["--target=armv6k-none-eabihf", "-mfloat-abi=hard", "-mfpu=vfpv2",
            "-isystem", f"{DKP}/devkitARM/arm-none-eabi/include"],
    "wasm": ["--target=wasm32-unknown-emscripten"],
}


HOST = ROOT / "build/hostsrc"


def include_flags():
    k = ROOT / "include/recvx-decomp-katana/KATANA/Include"
    return ["-DRECVX_PORT", "-I", str(ROOT / "port/shim"), "-I", str(ROOT / "port/host"),
            "-I", str(ROOT / "port/include"),
            "-I", str(HOST / "include/ps2/veronica/prog"),
            "-I", str(k),
            "-I", str(HOST / "include"),
            "-I", str(ROOT / "include"),
            "-I", str(ROOT / "include/recvx-decomp-cri/cri/mwlib/include"),
            "-I", str(ROOT / "include/recvx-decomp-cri/cri/mwlib/ee/include"),
            "-I", str(HOST / "include/cri/mwlib/ee/lib/libadxe")]


def flags(target):
    return (TARGETS[target] + ["-std=gnu99", "-fsyntax-only", "-ferror-limit=0", "-w",
            "-fno-short-enums", "-fms-extensions", "-Wno-error=implicit-function-declaration",
            "-Wno-error=implicit-int", "-Wno-error=int-conversion", "-Wno-error=incompatible-pointer-types",
            "-Wno-error=return-type", "-Wno-error=incompatible-function-pointer-types",
            "-include", str(ROOT / "port/include/port_pre.h")] + include_flags())


def run(src, target):
    p = subprocess.run([CLANG] + flags(target) + [str(src)], capture_output=True, text=True)
    return src, p.returncode, p.stderr


IOP = ("src/users/oshimi/adx/", "src/atick.c", "src/tq.c", "src/tsnddrv.c", "src/putsts.c", "src/sdd_com.c")


def is_iop(p):
    rel = str(p.relative_to(HOST))
    return any(rel.startswith(x) for x in IOP)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--target", default="3ds")
    ap.add_argument("--only", default="")
    ap.add_argument("--show", type=int, default=40)
    ap.add_argument("--file", action="store_true", help="print per-file error counts")
    a = ap.parse_args()
    subprocess.run([sys.executable, str(ROOT / "port/tools/hostify.py"), str(HOST)], check=True)
    srcs = sorted(p for p in (HOST / "src").rglob("*.c")
                  if a.only in str(p) and "src/cri/ps2/" not in str(p) and "/src/JPN/" not in str(p) and not is_iop(p))
    groups = collections.Counter()
    example = {}
    bad = {}
    with cf.ThreadPoolExecutor(os.cpu_count()) as ex:
        for src, rc, err in ex.map(lambda s: run(s, a.target), srcs):
            n = 0
            for line in err.splitlines():
                m = re.search(r"error: (.*)", line)
                if not m:
                    continue
                n += 1
                msg = re.sub(r"'[^']*' \(aka '[^']*'\)", "T", m.group(1))
                groups[msg] += 1
                example.setdefault(msg, line.split(" error:")[0].replace(str(HOST) + "/", "").replace(str(ROOT) + "/", ""))
            if rc:
                bad[src] = n
    print(f"{len(srcs)} files, {len(bad)} with errors, {sum(groups.values())} errors")
    for msg, c in groups.most_common(a.show):
        print(f"{c:6d}  {msg[:150]}   [{example[msg]}]")
    if a.file:
        for s, n in sorted(bad.items(), key=lambda kv: -kv[1]):
            print(f"{n:6d} {s.relative_to(HOST)}")


if __name__ == "__main__":
    main()

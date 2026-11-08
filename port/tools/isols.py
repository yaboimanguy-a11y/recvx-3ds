#!/usr/bin/env python3
"""isols.py IMAGE [PATH]: list an ISO 9660 directory (2048-byte sectors), or
print a file with --cat PATH / save it with --out."""
import struct, sys, argparse

def rd(f, lsn, n=2048):
    f.seek(lsn * 2048); return f.read(n)

def entries(f, lsn, size):
    data = rd(f, lsn, size); i = 0
    while i < len(data):
        l = data[i]
        if l == 0:
            i = (i // 2048 + 1) * 2048; continue
        e_lsn, e_size = struct.unpack_from("<I", data, i + 2)[0], struct.unpack_from("<I", data, i + 10)[0]
        flags, nl = data[i + 25], data[i + 32]
        name = data[i + 33:i + 33 + nl].decode("ascii", "replace")
        if name not in ("\x00", "\x01"):
            yield name.split(";")[0], e_lsn, e_size, bool(flags & 2)
        i += l

def find(f, path):
    pvd = rd(f, 16)
    assert pvd[1:6] == b"CD001", "not ISO 9660"
    lsn, size = struct.unpack_from("<I", pvd, 158)[0], struct.unpack_from("<I", pvd, 166)[0]
    for part in [p for p in path.split("/") if p]:
        for n, l, s, d in entries(f, lsn, size):
            if n.upper() == part.upper():
                lsn, size = l, s; break
        else:
            sys.exit(f"not found: {part}")
    return lsn, size

ap = argparse.ArgumentParser(); ap.add_argument("image"); ap.add_argument("path", nargs="?", default="/")
ap.add_argument("--cat", action="store_true"); ap.add_argument("--out")
a = ap.parse_args()
with open(a.image, "rb") as f:
    lsn, size = find(f, a.path)
    if a.cat or a.out:
        data = rd(f, lsn, size)
        if a.out: open(a.out, "wb").write(data)
        else: sys.stdout.write(data.decode("ascii", "replace"))
    else:
        for n, l, s, d in entries(f, lsn, size):
            print(f"{'D' if d else ' '} {l:8d} {s:10d} {n}")

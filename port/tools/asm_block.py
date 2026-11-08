#!/usr/bin/env python3
"""asm_block.py: replace one inline-asm statement inside a game function with
    #ifndef RECVX_PORT <the asm> #else <C> #endif

    from asm_block import wrap
    wrap(path, marker, c_code)   # marker: text unique to that asm statement
                                 # (a label such as l_0012DE68)
The asm statement is found around the marker: from the "asm" keyword before
it to the end of the statement ("); " for asm("...") or the closing brace for
asm { }).  Already-wrapped statements are skipped."""
import re


def _bounds(src, marker):
    m = src.index(marker)
    a = max(src.rfind("asm volatile", 0, m), src.rfind("asm\n", 0, m), src.rfind("asm (", 0, m))
    if a < 0:
        raise ValueError(f"no asm before {marker}")
    a = src.rfind("\n", 0, a) + 1
    j = a + src[a:].index("asm") + 3
    while src[j] not in "({":
        j += 1
    if src[j] == "{":
        depth, k = 0, j
        while True:
            depth += src[k] == "{"
            depth -= src[k] == "}"
            k += 1
            if depth == 0:
                break
    else:
        depth, k, instr = 0, j, False
        while True:
            c = src[k]
            if c == '"':
                instr = not instr
            elif not instr:
                depth += c == "("
                depth -= c == ")"
            k += 1
            if depth == 0 and not instr:
                break
        while src[k] != ";":
            k += 1
        k += 1
    if src[k] == "\n":
        k += 1
    return a, k


def wrap(path, marker, c_code):
    src = open(path, encoding="utf-8", errors="surrogateescape").read()
    a, k = _bounds(src, marker)
    if src[:a].rstrip().endswith("#ifndef RECVX_PORT"):
        return False
    block = src[a:k]
    if not block.endswith("\n"):
        block += "\n"
    c = c_code.rstrip("\n") + "\n"
    src = src[:a] + "#ifndef RECVX_PORT\n" + block + "#else\n" + c + "#endif\n" + src[k:]
    open(path, "w", encoding="utf-8", errors="surrogateescape").write(src)
    return True

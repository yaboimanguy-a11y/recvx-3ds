#!/usr/bin/env python3
"""port_guard.py FILE FUNC...: wrap each named function definition in FILE in
    #ifndef RECVX_PORT ... #endif
so the PS2 build keeps the original (assembly) body and the port build takes
the C version from port/host/.  Idempotent: a function already wrapped is
left alone.  --list prints the functions in FILE that contain inline asm."""
import re
import sys

GUARD = "#ifndef RECVX_PORT /* port: C version in port/host/ */\n"


def find_def(src, name):
    # a definition starts at column 0 and its parameter list is followed by '{'
    for m in re.finditer(r"^(?!\s)(?![#/])[^;\n]*?\b%s\s*\(" % re.escape(name), src, re.M):
        i = src.index("(", m.end() - 1)
        depth = 0
        while True:
            c = src[i]
            depth += c == "("
            depth -= c == ")"
            i += 1
            if depth == 0:
                break
        j = i
        while True:
            while src[j] in " \t\r\n":
                j += 1
            if src.startswith("//", j):          # a comment after the parameter list
                j = src.index("\n", j)
                continue
            break
        if src[j] != "{":
            continue  # a prototype
        depth = 0
        k = j
        while True:
            c = src[k]
            if c == '"':  # skip strings (asm bodies)
                k = src.index('"', k + 1)
            depth += c == "{"
            depth -= c == "}"
            k += 1
            if depth == 0:
                break
        # include a preceding comment line ("// 100% matching!")
        start = m.start()
        prev = src.rfind("\n", 0, start - 1)
        if src[prev + 1:start].lstrip().startswith("//"):
            start = prev + 1
        return start, k
    return None


def asm_functions(src):
    names = []
    for m in re.finditer(r"^(?!\s)(?![#/])[^;\n(]*?\b(\w+)\s*\([^;{]*\)\s*(?://[^\n]*)?\s*\{", src, re.M):
        span = find_def(src, m.group(1))
        if span and re.search(r"\basm\b|__asm__", src[span[0]:span[1]]):
            names.append(m.group(1))
    return names


def main():
    path, names = sys.argv[1], sys.argv[2:]
    src = open(path, encoding="utf-8", errors="surrogateescape").read()
    if names == ["--list"]:
        print(" ".join(asm_functions(src)))
        return
    if names == ["--asm"]:
        names = asm_functions(src)
    for name in names:
        span = find_def(src, name)
        if not span:
            sys.exit(f"{path}: no definition of {name}")
        a, b = span
        if src[max(0, a - len(GUARD)):a] == GUARD:
            continue
        src = src[:a] + GUARD + src[a:b] + "\n#endif /* RECVX_PORT */" + src[b:]
    open(path, "w", encoding="utf-8", errors="surrogateescape").write(src)
    print(f"{path}: guarded {len(names)}")


if __name__ == "__main__":
    main()

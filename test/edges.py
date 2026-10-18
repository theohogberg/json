#!/usr/bin/env python3
"""Inputs that end exactly where a vector scan would stop, for an ASan build:
strings, whitespace runs and multi-byte UTF-8 of every length 0-199 running
to the last byte of the file, terminated or not. A file is slurped into a
buffer of exactly its size, so any load past the end is a reported overflow.
Planting `i + 15 <= n` in a 16-byte loop is caught here; the corpus is not.

usage: edges.py binary"""
import os
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor

# Windows CreateProcess cannot resolve "out/json.exe": relative with a slash
BIN = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "out/json.asan")
d = tempfile.mkdtemp()
paths = []
for n in range(200):
    for body in ('"' + "a" * n + '"', '"' + "a" * n, '"' + "a" * n + '\\"',
                 "[" + " " * n + "1]", "[1" + " " * n, "\n" * n,
                 '"' + "a" * n + "å\"", '"' + "b" * n + "\\u12"):
        p = os.path.join(d, "%d.json" % len(paths))
        with open(p, "w", encoding="utf-8") as f:
            f.write(body)
        paths.append(p)


def run(p):
    with open(p, "rb") as f:
        r = subprocess.run([BIN], stdin=f, capture_output=True)
    err = r.stderr.decode(errors="replace")
    return p if "Sanitizer" in err or "runtime error" in err else None


with ThreadPoolExecutor(os.cpu_count() or 4) as ex:
    bad = [p for p in ex.map(run, paths) if p]
for p in bad[:5]:
    print("SANITIZER:", open(p, encoding="utf-8").read()[:80])
print("%d edge inputs, %d sanitizer reports" % (len(paths), len(bad)))
sys.exit(1 if bad else 0)

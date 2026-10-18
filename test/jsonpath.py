#!/usr/bin/env python3
"""The JSONPath Compliance Test Suite against `json --rfc9535`: a valid
selector must print its nodelist (one value per line; any listed order), an
invalid one must exit nonzero. Input goes through a pipe and through a file,
so both ways of reading are held to the same answers.

usage: jsonpath.py [binary] [cts.json]"""
import json
import os
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor

# Windows CreateProcess cannot resolve "out/json.exe": relative with a slash
BIN = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "out/json")
CTS = sys.argv[2] if len(sys.argv) > 2 else \
    "jsonpath-compliance-test-suite/cts.json"
tests = json.load(open(CTS, encoding="utf-8"))["tests"]
tmp = tempfile.mkdtemp()


def values(out):
    dec, s, i, vals = json.JSONDecoder(), out.decode("utf-8"), 0, []
    while i < len(s):
        if s[i].isspace():
            i += 1
            continue
        v, i = dec.raw_decode(s, i)
        vals.append(v)
    return vals


def run(k):
    t = tests[k]
    doc = json.dumps(t.get("document", None)).encode()
    path = os.path.join(tmp, "%d.json" % k)
    with open(path, "wb") as f:
        f.write(doc)
    cmd = [BIN, "--rfc9535", "--", t["selector"]]
    with open(path, "rb") as f:
        a = subprocess.run(cmd, stdin=f, capture_output=True)
    b = subprocess.run(cmd, input=doc, capture_output=True)
    return a, b


def verdict(t, p):
    if t.get("invalid_selector"):
        return None if p.returncode != 0 else "accepted an invalid selector"
    if p.returncode != 0:
        return "rejected: " + p.stderr.decode(errors="replace").strip()
    try:
        got = values(p.stdout)
    except ValueError as e:
        return "unparseable output (%s): %r" % (e, p.stdout[:200])
    want = t["results"] if "results" in t else [t["result"]]
    return None if got in want else "got %s, want %s" % (
        json.dumps(got)[:300], " or ".join(json.dumps(w)[:300] for w in want))


skipped = [k for k, t in enumerate(tests) if "\0" in t["selector"]]
todo = [k for k in range(len(tests)) if k not in skipped]
with ThreadPoolExecutor(os.cpu_count() or 4) as ex:
    results = dict(zip(todo, ex.map(run, todo)))

fails = 0
for k in todo:
    for how, p in zip(("file", "pipe"), results[k]):
        why = verdict(tests[k], p)
        if why:
            fails += 1
            print("FAIL [%s] %s: %r\n    %s" % (how, tests[k]["name"],
                                               tests[k]["selector"], why))
print("\n%d passed, %d failed, %d skipped (NUL in selector cannot be an "
      "argument)" % (len(todo) - len({k for k in todo if any(
          verdict(tests[k], p) for p in results[k])}), fails, len(skipped)))
sys.exit(1 if fails else 0)

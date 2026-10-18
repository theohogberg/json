#!/usr/bin/env python3
"""Differential fuzz against Python's json: valid input round-trips, the
accept/reject verdict on mutated input agrees, and path selection matches.

usage: fuzz.py [iterations] [binary] [seed]   (the seed is printed; reuse it
to replay a run)"""
import json
import os
import random
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

# Windows CreateProcess cannot resolve "out/json.exe": relative with a slash
BIN = os.path.abspath(sys.argv[2] if len(sys.argv) > 2 else "out/json")
N = int(sys.argv[1]) if len(sys.argv) > 1 else 2000
SEED = int(sys.argv[3]) if len(sys.argv) > 3 else random.randrange(2**32)
random.seed(SEED)

ATOMS = [
    None, True, False, 0, -1, 42, 3.5, -0.25, 1e10, 1e-10,
    "", "plain", "a\'\"b", "a\'b", "a\"b", "a\\b", "tab\there", "nl\nhere",
    "\u00e5\u00e4\u00f6", "\u4e2d\u6587", "\U0001f600", "\u0001\u001f",
    "  spaced  ", "0", "null",
]


def gen(depth=0):
    r = random.random()
    if depth > 4 or r < 0.45:
        return random.choice(ATOMS)
    if r < 0.72:
        return [gen(depth + 1) for _ in range(random.randint(0, 5))]
    keys = ["a", "b", "key", "with space", "with.dot", "\u00e5", "0", ""]
    return {random.choice(keys): gen(depth + 1)
            for _ in range(random.randint(0, 5))}


def run(data, arg=None):
    cmd = [BIN] + ([arg] if arg is not None else [])
    p = subprocess.run(cmd, input=data, stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE)
    return p.returncode, p.stdout


def mutate(b):
    b = bytearray(b)
    if not b:
        return b"x"

    for _ in range(random.randint(1, 3)):
        op = random.randrange(3)
        i = random.randrange(len(b))
        if op == 0:
            b[i] = random.randrange(256)
        elif op == 1 and len(b) > 1:
            del b[i]
        else:
            b.insert(i, random.choice(b'{}[]",:\\ \t0123456789eE.-+nultrafse\x00\xff'))
    return bytes(b)


# well-formed input must round-trip
docs = [gen() for _ in range(N)]
raws = [json.dumps(d, ensure_ascii=random.choice([True, False])) for d in docs]

# mutated input must be accepted exactly when Python accepts it
seeds = [json.dumps(gen()).encode() for _ in range(200)]
muts = [mutate(random.choice(seeds)) for _ in range(N)]

# a top-level key must select what Python indexes
paths = []
for _ in range(N // 4):
    doc = gen(2)
    if not isinstance(doc, dict) or not doc:
        continue
    k = random.choice(list(doc))
    if "." in k or k == "":
        continue
    paths.append((doc, k))

jobs = ([(r.encode(), None) for r in raws] + [(m, None) for m in muts] +
        [(json.dumps(d).encode(), k) for d, k in paths])
# spawning is the whole cost; a third of inputs repeat, so run each once
uniq = list(dict.fromkeys(jobs))
with ThreadPoolExecutor(os.cpu_count() or 4) as ex:
    seen = dict(zip(uniq, ex.map(lambda j: run(*j), uniq)))
results = [seen[j] for j in jobs]
r1, r2, r3 = (results[:N], results[N:2 * N], results[2 * N:])

fails = 0

for doc, raw, (rc, out) in zip(docs, raws, r1):
    if rc != 0:
        print("REJECTED VALID INPUT:", raw[:200]); fails += 1; continue
    try:
        back = json.loads(out)
    except Exception as e:
        print("UNPARSEABLE OUTPUT for", raw[:120], "->", out[:120], e)
        fails += 1
        continue
    if back != doc:
        print("ROUND-TRIP MISMATCH:", raw[:120], "->", out[:120])
        fails += 1

for data, (rc, out) in zip(muts, r2):
    try:
        json.loads(data.decode("utf-8"),
                   parse_constant=lambda c: (_ for _ in ()).throw(ValueError(c)))
        ref_ok = True
    except Exception:
        ref_ok = False
    if ref_ok and rc != 0:
        print("TOO STRICT:", data[:120]); fails += 1
    elif not ref_ok and rc == 0:
        print("TOO LENIENT:", data[:120]); fails += 1

for (doc, k), (rc, out) in zip(paths, r3):
    if rc != 0:
        print("PATH FAILED:", k, json.dumps(doc)[:120]); fails += 1; continue
    if json.loads(out) != doc[k]:
        print("PATH MISMATCH:", k, out[:120]); fails += 1

print("failures:", fails, "(seed %d)" % SEED)
sys.exit(1 if fails else 0)

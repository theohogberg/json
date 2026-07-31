#!/usr/bin/env python3
"""fuzz.py - differential test of ./json against a reference JSON parser.

Two properties are checked:

  1. round-trip: for valid input, ./json output must parse back to a
     semantically identical document.
  2. agreement:  ./json must accept exactly what the reference accepts.
                 (a parser that is too lenient is a security bug)

usage: ./fuzz.py [iterations] [binary]
"""
import json
import random
import subprocess
import sys

BIN = sys.argv[2] if len(sys.argv) > 2 else "./json"
N = int(sys.argv[1]) if len(sys.argv) > 1 else 2000

ATOMS = [
    None, True, False, 0, -1, 42, 3.5, -0.25, 1e10, 1e-10,
    "", "plain", "a\"b", "a\\b", "tab\there", "nl\nhere",
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


fails = 0

# --- property 1: round-trip fidelity on well-formed input ---
for i in range(N):
    doc = gen()
    raw = json.dumps(doc, ensure_ascii=random.choice([True, False]))
    rc, out = run(raw.encode())
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

# --- property 2: accept/reject agreement on mutated input ---
seeds = [json.dumps(gen()).encode() for _ in range(200)]
for i in range(N):
    data = mutate(random.choice(seeds))
    rc, out = run(data)
    try:
        json.loads(data.decode("utf-8"),
                   parse_constant=lambda c: (_ for _ in ()).throw(ValueError(c)))
        ref_ok = True
    except Exception:
        ref_ok = False
    if ref_ok and rc != 0:
        # reference accepts, we reject -> too strict
        print("TOO STRICT:", data[:120]); fails += 1
    elif not ref_ok and rc == 0:
        # we accept, reference rejects -> too lenient (worse)
        print("TOO LENIENT:", data[:120]); fails += 1

# --- property 3: path selection agrees with manual navigation ---
for i in range(N // 4):
    doc = gen(2)
    if not isinstance(doc, dict) or not doc:
        continue
    k = random.choice(list(doc))
    if "." in k or k == "":
        continue
    rc, out = run(json.dumps(doc).encode(), k)
    if rc != 0:
        print("PATH FAILED:", k, json.dumps(doc)[:120]); fails += 1; continue
    if json.loads(out) != doc[k]:
        print("PATH MISMATCH:", k, out[:120]); fails += 1

print("failures:", fails)
sys.exit(1 if fails else 0)

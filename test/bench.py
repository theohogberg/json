"""Benchmark json against jq on the same work: `make bench`.

Generates the inputs into out/bench/ (seeded, so every run sees the same
bytes), checks that both tools print byte-identical output for each case, and
prints a markdown table of median wall time and peak RSS, input from a file.
Usage: bench.py [BIN] [RUNS]
"""
import json
import os
import random
import statistics
import subprocess
import sys
import time

BIN = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "out/json")
RUNS = int(sys.argv[2]) if len(sys.argv) > 2 else 15
DIR = "out/bench"

CASES = [  # file, label, json args, jq args
    ("records.json", "reformat", [], ["."]),
    ("records.json", "select one deep value", ["5000.nested.a.b"], [".[5000].nested.a.b"]),
    ("records.json", "a field of every element", ["[@.name]"], [".[].name"]),
    ("records.json", "the same, raw strings", ["-r", "[@.name]"], ["-r", ".[].name"]),
    ("records.json", "filter, then a field", ["[?@.age > 80].id"], [".[] | select(.age > 80) | .id"]),
    ("records.json", "slice", ["[100:110]"], [".[100:110]"]),
    ("issues.json", "reformat", [], ["."]),
    ("issues.json", "a nested field per element", ["items[@.user.login]"], [".items[].user.login"]),
    ("issues.json", "deep select", ["items.7000.comments"], [".items[7000].comments"]),
    ("small.json", "reformat", [], ["."]),
]


def generate():
    """flat records, a nested string-heavy API-like document, and a small file"""
    random.seed(1)
    words = "alpha beta gamma delta epsilon zeta eta theta iota kappa lambda mu".split()

    def text(n):
        return " ".join(random.choice(words) for _ in range(n))

    records = [{
        "id": i,
        "name": f"user{i}",
        "email": f"user{i}@example.com",
        "age": random.randint(18, 90),
        "score": round(random.uniform(0, 1000), 3),
        "active": random.random() < 0.5,
        "tags": random.sample(words, 3),
        "bio": text(random.randint(3, 25)),
        "nested": {"a": {"b": i * 7, "c": None},
                   "loc": [random.uniform(-90, 90), random.uniform(-180, 180)]},
    } for i in range(60000)]
    issues = {"total": 8000, "items": [{
        "id": i,
        "title": text(8) + " éè ☃",
        "user": {"login": f"dev{i % 500}", "id": i % 500, "site_admin": False,
                 "urls": {k: f"https://api.example.com/users/dev{i % 500}/{k}"
                          for k in ("repos", "events", "gists")}},
        "labels": [{"name": w, "color": "%06x" % random.randrange(1 << 24)}
                   for w in random.sample(words, 2)],
        "body": text(random.randint(20, 120)) + "\n\n```\ncode \"quoted\" \\ path\n```",
        "comments": [{"user": f"dev{random.randrange(500)}", "body": text(random.randint(5, 40))}
                     for _ in range(random.randint(0, 6))],
        "reactions": {"+1": random.randrange(50), "-1": random.randrange(5),
                      "heart": random.randrange(20)},
    } for i in range(8000)]}
    os.makedirs(DIR, exist_ok=True)
    for name, doc in (("records.json", records), ("issues.json", issues),
                      ("small.json", records[:60])):
        with open(os.path.join(DIR, name), "w", encoding="utf-8") as f:
            json.dump(doc, f, ensure_ascii=False)


def run(argv, path):
    """wall seconds and peak RSS in MB of one run, output discarded"""
    with open(path, "rb") as f:
        t = time.perf_counter()
        p = subprocess.Popen(argv, stdin=f, stdout=subprocess.DEVNULL)
        if hasattr(os, "wait4"):
            _, status, ru = os.wait4(p.pid, 0)
            p.returncode = os.waitstatus_to_exitcode(status)
            # ru_maxrss is bytes on macOS, KiB on Linux
            rss = ru.ru_maxrss / (2**20 if sys.platform == "darwin" else 2**10)
        else:
            p.wait()
            rss = float("nan")
        dt = time.perf_counter() - t
    if p.returncode:
        sys.exit(f"{argv} failed with {p.returncode}")
    return dt, rss


if not all(os.path.exists(os.path.join(DIR, c[0])) for c in CASES):
    generate()
jq = subprocess.run(["jq", "--version"], capture_output=True, text=True).stdout.strip()
print(f"{RUNS} runs per case, median; {jq}\n")
print("| input | case | jq | json | speedup | peak RSS, jq / json |")
print("|---|---|---|---|---|---|")
bad = 0
for file, label, ja, qa in CASES:
    path = os.path.join(DIR, file)
    outs = [subprocess.run(argv, stdin=open(path, "rb"), capture_output=True).stdout
            for argv in ([BIN] + ja, ["jq"] + qa)]
    if outs[0] != outs[1]:
        print(f"output differs: json {' '.join(ja)} vs jq {' '.join(qa)}", file=sys.stderr)
        bad += 1
    res = []
    for argv in (["jq"] + qa, [BIN] + ja):
        runs = [run(argv, path) for _ in range(RUNS + 1)][1:]  # first warms the cache
        res.append((statistics.median(r[0] for r in runs) * 1e3, max(r[1] for r in runs)))
    size = os.path.getsize(path)
    size = f"{size / 1e6:.1f} MB" if size > 1e6 else f"{size / 1e3:.0f} KB"
    print(f"| {file} ({size}) | {label} | {res[0][0]:.1f} ms | {res[1][0]:.1f} ms | "
          f"{res[0][0] / res[1][0]:.1f}x | {res[0][1]:.0f} / {res[1][1]:.0f} MB |", flush=True)
sys.exit(1 if bad else 0)

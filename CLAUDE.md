# json

A zero-dependency JSON formatter and query tool for the shell. C99, libc only,
one translation unit: `src/json.c`.

```
curl -s URL | json                      # reformat
curl -s URL | json users.0.name         # select
curl -s URL | json -r users[@.name]     # stream, unquoted
```

## Build and test

```
make                      # out/json
make test                 # regression suite
make scalar               # same suite, built -DNOSIMD
make check                # suite + conformance under ASan+UBSan
make conformance          # JSONTestSuite (cloned on first run)
make fuzz [SEED=N]        # differential vs Python's json; prints its seed
make diff                 # SIMD and scalar builds must emit identical output
make ci                   # all of the above, ~90 s — run before committing
make DEPTH=256 test       # flag changes rebuild; checked when make starts
```

`make ci` must be green before any commit. Baseline: **76/76 tests, 283/283
conformance, fuzz clean**.

## Non-negotiables

Settled decisions, not preferences. Changing one needs a reason.

- **No dependencies beyond libc.** No getopt_long (GNU-only), nothing
  third-party. Option parsing is hand-rolled on purpose.
- **Builds clean** under `-std=c99 -O3 -Wall -Wextra -pedantic`. Fix a
  warning, don't silence it.
- **Output is byte-identical to `jq .`** for reformatting (verified on 4.6 MB
  against jq 1.7.1). A change that alters output is a bug unless it's the point.
- **Never free.** Exit reclaims. LeakSanitizer is therefore useless, and
  `ASAN_OPTIONS=detect_leaks=0` in `SANENV` must stay.
- **Numbers keep their raw lexeme** (a pointer into the input buffer), so
  `1.50` and `1e309` round-trip. Never convert to double. The input buffer must
  outlive the tree.

## Architecture

1. **No filter → streaming.** `emit_value()` validates and reformats in one
   pass, no allocation, no tree. ~18x faster than jq; never route it through
   the tree.
2. **Filter → tree.** `parse_value()` builds a DOM, `walk()` applies the path,
   results go to a `sink_fn`.

**Results go through a sink** because `[@]` makes the result count a runtime
property; anything returning one value had to be rewritten once already. A
stream is a stream whatever its cardinality: a future collect flag must give
`["ada"]`, not `"ada"`, for a one-element array.

**Per-element memory is bounded.** `stream_array()` restores an arena mark
after each element, so everything built while answering a query must come from
the arena (`push` uses `arena_grow`, never `xrealloc`). At 400k elements `[@]`,
`[@.name]` and `[@.+ie]` peak within noise of each other (~150 MB; macOS RSS
varies ±5% run to run).

**The tree is exact-size.** The parser does not use `push`: doubling in the
arena abandons every outgrown block (+14% RSS). Children collect on a reusable
scratch stack and each container is copied into the arena once when it closes:
84 MB on a 12 MB file, against 141 MB for the old realloc tree. Arena alignment
is 8; 16 padded every 40-byte `jval` to 48.

**Hot-path inlining is load-bearing.** `om`, `str_end` and `scan_number` are
`always_inline`, with rare branches split into `om_slow`/`str_slow`. Without
it clang won't inline them and short strings run 15–25% slower. Literals are
emitted with constant-length `os_`; the variable-length copy cost 8%.

**SIMD.** None yet. `scan_str` and `scan_ws` are the only functions intrinsics
belong in: read only `s[0..n)`, return what the scalar loop would, fall through
to it for the tail, and guard with `!defined(NOSIMD)`. `make scalar` and
`make diff` then cover the fallback automatically. The old SSE2 bodies are in
`git show 04a1c92:src/json.c`.

## Query syntax

Not JSONPath, though it borrows from it. RFC 9535's `[*]`, and `@` only inside
filters, are deliberate deviations.

```
users.0.name        dotted path; works on objects and arrays
users[0]            bracket index, negative allowed: users[-1]
users[1:3]          slice — Python semantics exactly
users.na+           glob object keys; '+' not '*'
users[@.name]       stream: array → one result per element
```

**Slices are Python's**, verified case by case: negative indices, clamped
bounds, negative steps, `[3:1]` is `[]`, a zero step is an error (not RFC's
"selects nothing"). A slice is one sub-array, not a stream.

**`+` globs, not `*`**: the shell expands `*` against the working directory
(`json pre*.hello` with a file `pre1.hello` present silently becomes another
valid query). `+` survives sh, bash and zsh unquoted. Otherwise exactly
`fnmatch(3)` (2908 patterns, zero mismatches); no character classes, since `[`
means index-or-slice. Globbing an object returns an object; globbing anything
else is an error.

**`[@...]` is meant to be terminal**: `users[@.name]`, with nesting as
`n[@[@]]`, because a stream is not an object for a `.` suffix to select from.
Not enforced yet: `users[@].name` still succeeds.

**Errors vs empty.** An absent named key is an error (exit 1); a pattern
matching nothing is empty — Python's `d['x']` vs a comprehension. Out-of-range
index errors; out-of-range slice clamps.

## Flags

```
-r, --raw-output    string result without quotes or escapes
    --raw           alias
    --rfc9535       recognized, refuses to run (see below)
-h, --help
--                  end of options
```

Short options cluster (`-rh`); long options take no abbreviations. `-r` applies
only when the result itself is a string; nested strings stay quoted so the
output is still JSON. `--raw-output` is primary because jq also has
`--raw-input`, which makes a bare `--raw` ambiguous.

## Not implemented

- **`--rfc9535`** exits naming what would change: slices yielding one result
  per element, a missing key yielding empty. Placeholder for full JSONPath.
- **`[?...]` predicates**, e.g. `[?@.name == 'john']`. The next feature. `@`
  must mean "current element" so `[@.x]` and `[?@.x == 1]` read alike.
- **Multiple JSON values on stdin.** A second value is "trailing data" today,
  so `json a[@.x] | json y` cannot compose. A prerequisite for pipelines.
- **Incremental input.** `slurp()` reads to EOF, so input must fit in RAM. The
  streaming path and tree numbers point into the input buffer, so this needs a
  sliding window with flush-before-refill, or number lexemes copied out.
- **Error tolerance.** `n[@.name]` fails if any element lacks `name`. jq's `?`
  is the precedent; it is unrelated to RFC 9535's `[?...]`, so decide the
  spelling before implementing either.

## Testing

`test/test.sh` (`t NAME INPUT RC OUT [ARGS...]`; stdout checked only on rc 0),
`test/conformance.sh` (JSONTestSuite), `test/fuzz.py` (vs Python), and
`test/corpus.py` (input for `make diff`). Everything runs one process per core
(`JOBS=N` for the shell scripts): a sanitizer build spends ~45 ms starting and
<1 ms working, and no `ASAN_OPTIONS` setting changes that.

**The suite is minimal by measurement; don't prune or pad it by eye.** It was
cut from 140 cases with 1291 mutants of `json.c` (operator, literal and ±1
swaps including hex, statement deletion, and each `die`/`perr` turned into a
silent `exit(0)`). Each of the 76 cases is the only one catching some mutant.
A by-judgement pass had kept 111 yet dropped six cases that were the only ones
catching something. Error cases check only the exit code, so only the `exit(0)`
operator tells error sites apart.

Add a case for every behaviour change. Known gaps: no case reaches the tree
parser's syntax errors (malformed input is only tested without a filter, and
`json a` parses differently from `json`), `path nesting too deep`, `number too
large in path`, or `index ... is too large`.

**Conformance alone is not enough.** An injected off-by-one in a vector loop
passed every test, all conformance and byte-identical output on 4.6 MB while
reading out of bounds; ASan caught it only with an input sized to the
allocation edge. The arena blunts ASan further: overflowing one sub-allocation
into the next is a legal write inside a live chunk.

## Benchmarks

Against jq 1.7.1 on a 4.6 MB array of 15,840 objects, output byte-identical:

| | jq | json |
|---|---|---|
| reformat whole doc | 154.8 ms | 8.5 ms (18x) |
| select one deep value | 54.0 ms | 12.5 ms (4.3x) |

`jq .` and `json` both use memory proportional to input; `jq --stream` is flat
at 6.2 MB and we have no equivalent.

Measure at a realistic size: at 18 KB, process spawn is 0.85 ms of a 1.05 ms
run. A single `time cat x | json > f` measures disk writeback, not the build.
Didn't matter: `-O3` over `-O2` (+1.8%), hand unrolling, SIMD alone (+5–8%).
Did: no tree (3x), batched output (+24% on string-heavy data), a memcpy fast
path for escape-free strings (+18%).

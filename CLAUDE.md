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
make scalar               # same suite, built -DNOSIMD (the scalar fallback)
make check                # same suite + conformance, under ASan+UBSan
make conformance          # JSONTestSuite (clones it on first run)
make fuzz                 # differential fuzz vs Python's json
make diff                 # all variants, SIMD and scalar, emit identical output
make ci                   # everything — run this before committing (~90 s)
make fuzz SEED=N          # replay a fuzz run; every run prints its seed
make DEPTH=256 test       # rebuilds; the flags stamp forces it
```

`make ci` must be green before any commit. Current baseline: **76/76 tests,
283/283 conformance, fuzz clean**.

The Makefile discovers variants from `src/*.c`, so dropping in a second source
file gets it built and tested with no edit. There is only one today.

## Non-negotiables

These are settled decisions, not preferences. Changing one needs a reason.

- **No dependencies beyond libc.** No getopt_long (GNU-only), no third-party
  anything. Option parsing is hand-rolled on purpose.
- **Builds clean** under `-std=c99 -O3 -Wall -Wextra -pedantic`. Warnings are
  errors in practice. Don't silence one — fix it or wire the thing up.
- **Output is byte-identical to `jq .`** for reformatting. Verified against a
  real jq binary on a 4.6 MB file. Any change that alters output is a bug
  unless it's the point of the change.
- **Never free.** The process is short-lived; exit reclaims. This means
  LeakSanitizer is useless here — `ASAN_OPTIONS=detect_leaks=0` is already set
  in the Makefile's `SANENV`, and must stay.
- **Numbers keep their raw lexeme** (`const char *` into the input buffer).
  That's what makes `1.50` and `1e309` round-trip exactly. Don't convert to
  double. It also means the input buffer must outlive the tree.

## Architecture

Two execution paths, chosen in `main`:

1. **No filter → streaming.** `emit_value()` validates and reformats in one
   pass with zero allocation. No tree is built. This is ~18x faster than jq
   and must stay that way; don't route it through the tree.
2. **Filter → tree.** `parse_value()` builds a DOM, `walk()` applies the path,
   results go to a `sink_fn` callback.

Source layout (`src/json.c`, in order):

| Section | What |
|---|---|
| arena | Bump allocator, linked reusable chunks, `arena_save`/`arena_restore` |
| value model | `jval` (arrays and objects share one container struct), `push` |
| scanners | `scan_str`/`scan_ws` — the only hot loops, the SIMD seam |
| lexer | `scan_number`, `str_end`, `unescape`, UTF-8 validation — shared by both paths |
| output buffer | `om`/`oc`/`os_`/`oflush`, `oesc` — 64 KB, bypasses stdio |
| parser / printer | `parse_*` (scratch stack, exact-size containers), `print_val` — the filter path |
| streaming reformatter | `emit_*`, the no-filter path |
| path selection | `walk`, `apply_key`, `apply_bracket`, `stream_array` |
| result sink | `sink_fn`, `print_sink` |
| globbing | `glob_match`, in `apply_key` |
| main | Option parsing, dispatch |

### Why results go through a sink

`walk()` returns nothing. `[@]` yields one result per element, so a filter
produces a *stream*, and the count is a runtime property. Anything that returns
a single value has to be rewritten the moment iteration exists — which already
happened once. Keep the callback.

**A stream is always a stream, whatever its cardinality.** A one-element array
through `[@.name]` is a stream of one, not a value. This matters when a collect
flag gets added: it must give `["ada"]`, not `"ada"`.

### Per-element memory is bounded, and it's load-bearing

`stream_array()` takes an arena mark and restores it after each element. For
that to work, **everything built while answering a query must come from the
arena** — which is why `push` uses `arena_grow`, not `xrealloc`.

The parser does *not* use `push`. Doubling inside the arena abandons every
outgrown block, which cost 14% more RSS than the old malloc'd tree. Instead
children collect on a reusable scratch stack and each container is copied
into the arena once, at exact size, when it closes. On a 12 MB file that is
84 MB RSS against 141 MB for the old realloc tree. Arena alignment is 8, not
16: nothing in it needs more, and 16 padded every 40-byte `jval` to 48.

Verified: at 400k elements, `[@]`, `[@.name]` and `[@.+ie]` peak within
run-to-run noise of each other (143–159 MB on macOS, where RSS varies ±5%
between identical runs; 205–221 MB before the scratch-stack parser). 400k
glob results cost no additional memory.

If you add anything that allocates per element, allocate it from the arena.

### SIMD

There are none yet. `scan_str` and `scan_ws` are plain scalar loops and the
only two functions intrinsics need to touch: read only `s[0..n)`, return what
the scalar loop would, and fall through to it for the tail. Guard the vector
branch with `!defined(NOSIMD) && defined(__SSE2__)` (or `__ARM_NEON`).

`make scalar` builds with `-DNOSIMD` and runs the suite, and `make diff`
byte-compares it against the normal build; both are in `make ci`. Today they
are the same code, and that is the point: when a vector path lands, the
fallback is already under test. The old SSE2 bodies are in git history
(`git show 04a1c92:src/json.c`).

### Hot-path inlining

`om`, `str_end` and `scan_number` are `INLINE` (`always_inline` under GCC and
Clang), and `om`/`str_end` keep their rare branches in out-of-line `om_slow`
and `str_slow`. Without this, clang declines to inline them (cost 470–505
against a threshold of 250) and short strings and small objects ran 15–25%
slower than the code before the refactor. With it they are 9–19% faster.
`emit_value` writes `true`/`false`/`null` with `os_` so the length is a
constant; routing them through the variable-length number copy cost 8% on
literal-heavy input.

## Query syntax

Not JSONPath, though it borrows from it. RFC 9535 uses `[*]` for iteration and
`@` only inside filters; both are deviations we took deliberately.

```
users.0.name        dotted path; works on objects and arrays
users[0]            bracket index, negative allowed: users[-1]
users[1:3]          slice — Python semantics exactly
users.na+           glob object keys; '+' not '*'
users[@.name]       stream: array → one result per element
```

**Slices follow Python, verified case by case against it.** Negative indices,
clamped bounds, steps including negative. `[3:1]` is `[]`, `[::-1]` reverses,
`[0:99]` clamps. A zero step is an error (Python's `ValueError`), not "selects
nothing" (RFC). A slice returns a **sub-array**, one value — not a stream.

**`+` is the glob wildcard, not `*`.** The shell expands `*` against the
working directory before the program runs — with a file named `pre1.hello`
present, `json pre*.hello` silently becomes a *different valid query*. Verified.
`+` survives unquoted in sh, bash and zsh. Semantics are otherwise exactly
`fnmatch(3)`: differential-tested over 2908 patterns, zero mismatches. No
character classes, since `[` already means index-or-slice.

Globbing an object returns an object of matching key/value pairs — type
preserving. Globbing an array or scalar is an error.

**`[@...]` is terminal.** The per-element path goes *inside* the brackets:
`users[@.name]`, not `users[@].name`. A `.` selects a key from an object and a
stream is not an object, so there is nothing for a suffix to apply to. Nesting
is `n[@[@]]`, which works because the inner path is walked recursively.

> The uploaded tree may predate this change. If `users[@].name` succeeds, the
> terminal-bracket version wasn't applied — it also bumps the suite to 144.

**Errors vs empty.** A named key that is absent is an error (exit 1). A pattern
that matches nothing is an empty result. Same split as Python's `d['x']` vs a
comprehension. Out-of-range index errors; out-of-range slice clamps.

## Flags

```
-r, --raw-output    string result without quotes or escapes
    --raw           alias
    --rfc9535       recognized, refuses to run (see below)
-h, --help
--                  end of options
```

Short options cluster (`-rh`); long options don't take abbreviations.

`-r` applies only when the result itself is a string. Strings nested inside a
printed object stay quoted — that output still has to be valid JSON.

Naming: jq has both `--raw-input` and `--raw-output`, so a bare `--raw` is
ambiguous to anyone arriving from jq. `--raw-output` is primary.

## Not implemented

**`--rfc9535`** is parsed and exits with a message naming what would change:
slices yielding one result per element instead of a sub-array, and a missing
key yielding empty instead of erroring. It's a placeholder for full JSONPath
semantics.

**`[?...]` predicates** — RFC 9535 filter expressions, `[?@.name == 'john']`
and `[?search(@.name, 'john')]`. This is the next feature. `@` must mean
"current element" consistently so `[@.x]` and `[?@.x == 1]` read the same way.

**Multiple JSON values on stdin.** Today a second value is "trailing data".
This blocks `json a[@.x] | json y` from composing at all, since a stream emits
several values. It's a prerequisite for pipelines, not an optional extra.

**Incremental input.** `slurp()` reads stdin to EOF, so memory tracks input
size and a file larger than RAM is impossible. Note the streaming path echoes
strings by pointing directly into the input buffer, and tree numbers do too —
so an incremental reader means either a sliding window with flush-before-refill,
or copying number lexemes into the arena.

**Error tolerance.** `n[@.name]` fails the whole query if any element lacks
`name`. On heterogeneous API data that's fragile. jq's `?` suffix is the
precedent, and it's a different thing from RFC 9535's `[?...]` despite sharing
a character — decide the spelling before implementing either.

## Testing

- `test/test.sh` — regression suite. One helper, `t NAME INPUT RC OUT [ARGS...]`.
  On success stdout must match; on failure only the exit code is checked.
- `test/conformance.sh` — JSONTestSuite; `y_` must pass, `n_` must fail.
- `test/fuzz.py` — differential against Python's `json`.

All three run in parallel, one process per core (`JOBS=N` overrides it for
the two shell scripts). That is the whole speed story: a sanitizer build
spends ~45 ms starting up and under 1 ms on its input, no `ASAN_OPTIONS`
setting moves that, and spawns are what every suite is made of. `fuzz.py`
also runs each distinct input once, since a third of generated cases repeat.
`make ci` went from ~9 minutes to ~90 s; fuzz is still most of it. Output is
collected per case and printed in source order, so reports don't depend on
scheduling.

**The suite is minimal, and that was measured, not judged.** On 2026-10-17 it
was reduced from 140 cases by mutation analysis: 1291 mutants of `json.c`
(operator and literal swaps, ±1 on decimal and hex constants, statement
deletion, and every `die`/`perr` site turned into a silent `exit(0)`), each
run against every case. The kept 76 are a minimum cover: every one is the
only case catching at least one mutant, so dropping any leaves a gap. Ties
went to the most direct case. 813 of the 1283 that compile are caught; the
rest are mostly equivalent (performance-only code, error-message text). An earlier by-judgement pass to 111 missed
15 of those mutants, and removed six cases that were each the only one catching
something, so don't prune by eye.

Two blind spots shaped the operators. Error tests check only the exit code,
so deleting a `die` just falls through to a later one that also exits 1; only
the `exit(0)` operator tells error sites apart. And with lines carrying a
trailing comment, or hex constants, left unmutated, all of `utf8_check` looked
untested.

Known gaps (no case reaches them): every tree-parser syntax error. All
malformed-input cases run without a filter, so only the streaming parser's
errors are exercised; `json a <<< '{"a" 1}'` takes a different code path from
`json <<< '{"a" 1}'`. Also `path nesting too deep`, `number too large in path`
and `index ... is too large`.

Add a case for every behaviour change. The suite has already caught things
conformance and 5000 fuzz cases both missed — the escape-normalization
regression when streaming was added, and a dead depth test that silently
stopped testing anything.

**Conformance alone is not enough.** An injected off-by-one in the vector loop
passed 82/82 tests, 283/283 conformance, and produced byte-identical output on
4.6 MB — while reading out of bounds. ASan caught it only with an input sized
to the allocation edge. That's why `make check` exists separately, and why the
corpus being all small files is a real weakness.

Note the arena blunts ASan further: an overflow from one sub-allocation into
the next is, to ASan, a legal write inside a live chunk. The hottest two data
structures are the least protected.

## Benchmarks

Against jq 1.7.1 on a 4.6 MB array of 15,840 objects, output byte-identical:

| | jq | json |
|---|---|---|
| reformat whole doc | 154.8 ms | 8.5 ms (18x) |
| select one deep value | 54.0 ms | 12.5 ms (4.3x) |

Memory: `jq .` and `json` both track input size. `jq --stream` is flat at
6.2 MB regardless of input; we have no equivalent.

Measure before optimizing, and measure at a realistic size. At 18 KB — an
actual API response — process spawn is 0.85 ms of a 1.05 ms run and every
optimization in this file is invisible. A single `time` run through
`cat x | json > f` cannot distinguish two builds; it measures disk writeback.

Things that turned out not to matter: `-O3` over `-O2` (+1.8%), hand unrolling,
SIMD alone (+5-8%). Things that did: not building a tree (3x), batching stdio
(+24% on string-heavy data), a memcpy fast path for escape-free strings (+18%).

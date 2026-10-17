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
make check                # same suite + conformance, under ASan+UBSan
make conformance          # JSONTestSuite (clones it on first run)
make fuzz                 # differential fuzz vs Python's json
make diff                 # all variants must emit identical output
make ci                   # everything — run this before committing
make DEPTH=256 test       # rebuilds; the flags stamp forces it
```

`make ci` must be green before any commit. Current baseline: **111/111 tests,
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
| value model | `jval`, `jstr`, `arr_push`, `obj_push` |
| scanners | `scan_str`/`scan_ws` — the only hot loops, the SIMD seam |
| lexer | `scan_number`, `str_end`, `unescape`, UTF-8 validation — shared by both paths |
| output buffer | `om`/`oc`/`os_`/`oflush`, `oesc` — 64 KB, bypasses stdio |
| parser / printer | `parse_*`, `print_val` — the filter path |
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
arena** — which is why `arr_push`/`obj_push` use `arena_grow`, not `xrealloc`.
The parser uses the same two functions, so the whole tree is arena-backed.

Verified: at 400k elements, `[@]`, `[@.name]` and `[@.+ie]` all peak at exactly
the same RSS. 400k glob results cost zero additional memory.

If you add anything that allocates per element, allocate it from the arena.

### SIMD

There are none yet. `scan_str` and `scan_ws` are plain scalar loops and the
only two functions intrinsics need to touch: read only `s[0..n)`, return what
the scalar loop would, and fall through to it for the tail. Add a
`#if defined(__SSE2__)` / `__ARM_NEON` branch inside them. The old SSE2 bodies
are in git history (`git show 04a1c92:src/json.c`). Keep a scalar-only CI job
once a vector path exists, or the fallback rots.

The scalar-only refactor costs roughly 15–20% on the streaming reformat
versus the previous scalar build; measure before and after adding intrinsics.

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
-r, --raw-output    string result without quotes or escapes (no clustering: -rr is an error)
    --raw           alias
    --rfc9535       recognized, refuses to run (see below)
-h, --help
--                  end of options
```

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

- `test/test.sh` — regression suite. `check NAME INPUT ARG RC OUT` for a single
  argument, `checkf NAME INPUT RC OUT ARGS...` for flags.
- `test/conformance.sh` — JSONTestSuite; `y_` must pass, `n_` must fail.
- `test/fuzz.py` — differential against Python's `json`.

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

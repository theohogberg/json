# json

A zero-dependency JSON formatter and query tool for the shell. C99, libc only
(POSIX `read`/`fstat`, or Win32 `ReadFile`/`GetFileType` under `_WIN32`), one
translation unit: `src/json.c` (which includes the generated `src/unicat.h`).

```
curl -s URL | json                      # reformat
curl -s URL | json users.0.name         # select
curl -s URL | json -r users[@.name]     # stream, unquoted
curl -s URL | json --rfc9535 '$.users[?@.age > 30].name'
```

## Build and test

```
make                      # out/json
make test                 # regression suite
make scalar               # same suite, built -DNOSIMD
make check                # suite + conformance under ASan+UBSan
make conformance          # JSONTestSuite (cloned on first run)
make jsonpath             # JSONPath Compliance Test Suite (cloned on first run)
make fuzz [SEED=N]        # differential vs Python's json; prints its seed
make diff                 # SIMD and scalar builds must emit identical output
make ci                   # all of the above, ~2 min — run before committing
make DEPTH=256 test       # flag changes rebuild; checked when make starts
```

On Windows the same targets work from MSYS2, MinGW or Git Bash (`OS=Windows_NT`
adds `.exe`, exports `PYTHONUTF8=1`, and defaults `CC` to the first of gcc,
clang, cl on PATH). Recipes need a POSIX `sh`: from `cmd.exe`, where make
would fall back to cmd, the makefile finds Git for Windows' `usr\bin\sh.exe`
via `git.exe` and puts that directory on PATH. With `cl` or `clang-cl` the
makefile switches to MSVC flags (`-std:c11 -O2 -W4`), and `check`/`fuzz` use
MSVC's ASan, without UBSan; MinGW has no ASan. Without make:
`cl /O2 /std:c11 src\json.c /Fe:json.exe`. `make ci` is verified green with
MSVC 19.44 (x86) from Git Bash and GnuWin32 make 3.81; MinGW gcc is untested.
A Developer Command Prompt's `cl` targets 32-bit x86 by default; build from an
x64 prompt (`vcvars64.bat`), which is ~15% faster (see Benchmarks).
`os_init` rebuilds `argv` as UTF-8 from `GetCommandLineW`, since the CRT's
ANSI `argv` turns a query like `$.☺` into `$.?`.

`make ci` must be green before any commit. Baseline: **108/108 tests, 283/283
conformance, 713/713 JSONPath compliance (2 skipped: NUL in the selector),
fuzz clean**.

## Non-negotiables

Settled decisions, not preferences. Changing one needs a reason.

- **No dependencies beyond libc.** No getopt_long (GNU-only), nothing
  third-party. Option parsing is hand-rolled on purpose. The only OS calls are
  `os_read`, `os_file_size`, `os_init` and `os_out_closed`, one `#ifdef _WIN32`
  block: POSIX `read`/`fstat` (`_POSIX_C_SOURCE` at the top) or Win32
  `ReadFile`, with a pipe's `ERROR_BROKEN_PIPE` as EOF and stdout set binary,
  since a `\r\n` would break byte-identity with `jq .`. A reader closing the
  pipe early (`json ... | head`) exits 1 without a message: SIGPIPE on POSIX,
  `ERROR_NO_DATA` on Windows, where the CRT reports it as `EINVAL`.
- **Builds clean** under `-std=c99 -O3 -Wall -Wextra -pedantic`. Fix a
  warning, don't silence it.
- **Output is byte-identical to `jq .`** for reformatting (verified on 4.6 MB
  against jq 1.7.1). A change that alters output is a bug unless it's the point.
- **Never free.** Exit reclaims. LeakSanitizer is therefore useless, and
  `ASAN_OPTIONS=detect_leaks=0` in `SANENV` must stay.
- **Numbers keep their raw lexeme**, so `1.50` and `1e309` round-trip. Never
  convert to double, not even to compare: RFC filters compare lexemes as exact
  decimals (`numcmp`). A slurped buffer never moves, so the tree points into
  it; a streamed window does, so `parse_value` copies lexemes to the arena.

## Architecture

**Input: slurp or stream, by `fstat`.** `input()` reads a regular file under
`SLURP_MAX` (256 MiB) whole, in one `read`. Anything else (pipe, socket,
terminal, huge file) streams through a `WINDOW` (64 KiB) that `more()` slides,
keeping the unread `s[i..n)`. So **scanners never advance `i` mid-token**:
`str_end` re-bases in `str_slow`, `scan_number` rescans a number that runs
into the window's end, `lit` asks `avail()`. `skipws()` leaves a byte in the
window unless input is over, which is why `peek()` never refills. `more()`
flushes output before it blocks, so results appear before EOF. Piped, any
input reformats or answers `[@.name]` in ~1.3 MB.

1. **No filter → streaming.** `emit_value()` validates and reformats in one
   pass, no allocation, no tree. ~18x faster than jq; never route it through
   the tree.
2. **Path → `swalk()` over the parse.** A key or a non-negative index steps
   into one member and `skip_value()`s the rest unbuilt; `[@]` handles one
   element at a time. Whatever needs a value whole (globs, slices, negative
   indexes, `-r` strings) and every error goes to `walk()` on a tree of just
   that value, so the two share one set of semantics. Output can precede a
   parse error later in the input; the exit code is still 1.
3. **`--rfc9535` → tree.** The query compiles first (`rfc_parse`), so a bad
   one fails before input is read. `$` in a filter can reach anything, so the
   whole document is parsed; `r_apply()` sends nodes to the sink depth first,
   which is RFC order.

**Results go through a sink** because `[@]` makes the result count a runtime
property; anything returning one value had to be rewritten once already. A
stream is a stream whatever its cardinality: a future collect flag must give
`["ada"]`, not `"ada"`, for a one-element array.

**Per-element memory is bounded.** `sstream()` and `stream_array()` restore an
arena mark after each element, so everything built while answering a query
must come from the arena (`push` uses `arena_grow`, never `xrealloc`). On a
15 MB file piped, `[@]`, `[@.name]` and `[@.+ie]` all peak at ~1.3 MB; `[-1]`
still trees the whole array.

**The tree is exact-size.** The parser does not use `push`: doubling in the
arena abandons every outgrown block (+14% RSS). Children collect on a reusable
scratch stack and each container is copied into the arena once when it closes:
84 MB on a 12 MB file, against 141 MB for the old realloc tree. Arena alignment
is 8; 16 padded every 40-byte `jval` to 48.

**Hot-path inlining is load-bearing.** `om`, `os_`, `str_end`, `num_len` and
`scan_number` are `always_inline`, with rare branches split into
`om_slow`/`str_slow`, which are `COLD` (noinline): left to itself clang inlined
`om_slow` back into every `om` once the file grew. Without it short strings run
15–25% slower. Literals are emitted with constant-length `os_`; the
variable-length copy cost 8%, and `os_` stopped inlining when the RFC code was
added.

**SIMD lives in `scan_str` only.** It must read only `s[0..n)`, return what
the scalar loop would, fall through to it for the tail, and sit under
`!defined(NOSIMD)`. Per target:
- x86: SSE2, which every x86-64 has; SSE4.2's string instructions are slower
  for this. AVX2 takes strings past 24 bytes when the build targets it
  (`-mavx2`) or when the CPU reports it at run time (`simd_init()`:
  `__builtin_cpu_supports` with GCC/clang, `__cpuid` with MSVC), so one binary
  fits every x86-64. `-DNOAVX2` stops at SSE2 (`out/json.sse2`).
- 32-bit x86 with MSVC: no `_BitScanForward64`, so `ctz64` takes two 32-bit
  scans. The bit-by-bit fallback it used to hit ran on nearly every string
  (the SWAR test below finds most closing quotes) and made the SIMD build 50%
  slower than scalar.
- ARM: NEON, with `vshrn` standing in for `movemask`.

The first 8 bytes are a 64-bit SWAR test, not a vector: most strings are
short keys, and NEON without it made `[@.name]` 8% *slower* than scalar.
`scan_ws` stays scalar because a vector loop lost on both compact (1-byte
runs) and indented input. NEON, measured: reformat −10%, long strings 2.1x,
queries even. x86-64, measured with MSVC on Zen 5 (see Benchmarks): SIMD over
scalar is −7% on reformat and filters, even on queries; AVX2 is level with
SSE2, and run-time dispatch with `/arch:AVX2`. On 32-bit x86 SIMD gains
nothing.

## Query syntax

Not JSONPath, though it borrows from it. RFC 9535's `[*]`, and `@` only inside
filters, are deliberate deviations.

```
users.0.name        dotted path; works on objects and arrays
users[0]            bracket index, negative allowed: users[-1]
users[1:3]          slice — Python semantics exactly
users.na+           glob object keys; '+' not '*'
users[@.name]       stream: one result per element, or per object value
users[?@.age > 30]  filter: stream the elements (or values) that match
..id  a..name       search every depth: ..key, ..['k'], ..[n], ..[@], ..[?]
['a.b']  ["x y"]    quoted name: any key, '.', '[', '+' and all
a[0:3,1]  o['x','y']  union: one sub-array or sub-object, each member once
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

**`--rfc9535`** switches to RFC 9535 JSONPath: `$` required, `[*]`, `..`,
`['name']`, unions, filters `[?...]` with `== != < <= > >=`, `&& || !`, and
`length count value match search`. Results are a nodelist, one node per line,
nothing (and rc 0) for none; a slice yields its elements, step 0 selects
nothing, a missing key selects nothing. `match`/`search` take I-Regexp
(RFC 9485) on a Pike VM over code points, so no pattern backtracks; `\p{..}`
uses `src/unicat.h`, generated by `src/unicat.py` from Python's Unicode
tables. `^` and `$` anchor, although RFC 9485's grammar makes them plain
characters, because the compliance suite expects it. Duplicate keys select
the first, as in the default dialect.

**`[?...]` filters are RFC 9535's**, parsed and evaluated by the RFC code
(`path_filter`, `x_true`), so inside one `@`/`$` paths are JSONPath
(`@.tags[0]`, not `@.tags.0`) and a missing member is false, not an error.
`match()`/`search()` are refused: regex in the standard dialect will get its
own behaviour later. A filter streams each matching element or object value
through the rest of the path, as `[@]` does. `bracket_end`
skips a filter whole, so `"]"` inside one is safe. All filters compile before
input is read; one using `$` makes `main` tree the whole document, otherwise
`sfilter()` trees one element at a time (1.4 MB piped on 15 MB).

**`[@]` and `[?]` take arrays and objects**; on an object they see the
values and drop the keys, so `users[@.name]` works whether users is a list or
a map like `{"u1": {...}}`. That is RFC's `*`, spelled the shell-safe way.

**`..` is a search**, so it never errors on what it doesn't find: `..id` is
`$..id`, the selector applied to the node and then to every descendant, depth
first, a node's own matches before its children's (RFC and jq order). What
follows the selector applies to each match with the usual strictness, so
`..[@.name]` fails on the first element without `name`; write `..name`.
`..` trees the value it searches.

**Quoted names** (`['a.b']`, `["x y"]`) take RFC string syntax and escapes and
select exactly that key: no globbing, missing is an error. `bracket_end` and
the root prescan skip them whole, so `['k]']` is safe; a quote starts a name
only right after `[` or `,`.

**Unions keep the shape, unlike RFC's.** `a[0:3,1]` is one array `[a0, a1, a2]`
and `o['y','x']` one object, as a slice or a glob is one value: each member
once, in the order first asked, never a stream. A missing name or index
errors; a slice in a union clamps. Names need an object and indexes an array.
Whitespace is allowed inside brackets.

**`[@...]` is meant to be terminal**: `users[@.name]`, with nesting as
`n[@[@]]`, because a stream is not an object for a `.` suffix to select from.
Not enforced yet: `users[@].name` still succeeds.

**Errors vs empty.** An absent named key is an error (exit 1); a pattern
matching nothing is empty — Python's `d['x']` vs a comprehension. Out-of-range
index errors; out-of-range slice clamps. A filter or a `..` search that finds
nothing is empty.

## Flags

```
-r, --raw-output    string result without quotes or escapes
    --raw           alias
    --rfc9535       the path is RFC 9535 JSONPath (see Query syntax)
-h, --help
--                  end of options
```

Short options cluster (`-rh`); long options take no abbreviations. `-r` applies
only when the result itself is a string; nested strings stay quoted so the
output is still JSON. `--raw-output` is primary because jq also has
`--raw-input`, which makes a bare `--raw` ambiguous.

## Not implemented

- **Regex in standard filters.** `match`/`search` exist only under
  `--rfc9535`; the standard dialect will get its own regex behaviour.
- **Streaming `--rfc9535`.** It always builds the whole tree; `$.a[*].b` with
  no `$` in a filter could stream like `[@]`.
- **Multiple JSON values on stdin.** A second value is "trailing data" today,
  so `json a[@.x] | json y` cannot compose. The window makes this a matter of
  looping in `main`.
- **Error tolerance.** `n[@.name]` fails if any element lacks `name`. jq's `?`
  is the precedent; it is unrelated to RFC 9535's `[?...]`, so decide the
  spelling before implementing either.

## Testing

`test/test.sh` (`t NAME INPUT RC OUT [ARGS...]`; stdout checked only on rc 0),
`test/conformance.sh` (JSONTestSuite), `test/jsonpath.py` (the JSONPath
Compliance Test Suite), `test/fuzz.py` (vs Python), and `test/corpus.py`
(input for `make diff`), and `test/edges.py` (`make check`: strings and
whitespace of every length 0-199 ending at the file's last byte). The first
three run every case twice, piped
(streamed) and redirected (slurped), and the ASan build uses `-DWINDOW=7` so
`make check` puts every token across a refill. Everything runs one process per core
(`JOBS=N` for the shell scripts): a sanitizer build spends ~45 ms starting and
<1 ms working, and no `ASAN_OPTIONS` setting changes that.

**The suite is minimal by measurement; don't prune or pad it by eye.** The 76
default-dialect cases were cut from 140 with 1291 mutants of `json.c` (operator, literal and ±1
swaps including hex, statement deletion, and each `die`/`perr` turned into a
silent `exit(0)`). Each of the 76 cases is the only one catching some mutant.
A by-judgement pass had kept 111 yet dropped six cases that were the only ones
catching something. Error cases check only the exit code, so only the `exit(0)`
operator tells error sites apart. The `flt-`, `desc-`, `qname-`, `union-`, `obj-` and `rfc-` cases (the `rfc-` ones cover what the compliance
suite cannot: output format, `-r`, exact numbers) and `pipe-closed` (no stderr
when the reader quits early; only Windows can fail it) have not been through a
mutation run yet.

Add a case for every behaviour change. Known gaps: no case reaches the tree
parser's syntax errors (malformed input is only tested without a filter, and
`json a` parses differently from `json`), `path nesting too deep`, `number too
large in path`, or `index ... is too large`.

**Conformance alone is not enough.** An injected off-by-one in a vector loop
passed every test, all conformance and byte-identical output on 4.6 MB while
reading out of bounds; ASan caught it only with an input sized to the
allocation edge. Hence `test/edges.py`, and a slurped file's buffer is exactly
its size (`more()` probes one byte before growing a full buffer): with one
byte of slack, a planted `i + 15 <= n` in every SIMD loop went unreported. The arena blunts ASan further: overflowing one sub-allocation
into the next is a legal write inside a live chunk.

## Benchmarks

Against jq 1.7.1 on a 4.6 MB array of 15,840 objects, output byte-identical:

| | jq | json |
|---|---|---|
| reformat whole doc | 154.8 ms | 8.5 ms (18x) |
| select one deep value | 54.0 ms | 12.5 ms (4.3x) |

Those predate streaming. Against the previous commit, on a 15 MB array of
60,000 objects (best of 20; arm64): reformatting a file 32.5 → 34.1 ms (the
refill checks; piped 36.4 → 34.2), `5000.nested.a.b` 32 → 24 ms and
`[@.name]` 38 → 27 ms, at 15.7 MB instead of 80 MB from a file and 1.3 MB
piped. `jq --stream` is flat at 6.2 MB.

Windows, MSVC 19.44 `-O2`, Ryzen 9 9900X, on a synthetic 23.9 MB array of
60,000 objects (best of 15, input from a file, ms; piped is 3–4 ms faster):

| | x86 | x86 scalar | x64 | x64 SSE2 | x64 scalar |
|---|---|---|---|---|---|
| reformat | 38.6 | 39.1 | 33.2 | 34.3 | 35.6 |
| `5000.nested.a.b` | 29.3 | 29.3 | 27.9 | 27.0 | 28.2 |
| `[@.name]` | 34.5 | 34.6 | 31.1 | 31.3 | 30.9 |
| `[?@.age > 80].id` | 39.5 | 39.8 | 34.2 | 34.4 | 37.2 |
| `--rfc9535 '$[?@.age > 80].id'` | 57.1 | 57.1 | 57.8 | 58.4 | 61.3 |

x86 and x64 are the default builds (AVX2 chosen at run time); x86 is after
the `ctz64` fix, before which it took 48–75 ms. Process spawn is 3.5–4.5 ms of
each run. Peak working set: 28 MB from a file, 5.4 MB piped (of which ~4 MB is
the bare process), and 133 MB (x64) or 97 MB (x86) for `--rfc9535`'s tree.
Python's `json` takes 1970 ms to reformat the same file; jq was not measured.

Measure at a realistic size: at 18 KB, process spawn is 0.85 ms of a 1.05 ms
run. A single `time cat x | json > f` measures disk writeback, not the build.
Didn't matter: `-O3` over `-O2` (+1.8%), hand unrolling, vector `scan_ws`.
Did: no tree (3x), batched output (+24% on string-heavy data), a memcpy fast
path for escape-free strings (+18%).

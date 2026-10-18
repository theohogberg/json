# json

`json` is a fast replacement for jq's common uses: reformat JSON, select from
it, and query it with JSONPath (RFC 9535). It is written in C99 against libc
only and accelerates its hot loop with SIMD. Everything is one translation
unit, `src/json.c`, which includes the generated `src/unicat.h`. The only OS
calls are POSIX `read`/`fstat`, or Win32 `ReadFile`/`GetFileType` under
`_WIN32`.

```
curl -s URL | json                      # reformat
curl -s URL | json users.0.name         # select
curl -s URL | json -r users[@.name]     # stream, unquoted
curl -s URL | json --rfc9535 '$.users[?@.age > 30].name'
```

The goals, in priority order:

1. **Correct.** Reformatted output is byte-identical to `jq .`, `--rfc9535`
   passes the JSONPath Compliance Test Suite, and the parser passes
   JSONTestSuite.
2. **Fast.** Reformatting is ~25x faster than jq 1.8.2 and queries 8–10x
   (`make bench`). Every change to a hot path is measured (see Benchmarks).
3. **Bounded memory.** Piped input is processed in a sliding window, so
   reformatting and per-element queries use memory bounded by the largest
   element, at any input size (~1.3 MB peak on a 15 MB file).
4. **Shell-friendly.** The default query dialect avoids `$` and `*`, so
   dotted paths and `+` globs work unquoted in sh, bash and zsh. Brackets
   still need quotes in zsh (see DESIGN.md, section 1).

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
make ci                   # all of the above, ~2 min; run before committing
make bench [RUNS_BENCH=N] # against jq (on PATH), not part of ci; ~1 min
make DEPTH=256 test       # changed flags trigger a rebuild, checked at startup
```

Run `make ci` before every commit; it must pass. A commit that changes only
documentation (`*.md`) does not need it. Releases are tags named
`v0.N`, N being the commit count (`git rev-list --count HEAD`). Make a tag
only when a new design has been implemented in full, and update DESIGN.md
with it (see its introduction). The current
baseline is **109/109 tests, 283/283 conformance, 713/713 JSONPath compliance (2 skipped
because their selector contains NUL), fuzz clean**.

### Windows

The same targets work from MSYS2, MinGW or Git Bash. When `OS=Windows_NT`,
the makefile:

- adds `.exe` to every output;
- exports `PYTHONUTF8=1` so the test scripts write UTF-8;
- sets `CC` to cl, or clang-cl if cl is not on PATH;
- switches to MSVC flags (`-std:c11 -O2 -W4`) when `CC` is `cl` or
  `clang-cl`. `check` and `fuzz` then use MSVC's ASan, which has no UBSan.

Recipes need a POSIX `sh`. When make runs from `cmd.exe`, it would use cmd
as its shell; to prevent that, the makefile locates `git.exe`, takes Git for
Windows' `usr\bin\sh.exe` beside it, and adds that directory to PATH.

Without make: `cl /O2 /std:c11 src\json.c /Fe:json.exe`.

Use an x64 prompt (`vcvars64.bat`). A Developer Command Prompt's `cl`
targets 32-bit x86 by default, and that build is ~15% slower (see
Benchmarks).

`os_init` rebuilds `argv` as UTF-8 from `GetCommandLineW`. The CRT's own
`argv` uses the ANSI code page, which turns a query like `$.☺` into `$.?`.

Verified: `make ci` passes with MSVC 19.44 (x86) under Git Bash and GnuWin32
make 3.81.

Build with each platform's native compiler: MSVC on Windows, clang or gcc on
macOS and Linux. MinGW gcc is not a supported build.

## Rules

Each rule records a decision already made. To change one, give a reason.

- **Depend on libc only.** Nothing third-party, and no GNU-only functions
  such as `getopt_long`; option parsing is hand-rolled for this reason. All
  OS-specific code is four functions in one `#ifdef _WIN32` block:
  `os_read`, `os_file_size`, `os_init` and `os_out_closed`.
  - POSIX uses `read`/`fstat`, enabled by `_POSIX_C_SOURCE` at the top of
    the file.
  - Windows uses `ReadFile` and treats a pipe's `ERROR_BROKEN_PIPE` as EOF.
    It sets stdout to binary mode, because a `\r\n` would break
    byte-identity with `jq .`.
  - When the reader closes the pipe early (`json ... | head`), exit 1
    without a message. On POSIX, SIGPIPE does this. On Windows the write
    fails with `ERROR_NO_DATA`, which the CRT reports as `EINVAL`.
- **Build without warnings** under `-std=c99 -O3 -Wall -Wextra -pedantic`.
  Fix the code that causes a warning; do not suppress the warning.
- **Keep reformatted output byte-identical to `jq .`** (verified on 4.6 MB
  against jq 1.7.1). Treat any change in output as a bug unless the change
  is the purpose of the commit.
- **Never free memory.** The process exit reclaims it. For this reason
  LeakSanitizer is useless here, and `ASAN_OPTIONS=detect_leaks=0` in
  `SANENV` must stay.
- **Keep every number's raw lexeme**, so `1.50` and `1e309` round-trip.
  Never convert a number to double, even to compare it: RFC filters compare
  lexemes as exact decimals (`numcmp`). A slurped buffer never moves, so the
  tree points into it. A streamed window does move, so `parse_value` copies
  lexemes into the arena.

## Architecture

### Input: slurp or stream, chosen by `fstat`

`input()` reads a regular file smaller than `SLURP_MAX` (256 MiB) whole, in
one `read`. Any other input (pipe, socket, terminal, larger file) streams
through a `WINDOW` (64 KiB) that `more()` slides forward, keeping the unread
bytes `s[i..n)`.

Because the window can move, **a scanner must never advance `i` in the
middle of a token**:

- `str_end` re-bases its position in `str_slow`.
- `scan_number` rescans a number that runs into the end of the window.
- `lit` asks `avail()` for enough bytes before matching a literal.
- `skipws()` leaves at least one byte in the window unless input has ended,
  so `peek()` never needs to refill.

`more()` flushes pending output before it blocks on a read, so results
appear before EOF. Piped, reformatting or answering `[@.name]` uses memory
bounded by the largest element (~1.3 MB on a 15 MB file).

### Three evaluation paths

1. **No query: stream.** `emit_value()` validates and reformats in a single
   pass, with no allocation and no tree. This path is ~18x faster than jq;
   never route it through the tree.
2. **Default-dialect path: `swalk()` over the parse.** A key or a
   non-negative index steps into one member and passes over the rest with
   `skip_value()`, which builds nothing. `[@]` handles one element at a
   time. Anything that needs a whole value (globs, slices, negative indexes,
   `-r` strings), and every error, goes to `walk()` on a tree of just that
   value, so both functions share one set of semantics. Output can precede a
   parse error later in the input; the exit code is then still 1.
3. **`--rfc9535`: whole tree.** The query compiles first (`rfc_parse`), so a
   bad query fails before any input is read. A `$` inside a filter can reach
   any part of the document, so the whole document is parsed. `r_apply()`
   sends nodes to the sink depth first, which is RFC order.

### Results go through a sink

`[@]` makes the number of results a runtime property, and code that returned
a single value had to be rewritten once already for this reason. Treat every
result as a stream, of any length. A future collect flag must
therefore print `["ada"]` for a one-element array, and never a bare `"ada"`.

### Per-element memory is bounded

`sstream()` and `stream_array()` restore an arena mark after each element.
Everything built while answering a query must therefore come from the arena:
`push` uses `arena_grow` and never `xrealloc`. On a 15 MB file piped,
`[@]`, `[@.name]` and `[@.+ie]` all peak at ~1.3 MB. `[-1]` still builds a
tree of the whole array.

### The tree is allocated at its exact size

The parser does not use `push`, because doubling inside the arena abandons
every outgrown block (+14% RSS). Children collect on a reusable scratch
stack, and each container is copied into the arena once, when it closes.
Result: 84 MB on a 12 MB file, against 141 MB for the old realloc-based
tree. Arena alignment is 8; an alignment of 16 padded every 40-byte `jval`
to 48 bytes.

### Hot-path inlining

`om`, `os_`, `str_end`, `num_len` and `scan_number` are `always_inline`.
Their rare branches are split out into `om_slow` and `str_slow`, which are
marked `COLD` (noinline). Keep both markings: without `COLD`, clang inlined
`om_slow` back into every `om` once the file grew, and without inlining,
short strings run 15–25% slower.

Emit literals with constant-length `os_`. A variable-length copy cost 8%, and
`os_` stopped being inlined when the RFC code was added.

### SIMD

All SIMD code lives in `scan_str`. It must:

- read only `s[0..n)`;
- return exactly what the scalar loop would;
- fall through to the scalar loop for the tail;
- sit under `!defined(NOSIMD)`.

Per target:

- **x86-64:** SSE2, which every x86-64 CPU has. SSE4.2's string
  instructions are slower for this job. Strings longer than 24 bytes go to
  AVX2 when the build targets it (`-mavx2`) or when the CPU reports it at
  run time. `simd_init()` checks with `__builtin_cpu_supports` under
  GCC/clang and `__cpuid` under MSVC, so one binary fits every x86-64 CPU.
  `-DNOAVX2` stops at SSE2 (`out/json.sse2`).
- **32-bit x86 with MSVC:** there is no `_BitScanForward64`, so `ctz64` uses
  two 32-bit scans. It previously fell back to a bit-by-bit loop, which ran
  on nearly every string (the SWAR test below finds most closing quotes)
  and made the SIMD build 50% slower than scalar.
- **ARM:** NEON, with `vshrn` in place of x86's `movemask`.

The first 8 bytes of every string are tested as one 64-bit word (SWAR)
before any vector code runs. Most strings are short keys; without this test,
NEON made `[@.name]` 8% slower than scalar.

`scan_ws` stays scalar. A vector loop was slower on both compact input
(whitespace runs of 1 byte) and indented input.

Measured gains over scalar:

- NEON: reformat −10%, long strings 2.1x faster, queries unchanged.
- x86-64, MSVC on Zen 5 (see Benchmarks): reformat and filters −7%, queries
  unchanged. AVX2 performs the same as SSE2, and run-time dispatch performs
  the same as `/arch:AVX2`.
- 32-bit x86: no gain.

## Query language

### Default dialect

The default dialect is based on JSONPath, with two deliberate differences
from RFC 9535: iteration is written `[@]` where RFC writes `[*]`, and `@`
also appears outside filters.

```
users.0.name        dotted path; works on objects and arrays
users[0]            bracket index, negative allowed: users[-1]
users[1:3]          slice, with Python semantics
users.na+           glob object keys; '+' instead of '*'
users[@.name]       stream: one result per element, or per object value
users[?@.age > 30]  filter: stream the elements (or values) that match
..id  a..name       search every depth: ..key, ..['k'], ..[n], ..[@], ..[?]
['a.b']  ["x y"]    quoted name: any key, including '.', '[' and '+'
a[0:3,1]  o['x','y']  union: one sub-array or sub-object, each member once
```

**Streams and filters are the two bracket forms that yield one result per
element.** Below, `P` stands for a path and `E` for an expression; both are
placeholders, not syntax.

- A **stream** `[@P]` runs `P` on each element of an array, or each value
  of an object, with `@` standing for that element. `P` may be empty
  (`[@]` yields each element itself), a member path (`[@.name]`,
  `[@.head.name]`), or contain another stream (`[@[@]]`,
  `[@.members[@.age]]`).
- A **filter** `[?E]` keeps each element for which `E`, an RFC 9535
  logical expression, is true (`[?@.age > 30]`), and runs the rest of the
  path on it.

The rest of this file and DESIGN.md say "stream" and "filter" for these.

**Slices follow Python exactly**, verified case by case: negative indices,
clamped bounds, negative steps, and `[3:1]` gives `[]`. A zero step is an
error (RFC 9535 selects nothing instead). A slice yields one sub-array.

**Globs use `+` because the shell expands `*`** against the working
directory: with a file `pre1.hello` present, `json pre*.hello` would run the
query `pre1.hello`. `+` passes through sh, bash and zsh unquoted. Apart from
that, globs behave exactly like `fnmatch(3)` (2908 patterns compared, zero
mismatches). There are no character classes, because `[` starts an index or
slice. Globbing an object returns an object; globbing any other type is an
error.

**Filters use RFC 9535's grammar and semantics.** The RFC code
parses and evaluates them (`path_filter`, `x_true`). Inside a filter, paths
from `@` or `$` are JSONPath (`@.tags[0]`; the default form `@.tags.0` does
not work there), and a missing member makes the test false instead of
raising an error. `match()` and `search()` are refused in this dialect; the
default dialect will get its own regex behaviour later. A filter streams
each matching element or object value through the rest of the path, as
`[@]` does. `bracket_end` skips a filter as a whole, so a `"]"` inside one
is safe. All filters compile before input is read. If a filter uses `$`,
`main` builds a tree of the whole document; otherwise `sfilter()` builds a
tree of one element at a time (1.4 MB piped on a 15 MB file).

**`[@]` and `[?]` accept arrays and objects.** On an object they see the
values and drop the keys, so `users[@.name]` works whether `users` is a list
or a map such as `{"u1": {...}}`. This is RFC 9535's `*` in a shell-safe
spelling.

**`..` is a search, so it never fails on a missing match.** `..id` means
`$..id`: the selector applies to the node and then to every descendant,
depth first, with a node's own matches before its children's (the order RFC
9535 and jq use). Whatever follows the selector applies to each match with
the usual strictness, so `..[@.name]` fails on the first element that lacks
`name`; write `..name` instead. `..` builds a tree of the value it searches.

**Quoted names** (`['a.b']`, `["x y"]`) use RFC string syntax and escapes and
select exactly that key: no globbing, and a missing key is an error.
`bracket_end` and the root prescan skip quoted names as a whole, so `['k]']`
is safe. A quote starts a name only directly after `[` or `,`.

**Unions keep their shape**, unlike RFC 9535's. `a[0:3,1]` gives one array
`[a0, a1, a2]` and `o['y','x']` one object, in the same way a slice or a
glob gives one value. Each member appears once, in the order first asked
for; a union never streams. A missing name or index is an error; a slice
inside a union clamps. Names require an object and indexes an array.
Whitespace is allowed inside brackets.

**A stream is meant to end a path**: `users[@.name]`, nested as `n[@[@]]`. A
stream is not an object, so a `.` suffix has nothing to select from. This is
not enforced yet: `users[@].name` still succeeds.

**Errors versus empty results.** A missing named key is an error (exit 1). A
pattern that matches nothing gives an empty result. This mirrors Python's
`d['x']` versus a comprehension. An out-of-range index is an error; an
out-of-range slice clamps. A filter or a `..` search that finds nothing
gives an empty result.

### `--rfc9535`

With `--rfc9535` the query is RFC 9535 JSONPath: `$` is required, and it
supports `[*]`, `..`, `['name']`, unions, and filters (`[?@.x > 1]`) with
`== != < <= > >=`, `&& || !` and the functions `length`, `count`, `value`,
`match` and `search`.

- The result is a nodelist, printed one node per line. No match prints
  nothing and exits 0.
- A slice yields its elements one by one; step 0 selects nothing; a missing
  key selects nothing.
- `match` and `search` take I-Regexp (RFC 9485) and run on a Pike VM over
  code points, so no pattern can backtrack. `\p{..}` uses `src/unicat.h`,
  generated by `src/unicat.py` from Python's Unicode tables.
- `^` and `$` act as anchors, because the compliance suite expects it, even
  though RFC 9485's grammar treats them as plain characters.
- With duplicate keys, the first one is selected, as in the default dialect.

## Flags

```
-r, --raw-output    string result without quotes or escapes
    --raw           alias
    --rfc9535       the path is RFC 9535 JSONPath (see Query language)
-h, --help
--                  end of options
```

Short options cluster (`-rh`). Long options cannot be abbreviated. `-r`
applies only when the result itself is a string; strings nested inside the
result stay quoted, so the output remains valid JSON. `--raw-output` is the
primary name because jq also has `--raw-input`, which makes a bare `--raw`
ambiguous.

## Not implemented

- **Regex in default-dialect filters.** `match` and `search` exist only
  under `--rfc9535`; the default dialect will get its own regex behaviour.
- **Streaming `--rfc9535`.** It always builds the whole tree. A query such
  as `$.a[*].b`, with no `$` inside a filter, could stream the way `[@]`
  does.
- **Multiple JSON values on stdin.** A second value is rejected as
  "trailing data", so `json a[@.x] | json y` cannot compose. The window
  reduces this to a loop in `main`.
- **Error tolerance.** `n[@.name]` fails if any element lacks `name`. jq's
  `?` operator is the precedent. It is unrelated to RFC 9535's filters, so
  choose a spelling before implementing either.

## Testing

| file | what it runs |
|---|---|
| `test/test.sh` | regression cases: `t NAME INPUT RC OUT [ARGS...]`; stdout is checked only when rc is 0 |
| `test/conformance.sh` | JSONTestSuite |
| `test/jsonpath.py` | JSONPath Compliance Test Suite |
| `test/fuzz.py` | differential test against Python's `json` |
| `test/corpus.py` | generates the input for `make diff` |
| `test/edges.py` | `make check`: strings and whitespace of every length 0–199, ending at the file's last byte |
| `test/bench.py` | `make bench`: generates inputs into `out/bench/`, checks json and jq print the same bytes, times both |

The first three run every case twice: piped (streamed) and redirected
(slurped). The ASan build uses `-DWINDOW=7`, so under `make check` every
token straddles a window refill. All scripts run one process per core (set
`JOBS=N` for the shell scripts): a sanitizer build spends ~45 ms starting
and under 1 ms working, and no `ASAN_OPTIONS` setting reduces the startup.

**The suite is minimal by measurement. Do not prune or pad it by
judgement.** The 76 default-dialect cases were cut from 140 using 1291
mutants of `json.c`: operator, literal and ±1 swaps (hex included),
statement deletion, and each `die`/`perr` replaced by an `exit(0)` without a message.
Each of the 76 cases is the only one that catches some mutant. An earlier
pass by judgement kept 111 cases, yet dropped six that were the only ones
catching something. Error cases check only the exit code, so only the
`exit(0)` mutant tells error sites apart.

Not yet through a mutation run: the `flt-`, `desc-`, `qname-`, `union-`,
`obj-` and `rfc-` cases, `pipe-closed` and `st-empty-bytes`. The `rfc-`
cases cover what the compliance suite cannot: output format, `-r` and exact
numbers. `st-empty-bytes` counts bytes because `$(...)` strips the newline
it checks for.
`pipe-closed` checks that nothing reaches stderr when the reader quits
early; only a Windows build can fail it.

Add a case for every behaviour change. Known gaps, with no case reaching
them:

- the tree parser's syntax errors (malformed input is tested only without a
  query, and `json a` parses differently from `json`);
- `path nesting too deep`;
- `number too large in path`;
- `index ... is too large`.

**Run the sanitizers; conformance is not enough.** An off-by-one injected
into a vector loop passed every test, all of conformance and a byte-identical
4.6 MB output comparison, while reading out of bounds. ASan caught it only
with an input sized to end exactly at the allocation edge. Two measures
follow from this:

- `test/edges.py` generates inputs that end at every length.
- A slurped file's buffer is exactly the file's size: `more()` probes for
  one more byte before it grows the buffer. With one byte of slack, a
  planted `i + 15 <= n` in every SIMD loop went unreported.

The arena weakens ASan further: an overflow from one sub-allocation into the
next is a legal write inside a live chunk.

## Benchmarks

`make bench` against jq 1.8.2 on an M1 Pro (median of 15, input from a
file; README.md has the full table, which must be updated with it):

| | jq | json |
|---|---|---|
| reformat, 19.8 MB of 60k records | 967 ms | 39.4 ms (24.5x) |
| `[@.name]` on the same | 324 ms | 34.0 ms (9.5x) |
| `[?@.age > 80].id` on the same | 320 ms | 38.8 ms (8.2x) |
| reformat, 10.9 MB nested and string-heavy | 455 ms | 15.8 ms (28.9x) |
| reformat, 19 KB | 5.8 ms | 2.7 ms (2.2x; ~2.5 ms is process spawn) |

jq peaks at ~190 MB on the 19.8 MB file, json at 20 MB (the slurped input).

Against jq 1.7.1 on a 4.6 MB array of 15,840 objects, output byte-identical
(measured before streaming input existed):

| | jq | json |
|---|---|---|
| reformat whole doc | 154.8 ms | 8.5 ms (18x) |
| select one deep value | 54.0 ms | 12.5 ms (4.3x) |

Effect of adding streaming input, on a 15 MB array of 60,000 objects (best
of 20; arm64):

| | before | after |
|---|---|---|
| reformat, from a file | 32.5 ms | 34.1 ms (cost of the refill checks) |
| reformat, piped | 36.4 ms | 34.2 ms |
| `5000.nested.a.b` | 32 ms | 24 ms |
| `[@.name]` | 38 ms | 27 ms |
| peak memory | 80 MB | 15.7 MB from a file, 1.3 MB piped |

For comparison, `jq --stream` stays flat at 6.2 MB.

Windows, MSVC 19.44 `-O2`, Ryzen 9 9900X, on a synthetic 23.9 MB array of
60,000 objects (best of 15, input from a file, in ms; piped input is 3–4 ms
faster):

| | x86 | x86 scalar | x64 | x64 SSE2 | x64 scalar |
|---|---|---|---|---|---|
| reformat | 38.6 | 39.1 | 33.2 | 34.3 | 35.6 |
| `5000.nested.a.b` | 29.3 | 29.3 | 27.9 | 27.0 | 28.2 |
| `[@.name]` | 34.5 | 34.6 | 31.1 | 31.3 | 30.9 |
| `[?@.age > 80].id` | 39.5 | 39.8 | 34.2 | 34.4 | 37.2 |
| `--rfc9535 '$[?@.age > 80].id'` | 57.1 | 57.1 | 57.8 | 58.4 | 61.3 |

- The x86 and x64 columns are the default builds, which choose AVX2 at run
  time. The x86 figures are after the `ctz64` fix; before it, x86 took
  48–75 ms.
- Process spawn accounts for 3.5–4.5 ms of each run.
- Peak working set: 28 MB from a file; 5.4 MB piped, of which ~4 MB is the
  bare process; 133 MB (x64) or 97 MB (x86) for the `--rfc9535` tree.
- Python's `json` takes 1970 ms to reformat the same file. jq was not
  measured on this machine.

### How to measure

- Use a realistic input size. At 18 KB, process spawn is 0.85 ms of a
  1.05 ms run.
- Do not time a single `time cat x | json > f`; it measures disk writeback
  instead of the build.

Measured and found irrelevant: `-O3` over `-O2` (+1.8%), hand unrolling,
vector `scan_ws`. Measured and kept: no tree (3x), batched output (+24% on
string-heavy data), a memcpy fast path for escape-free strings (+18%).

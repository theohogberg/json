# Design record

This file records the analysis behind three decisions in `json`:

1. whether to follow RFC 9535 (JSONPath);
2. whether to stream input or read the whole document;
3. how SIMD intrinsics fit each way of reading input.

The analysis was written at `v0.18`. Its measurements were taken at
`v0.17` on arm64 macOS (8 cores, Apple clang 17) unless marked as an
estimate. The test file is a 12.1 MB array of 60,000 API-shaped objects.
Most proposals have since been implemented, and the numbers in sections 1–3
predate that work. CLAUDE.md describes the current design and has current
benchmarks.

Versions are tags named `v0.N`, where N is the number of commits up to the
tagged one (`git rev-list --count`). The Status table and the "json now"
column below are current as of `v0.36`. **Whenever a new tag is made, update
this document:** bring the Status table and "json now" column up to date and
change the version in this paragraph to the new tag.

## Status

| proposal | section | status |
|---|---|---|
| Use RFC 9535 grammar and semantics for filters (`[?@.age > 30]`) | 1 | Done, in both dialects |
| Defer `match`/`search` until a regex engine is worth its size | 1 | Reversed: implemented under `--rfc9535` as I-Regexp on a Pike VM; the default dialect refuses them and has `@.x ~ /re/` (a search; `^` and `$` anchor) on the same engine |
| A `--rfc9535` mode that runs the compliance suite | 1 | Done: 713/713 |
| Parse both dialects into one internal selector list | 1 | Not done: the default dialect has its own path parser, and only filters go through the RFC code |
| Bracket-free spellings: `users.-1`, `users.1:3`, `users.@.name` | 1 | Done (`rebracket`): a component that is `-N` or `a:b:c` is always an index or slice, so keys such as `"-1"` need quotes (`['-1']`) |
| `..` descendants and quoted names in the default dialect | 1 | Done |
| Choose slurp or stream per input with `fstat` | 2 | Done; a regular file is read with one `read` instead of `mmap` |
| Per-element trees for piped streams (`[@.name]`) | 2 | Done (`sstream`, `stream_array`, `sfilter`) |
| Random-access queries buffer only the array they need | 2 | Done: `[-1]` builds a tree of that whole array; there is no k-element ring buffer |
| Forward-only skip-scan for plain paths | 2 | Done (`swalk`, `skip_value`) |
| Pad the input buffer by 64 bytes | 3 | Rejected: the buffer is exactly the input's size, so ASan reports any read past the end (CLAUDE.md, Testing) |
| SSE2 and NEON `scan_str` | 3 | Done, plus AVX2 with run-time dispatch and an 8-byte SWAR test |
| Vectorised `scan_ws` | 3 | Measured and rejected: slower on compact and indented input |
| Vectorised UTF-8 validation | 3 | Not done: `utf8_check` is scalar |
| Vectorised output escaping in `print_str` | 3 | Not done |
| Stage-1 structural indexing | 3 | Not done |

## 1. Following RFC 9535 (JSONPath)

RFC 9535 standardises JSONPath:

- A query starts at `$` and is a sequence of segments: child segments
  (`.name`, `['name']`, `[0]`, `[1:3]`, `[*]`) and descendant segments
  (`..`).
- Filters such as `[?@.x == 1]` use `@` for the current node, comparisons, `&&`/`||`/`!`
  and the functions `length`, `count`, `match`, `search` and `value`.
  `match` and `search` use I-Regexp (RFC 9485).
- The result is always a *nodelist*.
- Evaluation never fails. An invalid query is rejected before evaluation;
  every valid query, including one naming a missing member, selects zero or
  more nodes.

### How json differed

The middle column is `json` at `v0.17`, when these measurements were taken.

| | RFC 9535 | json at `v0.17` | json now |
|---|---|---|---|
| root | `$` required | implicit; `.a` and `a` both work | unchanged |
| iteration | `[*]` | `[@]`, `[@.name]`, `[@.a[@.b]]` | also `users.@.name`; `[*]` under `--rfc9535` |
| glob on keys | none (only `*`) | `na+`, with `fnmatch` semantics | unchanged |
| slice result | nodelist: one result per element | one sub-array value | unchanged, also as `users.1:3`; nodelist under `--rfc9535` |
| slice step 0 | selects nothing | error, as in Python | unchanged; selects nothing under `--rfc9535` |
| missing member | empty nodelist, success | error, exit 1 | unchanged; empty under `--rfc9535` and inside filters |
| descendants | `..` | none | `..` |
| filters | `[?@.x == 1]`, functions | not implemented | RFC 9535 filters, plus `~` regex in the default dialect; `match`/`search` only under `--rfc9535` |
| member names | `['a b']`, `["a"]` | `a b` as one argument | `['a b']`, `["a"]` |

### Arguments for following RFC 9535

- **The syntax is known and queries are portable.** Anyone who has used
  JSONPath in a library, an API gateway or a database can read
  `$.users[*].name` without documentation, and the query behaves the same
  everywhere.
- **The next feature already has a specification.** Filters
  were next on the roadmap. RFC 9535 defines the filter grammar, comparison
  semantics across types, the five functions and their well-typedness rules.
  Inventing our own would mean designing all of that and defending every
  corner case.
- **A compliance suite exists.** The JSONPath Compliance Test Suite is a
  shared corpus of queries and expected nodelists. It gives the query side
  the same advantage JSONTestSuite gave the parser (283 cases we did not have
  to write).
- **It removes the error-tolerance problem.** CLAUDE.md lists as a known
  weakness that `n[@.name]` fails the whole query when one element lacks
  `name`. Under RFC semantics, that element contributes an empty result and
  the rest of the query continues.
- **It defines normalized paths.** The RFC defines a canonical location
  string (`$['users'][0]['name']`), which is a ready-made output format for
  a future flag that prints where each result came from.

### Arguments against

- **RFC syntax conflicts with the shell, and the shell is the whole
  interface.** `$`, `*`, `?`, `[` and quotes are all special. Measured in
  this repo:
  - In zsh, the default macOS shell, every unquoted query containing `[`
    fails with `no matches found`, and `$['a']` is a syntax error.
  - In sh, bash and zsh alike, a file named `users0` in the working
    directory turns `users[0]` into the query `users0` without any warning.

  RFC syntax therefore means quoting every query, every time.

  The default syntax has the same problem with brackets: they glob, so the
  CLAUDE.md example `json -r users[@.name]` fails unquoted in zsh. Choosing
  `+` over `*` solved only half the problem; see the recommendation.
- **Missing keys give empty results without a warning.** Under RFC rules, a
  mistyped key exits 0 with no output, and a `set -e` script carries on with
  nothing. The default dialect's error on a missing key catches the typo. jq
  makes a similar choice: `.foo` on a missing key prints `null` instead of
  printing nothing.
- **Results are nodelists.** `users[1:3]` would print two separate values
  instead of one array, so getting the sub-array back needs a collect flag.
  Both jq and Python users expect the Python-style sub-array.
- **It covers only half the tool.** The RFC defines selection. Output
  formatting, `-r`, byte-identical `jq .` output and multiple values on
  stdin remain ours to define whichever syntax we pick.
- **Full conformance conflicts with "libc only".** `match`/`search` need an
  I-Regexp engine. POSIX `regcomp` is outside C99, and its ERE dialect
  differs from I-Regexp (Unicode character class escapes, for one). That
  means a regex engine inside `json.c`, plausibly as large as everything
  else in the file.
- **Two dialects double the test surface.** A `--rfc9535` mode means every
  query feature behaves two ways, and the minimal suite (each case the only
  one catching some mutant) would need cases for both.

### Recommendation

Adopt RFC 9535 *semantics* wherever `json` has none yet, and keep the
shell-safe *syntax*:

1. Take the filter grammar, the comparison rules and
   `length`/`count`/`value` for filters as specified. Defer
   `match`/`search` until a regex engine is worth its size.
2. Parse both dialects into one internal selector list. Express the real
   semantic differences as evaluation flags that `--rfc9535` sets, so that
   mode can run the compliance suite. Those differences are: sub-array or
   stream for slices, error or empty for a missing member, and error or
   nothing for step 0.
3. Make every query expressible without brackets, so quoting becomes
   optional: `users.-1`, `users.1:3`, `users.@.name`. `@`, `:` and `-` are
   not special in sh, bash or zsh. Keep brackets as an alias for users who
   quote.

## 2. Streaming versus reading the whole document

Streaming and reading are two separate axes. At `v0.17`, `json` used one
corner of each:

| | read incrementally | read to EOF (`slurp`) |
|---|---|---|
| single pass, no tree | — | **no query** (`emit_value`) |
| tree for one element at a time | — | — |
| tree for the whole document | — | **any query** (`parse_value`) |

The "streaming" path therefore streamed its processing but read all of its
input first. Measured: when input arrived over a pipe, the first output byte
appeared only after the input had ended.

| on the 12.1 MB file | peak RSS | time |
|---|---|---|
| reformat (single pass) | 21 MB | 33 ms |
| `5000.nested.a.b` (whole tree) | 84 MB | 35 ms |
| `[@.name]` (whole tree) | 85 MB | 42 ms |

The single-pass path costs the input buffer plus a 64 KB output buffer. The
tree costs about 7x the input, even though `5000.nested.a.b` needs only one
of the 60,000 elements.

### Option A: stream the input, with bounded memory

Advantages:

- **Memory is bounded by the largest element instead of the input.** Inputs
  larger than RAM become possible, and `jq --stream` (flat at 6.2 MB)
  stops being the only tool that handles them.
- **Output appears before EOF.** Slow APIs, `tail -f` and NDJSON feeds show
  results as they arrive.
- **Pipelines need it anyway.** Multiple values on stdin (so that
  `json a[@.x] | json y` composes) means reading value after value from a
  stream that may never end.
- **Queries can evaluate one element at a time.** A stream such as `[@.name]` can build
  a tree of one element, then restore the arena mark before the next. That
  machinery already existed in `stream_array`; only the input side was
  missing.

Disadvantages:

- **There is no lookahead.** `[-1]` and `[-k:]` need a ring buffer of k
  elements; `[::-1]`, `length()` and any sorting need the whole array. These
  queries must fall back to buffering, so bounded memory becomes a property
  of each query instead of the tool.
- **Invalid input produces partial output.** A syntax error at byte N is
  reported after everything before it has been printed. The reformat path
  already behaves this way, because it validates while printing. With
  incremental input this is the only option, because there is no second
  pass.
- **Pointers into the input break.** The streaming path echoes strings, and
  the tree stores number lexemes, as pointers into the input buffer. With a
  buffer that refills, output must be flushed before each refill, and
  lexemes must be copied into the arena. This keeps the raw-lexeme rule
  (CLAUDE.md, Rules) at the cost of a copy.
- **The lexer must be resumable.** A refill can cut a string, a number or a
  `\u` escape anywhere. Either the lexer carries state across refills, or
  the unfinished token moves to the front of the buffer before each refill.

### Option B: read the whole document into memory

Advantages:

- **It is simple.** One contiguous buffer gives random access, every pointer
  stays valid for the life of the process, and the never-free design fits
  naturally.
- **Input can be validated before anything is printed.** Two passes over a
  buffer in cache are cheap, and invalid input then prints nothing.
- **Zero copy is possible.** When stdin is a regular file (`json < f`),
  `fstat` plus `mmap` would replace the copy in `slurp()`. Pipes, as in
  `curl | json`, still need the copy.
- **It is the best case for SIMD.** A contiguous, paddable buffer can be
  scanned 64 bytes at a time with no state carried across refills
  (section 3).

Disadvantages:

- **Memory is proportional to the input**, and 7x the input with a tree.
  Inputs larger than RAM are impossible.
- **The first result waits for EOF.**
- **Unbounded streams and values arriving over time cannot be handled.**

### Recommendation

Use a hybrid, chosen per input with `fstat`:

- **Regular file:** keep the whole-buffer design and `mmap` the file. It is
  the fastest and simplest option, and the inputs people pass as files fit
  in memory.
- **Pipe:** read incrementally for the no-query path and for streams, with
  per-element trees.
- **Random-access queries** (negative indices, reversal) buffer only the
  array they need.

Plain dotted paths such as `5000.nested.a.b` need no tree on either side. A
forward-only skip-scan that parses only along the path would cut that query
from 84 MB to about 21 MB. That improvement is algorithmic: it needs no
intrinsics, so do it before any of them.

## 3. Using SIMD intrinsics

**State at `v0.17`.** `scan_str` and `scan_ws` were scalar loops behind an
interface that intrinsics could fill. Their contract: read only `s[0..n)`
and return what the scalar loop would. `-DNOSIMD` forces the scalar code,
and `make scalar` and `make diff` keep both paths tested.

**History.** Earlier SSE2 versions, since removed, were worth 5–8% on the
author's machine and reportedly ~30% on another.

**Profile.** Sampling the reformat path shows its time spread over
`emit_value`, `emit_string`, `memmove` and `om`; byte scanning is only part
of it. Expect intrinsics to speed up scanning and leave the output side
unchanged.

### Techniques for both reading modes

- **Classify 16–64 bytes per step.** For each byte, `scan_str` needs to know
  whether it equals `"`, equals `\`, is below `0x20`, or is at least `0x80`.
  On SSE2 that is two `cmpeq`s, a `min_epu8` test for the control range and
  the sign bit, followed by one `movemask` and a count of trailing zeros.

  NEON has no `movemask`. The standard replacement is
  `vshrn_n_u16(vreinterpretq_u16_u8(m), 4)`, which packs a 128-bit mask
  into 64 bits (4 bits per byte); count trailing zeros and divide by 4. Use
  `vmaxvq_u8` when the only question is whether any byte matched. NEON is
  part of the arm64 baseline, so this path is always available on Apple
  Silicon.
- **Choose the vector width per machine.** SSE2 is the x86-64 baseline.
  AVX2 and AVX-512 need either `-mavx2`, which excludes older CPUs, or
  run-time dispatch with `__builtin_cpu_supports` and
  `__attribute__((target("avx2")))`. Both are compiler builtins, so they fit
  "libc only"; they are outside C99, so put them under `#ifdef` beside
  `NOSIMD`.
- **Validate UTF-8 with vectors.** Every non-ASCII byte leaves the fast path
  for `utf8_check`, so CJK-heavy text runs at scalar speed. The
  Keiser–Lemire lookup-table method validates whole blocks in a few
  instructions per byte. For non-Latin data this is likely the largest SIMD
  gain available.
- **`scan_ws` matters only for pretty-printed input.** Minified input has
  whitespace runs of length zero, where setting up a vector costs more than
  it saves. Indented input has runs of 2–40 bytes. Branch on run length,
  and measure both shapes before keeping the change.
- **Vectorise output escaping.** `print_str` searches for bytes that need
  escaping. That is the same classification as `scan_str`, without the
  non-ASCII test.
- **Pad the buffer to avoid tail handling.** Allocating the input buffer
  with 64 zeroed bytes of slack lets every vector load run at full width,
  with no scalar tail loop. This widens the scanner contract from `s[0..n)`
  to `s[0..n+64)`. ASan still checks the accesses, because the padding is
  part of the allocation. A chunked reader must pad its buffers the same
  way.

### Whole file: structural indexing

With the whole document in a padded buffer, the simdjson approach becomes
available:

1. **Stage 1.** For each 64-byte block, compute bitmasks of quotes,
   backslashes, structural characters and whitespace. Remove escaped quotes
   by finding odd-length backslash runs (an add-with-carry trick). Compute
   the inside-a-string mask as a prefix XOR of the quote bits, using one
   carry-less multiply (`PCLMULQDQ` on x86, `vmull_p64` on ARM). Write the
   position of every structural character to a `uint32_t` array.
2. **Stage 2.** Walk that array instead of the bytes.

On the test file, the index holds 3.2 million entries, about 12.8 MB (105%
of the input); the tree holds 84 MB. Stage 1 enables:

- **Faster reformatting.** Reformatting becomes copying spans between
  indices, with no branch per byte. The output is about as large as the
  input plus indentation, so output bandwidth bounds the gain. *Estimate:*
  modest, similar to the 5–30% measured before.
- **Skipping without parsing.** The matching bracket of any container is
  found by counting depth over the index, so `5000.nested.a.b` steps over
  5,000 elements without building them. This is section 2's skip-scan, made
  fast. Memory falls from 84 MB to about 25 MB (buffer plus index), and to
  about 12 MB with `mmap`. *Estimate:* the largest gain on the query path.
- **Parsing on demand.** Only the values a query touches are decoded.
  `[@.name]` decodes 60,000 names and never builds the other fields.

Costs:

- A second representation, which must stay consistent with the streaming
  reformatter and therefore byte-identical to `jq .`.
- Index memory about the size of the input.
- Carry logic in every 64-byte step, where bugs hide at block edges.
  CLAUDE.md records an off-by-one in a vector loop that passed every test
  and was caught only by ASan, with an input sized to end at the allocation
  edge. `test/corpus.py` exists to cross those boundaries; extend it before
  writing any stage 1.

### Streaming: carry the state across chunks

Stage 1 already processes one block at a time and carries three things from
each block to the next: the inside-a-string flag, the backslash parity and,
optionally, the depth. A chunk refill is a block boundary that also moves
the buffer. Incremental input is therefore compatible with SIMD. It adds:

- **Tokens cut by a refill.** Before refilling, move the unfinished token to
  the front of the buffer, or copy it into the arena. This is the same
  problem as the broken input pointers in section 2.
- **Padding per chunk**, as described above.
- **Smaller gains on small inputs.** With 64 KB chunks the per-chunk
  overhead is negligible. At 18 KB (a typical API response), process spawn
  is already 0.85 ms of a 1.05 ms run, so no intrinsic makes a visible
  difference.

### Suggested order

Measure after each step, and drop any step that does not pay for itself.

1. Pad the input buffer by 64 bytes and widen the scanner contract.
2. SSE2 and NEON `scan_str` (the old SSE2 code is in `git show v0.13`).
3. Vectorised UTF-8 validation inside `scan_str`'s non-ASCII branch.
4. Forward-only skip-scan for plain paths. It needs no intrinsics and gives
   the largest memory reduction for queries.
5. Stage-1 structural indexing, only if profiles still show scanning
   dominating after steps 1–4.

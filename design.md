# Design notes

> Status: `--rfc9535` now implements RFC 9535 in full (section 1's dialect
> flag, including `match`/`search`), and input is slurped or streamed by
> `fstat` with a forward-only `swalk()` for paths (section 2's hybrid). The
> numbers below are from before either.

Three open questions for `json`: whether to follow RFC 9535, how SIMD
intrinsics fit each way of reading input, and streaming versus reading the
whole document. Numbers are measured on this repo at `694d744` (arm64 macOS,
8 cores, Apple clang 17) unless marked as an estimate. The test file is a
12.1 MB array of 60,000 API-shaped objects.

## 1. Following RFC 9535 (JSONPath)

RFC 9535 standardises JSONPath: a query starts at `$`, and is a sequence of
segments: child (`.name`, `['name']`, `[0]`, `[1:3]`, `[*]`) and descendant
(`..`). Filters `[?…]` use `@` for the current node, comparisons, `&&`/`||`/`!`
and the functions `length`, `count`, `match`, `search` and `value`; `match`
and `search` use I-Regexp (RFC 9485). The result is always a *nodelist*.
Evaluation never fails: an invalid query is rejected up front, and everything
else, including a missing member, selects zero or more nodes.

### Where json differs today

| | RFC 9535 | json today |
|---|---|---|
| root | `$` required | implicit; `.a` and `a` both work |
| iteration | `[*]` | `[@]`, `[@.name]` |
| glob on keys | none (only `*`) | `na+`, with `fnmatch` semantics |
| slice result | nodelist: one result per element | one sub-array value |
| slice step 0 | selects nothing | error, as in Python |
| missing member | empty nodelist, success | error, exit 1 |
| descendants | `..` | none |
| filters | `[?@.x == 1]`, functions | not implemented |
| member names | `['a b']`, `["a"]` | `a b` as one argument |

### Pros

- **Known syntax and portable queries.** Anyone who has used JSONPath in a
  library, an API gateway or a database reads `$.users[*].name` without
  documentation, and the query works the same everywhere.
- **The next feature already has a specification.** `[?...]` predicates are
  next on the roadmap. RFC 9535 defines the filter grammar, comparison
  semantics across types, the five functions and their well-typedness rules.
  Inventing our own means designing all of that ourselves and defending every
  corner case.
- **A compliance suite exists.** The JSONPath Compliance Test Suite is a
  shared corpus of queries and expected nodelists, the same leverage
  JSONTestSuite gave the parser (283 cases we didn't have to write).
- **It removes the error-tolerance problem.** `n[@.name]` failing the whole
  query when one element lacks `name` is listed as a known weakness. RFC
  semantics make that an empty result per element by construction.
- **Normalized paths.** The RFC defines a canonical location string
  (`$['users'][0]['name']`), a ready-made output format for a future
  "print where each result came from" flag.

### Cons

- **It is hostile to the shell, and the shell is the whole interface.** `$`,
  `*`, `?`, `[` and quotes are all special. Measured in this repo: in zsh, the
  default macOS shell, every unquoted query containing `[` fails with
  `no matches found`, and `$['a']` is a syntax error. In sh, bash and zsh
  alike, a file named `users0` in the working directory silently turns
  `users[0]` into the query `users0`. RFC syntax means quoting every query,
  always.

  This applies to the current syntax too. Brackets already glob, so the
  CLAUDE.md example `json -r users[@.name]` fails unquoted in zsh. Choosing
  `+` over `*` only solved half the problem; see the recommendation.
- **Silent empty results.** A typo'd key under RFC rules exits 0 with no
  output, and `set -e` scripts carry on with nothing. Today's error-on-missing
  catches it. jq makes the same choice (`.foo` on a missing key is `null`, not
  silence).
- **Nodelists, not values.** `users[1:3]` would print two separate values
  instead of one array, so getting the sub-array back needs a collect flag.
  The Python-style sub-array is what both jq and Python users expect.
- **It only covers half the tool.** The RFC defines selection, not output:
  formatting, `-r`, byte-identical `jq .` output and multiple values on stdin
  stay ours whatever we pick.
- **Cost against "libc only".** Full conformance needs an I-Regexp engine for
  `match`/`search`. POSIX `regcomp` is not C99, and its ERE dialect is not
  I-Regexp anyway (Unicode character class escapes, for one). That is a regex
  engine in `json.c`, plausibly as large as everything else in the file.
- **Two dialects double the test surface.** The planned `--rfc9535` mode means
  every query feature behaves two ways, and the minimal suite (each case the
  only one catching some mutant) would need cases for both.

### Recommendation

Adopt RFC 9535 *semantics* where we have none yet, and keep the shell-safe
*surface*:

1. Take the filter grammar, comparison rules and `length`/`count`/`value`
   for `[?...]` as specified. Defer `match`/`search` until a regex engine is
   worth its size.
2. Parse both dialects into one internal selector list. The real semantic
   differences (sub-array or stream for slices, error or empty for a missing
   member, error or nothing for step 0) become evaluation flags that
   `--rfc9535` sets, so that mode can run the compliance suite.
3. Make every query expressible without brackets, so quoting becomes
   optional rather than mandatory: `users.-1`, `users.1:3`, `users.@.name`.
   `@`, `:` and `-` are not special in sh, bash or zsh. Brackets stay as an
   alias for anyone who quotes.

## 2. Streaming versus reading the whole document

These are two separate axes, and `json` sits at one corner of each today:

| | read incrementally | read to EOF (`slurp`) |
|---|---|---|
| single pass, no tree | — | **no filter** (`emit_value`) |
| tree for one element at a time | — | — |
| tree for the whole document | — | **any filter** (`parse_value`) |

So the "streaming" path streams its *processing*, not its *input*. Measured:
when input arrives over a pipe, the first output byte appears only once the
input has finished.

| on the 12.1 MB file | peak RSS | time |
|---|---|---|
| reformat (single pass) | 21 MB | 33 ms |
| `5000.nested.a.b` (whole tree) | 84 MB | 35 ms |
| `[@.name]` (whole tree) | 85 MB | 42 ms |

The single-pass path costs the input buffer plus a 64 KB output buffer. The
tree costs about 7× the input, even though `5000.nested.a.b` needs only one
of the 60,000 elements.

### Streaming: incremental input, bounded memory

Pros:
- **Memory bounded by the largest element, not the input.** Inputs larger than
  RAM become possible, and `jq --stream` (flat at 6.2 MB) stops being the only
  option.
- **Output before EOF.** Slow APIs, `tail -f` and NDJSON feeds show results as
  they arrive.
- **Required for pipelines anyway.** Multiple values on stdin (so that
  `json a[@.x] | json y` composes) means reading value after value from a
  stream that never has to end.
- **Per-element evaluation.** `[@...]` queries can tree one element at a
  time and restore the arena mark between elements. That machinery already
  exists in `stream_array`; only the input side is missing.

Cons:
- **No lookahead.** `[-1]` and `[-k:]` need a ring buffer of k elements;
  `[::-1]`, `length()` and anything sorting need the whole array. These
  queries must fall back to buffering, so "bounded memory" stops being a
  property of the tool and becomes a property of each query.
- **Partial output on invalid input.** A syntax error at byte N arrives after
  everything before it has been printed. The reformat path already behaves
  this way (it validates while printing); with incremental input it is the
  only option, because there is no second pass.
- **Pointers into the input break.** The streaming path echoes strings, and
  the tree stores number lexemes, as pointers into the input buffer. With a
  refilled buffer, output must be flushed before each refill, and lexemes
  copied into the arena. That is a non-negotiable (raw lexemes) being honoured
  by a more expensive mechanism.
- **A resumable lexer.** Tokens straddle refills: a string, a number or a
  `\u` escape can be cut anywhere. The lexer needs to carry state across
  refills, or the unfinished token has to be moved to the front of the buffer
  before each refill.

### Direct: the whole document in memory

Pros:
- **Simple.** One contiguous buffer, random access, every pointer stays valid
  for the life of the process, and the never-free design fits naturally.
- **Validate before printing, if wanted.** Two passes over a buffer in cache
  are cheap, and nothing is printed for invalid input.
- **Zero copy is possible.** When stdin is a regular file (`json < f`),
  `fstat` + `mmap` would replace the copy in `slurp()`. Pipes, as in
  `curl | json`, still need the copy.
- **Best case for SIMD.** A contiguous, paddable buffer can be scanned 64
  bytes at a time with no state carried across refills (section 3).

Cons:
- **Memory proportional to input, and 7× that with a tree.** Inputs larger
  than RAM are impossible.
- **Latency until EOF**, even for the first result.
- **Cannot handle unbounded streams** or multiple values arriving over time.

### Recommendation

A hybrid, chosen per input by `fstat`:
- **Regular file:** keep the whole-buffer design and `mmap` it. It is the
  fastest and simplest, and the inputs people pass as files fit in memory.
- **Pipe:** read incrementally for the no-filter path and for `[@...]`, with
  per-element trees.
- **Random-access queries** (negative indices, reversal) buffer only the
  array they need.

Plain dotted paths such as `5000.nested.a.b` need no tree at all on either
side. A forward-only skip-scan that parses only along the path would cut that
query from 84 MB to about 21 MB. That improvement is algorithmic: it needs no
intrinsics, and it is worth doing before any of them.

## 3. Leveraging intrinsics

**Today.** `scan_str` and `scan_ws` are scalar loops behind a seam that
intrinsics can fill. Their contract is to read only `s[0..n)` and return what
the scalar loop would; `-DNOSIMD` forces scalar, and `make scalar` and
`make diff` keep both paths tested.

**History.** The removed SSE2 versions were worth 5–8% on the author's machine
and reportedly ~30% on another.

**Profile.** Sampling the reformat path shows the time spread over
`emit_value`, `emit_string`, `memmove` and `om`, with the byte scanning only a
part of it. Expect intrinsics to speed up scanning, not the output side.

### Techniques that apply to both reading modes

- **Classify 16–64 bytes per step.** `scan_str` needs, per byte: equals `"`,
  equals `\`, below `0x20`, or at least `0x80`. On SSE2 that is two
  `cmpeq`s, a `min_epu8` test for the control range, and the sign bit, then
  one `movemask` and a count-trailing-zeros.

  NEON has no `movemask`. The standard replacement is
  `vshrn_n_u16(vreinterpretq_u16_u8(m), 4)`, which packs a 128-bit mask into
  64 bits (4 bits per byte); count trailing zeros and divide by 4. Use
  `vmaxvq_u8` when only "any match?" matters. NEON is baseline on arm64, so
  this path is always available on Apple Silicon.
- **Choose the width per machine.** SSE2 is the x86-64 baseline. AVX2 and
  AVX-512 need `-mavx2`, which excludes older CPUs, or runtime dispatch with
  `__builtin_cpu_supports` and `__attribute__((target("avx2")))`. Both are
  compiler builtins, so they fit "libc only", but they are not C99 and must
  be `#ifdef`'d beside `NOSIMD`.
- **Validate UTF-8 with vectors.** Today every non-ASCII byte leaves the fast
  path for `utf8_check`, so CJK-heavy text gets scalar speed. The Keiser–Lemire
  lookup-table method validates whole blocks in a few instructions per byte.
  For non-Latin data this is likely the largest SIMD win available.
- **`scan_ws` matters only for pretty-printed input.** Minified input has
  whitespace runs of zero, where a vector setup costs more than it saves.
  Indented input has runs of 2–40 bytes. Branch on run length, and measure
  both shapes before keeping it.
- **Output escaping.** `print_str` searches for bytes that need escaping: the
  same classification as `scan_str`, minus the non-ASCII test.
- **Pad instead of handling a tail.** Allocating the input buffer with 64
  zeroed bytes of slack lets every vector load run full width, with no scalar
  tail loop. This changes the scanner contract from `s[0..n)` to
  `s[0..n+64)`. ASan still checks it, because the padding is part of the
  allocation. A chunked reader must pad its buffers the same way.

### Whole file: structural indexing

With the whole document in a padded buffer, the simdjson approach becomes
available:

1. **Stage 1.** For each 64-byte block, compute bitmasks of quotes,
   backslashes, structural characters and whitespace. Remove escaped quotes by
   finding odd-length backslash runs (an add-with-carry trick). Compute the
   inside-a-string mask as a prefix XOR of the quote bits, using one
   carry-less multiply (`PCLMULQDQ` on x86, `vmull_p64` on ARM). Emit the
   position of every structural character into a `uint32_t` array.
2. **Stage 2.** Walk that array instead of the bytes.

On the test file, the index holds 3.2 million entries, about 12.8 MB (105% of
the input). The tree holds 84 MB. What stage 1 buys:

- **Reformat** becomes copying spans between indices, with no branch per
  byte. The output is about as large as the input plus indentation, so the
  gain is bounded by output bandwidth. *Estimate:* modest, like the 5–30%
  measured before.
- **Queries can skip without parsing.** The matching bracket of any container
  is found by counting depth over the index, so `5000.nested.a.b` steps over
  5,000 elements without building them. This is the skip-scan from section 2,
  made fast. Memory falls from 84 MB to about 25 MB (buffer plus index), and
  to about 12 MB with `mmap`. *Estimate:* the largest gain on the filter path.
- **Lazy, on-demand parsing.** Only the values a query touches get decoded.
  `[@.name]` decodes 60,000 names and never builds the other fields.

Costs: a second representation to keep consistent with the streaming
reformatter, which must stay byte-identical to `jq .`; index memory about the
size of the input; and carry logic in every 64-byte step, where bugs hide at
block edges. CLAUDE.md records an off-by-one in a vector loop that passed
every test and was caught only by ASan with an input sized to the allocation
edge. `test/corpus.py` exists to cross those boundaries, so extend it before
writing any stage 1.

### Streaming: carry the state across chunks

Stage 1 already processes one block at a time and carries three things from
each block to the next: the inside-string flag, the backslash parity and,
optionally, the depth. A chunk refill is just a block boundary that also
moves the buffer. So incremental input does not rule out SIMD; it adds:

- **Tokens cut by a refill.** Before refilling, move the unfinished token to
  the front of the buffer, or carry it in the arena. That is the same problem
  as the "pointers into the input" con in section 2.
- **Padding per chunk**, as above.
- **Smaller wins on small inputs.** With 64 KB chunks the per-chunk overhead
  is negligible, but at 18 KB (a typical API response) process spawn is
  already 0.85 ms of a 1.05 ms run, so no intrinsic is visible there.

### Suggested order

Measure after each step; drop any step that doesn't pay for itself.

1. Pad the input buffer by 64 bytes and widen the scanner contract.
2. SSE2 and NEON `scan_str` (the old SSE2 body is in `git show 04a1c92`).
3. Vectorised UTF-8 validation inside `scan_str`'s non-ASCII branch.
4. Forward-only skip-scan for plain paths. No intrinsics, but the biggest
   memory win for queries.
5. Stage-1 structural indexing, only if profiles still show scanning
   dominating after 1–4.

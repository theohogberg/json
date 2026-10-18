# json

A fast, zero-dependency JSON formatter and query tool for the shell, built for
jq's most common jobs: pretty-printing, picking out values, and JSONPath
queries. Provides a custom syntax created by the author (theo), with
Python-style indexes and slices for a smoother shell experience. One C99 file,
libc only, for macOS, Linux and Windows.

- **Fast:** 8-28x faster than jq on multi-megabyte input (table below).
- **Exact:** reformatted output is byte-identical to `jq .`, and numbers keep
  their original text, so `1.50` and `1e309` come back unchanged.
- **Small memory:** piped input streams through a 64 KiB window, so
  reformatting and per-element queries need memory for the largest element
  at any input size: about 1.3 MB on a 15 MB file.
- **Shell-friendly:** paths like `users.0.name` and `+` globs need no quoting.
- **Standard when you want it:** `--rfc9535` takes RFC 9535 JSONPath queries
  and passes compliance tests.

`make bench` on an Apple M1 Pro against jq 1.8.2: the median of 15 runs, input
from a file. Both tools printed byte-identical output for every command.
The inputs are *large*, 60,000 flat objects (19.8 MB); *nested*, 8,000
deeper, string-heavy objects (10.9 MB); and *small*, the first 60 objects
of *large* (19 KB).

| input | command | json | jq | speedup | peak RSS, json / jq |
|---|---|---|---|---|---|
| large | `json` | 39.4 ms | 967 ms | 24.5x | 20 / 201 MB |
| nested | `json` | 15.8 ms | 455 ms | 28.9x | 12 / 56 MB |
| small | `json` | 2.7 ms | 5.8 ms | 2.2x | 1 / 3 MB |
| large | `json 5000.nested.a.b` | 32.8 ms | 285 ms | 8.7x | 20 / 190 MB |
| large | `json '[@.name]'` | 34.0 ms | 324 ms | 9.5x | 20 / 190 MB |
| large | `json -r '[@.name]'` | 33.1 ms | 307 ms | 9.3x | 20 / 189 MB |
| large | `json '[?@.age > 80].id'` | 38.8 ms | 320 ms | 8.2x | 20 / 190 MB |
| large | `json '[100:110]'` | 43.5 ms | 284 ms | 6.5x | 101 / 190 MB |
| nested | `json 'items[@.user.login]'` | 13.3 ms | 111 ms | 8.4x | 12 / 55 MB |
| nested | `json items.7000.comments` | 13.2 ms | 107 ms | 8.1x | 12 / 55 MB |

`test/bench.py` generates the inputs and checks that the outputs match; run
`make bench` to reproduce.

## Usage

```
curl -s URL | json                       # reformat
curl -s URL | json users.0.name          # select one value
curl -s URL | json users.-1              # negative index, or 'users[-1]'
curl -s URL | json users.1:3             # slice (Python semantics), or 'users[1:3]'
curl -s URL | json -r 'users[@.name]'    # one result per element, unquoted
curl -s URL | json -r users.@.name       # the same, without brackets
curl -s URL | json 'users[?@.age > 30]'  # filter
curl -s URL | json ..id                  # search every depth
curl -s URL | json '["10:10"]'           # a key that looks like a slice, -1 or @
curl -s URL | json --rfc9535 '$.users[?@.age > 30].name'
```

Flags:

```
-r, --raw-output   print a string result without quotes or escapes
    --raw          alias for --raw-output
    --rfc9535      the path is RFC 9535 JSONPath: '$.items[*].name';
                   prints each selected node, and nothing for none
-h, --help         show help, with a summary of the path syntax
--                 end of options
```

Short options combine (`-rh`). `-r` applies only when the result itself is a
string; strings nested inside it stay quoted, so the output is still JSON.

An absent key or an out-of-range index is an error (exit 1); a filter or
search that matches nothing returns empty. See [DESIGN.md](DESIGN.md) for
the reasoning behind the query syntax.

## Build

```
make                # builds out/json
make install        # into /usr/local/bin (PREFIX=... to change)
```

Any C99 compiler works: clang or gcc on macOS and Linux, MSVC on Windows (from
a Visual Studio developer prompt, under Git Bash, or as
`cl /O2 /std:c11 src\json.c /Fe:json.exe`).

## Caveats

- jq has other features compared to this tool: it is a full language, with
  arithmetic, construction and compact output. The benchmark covers the more
  used / core components of both, i.e. parsing and filtering.
- On the *small* benchmark input (19 KB), starting a process during benchmarks
  takes about 2.5 ms, so both times are mostly startup.
- From a file, `json` reads the input whole, hence the benchmark's 20 MB;
  when piped, it uses about 1.3 MB. A slice builds the whole array in memory.

## Tests

```
make ci             # everything below, ~2 min
make test           # regression suite
make conformance    # JSONTestSuite
make jsonpath       # JSONPath Compliance Test Suite
make check          # the suites under ASan and UBSan
make fuzz           # differential test against Python's json
```

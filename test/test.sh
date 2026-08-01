#!/bin/sh
# Regression tests. Set JSON to the binary under test and DEPTH to the
# recursion cap it was built with.
#
#   JSON=out/json test/test.sh
#
# exit 0 = all passed

JSON=${JSON:-./json}
pass=0
fail=0

# check NAME INPUT ARG EXPECTED_RC EXPECTED_STDOUT
check() {
	name=$1 input=$2 arg=$3 want_rc=$4 want_out=$5
	if [ -n "$arg" ]; then
		got_out=$(printf '%s' "$input" | $JSON "$arg" 2>/dev/null)
	else
		got_out=$(printf '%s' "$input" | $JSON 2>/dev/null)
	fi
	got_rc=$?
	if [ "$got_rc" != "$want_rc" ]; then
		printf 'FAIL %s: rc %s, want %s\n' "$name" "$got_rc" "$want_rc"
		fail=$((fail + 1))
		return
	fi
	if [ "$got_rc" = 0 ] && [ "$got_out" != "$want_out" ]; then
		printf 'FAIL %s: output\n--- got ---\n%s\n--- want ---\n%s\n' \
		    "$name" "$got_out" "$want_out"
		fail=$((fail + 1))
		return
	fi
	pass=$((pass + 1))
}

# checkf NAME INPUT WANT_RC WANT_OUT [ARGS...]  -- like check, but any
# number of command-line arguments, so flags can be tested.
checkf() {
	name=$1 input=$2 want_rc=$3 want_out=$4
	shift 4
	got_out=$(printf '%s' "$input" | $JSON "$@" 2>/dev/null)
	got_rc=$?
	if [ "$got_rc" != "$want_rc" ]; then
		printf 'FAIL %s: rc %s, want %s\n' "$name" "$got_rc" "$want_rc"
		fail=$((fail + 1))
		return
	fi
	if [ "$got_rc" = 0 ] && [ "$got_out" != "$want_out" ]; then
		printf 'FAIL %s: output\n--- got ---\n%s\n--- want ---\n%s\n' \
		    "$name" "$got_out" "$want_out"
		fail=$((fail + 1))
		return
	fi
	pass=$((pass + 1))
}


# --- scalars at top level (the JSON spec allows these) ---
check num-int      '42'        '' 0 '42'
check num-neg      '-1'        '' 0 '-1'
check num-exp      '1e10'      '' 0 '1e10'
check num-frac     '1.50'      '' 0 '1.50'
check num-zero     '0'         '' 0 '0'
check lit-true     'true'      '' 0 'true'
check lit-false    'false'     '' 0 'false'
check lit-null     'null'      '' 0 'null'
check str-plain    '"hi"'      '' 0 '"hi"'
check str-empty    '""'        '' 0 '""'

# --- number fidelity: must not round-trip through double ---
check num-bigint   '100000000000000000001' '' 0 '100000000000000000001'
check num-tiny     '1e-400'                '' 0 '1e-400'
check num-huge     '1e309'                 '' 0 '1e309'
check num-trailing '1.000'                 '' 0 '1.000'

# --- containers ---
check obj-empty    '{}'        '' 0 '{}'
check arr-empty    '[]'        '' 0 '[]'
check arr-nested   '[[]]'      '' 0 '[
  []
]'
check obj-one      '{"a":1}'   '' 0 '{
  "a": 1
}'
check arr-two      '[1,2]'     '' 0 '[
  1,
  2
]'

# --- string escapes ---
check esc-quote    '"a\"b"'         '' 0 '"a\"b"'
check esc-backsl   '"a\\b"'         '' 0 '"a\\b"'
check esc-newline  '"a\nb"'         '' 0 '"a\nb"'
check esc-tab      '"a\tb"'         '' 0 '"a\tb"'
check esc-slash    '"a\/b"'         '' 0 '"a/b"'
check esc-ctrl     '"\u0001"'       '' 0 '"\u0001"'
check esc-nul      '"\u0000"'       '' 0 '"\u0000"'
check esc-latin    '"\u00e5"'       '' 0 '"å"'
check esc-cjk      '"\u4e2d"'       '' 0 '"中"'
check esc-surro    '"\ud83d\ude00"' '' 0 '"😀"'
check esc-lone-hi  '"\ud83d"'       '' 0 '"�"'
check esc-lone-lo  '"\udc00"'       '' 0 '"�"'
check utf8-raw     '"åäö"'          '' 0 '"åäö"'

# --- path selection ---
check path-key     '{"a":1}'                  a       0 '1'
check path-dotted  '{"a":{"b":2}}'            a.b     0 '2'
check path-leaddot '{"a":{"b":2}}'            .a.b    0 '2'
check path-index   '[10,20,30]'               1       0 '20'
check path-mixed   '{"a":[{"b":9}]}'          a.0.b   0 '9'
check path-obj-out '{"a":{"b":2}}'            a       0 '{
  "b": 2
}'
check path-key-num '{"0":"x"}'                0       0 '"x"'
check path-empty   '{"":1}'                   ''      0 '{
  "": 1
}'

# --- path errors ---
check err-nokey    '{"a":1}'      b        1 ''
check err-oob      '[1,2]'        5        1 ''
check err-scalar   '{"a":1}'      a.b      1 ''
check err-strindex '[1,2]'        x        1 ''
check err-dotdot   '{"a":1}'      a..b     1 ''

# --- malformed input must be rejected ---
check bad-empty      ''                 '' 1 ''
check bad-ws         '   '              '' 1 ''
check bad-trail      '{"a":1} x'        '' 1 ''
check bad-two-vals   '{"a":1}{"b":2}'   '' 1 ''
check bad-comma      '{"a":1,}'         '' 1 ''
check bad-arr-comma  '[1,]'             '' 1 ''
check bad-unterm-obj '{"a":1'           '' 1 ''
check bad-unterm-arr '[1,2'             '' 1 ''
check bad-unterm-str '"abc'             '' 1 ''
check bad-nokey      '{a:1}'            '' 1 ''
check bad-nocolon    '{"a" 1}'          '' 1 ''
check bad-singleq    "{'a':1}"          '' 1 ''
check bad-leadzero   '01'               '' 1 ''
check bad-plus       '+1'               '' 1 ''
check bad-dot-lead   '.5'               '' 1 ''
check bad-dot-trail  '5.'               '' 1 ''
check bad-exp-empty  '1e'               '' 1 ''
check bad-bare-minus '-'                '' 1 ''
check bad-nan        'NaN'              '' 1 ''
check bad-inf        'Infinity'         '' 1 ''
check bad-tru        'tru'              '' 1 ''
check bad-comment    '{"a":1 /*x*/}'    '' 1 ''
check bad-rawctrl    "$(printf '"a\tb"')" '' 1 ''
check bad-badesc     '"\q"'             '' 1 ''
check bad-shortu     '"\u12"'           '' 1 ''
check bad-bom        "$(printf '\357\273\277{}')" '' 1 ''

# --- invalid UTF-8 must be rejected, output must always be valid JSON ---
check bad-utf8-lone   "$(printf '"a\377b"')"      '' 1 ''
check bad-utf8-trunc  "$(printf '"\303"')"        '' 1 ''
check bad-utf8-over   "$(printf '"\300\257"')"   '' 1 ''
check bad-utf8-surro  "$(printf '"\355\240\200"')" '' 1 ''
check bad-utf8-big    "$(printf '"\365\200\200\200"')" '' 1 ''
check bad-utf8-cont   "$(printf '"\200"')"        '' 1 ''
check ok-utf8-2byte   "$(printf '"\303\245"')"          '' 0 "$(printf '"\303\245"')"
check ok-utf8-3byte   "$(printf '"\344\270\255"')"     '' 0 "$(printf '"\344\270\255"')"
check ok-utf8-4byte   "$(printf '"\360\237\230\200"')" '' 0 "$(printf '"\360\237\230\200"')"

# --- depth limit, no stack smash ---
# Derived from the configured cap rather than hard-coded, so the test
# keeps checking the real boundary when the cap changes.
DEPTH=${DEPTH:-1024}
deep=$(python3 -c "print('['*($DEPTH+1) + ']'*($DEPTH+1))")
check deep-reject "$deep" '' 1 ''
ok=$(python3 -c "print('['*$DEPTH + ']'*$DEPTH)")
check deep-accept "$ok" '' 0 "$(printf '%s' "$ok" | $JSON)"

# --- slices: Python semantics (clamped, negative indices, steps) ---
A='{"a":[10,20,30,40,50],"s":"hi\tthere","n":42,"o":{"x":["deep"]}}'
check slice-basic   "$A" 'a[1:3]'   0 "$(printf '[\n  20,\n  30\n]')"
check slice-clamp   "$A" 'a[0:99]'  0 "$(printf '[\n  10,\n  20,\n  30,\n  40,\n  50\n]')"
check slice-neg     "$A" 'a[-2:]'   0 "$(printf '[\n  40,\n  50\n]')"
check slice-open    "$A" 'a[:2]'    0 "$(printf '[\n  10,\n  20\n]')"
check slice-step    "$A" 'a[::2]'   0 "$(printf '[\n  10,\n  30,\n  50\n]')"
check slice-rev     "$A" 'a[::-1]'  0 "$(printf '[\n  50,\n  40,\n  30,\n  20,\n  10\n]')"
check slice-empty   "$A" 'a[3:1]'   0 '[]'
check slice-all     "$A" 'a[:]'     0 "$(printf '[\n  10,\n  20,\n  30,\n  40,\n  50\n]')"
check slice-zerostep "$A" 'a[1:2:0]' 1 ''
check slice-on-str  "$A" 's[0:1]'   1 ''

# --- bracket indexing, including negative ---
check br-index      "$A" 'a[0]'     0 '10'
check br-negative   "$A" 'a[-1]'    0 '50'
check br-oob        "$A" 'a[9]'     1 ''
check br-neg-oob    "$A" 'a[-99]'   1 ''
check br-empty      "$A" 'a[]'      1 ''
check br-malformed  "$A" 'a[1:'     1 ''
check br-mixed      "$A" 'a[1:3].0' 0 '20'
check br-after-key  "$A" 'o.x[0]'   0 '"deep"'

# --- -r / --raw-output ---
checkf raw-short    "$A" 0 "$(printf 'hi\tthere')" -r s
checkf raw-long     "$A" 0 "$(printf 'hi\tthere')" --raw-output s
checkf raw-alias    "$A" 0 "$(printf 'hi\tthere')" --raw s
checkf raw-quoted   "$A" 0 '"hi\tthere"'           s
checkf raw-number   "$A" 0 '42'                    -r n
checkf raw-nested   "$A" 0 'deep'                  -r o.x[0]
checkf raw-toplevel '"bare"' 0 'bare'              -r
checkf raw-noop     '{"k":1}' 0 "$(printf '{\n  "k": 1\n}')" -r

# --- option handling ---
checkf opt-rfc-stub "$A" 1 '' --rfc9535
checkf opt-unknown  "$A" 1 '' --nope
checkf opt-unknown2 "$A" 1 '' -x
checkf opt-two-paths "$A" 1 '' a n


# --- globbing with '+' (object keys only) ---
G='{"array":[{"name":"ada","nation":"se","id":1}],"strs":["a","b"],"num":42,"deep":{"a":{"x":1},"ab":{"x":2},"b":{"x":3}}}'
check glob-prefix    "$G" 'array[0].na+' 0 "$(printf '{\n  "name": "ada",\n  "nation": "se"\n}')"
check glob-suffix    "$G" 'deep.+b'      0 "$(printf '{\n  "ab": {\n    "x": 2\n  },\n  "b": {\n    "x": 3\n  }\n}')"
check glob-all       "$G" 'deep.+'       0 "$(printf '{\n  "a": {\n    "x": 1\n  },\n  "ab": {\n    "x": 2\n  },\n  "b": {\n    "x": 3\n  }\n}')"
check glob-middle    "$G" 'array[0].n+n' 0 "$(printf '{\n  "nation": "se"\n}')"
check glob-nomatch   "$G" 'deep.zzz+'    0 '{}'
check glob-exact-still "$G" 'deep.a'     0 "$(printf '{\n  "x": 1\n}')"
check glob-missing-key "$G" 'deep.zzz'   1 ''

# globbing is only defined on objects
check glob-on-array  "$G" 'array.n+'     1 ''
check glob-on-strarr "$G" 'strs.+'       1 ''
check glob-on-scalar "$G" 'num.+'        1 ''
check glob-on-slice  "$G" 'array[0:1].n+' 1 ''

# a glob result is an ordinary object and can be walked further
check glob-then-key  "$G" 'deep.a+'      0 "$(printf '{\n  "a": {\n    "x": 1\n  },\n  "ab": {\n    "x": 2\n  }\n}')"

# raw output applies to a matched string like any other
checkf glob-raw      "$G" 0 "$(printf '{\n  "name": "ada",\n  "nation": "se"\n}')" -r 'array[0].na+'


printf '\n%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]

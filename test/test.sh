#!/bin/sh
# usage: JSON=binary DEPTH=cap [JOBS=n] test/test.sh
# Minimal: each case is the only one catching some mutant (CLAUDE.md, Testing).

JSON=${JSON:-out/json}
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}
out=$(mktemp -d) || exit 1
trap 'rm -rf "$out"' EXIT
n=0

# t NAME INPUT WANT_RC WANT_STDOUT [ARGS...]; stdout is checked only on rc 0.
# Verdicts go to numbered files so the report stays in source order.
t() {
	n=$((n + 1))
	run "$@" > "$out/$(printf '%04d' "$n")" &
	[ $((n % JOBS)) -ne 0 ] || wait
}

run() {
	name=$1 input=$2 want_rc=$3 want_out=$4
	shift 4
	got_out=$(printf '%s' "$input" | $JSON "$@" 2>/dev/null)
	got_rc=$?
	if [ "$got_rc" != "$want_rc" ]; then
		printf 'FAIL %s: rc %s, want %s\n' "$name" "$got_rc" "$want_rc"
	elif [ "$got_rc" = 0 ] && [ "$got_out" != "$want_out" ]; then
		printf 'FAIL %s: output\n--- got ---\n%s\n--- want ---\n%s\n' \
		    "$name" "$got_out" "$want_out"
	fi
}

t num-neg '-1' 0 '-1'
t num-frac '1.50' 0 '1.50'
t num-zero '0' 0 '0'
t lit-true 'true' 0 'true'
t lit-false 'false' 0 'false'
t lit-null 'null' 0 'null'

t num-tiny '1e-400' 0 '1e-400'

t obj-one '{"a":1}' 0 '{
  "a": 1
}'
t arr-two '[1,2]' 0 '[
  1,
  2
]'

t esc-quote '"a\"b"' 0 '"a\"b"'
t esc-backsl '"a\\b"' 0 '"a\\b"'
t esc-newline '"a\nb"' 0 '"a\nb"'
t esc-slash '"a\/b"' 0 '"a/b"'
t esc-ctrl '"\u0001"' 0 '"\u0001"'
t esc-latin '"\u00e5"' 0 '"å"'
t esc-cjk '"\u4e2d"' 0 '"中"'
t esc-surro '"\ud83d\ude00"' 0 '"😀"'
t esc-lone-hi '"\ud83d"' 0 '"�"'
t esc-lone-lo '"\udc00"' 0 '"�"'

t path-index '[10,20,30]' 0 '20' 1

t err-nokey '{"a":1}' 1 '' b
t err-oob '[1,2]' 1 '' 5
t err-scalar '{"a":1}' 1 '' a.b
t err-strindex '[1,2]' 1 '' x
t err-dotdot '{"a":1}' 1 '' a..b

t bad-empty '' 1 ''
t bad-unterm-obj '{"a":1' 1 ''
t bad-unterm-str '"abc' 1 ''
t bad-nokey '{a:1}' 1 ''
t bad-nocolon '{"a" 1}' 1 ''
t bad-leadzero '01' 1 ''
t bad-dot-trail '5.' 1 ''
t bad-exp-empty '1e' 1 ''
t bad-bare-minus '-' 1 ''
t bad-tru 'tru' 1 ''
t bad-rawctrl "$(printf '"a\tb"')" 1 ''
t bad-badesc '"\q"' 1 ''
t bad-shortu '"\u12"' 1 ''
t bad-bom "$(printf '\357\273\277{}')" 1 ''

t bad-utf8-trunc "$(printf '"\303"')" 1 ''
t bad-utf8-surro "$(printf '"\355\240\200"')" 1 ''
t bad-utf8-big "$(printf '"\365\200\200\200"')" 1 ''
t bad-utf8-cont "$(printf '"\200"')" 1 ''
t ok-utf8-2byte "$(printf '"\303\245"')" 0 "$(printf '"\303\245"')"
t ok-utf8-3byte "$(printf '"\344\270\255"')" 0 "$(printf '"\344\270\255"')"
t ok-utf8-4byte "$(printf '"\360\237\230\200"')" 0 "$(printf '"\360\237\230\200"')"

# derived from the cap, so these keep testing the real boundary
DEPTH=${DEPTH:-1024}
deep=$(python3 -c "print('['*($DEPTH+1) + ']'*($DEPTH+1))")
t deep-reject "$deep" 1 ''
ok=$(python3 -c "print('['*$DEPTH + ']'*$DEPTH)")
t deep-accept "$ok" 0 "$(python3 -c "d=$DEPTH; print('\n'.join(['  '*i+'[' for i in range(d-1)]+['  '*(d-1)+'[]']+['  '*i+']' for i in range(d-2,-1,-1)]))")"

A='{"a":[10,20,30,40,50],"s":"hi\tthere","n":42,"o":{"x":["deep"]}}'
t slice-clamp "$A" 0 "$(printf '[\n  10,\n  20,\n  30,\n  40,\n  50\n]')" 'a[0:99]'
t slice-neg "$A" 0 "$(printf '[\n  40,\n  50\n]')" 'a[-2:]'
t slice-open "$A" 0 "$(printf '[\n  10,\n  20\n]')" 'a[:2]'
t slice-rev "$A" 0 "$(printf '[\n  50,\n  40,\n  30,\n  20,\n  10\n]')" 'a[::-1]'
t slice-bigstep "$A" 0 "$(printf '[\n  20\n]')" 'a[1::9223372036854775807]'
t slice-zerostep "$A" 1 '' 'a[1:2:0]'
t slice-on-str "$A" 1 '' 's[0:1]'

t br-index "$A" 0 '10' 'a[0]'
t br-negative "$A" 0 '50' 'a[-1]'
t br-oob "$A" 1 '' 'a[9]'
t br-empty "$A" 1 '' 'a[]'
t br-malformed "$A" 1 '' 'a[1:'

t raw-long "$A" 0 "$(printf 'hi\tthere')" --raw-output s
t raw-alias "$A" 0 "$(printf 'hi\tthere')" --raw s
t raw-quoted "$A" 0 '"hi\tthere"' s
t raw-toplevel '"bare"' 0 'bare' -r

t opt-rfc-stub "$A" 1 '' --rfc9535
t opt-unknown-long "$A" 1 '' --nope
t opt-unknown-short "$A" 1 '' -x
t opt-two-paths "$A" 1 '' a n

G='{"array":[{"name":"ada","nation":"se","id":1}],"strs":["a","b"],"num":42,"deep":{"a":{"x":1},"ab":{"x":2},"b":{"x":3}}}'

t glob-on-array "$G" 1 '' 'array.n+'

t glob-prefix-multi "$G" 0 "$(printf '{\n  "a": {\n    "x": 1\n  },\n  "ab": {\n    "x": 2\n  }\n}')" 'deep.a+'

S='{"a":[{"name":"ada","nationalitie":"se","movie":1},{"name":"bob","nationalitie":"no","movie":2}],"n":[[1,2],[3,4]],"num":5,"o":{"k":1}}'
t st-glob "$S" 0 "$(printf '{\n  "nationalitie": "se",\n  "movie": 1\n}\n{\n  "nationalitie": "no",\n  "movie": 2\n}')" 'a[@.+ie]'
t st-nested "$S" 0 "$(printf '1\n2\n3\n4')" 'n[@][@]'
t st-empty "$S" 0 '' 'a[0:0][@]'

t st-on-scalar "$S" 1 '' 'num[@]'
t st-unclosed "$S" 1 '' 'a[@'
t st-traildot "$S" 1 '' 'a[@].'

wait
pass=0 fail=0
for f in "$out"/*; do
	if [ -s "$f" ]; then cat "$f"; fail=$((fail + 1)); else pass=$((pass + 1)); fi
done
printf '\n%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]

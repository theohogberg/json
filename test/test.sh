#!/bin/sh
# usage: JSON=binary DEPTH=cap [JOBS=n] test/test.sh
# Minimal: each case is the only one catching some mutant (CLAUDE.md, Testing).
# Each case runs twice, input piped (streamed) and redirected (slurped).

JSON=${JSON:-out/json}
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}
out=$(mktemp -d) && in=$(mktemp -d) || exit 1
trap 'rm -rf "$out" "$in"' EXIT
n=0

# t NAME INPUT WANT_RC WANT_STDOUT [ARGS...]; stdout is checked only on rc 0.
# Verdicts go to numbered files so the report stays in source order.
t() {
	n=$((n + 1))
	run "$@" > "$out/$(printf '%04d' "$n")" &
	[ $((n % JOBS)) -ne 0 ] || wait
}

run() {
	name=$1 input=$2 want_rc=$3 want_out=$4 f=$in/$n
	shift 4
	printf '%s' "$input" > "$f"
	for how in pipe file; do
		if [ $how = pipe ]; then
			got_out=$(printf '%s' "$input" | $JSON "$@" 2>/dev/null)
		else
			got_out=$($JSON "$@" < "$f" 2>/dev/null)
		fi
		got_rc=$?
		if [ "$got_rc" != "$want_rc" ]; then
			printf 'FAIL %s (%s): rc %s, want %s\n' "$name" $how "$got_rc" "$want_rc"
		elif [ "$got_rc" = 0 ] && [ "$got_out" != "$want_out" ]; then
			printf 'FAIL %s (%s): output\n--- got ---\n%s\n--- want ---\n%s\n' \
			    "$name" $how "$got_out" "$want_out"
		fi
	done
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
t err-dotdot '{"a":1}' 1 '' a..

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
t deep-accept "$ok" 0 "$(python3 -c "d=$DEPTH; print('\n'.join(['  '*i+'[' for i in range(d-1)]+['  '*(d-1)+'[]']+['  '*i+']' for i in range(d-2,-1,-1)]))" | tr -d '\r')"

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

# [?] filters: RFC 9535's, minus match() and search()
F='{"a":[{"name":"ada","age":36,"tags":["x"]},{"name":"bob","age":25},{"name":"c","age":40}],"min":30,"n":[["]","x"],["y"]]}'
t flt-cmp "$F" 0 "$(printf 'ada\nc')" -r 'a[?@.age > 30].name'
t flt-root "$F" 0 '"ada"' 'a[?@.age > $.min && @.tags].name'
t flt-quoted "$F" 0 '"]"' 'n[@[?@ == "]"]]'
t flt-missing "$F" 0 '' 'a[?@.nope == 1]'
t flt-noregex "$F" 1 '' 'a[?match(@.name, "a")]'
t flt-scalar "$F" 1 '' 'min[?@]'
t flt-bad "$F" 1 '' 'a[?@.age >]'
t flt-unclosed "$F" 1 '' 'a[?@.age > 1'

# .. search, quoted names, unions, and [@]/[?] over object values
D='{"id":1,"a":{"id":2,"x.y":"dot","it'"'"'s":"q","users":[{"id":3,"name":"ada","age":36},{"name":"bob","age":25}]},"m":{"u1":{"name":"cy","age":40},"u2":{"name":"di","age":20}},"arr":[10,20,30],"k]":"br"}'
t desc-key "$D" 0 "$(printf '1\n2\n3')" '..id'
t desc-under "$D" 0 "$(printf 'ada\nbob')" -r 'a..name'
t desc-none "$D" 0 '' '..zzz'
t desc-filter "$D" 0 "$(printf 'ada\ncy')" -r '..[?@.age > 30].name'
t desc-index "$D" 0 "$(printf '{\n  "name": "bob",\n  "age": 25\n}\n30')" '..[-1]'
t qname "$D" 0 '"dot"' "a['x.y']"
t qname-escape "$D" 0 '"q"' "a['it\\'s']"
t qname-bracket "$D" 0 '"br"' "['k]']"
t qname-missing "$D" 1 '' "a['nope']"
t union-dedupe "$D" 0 "$(printf '[\n  10,\n  20,\n  30\n]')" 'arr[0:3,1]'
t union-order "$D" 0 "$(printf '[\n  30,\n  10\n]')" 'arr[-1,0]'
t union-obj "$D" 0 "$(printf '{\n  "u2": {\n    "name": "di",\n    "age": 20\n  },\n  "u1": {\n    "name": "cy",\n    "age": 40\n  }\n}')" "m['u2','u1']"
t union-oob "$D" 1 '' 'arr[0,9]'
t union-mixed "$D" 1 '' "arr['x',0]"
t obj-stream "$D" 0 "$(printf 'cy\ndi')" -r 'm[@.name]'
t obj-filter "$D" 0 'cy' -r 'm[?@.age > 30].name'

# a bare @ component is a stream: a.@.b is a[@.b]
t at-stream "$D" 0 "$(printf 'cy\ndi')" -r 'm.@.name'
t at-root '[{"v":1},{"v":2}]' 0 "$(printf '1\n2')" '@.v'
t at-nested "$S" 0 "$(printf '1\n2\n3\n4')" 'n.@.@'
t at-inner "$S" 0 "$(printf '1\n2\n3\n4')" 'n[@.@]'
t at-desc '[1,[2]]' 0 "$(printf '1\n[\n  2\n]\n2')" '..@'
t at-key '{"@x":1}' 0 '1' '@x'
t at-quoted '{"a.@":1}' 0 '1' "['a.@']"

# a bare -N is an index and a bare a:b:c a slice: a.-1 is a[-1]
t bare-neg "$A" 0 '50' 'a.-1'
t bare-slice "$A" 0 "$(printf '[\n  20,\n  30\n]')" 'a.1:3'
t bare-step "$A" 0 "$(printf '[\n  50,\n  30,\n  10\n]')" 'a.::-2'
t bare-root '[1,2,3]' 0 '3' -- -1
t bare-on-obj '{"-1":1}' 1 '' -- -1
t bare-quoted '{"-1":1}' 0 '1' "['-1']"
t bare-desc '{"x":{"y":[1,2,3]}}' 0 '3' '..y.-1'
t bare-in-stream '[[1,2,3],[4,5,6]]' 0 "$(printf '[\n  1,\n  2\n]\n[\n  4,\n  5\n]')" '@.0:2'
t bare-colons "$A" 1 '' 'a.1:2:3:4'

# RFC 9535 itself is held to the compliance suite (make jsonpath); these are
# what it does not check
R='{"a":[{"n":"x","v":1},{"n":"y","v":2.50},{"v":3}],"s":"hi","big":1e309}'
t rfc-path "$R" 0 '"x"' --rfc9535 '$.a[0].n'
t rfc-filter "$R" 0 "$(printf '2.50\n3')" --rfc9535 '$.a[?@.v > 1].v'
t rfc-exact "$R" 0 '1e309' --rfc9535 '$[?@ < 2e309]'
t rfc-equal "$R" 0 '"y"' --rfc9535 '$.a[?@.v == 2.5].n'
t rfc-none "$R" 0 '' --rfc9535 '$.nope'
t rfc-raw "$R" 0 'hi' -r --rfc9535 '$.s'
t rfc-match "$R" 0 '"y"' --rfc9535 '$.a[?match(@.n, "[^x]")].n'
t rfc-norootq "$R" 1 '' --rfc9535 'a'

# an empty stream prints no bytes at all; $(...) in run() would strip the
# newline this checks for
n=$((n + 1))
got=$(printf '[]' | $JSON '[@]' | wc -c | tr -d ' ')
{ [ "$got" = 0 ] || printf 'FAIL st-empty-bytes: %s bytes, want 0\n' "$got"; } > "$out/$(printf '%04d' "$n")"

# a reader that stops early is not worth a message: SIGPIPE on POSIX, on
# Windows a write that fails with ERROR_NO_DATA
n=$((n + 1))
err=$( { awk 'BEGIN { printf "["; for (i = 0; i < 200000; i++) printf "1,"; print "1]" }' |
    $JSON | head -c 1 >/dev/null; } 2>&1)
{ [ -z "$err" ] || printf 'FAIL pipe-closed: stderr %s\n' "$err"; } > "$out/$(printf '%04d' "$n")"

wait
pass=0 fail=0
for f in "$out"/*; do
	if [ -s "$f" ]; then cat "$f"; fail=$((fail + 1)); else pass=$((pass + 1)); fi
done
printf '\n%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]

#!/bin/sh
# usage: JSON=binary [JOBS=n] test/conformance.sh
# JSONTestSuite: y_* must be accepted, n_* rejected, i_* may go either way.

JSON=${JSON:-out/json}
DIR=${DIR:-JSONTestSuite/test_parsing}
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}
yfail=0 nfail=0 pass=0 idef=0

# "RC NAME" per file, sorted so the report is independent of scheduling. Each
# file is read slurped and streamed; RC is X if the two disagree.
results=$(printf '%s\0' "$DIR"/*.json |
    xargs -0 -P "$JOBS" -I{} sh -c '"$0" < "$1" > /dev/null 2>&1; a=$?
	cat "$1" | "$0" > /dev/null 2>&1; [ $? = $a ] || a=X
	echo "$a ${1##*/}"' "$JSON" {} |
    sort -k2)

while read -r rc base; do
	if [ "$rc" = X ]; then
		printf 'FAIL (pipe and file disagree): %s\n' "$base"
		yfail=$((yfail+1)); continue
	fi
	case "$base" in
	y_*)
		if [ "$rc" -eq 0 ]; then pass=$((pass+1))
		else printf 'FAIL (should accept): %s\n' "$base"; yfail=$((yfail+1)); fi ;;
	n_*)
		if [ "$rc" -ne 0 ]; then pass=$((pass+1))
		else printf 'FAIL (should reject): %s\n' "$base"; nfail=$((nfail+1)); fi ;;
	i_*)
		idef=$((idef+1)) ;;
	esac
done <<EOF
$results
EOF

printf '\n%d passed, %d wrongly rejected, %d wrongly accepted, %d implementation-defined\n' \
    "$pass" "$yfail" "$nfail" "$idef"
[ $((yfail + nfail)) -eq 0 ]

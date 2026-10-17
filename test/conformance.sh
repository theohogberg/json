#!/bin/sh
# Run the JSON parsing conformance corpus against the binary in JSON.
#
#   y_*  must be accepted
#   n_*  must be rejected
#   i_*  implementation-defined, either is acceptable
#
# Only y_ and n_ failures are bugs. Files run JOBS at a time (default: one
# per core); a sanitizer build spends nearly all its time starting up.

JSON=${JSON:-out/json}
DIR=${DIR:-JSONTestSuite/test_parsing}
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}
yfail=0 nfail=0 pass=0 idef=0

# "RC NAME" per file, sorted so the report does not depend on scheduling
results=$(printf '%s\0' "$DIR"/*.json |
    xargs -0 -P "$JOBS" -I{} sh -c '"$0" < "$1" > /dev/null 2>&1; echo "$? ${1##*/}"' "$JSON" {} |
    sort -k2)

while read -r rc base; do
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

#!/bin/sh
# Run the JSON parsing conformance corpus against the binary in JSON.
#
#   y_*  must be accepted
#   n_*  must be rejected
#   i_*  implementation-defined, either is acceptable
#
# Only y_ and n_ failures are bugs.

JSON=${JSON:-out/json}
DIR=${DIR:-JSONTestSuite/test_parsing}
yfail=0 nfail=0 pass=0 idef=0

for f in "$DIR"/*.json; do
	base=$(basename "$f")
	"$JSON" < "$f" > /dev/null 2>&1
	rc=$?
	case "$base" in
	y_*)
		if [ $rc -eq 0 ]; then pass=$((pass+1))
		else printf 'FAIL (should accept): %s\n' "$base"; yfail=$((yfail+1)); fi ;;
	n_*)
		if [ $rc -ne 0 ]; then pass=$((pass+1))
		else printf 'FAIL (should reject): %s\n' "$base"; nfail=$((nfail+1)); fi ;;
	i_*)
		idef=$((idef+1)) ;;
	esac
done

printf '\n%d passed, %d wrongly rejected, %d wrongly accepted, %d implementation-defined\n' \
    "$pass" "$yfail" "$nfail" "$idef"
[ $((yfail + nfail)) -eq 0 ]

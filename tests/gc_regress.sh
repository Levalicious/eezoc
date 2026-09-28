#!/bin/bash
#
# GC regression (2026-09-11).
#
# eqNat 200 200 builds Church numerals large enough that evaluation
# allocates past a 16 MiB semispace: with the STG forwarding-pointer bug
# this printed garbage, and the native backends trapped on their
# unimplemented GC path. Expected answer: and(isZero 0)(isZero 0) = 00.
#
# The -H 65536 runs start from a 64 KiB semispace, so the native
# collector's grow-and-recollect path runs many times. Since the
# Kiselyov/let-sharing translation (2026-09-13) the program term itself is
# ~108 KB of App cells, larger than that initial space, so these two runs
# also cover: the JIT sizing its heaps to the term before building it
# (it used to write first and bounds-check after), and growth past 16x
# the initial size (the ceiling used to be tied to -H; the live set here
# exceeds 1 MiB).
#

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
WS="${EEZO_WS:-$(cd "${SCRIPT_DIR}/../.." && pwd)}"   # the workspace: this repository and its siblings (libeezo, eezo, eezoc, eezott, stdlib)
EEZOC="${EEZOC:-$WS/eezoc/eezoc}"
EEZO="${EEZO:-$WS/eezo/eezo}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
status=0

check() {  # name expected got
    if [ "$2" = "$3" ]; then echo "PASS: $1"; else echo "FAIL: $1"; echo "  Expected: $2"; echo "  Got:      $3"; status=1; fi
}

printf '#import fib\na := 200;\nb := twohundred;\nand(isZero(sub(a)(b)))(isZero(sub(b)(a)))' > "$TMP/n200.eezo"
bcl=$("$EEZOC" < "$TMP/n200.eezo")

check "gc: stg"                    "00" "$(echo "$bcl" | timeout 300 "$EEZO")"
check "gc: jit"                    "00" "$(echo "$bcl" | timeout 300 "$EEZO" -n)"
check "gc: jit, 64KiB heap (grow)" "00" "$(echo "$bcl" | timeout 300 "$EEZO" -n -H 65536)"

"$EEZOC" -e < "$TMP/n200.eezo" > "$TMP/n200.elf" && chmod +x "$TMP/n200.elf"
check "gc: elf"                    "00" "$(timeout 300 "$TMP/n200.elf")"
"$EEZOC" -e -H 65536 < "$TMP/n200.eezo" > "$TMP/n200_small.elf" && chmod +x "$TMP/n200_small.elf"
check "gc: elf, 64KiB heap (grow)" "00" "$(timeout 300 "$TMP/n200_small.elf")"

exit $status

#!/bin/bash
#
# STG garbage-collector regression (2026-09-11).
# Church-numeral subtraction at n=200 allocates enough to trigger a
# collection mid-normalization; before the root-set fix this printed
# "GC error: tried to enter forwarding pointer" and a bogus result.
# STG mode only: the JIT/ELF paths are correct but slow on this program.
#

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
EEZOC="${SCRIPT_DIR}/../eezoc/eezoc"
EEZO="${SCRIPT_DIR}/../eezo/eezo"

src='#import fib\na := 200;\nb := twohundred;\nand(isZero(sub(a)(b)))(isZero(sub(b)(a)))'
expected="00"   # true

got=$(echo -e "$src" | "$EEZOC" | "$EEZO" 2>&1)
if [ "$got" = "$expected" ]; then
    echo "PASS: gc regress (eqNat 200 under STG) = $got"
    exit 0
else
    echo "FAIL: gc regress (eqNat 200 under STG)"
    echo "  Expected: $expected"
    echo "  Got:      $got"
    exit 1
fi

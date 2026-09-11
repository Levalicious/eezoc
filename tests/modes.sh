#!/bin/bash
#
# Normalization modes, laziness and sharing regression (2026-09-11).
#
# 1. -N nf (default) and -N whnf must differ on a term whose weak head
#    normal form still hides a redex inside a partial application, and all
#    four evaluators (simple, STG, JIT, ELF) must agree in both modes.
#      (a -> b -> a)(not(true))   WHNF: K (not true)   NF: K (K I)
# 2. Evaluation is normal order everywhere: K true omega terminates.
# 3. STG update frames (sharing): pow(2)(4) takes exactly 50 S/K steps
#    with thunk updates; without them it took 81.
#

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
EEZOC="${SCRIPT_DIR}/../eezoc/eezoc"
EEZO="${SCRIPT_DIR}/../eezo/eezo"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
status=0

check() {  # name expected got
    if [ "$2" = "$3" ]; then echo "PASS: $1"; else echo "FAIL: $1"; echo "  Expected: $2"; echo "  Got:      $3"; status=1; fi
}

printf '#import bool\n(a -> b -> a)(not(true))' > "$TMP/knt.eezo"
probe=$("$EEZOC" < "$TMP/knt.eezo")
nf_expected="10010011010000"
whnf_expected="10011101110111010000100100110100001000000"

check "nf (simple)"    "$nf_expected"   "$(echo "$probe" | "$EEZO" -s -N nf)"
check "nf (stg)"       "$nf_expected"   "$(echo "$probe" | "$EEZO" -N nf)"
check "nf (jit)"       "$nf_expected"   "$(echo "$probe" | "$EEZO" -n -N nf)"
check "nf default"     "$nf_expected"   "$(echo "$probe" | "$EEZO")"
check "whnf (simple)"  "$whnf_expected" "$(echo "$probe" | "$EEZO" -s -N whnf)"
check "whnf (stg)"     "$whnf_expected" "$(echo "$probe" | "$EEZO" -N whnf)"
check "whnf (jit)"     "$whnf_expected" "$(echo "$probe" | "$EEZO" -n -N whnf)"
"$EEZOC" -e < "$TMP/knt.eezo" > "$TMP/nf.elf" && chmod +x "$TMP/nf.elf"
"$EEZOC" -e -N whnf < "$TMP/knt.eezo" > "$TMP/whnf.elf" && chmod +x "$TMP/whnf.elf"
check "nf (elf)"       "$nf_expected"   "$("$TMP/nf.elf")"
check "whnf (elf)"     "$whnf_expected" "$("$TMP/whnf.elf")"

printf '#import bool\n(a -> b -> a)(true)((x -> x(x))(x -> x(x)))' > "$TMP/lazy.eezo"
lazy=$("$EEZOC" < "$TMP/lazy.eezo")
check "lazy: K true omega (simple)" "00" "$(echo "$lazy" | timeout 10 "$EEZO" -s)"
check "lazy: K true omega (stg)"    "00" "$(echo "$lazy" | timeout 10 "$EEZO")"
check "lazy: K true omega (jit)"    "00" "$(echo "$lazy" | timeout 10 "$EEZO" -n)"
"$EEZOC" -e < "$TMP/lazy.eezo" > "$TMP/lazy.elf" && chmod +x "$TMP/lazy.elf"
check "lazy: K true omega (elf)"    "00" "$(timeout 10 "$TMP/lazy.elf")"

pow=$(echo -e '#import nat\npow(2)(4)' | "$EEZOC")
steps=$(echo "$pow" | "$EEZO" -v 2>&1 | grep -o 'Reduced ([0-9]* steps)' | grep -o '[0-9]*')
check "sharing: pow(2)(4) steps under STG" "50" "$steps"

exit $status

#!/bin/bash
#
# Normalization modes, laziness and sharing regression (2026-09-11).
#
# 1. -N nf (default) and -N whnf must differ on a term whose weak head
#    normal form still hides a redex inside a partial application, and all
#    four evaluators (simple, STG, JIT, ELF) must agree in nf mode.
#      (a -> b -> a)(not(true))   WHNF: K (not true)   NF: K (K I)
#    A weak head normal form is not canonical once definitions are shared
#    (eezoc binds each definition once, 2026-09-13): the read-back shows the
#    shared thunks, and the three graph-reduction evaluators (STG, JIT, ELF)
#    agree with each other while the substituting simple interpreter reads
#    back differently. So in whnf mode: STG, JIT and ELF agree, every
#    evaluator's WHNF differs from the NF, and every WHNF normalizes to it.
# 2. Evaluation is normal order everywhere: K true omega terminates.
# 3. STG update frames (sharing): pow(2)(4) takes 94 S/K steps with thunk
#    updates under Kiselyov's translation (B, C, T, R as S K trees; it took
#    50 under the Schoenfinkel translation), and fewer than the simple
#    interpreter, which shares nothing (128).
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

printf '#import bool\n(a -> b -> a)(not(true))' > "$TMP/knt.eezo"
probe=$("$EEZOC" < "$TMP/knt.eezo")
nf_expected="10010011010000"

check "nf (simple)"    "$nf_expected"   "$(echo "$probe" | "$EEZO" -s -N nf)"
check "nf (stg)"       "$nf_expected"   "$(echo "$probe" | "$EEZO" -N nf)"
check "nf (jit)"       "$nf_expected"   "$(echo "$probe" | "$EEZO" -n -N nf)"
check "nf default"     "$nf_expected"   "$(echo "$probe" | "$EEZO")"
whnf_stg=$(echo "$probe" | "$EEZO" -N whnf)
whnf_simple=$(echo "$probe" | "$EEZO" -s -N whnf)
check "whnf (jit) = whnf (stg)"   "$whnf_stg" "$(echo "$probe" | "$EEZO" -n -N whnf)"
check "whnf (stg) differs from nf"     "differ" "$([ "$whnf_stg" != "$nf_expected" ] && echo differ)"
check "whnf (simple) differs from nf"  "differ" "$([ "$whnf_simple" != "$nf_expected" ] && echo differ)"
check "whnf (stg) normalizes to nf"    "$nf_expected" "$(echo "$whnf_stg" | "$EEZO" -N nf)"
check "whnf (simple) normalizes to nf" "$nf_expected" "$(echo "$whnf_simple" | "$EEZO" -N nf)"
"$EEZOC" -e < "$TMP/knt.eezo" > "$TMP/nf.elf" && chmod +x "$TMP/nf.elf"
"$EEZOC" -e -N whnf < "$TMP/knt.eezo" > "$TMP/whnf.elf" && chmod +x "$TMP/whnf.elf"
check "nf (elf)"       "$nf_expected"   "$("$TMP/nf.elf")"
check "whnf (elf) = whnf (stg)" "$whnf_stg" "$("$TMP/whnf.elf")"

printf '#import bool\n(a -> b -> a)(true)((x -> x(x))(x -> x(x)))' > "$TMP/lazy.eezo"
lazy=$("$EEZOC" < "$TMP/lazy.eezo")
check "lazy: K true omega (simple)" "00" "$(echo "$lazy" | timeout 10 "$EEZO" -s)"
check "lazy: K true omega (stg)"    "00" "$(echo "$lazy" | timeout 10 "$EEZO")"
check "lazy: K true omega (jit)"    "00" "$(echo "$lazy" | timeout 10 "$EEZO" -n)"
"$EEZOC" -e < "$TMP/lazy.eezo" > "$TMP/lazy.elf" && chmod +x "$TMP/lazy.elf"
check "lazy: K true omega (elf)"    "00" "$(timeout 10 "$TMP/lazy.elf")"

pow=$(echo -e '#import nat\npow(2)(4)' | "$EEZOC")
steps=$(echo "$pow" | "$EEZO" -v 2>&1 | grep -o 'Reduced ([0-9]* steps)' | grep -o '[0-9]*')
steps_simple=$(echo "$pow" | "$EEZO" -s -v 2>&1 | grep -o 'Reduced ([0-9]* steps)' | grep -o '[0-9]*')
check "sharing: pow(2)(4) steps under STG" "94" "$steps"
check "sharing: STG (update frames) takes fewer steps than simple" "fewer" "$([ "$steps" -lt "$steps_simple" ] && echo fewer)"

exit $status

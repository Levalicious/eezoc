#!/bin/bash
#
# XBCL and the extended leaves (2026-09-13): B C T R, machine words and
# their primitives. The programs are spelled directly in XBCL bits (the
# surface syntax for words is a later stage); the NF and the WHNF are
# compared with the XBCL spelling of the expected term, on every evaluator
# that takes XBCL.
#
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
EEZO="${SCRIPT_DIR}/../eezo/eezo"
status=0
check() { if [ "$2" = "$3" ]; then echo "PASS: $1"; else echo "FAIL: $1"; echo "  Expected: $2"; echo "  Got:      $3"; status=1; fi; }

K=00; S=010
leaf() { local c=$1 o="" i; for ((i=4;i>=0;i--)); do o+=$(( (c >> i) & 1 )); done; echo -n "011$o"; }
I=$(leaf 0); B=$(leaf 1); C=$(leaf 2); T=$(leaf 3); R=$(leaf 4)
word() { local n=$1 o="" i; for ((i=63;i>=0;i--)); do o+=$(( (n >> i) & 1 )); done; echo -n "$(leaf 5)$o"; }
prim() { leaf $((6 + $1)); }
ADD=$(prim 0); SUB=$(prim 1); MUL=$(prim 2); AND=$(prim 3); OR=$(prim 4); XOR=$(prim 5); SHL=$(prim 6); SHR=$(prim 7)
EQ=$(prim 8); LT=$(prim 9); ADDC=$(prim 10); SUBB=$(prim 11); MULL=$(prim 12); DIVMOD=$(prim 13)
ap() { echo -n "1$1$2"; }
ap3() { ap "$(ap "$1" "$2")" "$3"; }
ap4() { ap "$(ap3 "$1" "$2" "$3")" "$4"; }
pair() { ap3 "$C" "$(ap "$T" "$1")" "$2"; }      # \p. p x y = C (T x) y
TRUE=$K; FALSE=$(ap "$K" "$I")

backends="-s stg -n"
for be in $backends; do
  flag=${be/stg/}
  run() { echo "$2" | timeout 60 "$EEZO" $flag -f xbcl $1; }
  for mode in "" "-N whnf"; do
    m="$be${mode:+ }$mode"
    check "$m: B I I K = K"          "$K"                                    "$(run "$mode" "$(ap4 "$B" "$I" "$I" "$K")")"
    check "$m: C K I S = S"          "$S"                                    "$(run "$mode" "$(ap4 "$C" "$K" "$I" "$S")")"
    check "$m: T K I = K"            "$K"                                    "$(run "$mode" "$(ap3 "$T" "$K" "$I")")"
    check "$m: R K I S = S K"        "$(ap "$S" "$K")"                       "$(run "$mode" "$(ap4 "$R" "$K" "$I" "$S")")"
    check "$m: add 2 3"              "$(word 5)"                             "$(run "$mode" "$(ap3 "$ADD" "$(word 2)" "$(word 3)")")"
    check "$m: add wraps"            "$(word 0)"                             "$(run "$mode" "$(ap3 "$ADD" "$(word -1)" "$(word 1)")")"
    check "$m: sub wraps"            "$(word -1)"                            "$(run "$mode" "$(ap3 "$SUB" "$(word 0)" "$(word 1)")")"
    check "$m: mul wraps"            "$(word 1)"                             "$(run "$mode" "$(ap3 "$MUL" "$(word -1)" "$(word -1)")")"
    check "$m: and 12 10"            "$(word 8)"                             "$(run "$mode" "$(ap3 "$AND" "$(word 12)" "$(word 10)")")"
    check "$m: or 12 10"             "$(word 14)"                            "$(run "$mode" "$(ap3 "$OR" "$(word 12)" "$(word 10)")")"
    check "$m: xor 12 10"            "$(word 6)"                             "$(run "$mode" "$(ap3 "$XOR" "$(word 12)" "$(word 10)")")"
    check "$m: shl 1 63"             "$(word $((1 << 63)))"                  "$(run "$mode" "$(ap3 "$SHL" "$(word 1)" "$(word 63)")")"
    check "$m: shl 1 64 = 0"         "$(word 0)"                             "$(run "$mode" "$(ap3 "$SHL" "$(word 1)" "$(word 64)")")"
    check "$m: shr 2^63 63"          "$(word 1)"                             "$(run "$mode" "$(ap3 "$SHR" "$(word $((1 << 63)))" "$(word 63)")")"
    check "$m: eq 7 7"               "$TRUE"                                 "$(run "$mode" "$(ap3 "$EQ" "$(word 7)" "$(word 7)")")"
    check "$m: eq 7 8"               "$FALSE"                                "$(run "$mode" "$(ap3 "$EQ" "$(word 7)" "$(word 8)")")"
    check "$m: lt 7 8"               "$TRUE"                                 "$(run "$mode" "$(ap3 "$LT" "$(word 7)" "$(word 8)")")"
    check "$m: lt 8 7"               "$FALSE"                                "$(run "$mode" "$(ap3 "$LT" "$(word 8)" "$(word 7)")")"
    check "$m: addc carry"           "$(pair "$(word 0)" "$(word 1)")"       "$(run "$mode" "$(ap3 "$ADDC" "$(word -1)" "$(word 1)")")"
    check "$m: addc no carry"        "$(pair "$(word 5)" "$(word 0)")"       "$(run "$mode" "$(ap3 "$ADDC" "$(word 2)" "$(word 3)")")"
    check "$m: subb borrow"          "$(pair "$(word -1)" "$(word 1)")"      "$(run "$mode" "$(ap3 "$SUBB" "$(word 0)" "$(word 1)")")"
    check "$m: mull 2^32 2^32"       "$(pair "$(word 0)" "$(word 1)")"       "$(run "$mode" "$(ap3 "$MULL" "$(word $((1 << 32)))" "$(word $((1 << 32)))")")"
    check "$m: divmod 7 2"           "$(pair "$(word 3)" "$(word 1)")"       "$(run "$mode" "$(ap3 "$DIVMOD" "$(word 7)" "$(word 2)")")"
    check "$m: divmod 7 0 = (0, 7)"  "$(pair "$(word 0)" "$(word 7)")"       "$(run "$mode" "$(ap3 "$DIVMOD" "$(word 7)" "$(word 0)")")"
    check "$m: strict arguments"     "$(word 5)"                             "$(run "$mode" "$(ap3 "$ADD" "$(ap "$I" "$(word 2)")" "$(ap3 "$K" "$(word 3)" "$S")")")"
    check "$m: fst (addc 2 3) = 5"   "$(word 5)"                             "$(run "$mode" "$(ap "$(ap3 "$ADDC" "$(word 2)" "$(word 3)")" "$K")")"
  done
  nf="$(pair "$(word 3)" "$(ap "$K" "$I")")"
  check "$be: a normal form reads back as itself" "$nf" "$(run "" "$nf")"
done
exit $status

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
EEZOC="${SCRIPT_DIR}/../eezoc/eezoc"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
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

# ---- the surface language: literals 5w and the primitives wadd ... wdivmod, through eezoc ----
compile() { printf '%s' "$1" > "$TMP/p.eezo"; "$EEZOC" -f xbcl < "$TMP/p.eezo"; }
LIMBS='#import pair
add2(a0)(a1)(b0)(b1) := waddc(a0)(b0)(s -> c -> pair(s)(wadd(wadd(a1)(b1))(c)));
add2(18446744073709551615w)(0w)(1w)(0w)'
LIMBS_EXPECT='#import pair
pair(0w)(1w)'
for be in $backends; do
  flag=${be/stg/}
  run_src() { compile "$1" | timeout 60 "$EEZO" $flag -f xbcl; }
  check "$be surface: wadd(2w)(3w)"          "$(word 5)"                          "$(run_src 'wadd(2w)(3w)')"
  check "$be surface: wadd wraps"            "$(word 0)"                          "$(run_src 'wadd(18446744073709551615w)(1w)')"
  check "$be surface: wmull 2^32 2^32"       "$(pair "$(word 0)" "$(word 1)")"    "$(run_src 'wmull(4294967296w)(4294967296w)')"
  check "$be surface: wdivmod 7 0"           "$(pair "$(word 0)" "$(word 7)")"    "$(run_src 'wdivmod(7w)(0w)')"
  check "$be surface: weq 7 7"               "$K"                                 "$(run_src 'weq(7w)(7w)')"
  check "$be surface: wlt 8 7"               "$(ap "$K" "$I")"                    "$(run_src 'wlt(8w)(7w)')"
  check "$be surface: two-limb add carries"  "$(run_src "$LIMBS_EXPECT")"         "$(run_src "$LIMBS")"
done
elf_run() { printf '%s' "$1" > "$TMP/p.eezo"; "$EEZOC" -e -f xbcl < "$TMP/p.eezo" > "$TMP/p.elf" && chmod +x "$TMP/p.elf" && timeout 60 "$TMP/p.elf"; }
check "elf surface: wadd(2w)(3w)"            "$(word 5)"                          "$(elf_run 'wadd(2w)(3w)')"
check "elf surface: wmull 2^32 2^32"         "$(pair "$(word 0)" "$(word 1)")"    "$(elf_run 'wmull(4294967296w)(4294967296w)')"
check "elf surface: wdivmod 7 0"             "$(pair "$(word 0)" "$(word 7)")"    "$(elf_run 'wdivmod(7w)(0w)')"
check "elf surface: two-limb add carries"    "$(elf_run "$LIMBS_EXPECT")"         "$(elf_run "$LIMBS")"
# ---- the limb list (2026-09-16): a natural as the C list of limbs, running itself ----
# The limb primitives are arity 2 on limb lists, and a machine word is a one-limb list. A limb list
# is an extended leaf of its own: leaf code 22 past the 14 word primitives (so 6 + 22 = 28), then a
# 32-bit limb count, then the limbs least significant first. They run on the simple interpreter,
# whose cells carry the C list (bn.h) directly: every operation is one pass of C over the limbs,
# not a fold unfolding.
BADD=$(prim 14); BSUB=$(prim 15); BMUL=$(prim 16); BDIVMOD=$(prim 17); BLT=$(prim 18); BEQ=$(prim 19)
BPOW=$(prim 20); BMINV=$(prim 21); BIGLEAF=$(leaf 28)
bits() { local n=$1 v=$2 o="" i; for ((i=n-1;i>=0;i--)); do o+=$(( (v >> i) & 1 )); done; echo -n "$o"; }
big() { local o; o=$BIGLEAF$(bits 32 $#); for v in "$@"; do o+=$(bits 64 "$v"); done; echo -n "$o"; }
N64=$(big 0 1); M64=$(big -1); N128=$(big 0 0 1); M128=$(big -1 -1)
# The limb list is the C list of limbs (bn.h) itself on the simple interpreter and on the STG machine, which
# keeps the same list in its heap and hands its limbs to the same functions; the native JIT takes them next.
for be in -s stg -n; do
  flag=${be/stg/}
  run_s() { echo "$2" | timeout 60 "$EEZO" $flag -f xbcl $1; }
check "$be: badd 2 3"                    "$(big 5)"                          "$(run_s "" "$(ap3 "$BADD" "$(word 2)" "$(word 3)")")"
check "$be: badd 2^64-1 1 (the list grows)" "$N64"                           "$(run_s "" "$(ap3 "$BADD" "$(word -1)" "$(word 1)")")"
check "$be: bsub 2^64 1"                 "$M64"                              "$(run_s "" "$(ap3 "$BSUB" "$N64" "$(word 1)")")"
check "$be: bsub 3 5 = 0 (monus)"        "$(big)"                            "$(run_s "" "$(ap3 "$BSUB" "$(word 3)" "$(word 5)")")"
check "$be: bmul 2^32 2^32"              "$N64"                              "$(run_s "" "$(ap3 "$BMUL" "$(word $((1 << 32)))" "$(word $((1 << 32)))")")"
check "$be: bsub 2^128 1 (the borrow runs)" "$M128"                          "$(run_s "" "$(ap3 "$BSUB" "$N128" "$(word 1)")")"
check "$be: bdivmod 2^128-1 2^64"        "$(pair "$M64" "$M64")"             "$(run_s "" "$(ap3 "$BDIVMOD" "$M128" "$N64")")"
check "$be: bdivmod 7 0 = (0, 7)"        "$(pair "$(big)" "$(big 7)")"       "$(run_s "" "$(ap3 "$BDIVMOD" "$(word 7)" "$(word 0)")")"
check "$be: blt 2^64 2^64-1"             "$FALSE"                            "$(run_s "" "$(ap3 "$BLT" "$N64" "$M64")")"
check "$be: blt 2^64-1 2^64"             "$TRUE"                             "$(run_s "" "$(ap3 "$BLT" "$M64" "$N64")")"
check "$be: beq 2^64 2^64"               "$TRUE"                             "$(run_s "" "$(ap3 "$BEQ" "$N64" "$N64")")"
check "$be: beq (badd 2 3) 5w"           "$TRUE"                             "$(run_s "" "$(ap3 "$BEQ" "$(ap3 "$BADD" "$(word 2)" "$(word 3)")" "$(word 5)")")"
check "$be: whnf of a limb list"         "$(big 5)"                          "$(run_s "-N whnf" "$(ap3 "$BADD" "$(word 2)" "$(word 3)")")"
check "$be: a limb list reads back as itself" "$M128"                       "$(run_s "" "$M128")"
surface() { compile "$1" | timeout 60 "$EEZO" $flag -f xbcl; }
check "$be surface: badd(2w)(3w)"        "$(big 5)"                          "$(surface 'badd(2w)(3w)')"
check "$be surface: bsub(2^64)(1w)"      "$M64"                              "$(surface 'bsub(badd(18446744073709551615w)(1w))(1w)')"
check "$be surface: bmul(2^32)(2^32)"    "$N64"                              "$(surface 'bmul(4294967296w)(4294967296w)')"
check "$be surface: bsub(2^128)(1w)"     "$M128"                             "$(surface 'bsub(bmul(bmul(4294967296w)(4294967296w))(bmul(4294967296w)(4294967296w)))(1w)')"
check "$be surface: blt(2^64-1)(2^64)"   "$TRUE"                             "$(surface 'blt(18446744073709551615w)(badd(18446744073709551615w)(1w))')"
# a limb-list literal (digits then b) and the two primitives the recursion equations need beyond
# addition and multiplication: exponentiation, and the modular inverse x ^ (y - 2) mod y
check "$be surface: the literal 2^64+1 minus 1" "$N64"                        "$(surface 'bsub(18446744073709551617b)(1w)')"
check "$be surface: bpow(2w)(64w)"       "$N64"                              "$(surface 'bpow(2w)(64w)')"
check "$be surface: bpow(2w)(256w)"      "$(big 0 0 0 0 1)"                  "$(surface 'bpow(2w)(256w)')"
check "$be surface: bpow(3w)(0w)"        "$(big 1)"                          "$(surface 'bpow(3w)(0w)')"
check "$be surface: bminv(3w)(7w) = 5"   "$(big 5)"                          "$(surface 'bminv(3w)(7w)')"
check "$be surface: 3 * 3^-1 = 1 mod 7"  "$(pair "$(big 2)" "$(big 1)")"     "$(surface 'bdivmod(bmul(bminv(3w)(7w))(3w))(7w)')"
# the modular power, at a modulus whose full power has no limbs to be held in: 3 ^ (2^127 - 3) mod 2^127-1
# is the inverse of 3 (it is prime), and three times the inverse is 2 * (2^127 - 1) + 1, so the division's
# pair is exactly (2, 1)
check "$be surface: 3 * bminv(3w)(2^127-1) = 2P + 1" "$(pair "$(big 2)" "$(big 1)")" \
      "$(surface 'bdivmod(bmul(bminv(3w)(bsub(bpow(2w)(127w))(1w)))(3w))(bsub(bpow(2w)(127w))(1w))')"
# the limb list is the C list's answer, not a model's: the word layer's modelled two-limb add of
# (2^64-1, 0) + (1, 0) is the pair (low 0, high 1) - and the C list, dividing its own sum by 2^64,
# gives that high limb as the quotient and the low limb as the remainder
check "$be surface: bdivmod(2^64)(2^64)" "$(pair "$(big 1)" "$(big)")"       "$(surface 'bdivmod(badd(18446744073709551615w)(1w))(bmul(4294967296w)(4294967296w))')"
done
# what none of the three evaluators has a meaning for: a word primitive on a limb list
for be in -s stg -n; do
  flag=${be/stg/}
  echo "$(ap3 "$ADD" "$(ap3 "$BADD" "$(word 2)" "$(word 3)")" "$(word 1)")" | timeout 60 "$EEZO" $flag -f xbcl >/dev/null 2>&1
  check "$be refuses a word primitive on a limb list (rc 1)" "1" "$?"
done 
# a limb list has no pure spelling either
printf 'badd(2w)(3w)' > "$TMP/l.eezo"
"$EEZOC" -f bcl < "$TMP/l.eezo" >/dev/null 2>/dev/null; check "bcl refuses a limb list (rc 1)" "1" "$?"
"$EEZOC" -f jot < "$TMP/l.eezo" >/dev/null 2>/dev/null; check "jot refuses a limb list (rc 1)" "1" "$?"
"$EEZOC" -e -f bcl < "$TMP/l.eezo" >/dev/null 2>/dev/null; check "elf -f bcl refuses a limb list (rc 1)" "1" "$?"
printf '18446744073709551617b' > "$TMP/lb.eezo"
"$EEZOC" -f bcl < "$TMP/lb.eezo" >/dev/null 2>/dev/null; check "bcl refuses a limb-list literal (rc 1)" "1" "$?"

# the pure formats cannot carry words
printf 'wadd(2w)(3w)' > "$TMP/w.eezo"
"$EEZOC" -f bcl < "$TMP/w.eezo" >/dev/null 2>/dev/null; check "bcl refuses words (rc 1)" "1" "$?"
"$EEZOC" -f jot < "$TMP/w.eezo" >/dev/null 2>/dev/null; check "jot refuses words (rc 1)" "1" "$?"
"$EEZOC" -e -f bcl < "$TMP/w.eezo" >/dev/null 2>/dev/null; check "elf -f bcl refuses words (rc 1)" "1" "$?"
exit $status

#!/bin/bash
#
# Stream I/O (Lazy-K / WHNF model) regression (2026-09-11).
#
# A program is a function from the input stream to the output stream
# (stdlib io.eezo). The driver forces the output one WHNF at a time and
# reads each Church numeral by marker unfolding - never a normal form.
# Every evaluator must agree: simple (-s), STG, JIT (-n), ELF (-e -i).
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

# run NAME SOURCE INPUT EXPECTED_OUT EXPECTED_RC [FILTER]
run() {
    local name="$1" src="$2" input="$3" filter="${6:-cat}"
    local want; want=$(printf '%b' "$4"; echo "|rc=$5")
    printf '%b' "$src" > "$TMP/$name.eezo"
    "$EEZOC" < "$TMP/$name.eezo" > "$TMP/$name.bcl" || { check "$name (compile)" "ok" "compile error"; return; }
    "$EEZOC" -e -i < "$TMP/$name.eezo" > "$TMP/$name.elf" && chmod +x "$TMP/$name.elf"
    local out rc
    for mode in "simple:$EEZO -s -i $TMP/$name.bcl" "stg:$EEZO -i $TMP/$name.bcl" "jit:$EEZO -n -i $TMP/$name.bcl" "elf:$TMP/$name.elf"; do
        local label="${mode%%:*}" cmd="${mode#*:}"
        out=$(printf '%b' "$input" | timeout 60 $cmd 2>"$TMP/err" | $filter; echo "|rc=${PIPESTATUS[1]}")
        check "$name ($label)" "$want" "$out"
    done
}

run cat   '#import io\ncat'                                                'hello'  'hello' 0
run hi    '#import io\ns -> cons(72)(cons(105)(cons(10)(stop)))'           ''       'Hi\n'  0
run exit3 '#import io\ns -> exit(3)'                                       'x'      ''      3
run inc   '#import io\nsmap(x -> succ(x))'                                 'hello'  'ifmmp' 1
run ones  '#import io\ns -> fix(r -> cons(49)(r))'                         ''       '11111' 141 'head -c 5'

exit $status

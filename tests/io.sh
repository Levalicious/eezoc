#!/bin/bash
#
# Monadic I/O regression (2026-09-28; stdlib io.eezo, alonzo's NewIO).
#
# A program is run(m) for m a value of the IO monad: a 4-tuple the driver reads with selectors, performing the
# action it names (putc, getc, exit) and continuing with the continuation applied to the result. Every evaluator
# must agree: simple (-s -m), STG (-m), JIT (-n -m), ELF (-e -m). The streams of the Lazy-K model are tests/stream.sh.
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

# run NAME SOURCE INPUT EXPECTED_OUT EXPECTED_RC
run() {
    local name="$1" src="$2" input="$3"
    local want; want=$(printf '%b' "$4"; echo "|rc=$5")
    printf '%b' "$src" > "$TMP/$name.eezo"
    "$EEZOC" < "$TMP/$name.eezo" > "$TMP/$name.bcl" || { check "$name (compile)" "ok" "compile error"; return; }
    "$EEZOC" -e -m < "$TMP/$name.eezo" > "$TMP/$name.elf" && chmod +x "$TMP/$name.elf"
    local out
    for mode in "simple:$EEZO -s -m $TMP/$name.bcl" "stg:$EEZO -m $TMP/$name.bcl" "jit:$EEZO -n -m $TMP/$name.bcl" "elf:$TMP/$name.elf"; do
        local label="${mode%%:*}" cmd="${mode#*:}"
        out=$(printf '%b' "$input" | timeout 60 $cmd 2>"$TMP/err"; echo "|rc=${PIPESTATUS[1]}")
        check "$name ($label)" "$want" "$out"
    done
}

# a byte at a time: putbyte, then halt with a status
run hi    '#import io\nrun(seq(putbyte(72))(seq(putbyte(105))(putbyte(10))))'           ''      'Hi\n' 0
run exit7 '#import io\nrun(seq(putbyte(65))(halt(7)))'                                  ''      'A'    7
# the order of a prompt, a read and the echo is the data flow: > then the byte then a newline, status 3
run echo  '#import io\nrun(seq(putbyte(62))(bind(getbyte)(b -> seq(putbyte(b))(seq(putbyte(10))(halt(3))))))' 'Z' '>Z\n' 3
# getbyte gives 256 at the end of the input: copy until then
run cat   '#import io\n#import nat\n#import bool\ncat := fix(loop -> bind(getbyte)(b -> if(isZero(sub(b)(255)))(seq(putbyte(b))(loop))(halt(zero))))\nrun(cat)' 'hello\n' 'hello\n' 0
# lines over bytes: getline drops the newline, putline adds one; the second read ends at the end of the input
run line  '#import io\nrun(bind(getline)(l -> seq(putline(l))(bind(getline)(m -> putline(m)))))'  'ab\ncd' 'ab\ncd\n' 0
# pure has nothing to perform: the program finishes with status 0, its result unobserved
run pure  '#import io\nrun(pure(zero))'                                                 ''      ''     0

exit $status

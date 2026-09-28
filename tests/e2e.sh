#!/bin/bash
#
# End-to-end tests for eezoc
# Full compile-and-run tests using stdlib
#

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
WS="${EEZO_WS:-$(cd "${SCRIPT_DIR}/../.." && pwd)}"   # the workspace: this repository and its siblings (libeezo, eezo, eezoc, eezott, stdlib)
EEZOC="${EEZOC:-$WS/eezoc/eezoc}"
EEZO="${EEZO:-$WS/eezo/eezo}"
PASSED=0
FAILED=0

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
NC='\033[0m' # No Color

pass() {
    echo -e "${GREEN}PASS${NC}: $1"
    PASSED=$((PASSED + 1))
}

fail() {
    echo -e "${RED}FAIL${NC}: $1"
    echo "  Expected: $2"
    echo "  Got:      $3"
    FAILED=$((FAILED + 1))
}

# Full e2e test: compile source, run result, check output
# Args: name, source, expected_bcl
test_e2e() {
    local name="$1"
    local source="$2"
    local expected="$3"
    
    # Compile
    local bcl
    bcl=$(echo -e "$source" | "$EEZOC" 2>&1)
    if [ $? -ne 0 ]; then
        fail "$name (compile)" "$expected" "COMPILE_ERROR: $bcl"
        return
    fi
    
    # =========================================
    # BCL Output Tests
    # =========================================
    
    # Execute via eezo evaluator (STG machine)
    local result
    result=$(echo "$bcl" | "$EEZO" -f bcl 2>&1)
    if [ $? -ne 0 ]; then
        fail "$name (exec)" "$expected" "EXEC_ERROR: $result"
        return
    fi
    
    if [ "$result" = "$expected" ]; then
        pass "$name"
    else
        fail "$name" "$expected" "$result"
    fi
    
    # Also test with simple interpreter
    result=$(echo "$bcl" | "$EEZO" -f bcl -s 2>&1)
    if [ $? -ne 0 ]; then
        fail "$name (simple)" "$expected" "EXEC_ERROR: $result"
        return
    fi
    
    if [ "$result" = "$expected" ]; then
        pass "$name (simple)"
    else
        fail "$name (simple)" "$expected" "$result"
    fi
    
    # Test with native JIT
    result=$(echo "$bcl" | "$EEZO" -f bcl -n 2>&1)
    if [ $? -ne 0 ]; then
        fail "$name (native)" "$expected" "EXEC_ERROR: $result"
        return
    fi
    
    if [ "$result" = "$expected" ]; then
        pass "$name (native)"
    else
        fail "$name (native)" "$expected" "$result"
    fi
    
    # Test ELF emission and execution
    local elf_tmp=$(mktemp)
    echo -e "$source" | "$EEZOC" -e > "$elf_tmp" 2>&1
    if [ $? -ne 0 ]; then
        fail "$name (elf compile)" "$expected" "ELF_COMPILE_ERROR"
        rm -f "$elf_tmp"
        return
    fi
    
    chmod +x "$elf_tmp"
    result=$("$elf_tmp" 2>&1)
    local elf_exit=$?
    rm -f "$elf_tmp"
    
    if [ $elf_exit -ne 0 ]; then
        fail "$name (elf)" "$expected" "ELF_EXEC_ERROR (exit $elf_exit): $result"
        return
    fi
    
    if [ "$result" = "$expected" ]; then
        pass "$name (elf)"
    else
        fail "$name (elf)" "$expected" "$result"
    fi
    
    # =========================================
    # Jot Output Tests
    # =========================================
    
    # Compile to Jot format
    local jot
    jot=$(echo -e "$source" | "$EEZOC" -f jot 2>&1)
    if [ $? -ne 0 ]; then
        fail "$name (jot compile)" "..." "COMPILE_ERROR: $jot"
        return
    fi
    
    # Get expected Jot output from STG interpreter (reference implementation)
    local expected_jot
    expected_jot=$(echo "$jot" | "$EEZO" -f jot 2>&1)
    
    # Test native JIT with Jot format
    result=$(echo "$jot" | "$EEZO" -f jot -n 2>&1)
    if [ $? -ne 0 ]; then
        fail "$name (native jot)" "$expected_jot" "EXEC_ERROR: $result"
        return
    fi
    
    if [ "$result" = "$expected_jot" ]; then
        pass "$name (native jot)"
    else
        fail "$name (native jot)" "$expected_jot" "$result"
    fi
    
    # Test ELF with Jot output
    elf_tmp=$(mktemp)
    echo -e "$source" | "$EEZOC" -e -f jot > "$elf_tmp" 2>&1
    if [ $? -ne 0 ]; then
        fail "$name (elf jot compile)" "$expected_jot" "ELF_COMPILE_ERROR"
        rm -f "$elf_tmp"
        return
    fi
    
    chmod +x "$elf_tmp"
    result=$("$elf_tmp" 2>&1)
    elf_exit=$?
    rm -f "$elf_tmp"
    
    if [ $elf_exit -ne 0 ]; then
        fail "$name (elf jot)" "$expected_jot" "ELF_EXEC_ERROR (exit $elf_exit): $result"
        return
    fi
    
    if [ "$result" = "$expected_jot" ]; then
        pass "$name (elf jot)"
    else
        fail "$name (elf jot)" "$expected_jot" "$result"
    fi
    
    # =========================================
    # Jomplement Output Tests
    # =========================================
    
    # Compile to Jomplement format
    local jomp
    jomp=$(echo -e "$source" | "$EEZOC" -f jomplement 2>&1)
    if [ $? -ne 0 ]; then
        fail "$name (jomplement compile)" "..." "COMPILE_ERROR: $jomp"
        return
    fi
    
    # Get expected Jomplement output from STG interpreter (reference implementation)
    local expected_jomp
    expected_jomp=$(echo "$jomp" | "$EEZO" -f jomplement 2>&1)
    
    # Test native JIT with Jomplement format
    result=$(echo "$jomp" | "$EEZO" -f jomplement -n 2>&1)
    if [ $? -ne 0 ]; then
        fail "$name (native jomplement)" "$expected_jomp" "EXEC_ERROR: $result"
        return
    fi
    
    if [ "$result" = "$expected_jomp" ]; then
        pass "$name (native jomplement)"
    else
        fail "$name (native jomplement)" "$expected_jomp" "$result"
    fi
    
    # Test ELF with Jomplement output
    elf_tmp=$(mktemp)
    echo -e "$source" | "$EEZOC" -e -f jomplement > "$elf_tmp" 2>&1
    if [ $? -ne 0 ]; then
        fail "$name (elf jomplement compile)" "$expected_jomp" "ELF_COMPILE_ERROR"
        rm -f "$elf_tmp"
        return
    fi
    
    chmod +x "$elf_tmp"
    result=$("$elf_tmp" 2>&1)
    elf_exit=$?
    rm -f "$elf_tmp"
    
    if [ $elf_exit -ne 0 ]; then
        fail "$name (elf jomplement)" "$expected_jomp" "ELF_EXEC_ERROR (exit $elf_exit): $result"
        return
    fi
    
    if [ "$result" = "$expected_jomp" ]; then
        pass "$name (elf jomplement)"
    else
        fail "$name (elf jomplement)" "$expected_jomp" "$result"
    fi
}

# Test compile-only (no execution)
test_compile() {
    local name="$1"
    local source="$2"
    local expected="$3"
    
    local got
    got=$(echo -e "$source" | "$EEZOC" 2>&1)
    
    if [ "$got" = "$expected" ]; then
        pass "$name"
    else
        fail "$name" "$expected" "$got"
    fi
}

echo "========================================"
echo "eezoc End-to-End Tests"
echo "========================================"
echo

#
# Boolean Tests
#
echo "=== Booleans ==="

# not true = false, not false = true
test_e2e "not(true) = false" "#import bool\nnot(true)" "10011010000"
test_e2e "not(false) = true" "#import bool\nnot(false)" "00"

# and
test_e2e "and(true)(true) = true" "#import bool\nand(true)(true)" "00"
test_e2e "and(true)(false) = false" "#import bool\nand(true)(false)" "10011010000"
test_e2e "and(false)(true) = false" "#import bool\nand(false)(true)" "10011010000"
test_e2e "and(false)(false) = false" "#import bool\nand(false)(false)" "10011010000"

# or
test_e2e "or(true)(true) = true" "#import bool\nor(true)(true)" "00"
test_e2e "or(true)(false) = true" "#import bool\nor(true)(false)" "00"
test_e2e "or(false)(true) = true" "#import bool\nor(false)(true)" "00"
test_e2e "or(false)(false) = false" "#import bool\nor(false)(false)" "10011010000"

echo

#
# Pair Tests
#
echo "=== Pairs ==="

test_e2e "fst(pair(true)(false)) = true" "#import bool\n#import pair\nfst(pair(true)(false))" "00"
test_e2e "snd(pair(true)(false)) = false" "#import bool\n#import pair\nsnd(pair(true)(false))" "10011010000"
test_e2e "fst(pair(false)(true)) = false" "#import bool\n#import pair\nfst(pair(false)(true))" "10011010000"
test_e2e "snd(pair(false)(true)) = true" "#import bool\n#import pair\nsnd(pair(false)(true))" "00"

# swap
test_e2e "fst(swap(pair(true)(false))) = false" "#import bool\n#import pair\nfst(swap(pair(true)(false)))" "10011010000"
test_e2e "snd(swap(pair(true)(false))) = true" "#import bool\n#import pair\nsnd(swap(pair(true)(false)))" "00"

echo

#
# Maybe Tests
#
echo "=== Maybe ==="

test_e2e "isNothing(nothing) = true" "#import bool\n#import maybe\nisNothing(nothing)" "00"
test_e2e "isNothing(just(true)) = false" "#import bool\n#import maybe\nisNothing(just(true))" "10011010000"
test_e2e "isJust(just(true)) = true" "#import bool\n#import maybe\nisJust(just(true))" "00"
test_e2e "fromMaybe(false)(just(true)) = true" "#import bool\n#import maybe\nfromMaybe(false)(just(true))" "00"
test_e2e "fromMaybe(true)(nothing) = true" "#import bool\n#import maybe\nfromMaybe(true)(nothing)" "00"

echo

#
# Summary
#
echo "========================================"
echo "Results: $PASSED passed, $FAILED failed"
echo "========================================"

# Exit with error if any tests failed
[ "$FAILED" -eq 0 ]

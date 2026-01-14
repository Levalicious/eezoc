#!/bin/bash
#
# End-to-end tests for eezoc
# Full compile-and-run tests using stdlib
#

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
EEZOC="${SCRIPT_DIR}/../eezoc"
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
    
    # Execute via interpreter
    local result
    result=$(echo "$bcl" | "$EEZOC" -x bcl 2>&1)
    if [ $? -ne 0 ]; then
        fail "$name (exec)" "$expected" "EXEC_ERROR: $result"
        return
    fi
    
    if [ "$result" = "$expected" ]; then
        pass "$name"
    else
        fail "$name" "$expected" "$result"
    fi
    
    result=$(echo "$bcl" | "$EEZOC" -x bcl -j 2>&1)
    if [ $? -ne 0 ]; then
        fail "$name (jit-exec)" "$expected" "EXEC_ERROR: $result"
        return
    fi
    
    if [ "$result" = "$expected" ]; then
        pass "$name (jit)"
    else
        fail "$name (jit)" "$expected" "$result"
    fi

    result=$(echo "$bcl" | "$EEZOC" -x bcl -n 2>&1)
    if [ $? -ne 0 ]; then
        fail "$name (native-exec)" "$expected" "EXEC_ERROR: $result"
        return
    fi
    
    if [ "$result" = "$expected" ]; then
        pass "$name (native)"
    else
        fail "$name (native)" "$expected" "$result"
    fi
    
    # Execute via ELF
    local elf_file
    elf_file=$(mktemp)
    if ! echo "$bcl" | "$EEZOC" -x bcl -e > "$elf_file" 2>&1; then
        fail "$name (elf-emit)" "$expected" "ELF_EMIT_ERROR"
        rm -f "$elf_file"
        return
    fi
    chmod +x "$elf_file"
    
    local elf_result
    elf_result=$("$elf_file" 2>&1)
    local elf_exit=$?
    rm -f "$elf_file"
    
    if [ $elf_exit -ne 0 ]; then
        fail "$name (elf-exec)" "$expected" "ELF_EXEC_ERROR (exit $elf_exit)"
        return
    fi
    
    if [ "$elf_result" = "$expected" ]; then
        pass "$name (elf)"
    else
        fail "$name (elf)" "$expected" "$elf_result"
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

#!/bin/bash
#
# Benchmark script for eezo execution paths
#
# Compares: STG interpreter, simple interpreter, native JIT, and ELF executable
#

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
EEZOC="${SCRIPT_DIR}/../eezoc/eezoc"
EEZO="${SCRIPT_DIR}/../eezo/eezo"
ITERATIONS=${1:-3}

# Colors
CYAN='\033[0;36m'
YELLOW='\033[0;33m'
RED='\033[0;31m'
GREEN='\033[0;32m'
NC='\033[0m'

echo "========================================"
echo "eezo Benchmark Suite"
echo "========================================"
echo "Iterations per test: $ITERATIONS"
echo

# Time a command, return milliseconds
time_ms() {
    local start end
    start=$(date +%s%N)
    "$@" > /dev/null 2>&1
    end=$(date +%s%N)
    echo $(( (end - start) / 1000000 ))
}

# Run benchmark for all execution paths
# Args: name, source
bench() {
    local name="$1"
    local source="$2"
    
    # Compile to BCL
    local bcl
    bcl=$(echo -e "$source" | "$EEZOC" 2>/dev/null)
    
    if [ -z "$bcl" ]; then
        echo -e "${CYAN}$name${NC}"
        echo "  SKIPPED: compile failed"
        echo
        return
    fi
    
    echo -e "${CYAN}$name${NC}"
    echo "  BCL size: ${#bcl} bits"
    
    # Check parity across all implementations
    local stg_out simple_out native_out elf_out
    local elf_file
    elf_file=$(mktemp)
    
    stg_out=$(echo "$bcl" | "$EEZO" -f bcl 2>&1)
    simple_out=$(echo "$bcl" | "$EEZO" -f bcl -s 2>&1)
    native_out=$(echo "$bcl" | "$EEZO" -f bcl -n 2>&1)
    
    # Compile to ELF from source
    echo -e "$source" | "$EEZOC" -f elf > "$elf_file" 2>/dev/null
    chmod +x "$elf_file"
    elf_out=$("$elf_file" 2>&1)
    
    local parity_ok=1
    
    if [ "$stg_out" != "$simple_out" ]; then
        echo -e "  ${RED}PARITY ERROR: STG vs simple disagree${NC}"
        parity_ok=0
    fi
    
    if [ "$stg_out" != "$native_out" ]; then
        echo -e "  ${RED}PARITY ERROR: STG vs native disagree${NC}"
        parity_ok=0
    fi
    
    if [ "$stg_out" != "$elf_out" ]; then
        echo -e "  ${RED}PARITY ERROR: STG vs ELF disagree${NC}"
        parity_ok=0
    fi
    
    if [ "$parity_ok" -eq 1 ]; then
        echo -e "  ${GREEN}Parity: OK${NC}"
    fi
    
    local stg_total=0
    local simple_total=0
    local native_total=0
    local elf_total=0
    
    for ((i=0; i<ITERATIONS; i++)); do
        # STG interpreter
        stg_total=$((stg_total + $(time_ms bash -c "echo '$bcl' | '$EEZO' -f bcl")))
        
        # Simple interpreter
        simple_total=$((simple_total + $(time_ms bash -c "echo '$bcl' | '$EEZO' -f bcl -s")))
        
        # Native JIT
        native_total=$((native_total + $(time_ms bash -c "echo '$bcl' | '$EEZO' -f bcl -n")))
        
        # ELF executable
        elf_total=$((elf_total + $(time_ms "$elf_file")))
    done
    
    rm -f "$elf_file"
    
    local stg_avg=$((stg_total / ITERATIONS))
    local simple_avg=$((simple_total / ITERATIONS))
    local native_avg=$((native_total / ITERATIONS))
    local elf_avg=$((elf_total / ITERATIONS))
    
    printf "  %-12s %6d ms (avg)\n" "Simple:" "$simple_avg"
    printf "  %-12s %6d ms (avg)" "STG:" "$stg_avg"
    if [ "$stg_avg" -gt 0 ] && [ "$simple_avg" -gt 0 ]; then
        printf "  (%.1fx vs Simple)" "$(echo "scale=1; $simple_avg / $stg_avg" | bc 2>/dev/null || echo "?")"
    fi
    echo
    printf "  %-12s %6d ms (avg)" "Native:" "$native_avg"
    if [ "$native_avg" -gt 0 ] && [ "$simple_avg" -gt 0 ]; then
        printf "  (%.1fx vs Simple)" "$(echo "scale=1; $simple_avg / $native_avg" | bc 2>/dev/null || echo "?")"
    fi
    echo
    printf "  %-12s %6d ms (avg)" "ELF:" "$elf_avg"
    if [ "$elf_avg" -gt 0 ] && [ "$simple_avg" -gt 0 ]; then
        printf "  (%.1fx vs Simple)" "$(echo "scale=1; $simple_avg / $elf_avg" | bc 2>/dev/null || echo "?")"
    fi
    echo
    echo
}

#
# Long-running benchmark programs
#

# =============================================================================
# Memoized Fibonacci via Binary Trie (O(log n) lookup)
# =============================================================================
#
# Binary Trie for memoization:
#   - Each natural n is looked up by its binary representation
#   - Trie = Leaf(value) | Node(val, left, right)
#   - left = 0-bit subtrie, right = 1-bit subtrie
#   - O(log n) lookup vs O(n) for streams
#
# We represent naturals in binary and navigate the trie:
#   n = 0 -> root value
#   n > 0 -> if even, go left with n/2; if odd, go right with (n-1)/2

TRIE_FIB_PROG='#import nat
#import pair
#import bool
#import prelude

# Binary trie: node(value)(left)(right)
# Represented as: trie(v)(l)(r) = selector -> selector(v)(l)(r)
tnode(v)(l)(r)(sel) := sel(v)(l)(r)
tval(t) := t(v -> l -> r -> v)
tleft(t) := t(v -> l -> r -> l)
tright(t) := t(v -> l -> r -> r)

# Check if n is zero
# isZero from nat: isZero(n) = n(x -> false)(true)

# Division by 2 (for Church numerals) - uses the halving trick
# half(n) = floor(n/2)
# We build it via: half = fst of n iterations of (h, toggle) -> toggle ? (succ h, false) : (h, true)
# starting from (zero, false)
halfStep(p) := snd(p)(pair(succ(fst(p)))(false))(pair(fst(p))(true))
half(n) := fst(n(halfStep)(pair(zero)(false)))

# Check if n is odd: odd(n) = n toggles a bit n times starting from false
odd(n) := n(not)(false)
even(n) := n(not)(true)

# Trie lookup: index(trie)(n)
# if n == 0: return tval(trie)
# elif odd(n): index(tright(trie))(half(n))  
# else: index(tleft(trie))(half(n))
tindex(t)(n) := isZero(n)(tval(t))(odd(n)(tindex(tright(t))(half(n)))(tindex(tleft(t))(half(n))))

# Trie tabulate: create trie where tindex(t)(n) = f(n)
# tabulate(f) = fix(go)(zero) where
#   go(self)(base) = node(f(base))(self(2*base))(self(2*base+1))
# But we need to be lazy! In Church encoding, this builds infinitely.
# We use the fact that Church numerals are lazy - the trie is only built on demand.
double(n) := add(n)(n)
ttabulate(f) := fix(self -> base -> tnode(f(base))(self(double(base)))(self(double(succ(base)))))(zero)

# Memoized function via trie
trieMemo(g) := fix(memo -> n -> tindex(ttabulate(g(memo)))(n))

# Fibonacci with explicit recursion (for memoization)
# fibF(self)(n) = n==0 ? 1 : n==1 ? 1 : self(n-1) + self(n-2)
isOne(n) := isZero(pred(n))
fibF(self)(n) := isZero(n)(one)(isOne(n)(one)(add(self(pred(n)))(self(pred(pred(n))))))

# Memoized fib
memoFib := trieMemo(fibF)

# Simple iterative fib (O(n) via Church numeral iteration) for comparison
fibStep(p) := pair(snd(p))(add(fst(p))(snd(p)))
fib(n) := fst(n(fibStep)(pair(one)(one)))

# Numbers for testing
four := succ(three)
five := succ(four)
six := succ(five)
seven := succ(six)
eight := succ(seven)
nine := succ(eight)
ten := succ(nine)
fifteen := add(ten)(five)
twenty := add(ten)(ten)
twentyfive := add(twenty)(five)
thirty := add(twenty)(ten)
forty := add(thirty)(ten)
fifty := add(forty)(ten)
hundred := mul(ten)(ten)
twohundred := add(hundred)(hundred)
fivehundred := mul(hundred)(five)
thousand := mul(hundred)(ten)'

echo -e "${YELLOW}=== Iterative Fibonacci (O(n) via Church iteration) ===${NC}"
echo

bench "fib(10)" "${TRIE_FIB_PROG}"$'\nfib(ten)'
bench "fib(20)" "${TRIE_FIB_PROG}"$'\nfib(twenty)'
bench "fib(50)" "${TRIE_FIB_PROG}"$'\nfib(fifty)'
bench "fib(100)" "${TRIE_FIB_PROG}"$'\nfib(hundred)'
bench "fib(200)" "${TRIE_FIB_PROG}"$'\nfib(twohundred)'
bench "fib(500)" "${TRIE_FIB_PROG}"$'\nfib(fivehundred)'
bench "fib(1000)" "${TRIE_FIB_PROG}"$'\nfib(thousand)'

echo -e "${YELLOW}=== Power of Church Numerals ===${NC}"
echo

bench "pow(2)(10) = 1024" '#import nat
ten := mul(succ(succ(succ(succ(succ(zero))))))(two)
pow(two)(ten)'

bench "pow(2)(12) = 4096" '#import nat
twelve := mul(succ(succ(succ(succ(succ(succ(zero)))))))(two)
pow(two)(twelve)'

echo "========================================"
echo "Benchmark complete"
echo "========================================"

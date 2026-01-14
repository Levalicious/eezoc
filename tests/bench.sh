#!/bin/bash
#
# Benchmark script for eezoc execution paths
#
# Compares: interpreter, STG JIT, native JIT, and ELF executable
#

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
EEZOC="${SCRIPT_DIR}/../eezoc"
ITERATIONS=${1:-3}

# Colors
CYAN='\033[0;36m'
YELLOW='\033[0;33m'
NC='\033[0m'

echo "========================================"
echo "eezoc Benchmark Suite"
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
# Args: name, bcl_input
bench() {
    local name="$1"
    local bcl="$2"
    
    if [ -z "$bcl" ]; then
        echo -e "${CYAN}$name${NC}"
        echo "  SKIPPED: empty BCL (compile failed?)"
        echo
        return
    fi
    
    echo -e "${CYAN}$name${NC}"
    echo "  BCL size: ${#bcl} bits"
    
    local interp_total=0
    local jit_total=0
    local native_total=0
    local elf_total=0
    
    # Create ELF once
    local elf_file
    elf_file=$(mktemp)
    echo "$bcl" | "$EEZOC" -x bcl -e > "$elf_file" 2>/dev/null
    chmod +x "$elf_file"
    
    for ((i=0; i<ITERATIONS; i++)); do
        # Interpreter
        # interp_total=$((interp_total + $(time_ms bash -c "echo '$bcl' | '$EEZOC' -x bcl")))
        
        # STG JIT
        jit_total=$((jit_total + $(time_ms bash -c "echo '$bcl' | '$EEZOC' -x bcl -j")))
        
        # Native JIT
        native_total=$((native_total + $(time_ms bash -c "echo '$bcl' | '$EEZOC' -x bcl -j -n")))
        
        # ELF executable
        elf_total=$((elf_total + $(time_ms "$elf_file")))
    done
    
    rm -f "$elf_file"
    
    local interp_avg=$((interp_total / ITERATIONS))
    local jit_avg=$((jit_total / ITERATIONS))
    local native_avg=$((native_total / ITERATIONS))
    local elf_avg=$((elf_total / ITERATIONS))
    
    printf "  %-12s %6d ms (avg)\n" "Interpreter:" "$interp_avg"
    printf "  %-12s %6d ms (avg)" "STG JIT:" "$jit_avg"
    [ "$jit_avg" -gt 0 ] && printf "  %.1fx" "$(echo "scale=1; $interp_avg / $jit_avg" | bc 2>/dev/null || echo "?")"
    echo
    printf "  %-12s %6d ms (avg)" "Native JIT:" "$native_avg"
    [ "$native_avg" -gt 0 ] && printf "  %.1fx" "$(echo "scale=1; $interp_avg / $native_avg" | bc 2>/dev/null || echo "?")"
    echo
    printf "  %-12s %6d ms (avg)" "ELF:" "$elf_avg"
    [ "$elf_avg" -gt 0 ] && printf "  %.1fx" "$(echo "scale=1; $interp_avg / $elf_avg" | bc 2>/dev/null || echo "?")"
    echo
    echo
}

# Compile eezo source to BCL
compile() {
    echo -e "$1" | "$EEZOC" 2>/dev/null
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

bench "fib(10)" "$(compile "${TRIE_FIB_PROG}"$'\nfib(ten)')"
bench "fib(20)" "$(compile "${TRIE_FIB_PROG}"$'\nfib(twenty)')"
bench "fib(50)" "$(compile "${TRIE_FIB_PROG}"$'\nfib(fifty)')"
bench "fib(100)" "$(compile "${TRIE_FIB_PROG}"$'\nfib(hundred)')"
bench "fib(200)" "$(compile "${TRIE_FIB_PROG}"$'\nfib(twohundred)')"
bench "fib(500)" "$(compile "${TRIE_FIB_PROG}"$'\nfib(fivehundred)')"
bench "fib(1000)" "$(compile "${TRIE_FIB_PROG}"$'\nfib(thousand)')"

echo -e "${YELLOW}=== Power of Church Numerals ===${NC}"
echo

bench "pow(2)(10) = 1024" "$(compile '#import nat
ten := mul(succ(succ(succ(succ(succ(zero))))))(two)
pow(two)(ten)')"

bench "pow(2)(12) = 4096" "$(compile '#import nat
twelve := mul(succ(succ(succ(succ(succ(succ(zero)))))))(two)
pow(two)(twelve)')"

echo "========================================"
echo "Benchmark complete"
echo "========================================"

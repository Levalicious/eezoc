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
    echo -e "$source" | "$EEZOC" -e > "$elf_file" 2>/dev/null
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

echo -e "${YELLOW}=== Iterative Fibonacci (O(n) via Church iteration) ===${NC}"
echo

bench "fib(10)" '#import fib
fib(ten)'
bench "fib(15)" '#import fib
fib(fifteen)'
# bench "fib(50)" '#import fib
# fib(fifty)'
# bench "fib(100)" '#import fib
# fib(hundred)'
# bench "fib(200)" '#import fib
# fib(twohundred)'
# bench "fib(500)" '#import fib
# fib(fivehundred)'
# bench "fib(1000)" '#import fib
# fib(thousand)'

# echo -e "${YELLOW}=== Power of Church Numerals ===${NC}"
# echo

# bench "pow(2)(10) = 1024" '#import nat
# ten := mul(succ(succ(succ(succ(succ(zero))))))(two)
# pow(two)(ten)'

# bench "pow(2)(12) = 4096" '#import nat
# twelve := mul(succ(succ(succ(succ(succ(succ(zero)))))))(two)
# pow(two)(twelve)'

echo "========================================"
echo "Benchmark complete"
echo "========================================"

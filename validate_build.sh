#!/bin/bash

# Build validation script for Pollard-Kangaroo Solver
echo "=== Pollard-Kangaroo Solver Build Validation ==="
echo

# Check if required files exist
echo "Checking required files..."

REQUIRED_FILES=(
    "Mark1.cpp"
    "Int.cpp"
    "SECP256K1.cpp"
    "Point.cpp"
    "Random.cpp"
    "IntMod.cpp"
    "IntGroup.cpp"
    "Timer.cpp"
    "DP-analyzer/DP-analyzer.cpp"
    "Int.h"
    "Point.h"
    "SECP256K1.h"
    "IntGroup.h"
    "Timer.h"
    "Random.h"
    "hashutil.h"
    "simd_block_bloom.h"
    "Makefile"
)

MISSING_FILES=()
for file in "${REQUIRED_FILES[@]}"; do
    if [ ! -f "$file" ]; then
        MISSING_FILES+=("$file")
    fi
done

if [ ${#MISSING_FILES[@]} -ne 0 ]; then
    echo "❌ Missing files:"
    for file in "${MISSING_FILES[@]}"; do
        echo "  - $file"
    done
    exit 1
else
    echo "✅ All required files present"
fi

# Check Makefile syntax
echo
echo "Checking Makefile syntax..."
if command -v make >/dev/null 2>&1; then
    if make -n release >/dev/null 2>&1; then
        echo "✅ Makefile syntax is valid"
    else
        echo "❌ Makefile syntax error"
        exit 1
    fi
else
    echo "⚠️  make command not available, skipping syntax check"
fi

# Check if g++ is available
echo
echo "Checking compiler availability..."
if command -v g++ >/dev/null 2>&1; then
    GCC_VERSION=$(g++ --version | head -n1)
    echo "✅ g++ available: $GCC_VERSION"
else
    echo "❌ g++ not found"
    exit 1
fi

# Test basic compilation (dry run)
echo
echo "Testing basic compilation..."
if g++ -c -std=c++17 -march=native -pthread -fopenmp -O3 Mark1.cpp -o /tmp/test.o 2>/dev/null; then
    echo "✅ Basic compilation test passed"
    rm -f /tmp/test.o
else
    echo "❌ Basic compilation test failed"
fi

echo
echo "Testing DP-analyzer compilation..."
if g++ -c -std=c++17 -march=native -pthread -fopenmp -O3 DP-analyzer/DP-analyzer.cpp -I. -o /tmp/test_dp.o 2>/dev/null; then
    echo "✅ DP-analyzer compilation test passed"
    rm -f /tmp/test_dp.o
else
    echo "❌ DP-analyzer compilation test failed"
fi

echo
echo "=== Build validation complete ==="
echo
echo "To build the project:"
echo "  make release    # Optimized build"
echo "  make debug      # Debug build"
echo "  make profile    # Profiling build"

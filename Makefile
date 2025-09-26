# ===================================================================
# Pollard-Kangaroo Solver (Mark1) - Optimized Makefile
# ===================================================================

# Compiler and flags
CXX = g++
CXXFLAGS_COMMON = -std=c++17 -march=native -pthread -fopenmp
CXXFLAGS_OPT = -O3 -funroll-loops -ftree-vectorize -fstrict-aliasing \
               -fno-semantic-interposition -fvect-cost-model=unlimited \
               -fno-trapping-math -fipa-ra -fipa-modref -flto \
               -fassociative-math -fomit-frame-pointer -ffast-math \
               -malign-data=cacheline -floop-nest-optimize \
               -floop-unroll-and-jam -fpeel-loops \
               -fvariable-expansion-in-unroller
CXXFLAGS_SIMD = -mavx2 -mbmi2 -madx
CXXFLAGS_DEBUG = -g -O0 -DDEBUG

# Target names
TARGET_MARK1 = Mark1
TARGET_DP_ANALYZER = DP-analyzer

# Source files
SRCS_MARK1 = Mark1.cpp Int.cpp SECP256K1.cpp Point.cpp Random.cpp IntMod.cpp IntGroup.cpp Timer.cpp
SRCS_DP_ANALYZER = DP-analyzer/DP-analyzer.cpp Int.cpp
HDRS = Int.h Point.h SECP256K1.h IntGroup.h Timer.h Random.h hashutil.h simd_block_bloom.h

# Build directories
BUILD_DIR = build
OBJ_DIR = $(BUILD_DIR)/obj
BIN_DIR = $(BUILD_DIR)/bin

# Object files
OBJS_MARK1 = $(OBJ_DIR)/Mark1.o $(OBJ_DIR)/Int.o $(OBJ_DIR)/SECP256K1.o $(OBJ_DIR)/Point.o $(OBJ_DIR)/Random.o $(OBJ_DIR)/IntMod.o $(OBJ_DIR)/IntGroup.o $(OBJ_DIR)/Timer.o
OBJS_DP_ANALYZER = $(OBJ_DIR)/DP-analyzer.o $(OBJ_DIR)/Int.o

# Default target
all: release

# Release build (optimized)
release: CXXFLAGS = $(CXXFLAGS_COMMON) $(CXXFLAGS_OPT) $(CXXFLAGS_SIMD)
release: $(BIN_DIR)/$(TARGET_MARK1) $(BIN_DIR)/$(TARGET_DP_ANALYZER)

# Debug build
debug: CXXFLAGS = $(CXXFLAGS_COMMON) $(CXXFLAGS_DEBUG) $(CXXFLAGS_SIMD)
debug: $(BIN_DIR)/$(TARGET_MARK1)_debug $(BIN_DIR)/$(TARGET_DP_ANALYZER)_debug

# Profile build
profile: CXXFLAGS = $(CXXFLAGS_COMMON) $(CXXFLAGS_OPT) $(CXXFLAGS_SIMD) -pg
profile: $(BIN_DIR)/$(TARGET_MARK1)_profile

# Create directories
$(BUILD_DIR) $(OBJ_DIR) $(BIN_DIR):
	mkdir -p $@

# Compile object files (general rule, specific dependencies override)
$(OBJ_DIR)/%.o: %.cpp | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Special rule for DP-analyzer in subdirectory
$(OBJ_DIR)/DP-analyzer.o: DP-analyzer/DP-analyzer.cpp | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Link Mark1
$(BIN_DIR)/$(TARGET_MARK1): $(OBJS_MARK1) | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $^ -o $@ -fopenmp -pthread

# Link DP-analyzer
$(BIN_DIR)/$(TARGET_DP_ANALYZER): $(OBJS_DP_ANALYZER) | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $^ -o $@

# Debug versions
$(BIN_DIR)/$(TARGET_MARK1)_debug: $(OBJS_MARK1) | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $^ -o $@ -fopenmp -pthread

$(BIN_DIR)/$(TARGET_DP_ANALYZER)_debug: $(OBJS_DP_ANALYZER) | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $^ -o $@

# Profile version
$(BIN_DIR)/$(TARGET_MARK1)_profile: $(OBJS_MARK1) | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $^ -o $@ -fopenmp -pthread -pg

# Clean
clean:
	rm -rf $(BUILD_DIR)

# Deep clean (removes DP table files too)
distclean: clean
	rm -f dp_table.bin DP.bin

# Dependencies
$(OBJ_DIR)/Mark1.o: Mark1.cpp Int.h Point.h SECP256K1.h IntGroup.h Timer.h Random.h hashutil.h simd_block_bloom.h
$(OBJ_DIR)/Int.o: Int.cpp Int.h
$(OBJ_DIR)/SECP256K1.o: SECP256K1.cpp SECP256K1.h Int.h Point.h
$(OBJ_DIR)/Point.o: Point.cpp Point.h Int.h
$(OBJ_DIR)/Random.o: Random.cpp Random.h
$(OBJ_DIR)/IntMod.o: IntMod.cpp Int.h
$(OBJ_DIR)/IntGroup.o: IntGroup.cpp IntGroup.h Int.h
$(OBJ_DIR)/Timer.o: Timer.cpp Timer.h
$(OBJ_DIR)/DP-analyzer.o: DP-analyzer/DP-analyzer.cpp Int.h

# Help
help:
	@echo "Pollard-Kangaroo Solver (Mark1) - Makefile"
	@echo ""
	@echo "Targets:"
	@echo "  all          - Build optimized release version (default)"
	@echo "  release      - Build optimized release version"
	@echo "  debug        - Build debug version with symbols"
	@echo "  profile      - Build with profiling support"
	@echo "  clean        - Remove build artifacts"
	@echo "  distclean    - Remove build artifacts and data files"
	@echo "  help         - Show this help"
	@echo ""
	@echo "Executables will be placed in $(BIN_DIR)/"
	@echo ""
	@echo "Usage examples:"
	@echo "  make release    # Build optimized version"
	@echo "  make debug      # Build debug version"
	@echo "  ./build/bin/Mark1 --help"

.PHONY: all release debug profile clean distclean help

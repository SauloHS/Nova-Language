CXX = g++
LLVM_CXXFLAGS = $(shell llvm-config --cxxflags | sed 's/-fno-exceptions//')
CXXFLAGS = -std=c++17 -Wall -Iinclude $(LLVM_CXXFLAGS)

BUILD_VERSION = $(shell cat .build_no)
CXXFLAGS += -DNOVA_BUILD_VERSION=$(BUILD_VERSION)

# Linkagem
LDFLAGS_DYNAMIC = $(shell llvm-config --ldflags --system-libs --libs all)
LIBS = -lncurses

SRCS = src/main.cpp src/lexer.cpp src/parser.cpp src/codegen.cpp src/analysis.cpp src/editor.cpp
TARGET = n++

# ── Compilador Nova (n++) ─────────────────────────────────────────────────────

$(TARGET): $(SRCS)
	$(CXX) $(CXXFLAGS) $(SRCS) $(LDFLAGS_DYNAMIC) $(LIBS) -o $(TARGET) -O3

release: $(SRCS)
	$(CXX) $(CXXFLAGS) $(SRCS) \
	/usr/lib/gcc/x86_64-linux-gnu/14/libstdc++.a \
	$(LDFLAGS_DYNAMIC) $(LIBS) \
	-static-libgcc -o $(TARGET) -O3

# ── Orbit (gerenciador de build/pacotes) ──────────────────────────────────────

orbit:
	@echo "  → Building orbit..."
	@$(MAKE) -C orbit --no-print-directory
	@echo "  ✓ orbit built"

install-orbit:
	@echo "  → Installing orbit..."
	@bash orbit/install.sh
	@echo "  ✓ orbit installed"

# ── Instalação completa (n++ + orbit) ────────────────────────────────────────

install: $(TARGET) orbit
	@bash install.sh
	@bash orbit/install.sh

# ── Limpeza ───────────────────────────────────────────────────────────────────

clean:
	rm -f $(TARGET) output.o output
	$(MAKE) -C orbit clean --no-print-directory

.PHONY: clean release orbit install-orbit install

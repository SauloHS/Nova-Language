CXX      = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -Isrc
LIBS     = -lncurses

ORBIT_VERSION = 0.1.0
CXXFLAGS += -DORBIT_VERSION=\"$(ORBIT_VERSION)\"

SRCS = src/main.cpp       \
       src/ui.cpp         \
       src/toml_parser.cpp\
       src/project.cpp    \
       src/package.cpp    \
       src/downloader.cpp \
       src/builder.cpp

TARGET = orbit

# ── Default (dev) ─────────────────────────────────────────────────────────────
$(TARGET): $(SRCS)
	$(CXX) $(CXXFLAGS) $(SRCS) $(LIBS) -o $(TARGET) -O2

# ── Release (with libcurl if available) ───────────────────────────────────────
release: CXXFLAGS += -DORBIT_USE_LIBCURL
release: LIBS     += -lcurl
release: $(SRCS)
	$(CXX) $(CXXFLAGS) $(SRCS) $(LIBS) -o $(TARGET) -O3 -DNDEBUG

clean:
	rm -f $(TARGET)

.PHONY: clean release

#include "ui.h"
#include <ncurses.h>
#include <cstdio>
#include <cstring>
#include <unistd.h>

// We use ncurses only for color support, but write directly to stdout
// so that piping/redirection still works reasonably.
// The trick: we use initscr()+endwin() only to detect color capability,
// then use tput-style escape sequences for actual output.

// ANSI fallback (used when ncurses init fails or no color support)
static bool g_color_enabled = false;

static const char* ANSI_RESET   = "\033[0m";
static const char* ANSI_BOLD    = "\033[1m";
static const char* ANSI_CYAN    = "\033[1;36m";
static const char* ANSI_GREEN   = "\033[1;32m";
static const char* ANSI_RED     = "\033[1;31m";
static const char* ANSI_YELLOW  = "\033[1;33m";
static const char* ANSI_GRAY    = "\033[0;90m";
static const char* ANSI_BLUE    = "\033[1;34m";
static const char* ANSI_MAGENTA = "\033[1;35m";

namespace ui {

void init() {
    // Check if stdout is a terminal
    if (isatty(fileno(stdout))) {
        g_color_enabled = true;
    }
}

void shutdown() {
    // nothing to clean up in ANSI mode
}

static const char* get_ansi(int color_pair, bool bold) {
    if (!g_color_enabled) return "";
    switch (color_pair) {
        case C_CYAN:    return ANSI_CYAN;
        case C_GREEN:   return ANSI_GREEN;
        case C_RED:     return ANSI_RED;
        case C_YELLOW:  return ANSI_YELLOW;
        case C_GRAY:    return ANSI_GRAY;
        case C_BLUE:    return ANSI_BLUE;
        case C_MAGENTA: return ANSI_MAGENTA;
        default:        return bold ? ANSI_BOLD : "";
    }
}

static const char* rst() {
    return g_color_enabled ? ANSI_RESET : "";
}

void print_colored(int color_pair, const std::string& msg, bool bold) {
    if (bold && g_color_enabled) printf("%s", ANSI_BOLD);
    printf("%s%s%s", get_ansi(color_pair, bold), msg.c_str(), rst());
}

void blank() {
    printf("\n");
}

void divider() {
    printf("%s  %s%s\n", get_ansi(C_GRAY, false),
           "────────────────────────────────────────────────────", rst());
}

void info(const std::string& msg) {
    printf("%s  →%s %s\n", get_ansi(C_CYAN, false), rst(), msg.c_str());
}

void success(const std::string& msg) {
    printf("%s  ✓%s %s%s%s\n",
           get_ansi(C_GREEN, false), rst(),
           ANSI_BOLD, msg.c_str(), rst());
}

void warn(const std::string& msg) {
    printf("%s  ⚠%s  %s\n", get_ansi(C_YELLOW, false), rst(), msg.c_str());
}

void error(const std::string& msg) {
    fprintf(stderr, "%s  ✗ error:%s %s%s%s\n",
            get_ansi(C_RED, false), rst(),
            ANSI_BOLD, msg.c_str(), rst());
}

void step(int current, int total, const std::string& msg) {
    printf("%s  [%d/%d]%s %s\n",
           get_ansi(C_BLUE, false), current, total, rst(), msg.c_str());
}

void header(const std::string& title, const std::string& subtitle) {
    printf("\n");
    printf("%s%s  %s%s\n",
           ANSI_BOLD, get_ansi(C_CYAN, false), title.c_str(), rst());
    if (!subtitle.empty()) {
        printf("%s  %s%s\n", get_ansi(C_GRAY, false), subtitle.c_str(), rst());
    }
    printf("\n");
}

void print_kv(const std::string& key, const std::string& val) {
    printf("  %s%-18s%s %s\n",
           get_ansi(C_GRAY, false), key.c_str(), rst(), val.c_str());
}

void progress(const std::string& label, int pct) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    int filled = pct * 30 / 100;
    int empty  = 30 - filled;

    printf("  %s%-20s%s [", get_ansi(C_GRAY, false), label.c_str(), rst());
    printf("%s", get_ansi(C_GREEN, false));
    for (int i = 0; i < filled; i++) printf("█");
    printf("%s", rst());
    for (int i = 0; i < empty; i++) printf("░");
    printf("] %s%d%%%s\n", ANSI_BOLD, pct, rst());
}

} // namespace ui

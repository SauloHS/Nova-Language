#pragma once
#include <string>
#include <ncurses.h>

namespace ui {

// Color pair IDs
enum Colors {
    C_RESET  = 0,
    C_CYAN   = 1,
    C_GREEN  = 2,
    C_RED    = 3,
    C_YELLOW = 4,
    C_GRAY   = 5,
    C_BOLD   = 6,
    C_BLUE   = 7,
    C_MAGENTA= 8,
};

void init();
void shutdown();

void info   (const std::string& msg);
void success(const std::string& msg);
void warn   (const std::string& msg);
void error  (const std::string& msg);
void step   (int current, int total, const std::string& msg);
void header (const std::string& title, const std::string& subtitle = "");
void divider();
void print_kv(const std::string& key, const std::string& val);
void blank();

// Progress bar [####....] percentage
void progress(const std::string& label, int pct);

// Raw colored print (does NOT add newline prefix icons)
void print_colored(int color_pair, const std::string& msg, bool bold = false);

} // namespace ui

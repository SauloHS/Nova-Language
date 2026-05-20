#include "config.h"

#ifdef _WIN32
#include <ncurses/curses.h>
#else
#include <ncurses.h>
#endif

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>

// ── Valores padrão
// ────────────────────────────────────────────────────────────
int cfg_time_LSP_check = 10;
int cfg_tab_size = 4;
bool cfg_show_line_numbers = true;
int cfg_scroll_off = 3;

int cfg_color_keyword = COLOR_BLUE;
int cfg_color_type = COLOR_CYAN;
int cfg_color_number = COLOR_YELLOW;
int cfg_color_string = COLOR_YELLOW;
int cfg_color_comment = COLOR_BLACK;
int cfg_color_literal = COLOR_MAGENTA;
int cfg_color_char = COLOR_GREEN;
int cfg_color_operator = COLOR_CYAN;

std::vector<KeyRemap> cfg_key_remaps;
std::vector<std::string> cfg_check_extensions = {".npp"};

std::vector<std::string> cfg_compile_extensions = {".npp"};
std::string cfg_compile_arg = "";

// ── Helpers
// ───────────────────────────────────────────────────────────────────
static std::string trim(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  size_t b = s.find_last_not_of(" \t\r\n");
  return (a == std::string::npos) ? "" : s.substr(a, b - a + 1);
}

int parseColor(const std::string& name) {
  if (name == "black") return COLOR_BLACK;
  if (name == "red") return COLOR_RED;
  if (name == "green") return COLOR_GREEN;
  if (name == "yellow") return COLOR_YELLOW;
  if (name == "blue") return COLOR_BLUE;
  if (name == "magenta") return COLOR_MAGENTA;
  if (name == "cyan") return COLOR_CYAN;
  if (name == "white") return COLOR_WHITE;
  return -1;  // padrão do terminal
}

int applyRemap(const std::string& keyName) {
  for (auto& r : cfg_key_remaps)
    if (r.from == keyName) return 1;  // sinaliza que há remap
  return -1;
}

std::string getRemapTarget(const std::string& keyName) {
  for (auto& r : cfg_key_remaps)
    if (r.from == keyName) return r.to;
  return "";
}

// ── Carrega o cfg
// ─────────────────────────────────────────────────────────────
void loadConfig() {
  // Determina o caminho
  std::string cfgPath;
  const char* stdlibPath = getenv("NOVA_STDLIB_PATH");
  if (stdlibPath) {
    cfgPath = std::string(stdlibPath) + "/nova.cfg";
  } else {
#ifdef _WIN32
    const char* home = getenv("USERPROFILE");
    if (!home) home = getenv("HOME");
    cfgPath = std::string(home ? home : ".") + "/.local/lib/nova/nova.cfg";
#else
    cfgPath = "/usr/local/lib/nova/nova.cfg";
#endif
  }

  std::ifstream f(cfgPath);
  if (!f) return;  // sem cfg, usa defaults

  std::string line;
  while (std::getline(f, line)) {
    // Remove comentário
    size_t hash = line.find('#');
    if (hash != std::string::npos) line = line.substr(0, hash);
    line = trim(line);
    if (line.empty()) continue;

    size_t eq = line.find('=');
    if (eq == std::string::npos) continue;

    std::string key = trim(line.substr(0, eq));
    std::string val = trim(line.substr(eq + 1));
    if (key.empty() || val.empty()) continue;

    if (key == "time_LSP_check") {
      cfg_time_LSP_check = std::stoi(val);
    } else if (key == "tab_size") {
      cfg_tab_size = std::stoi(val);
    } else if (key == "show_line_numbers") {
      cfg_show_line_numbers = (val == "true");
    } else if (key == "scroll_off") {
      cfg_scroll_off = std::stoi(val);
    } else if (key == "color_keyword") {
      int c = parseColor(val);
      if (c != -1) cfg_color_keyword = c;
    } else if (key == "color_type") {
      int c = parseColor(val);
      if (c != -1) cfg_color_type = c;
    } else if (key == "color_number") {
      int c = parseColor(val);
      if (c != -1) cfg_color_number = c;
    } else if (key == "color_string") {
      int c = parseColor(val);
      if (c != -1) cfg_color_string = c;
    } else if (key == "color_comment") {
      int c = parseColor(val);
      if (c != -1) cfg_color_comment = c;
    } else if (key == "color_literal") {
      int c = parseColor(val);
      if (c != -1) cfg_color_literal = c;
    } else if (key == "color_char") {
      int c = parseColor(val);
      if (c != -1) cfg_color_char = c;
    } else if (key == "color_operator") {
      int c = parseColor(val);
      if (c != -1) cfg_color_operator = c;
    } else if (key == "key_remap") {
      // formato: FROM -> TO
      size_t arrow = val.find("->");
      if (arrow != std::string::npos) {
        KeyRemap r;
        r.from = trim(val.substr(0, arrow));
        r.to = trim(val.substr(arrow + 2));
        cfg_key_remaps.push_back(r);
      }
    }
  else if (key == "check_error_extension_file") {
    cfg_check_extensions.clear();
    std::stringstream ss(val);
    std::string ext;
    while (std::getline(ss, ext, ',')) {
      ext = trim(ext);
      if (!ext.empty()) cfg_check_extensions.push_back(ext);
    }
    }
  else if (key == "compile_file_extension") {
    cfg_compile_extensions.clear();
    if (val != "None") {
      std::stringstream ss(val);
      std::string ext;
      while (std::getline(ss, ext, ',')) {
        ext = trim(ext);
        if (!ext.empty()) cfg_compile_extensions.push_back(ext);
      }
    }
  }
  else if (key == "compile_arg") {
    cfg_compile_arg = (val == "None") ? "" : val;
  }
  }
}
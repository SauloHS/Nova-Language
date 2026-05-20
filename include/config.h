#pragma once
#include <map>
#include <string>
#include <vector>

struct KeyRemap {
  std::string from;
  std::string to;
};

// Configurações carregadas do nova.cfg
extern int cfg_time_LSP_check;
extern int cfg_tab_size;
extern bool cfg_show_line_numbers;
extern int cfg_scroll_off;

extern int cfg_color_keyword;
extern int cfg_color_type;
extern int cfg_color_number;
extern int cfg_color_string;
extern int cfg_color_comment;
extern int cfg_color_literal;
extern int cfg_color_char;
extern int cfg_color_operator;

extern std::vector<KeyRemap> cfg_key_remaps;
extern std::vector<std::string> cfg_check_extensions;

extern std::vector<std::string> cfg_compile_extensions;
extern std::string cfg_compile_arg;

void loadConfig();
int parseColor(const std::string& name);
// Retorna o keycode remapeado, ou -1 se não há remap
int applyRemap(const std::string& keyName);
#include "plugin_manager.h"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#define DLOPEN(path) LoadLibraryA(path)
#define DLSYM(h, name) GetProcAddress((HMODULE)(h), name)
#define DLCLOSE(h) FreeLibrary((HMODULE)(h))
#define DLERROR() "LoadLibrary failed"
#define LIB_EXT ".dll"
#else
#include <dirent.h>
#include <dlfcn.h>
#define DLOPEN(path) dlopen(path, RTLD_LAZY)
#define DLSYM(h, name) dlsym(h, name)
#define DLCLOSE(h) dlclose(h)
#define DLERROR() dlerror()
#define LIB_EXT ".so"
#endif

static std::vector<LoadedPlugin> g_plugins;

// ── Pending UI state
// ──────────────────────────────────────────────────────────
PendingUI g_pendingUI;

static std::vector<std::string> splitStr(const std::string& s, char delim) {
  std::vector<std::string> out;
  std::stringstream ss(s);
  std::string tok;
  while (std::getline(ss, tok, delim)) {
    size_t a = tok.find_first_not_of(" \t");
    size_t b = tok.find_last_not_of(" \t");
    if (a != std::string::npos) out.push_back(tok.substr(a, b - a + 1));
  }
  return out;
}

static std::vector<std::string> listPluginFiles(const std::string& dir) {
  std::vector<std::string> files;
#ifdef _WIN32
  WIN32_FIND_DATAA fd;
  std::string pattern = dir + "\\*" + LIB_EXT;
  HANDLE hFind = FindFirstFileA(pattern.c_str(), &fd);
  if (hFind != INVALID_HANDLE_VALUE) {
    do {
      files.push_back(dir + "\\" + fd.cFileName);
    } while (FindNextFileA(hFind, &fd));
    FindClose(hFind);
  }
#else
  DIR* d = opendir(dir.c_str());
  if (!d) return files;
  struct dirent* entry;
  while ((entry = readdir(d))) {
    std::string name = entry->d_name;
    if (name.size() >= 3 && name.substr(name.size() - 3) == LIB_EXT)
      files.push_back(dir + "/" + name);
  }
  closedir(d);
#endif
  return files;
}

void pluginManagerInit(const std::string& pluginDir) {
  auto files = listPluginFiles(pluginDir);
  for (auto& path : files) {
    void* handle = DLOPEN(path.c_str());
    if (!handle) {
      std::cerr << "plugin: failed to load " << path << ": " << DLERROR()
                << "\n";
      continue;
    }
    LoadedPlugin p;
    p.handle = handle;
    p.fn_info = (const char* (*)())DLSYM(handle, "plugin_info");
    p.fn_commands = (const char* (*)())DLSYM(handle, "plugin_commands");
    p.fn_on_event = (NovaResponse (*)(NovaEvent*, NovaBuffer*))DLSYM(
        handle, "plugin_on_event");
    p.fn_free_resp =
        (void (*)(NovaResponse*))DLSYM(handle, "nova_free_response");

    if (!p.fn_info || !p.fn_on_event || !p.fn_free_resp) {
      std::cerr << "plugin: " << path << " missing required exports\n";
      DLCLOSE(handle);
      continue;
    }
    auto parts = splitStr(p.fn_info(), '|');
    p.name = parts.size() > 0 ? parts[0] : path;
    p.version = parts.size() > 1 ? parts[1] : "?";
    p.author = parts.size() > 2 ? parts[2] : "?";
    if (p.fn_commands) {
      std::string cmds = p.fn_commands();
      if (!cmds.empty()) p.commands = splitStr(cmds, ',');
    }
    NovaEvent ev = {NOVA_EVENT_INIT, 0, nullptr, nullptr, 0, 0, nullptr};
    NovaBuffer buf = {nullptr, 0, 0, 0, nullptr, 0};
    NovaResponse resp = p.fn_on_event(&ev, &buf);
    p.fn_free_resp(&resp);
    g_plugins.push_back(p);
    std::cerr << "plugin: loaded " << p.name << " v" << p.version << "\n";
  }
}

void pluginManagerShutdown() {
  for (auto& p : g_plugins) {
    NovaEvent ev = {NOVA_EVENT_SHUTDOWN, 0, nullptr, nullptr, 0, 0, nullptr};
    NovaBuffer buf = {nullptr, 0, 0, 0, nullptr, 0};
    NovaResponse resp = p.fn_on_event(&ev, &buf);
    p.fn_free_resp(&resp);
    DLCLOSE(p.handle);
  }
  g_plugins.clear();
}

bool pluginManagerHasCommand(const std::string& cmd) {
  for (auto& p : g_plugins)
    for (auto& c : p.commands)
      if (c == cmd) return true;
  return false;
}

bool pluginManagerHasPendingUI() { return g_pendingUI.active; }

PendingUI& pluginManagerGetPendingUI() { return g_pendingUI; }

void pluginManagerClearPendingUI() {
  g_pendingUI.active = false;
  g_pendingUI.ownerPlugin = nullptr;
  g_pendingUI.items.clear();
}

// ── ncurses color pair registration ──────────────────────────────────────────
// We use color pairs starting at 20 to avoid conflicts with the editor
static int g_nextColorPair = 20;
static std::map<std::pair<int, int>, int> g_colorPairCache;

static int getColorPair(int fg, int bg) {
  auto key = std::make_pair(fg, bg);
  auto it = g_colorPairCache.find(key);
  if (it != g_colorPairCache.end()) return it->second;
  int pair = g_nextColorPair++;
  init_pair(pair, fg == NOVA_COLOR_NONE ? -1 : fg,
            bg == NOVA_COLOR_NONE ? -1 : bg);
  g_colorPairCache[key] = pair;
  return pair;
}

static int styleToAttr(const NovaStyle& s) {
  int pair = getColorPair(s.fg, s.bg);
  int attr = COLOR_PAIR(pair);
  if (s.bold) attr |= A_BOLD;
  if (s.underline) attr |= A_UNDERLINE;
  return attr;
}

// ── Apply response to editor state ───────────────────────────────────────────
static std::string applyResponse(NovaResponse& resp,
                                 std::vector<std::string>& lines, int& curRow,
                                 int& curCol, bool& dirty,
                                 std::vector<NovaHighlight>& highlights,
                                 std::string& inlineText, LoadedPlugin* owner) {
  std::string statusMsg;
  for (int i = 0; i < resp.actionCount; i++) {
    auto& a = resp.actions[i];
    switch (a.type) {
      case NOVA_ACTION_SET_LINE:
        if (a.row >= 0 && a.row < (int)lines.size()) {
          lines[a.row] = a.text ? a.text : "";
          dirty = true;
        }
        break;
      case NOVA_ACTION_INSERT_LINE:
        if (a.row >= 0 && a.row <= (int)lines.size()) {
          lines.insert(lines.begin() + a.row, a.text ? a.text : "");
          dirty = true;
        }
        break;
      case NOVA_ACTION_DELETE_LINE:
        if (a.row >= 0 && a.row < (int)lines.size()) {
          lines.erase(lines.begin() + a.row);
          if (lines.empty()) lines.push_back("");
          dirty = true;
        }
        break;
      case NOVA_ACTION_SET_CURSOR:
        curRow = std::max(0, std::min(a.row, (int)lines.size() - 1));
        curCol = std::max(0, a.col);
        break;
      case NOVA_ACTION_STATUS_MSG:
        if (a.text) statusMsg = a.text;
        break;
      case NOVA_ACTION_SET_DIRTY:
        dirty = true;
        break;
      case NOVA_ACTION_ADD_HIGHLIGHT:
        if (a.highlights) {
          for (int h = 0; h < a.highlightCount; h++)
            highlights.push_back(a.highlights[h]);
        }
        break;
      case NOVA_ACTION_CLEAR_HIGHLIGHTS:
        highlights.clear();
        break;
      case NOVA_ACTION_SET_INLINE_TEXT:
        inlineText = a.text ? a.text : "";
        break;
      case NOVA_ACTION_CLEAR_INLINE_TEXT:
        inlineText = "";
        break;
      case NOVA_ACTION_SHOW_POPUP:
        if (a.popup) {
          g_pendingUI.active = true;
          g_pendingUI.type = NOVA_UI_POPUP;
          g_pendingUI.ownerPlugin = owner;
          g_pendingUI.popup = *a.popup;
          // COPIE o título para string própria:
          g_pendingUI.popupTitle = a.popup->title ? a.popup->title : "";
          g_pendingUI.popup.title = g_pendingUI.popupTitle.c_str();
          g_pendingUI.items.clear();
          for (int j = 0; j < a.popup->itemCount; j++)
            g_pendingUI.items.push_back(a.popup->items[j] ? a.popup->items[j]
                                                          : "");
          g_pendingUI.selectedIndex = 0;
        }
        break;
      case NOVA_ACTION_SHOW_INPUT:
        if (a.input) {
          g_pendingUI.active = true;
          g_pendingUI.type = NOVA_UI_INPUT;
          g_pendingUI.ownerPlugin = owner;
          g_pendingUI.inputDialog = *a.input;
          g_pendingUI.inputText =
              a.input->defaultValue ? a.input->defaultValue : "";
          g_pendingUI.selectedIndex = 0;
        }
        break;
      case NOVA_ACTION_SHOW_CONFIRM:
        if (a.confirm) {
          g_pendingUI.active = true;
          g_pendingUI.type = NOVA_UI_CONFIRM;
          g_pendingUI.ownerPlugin = owner;
          g_pendingUI.confirmDialog = *a.confirm;
          // COPIE título e mensagem:
          g_pendingUI.confirmTitle = a.confirm->title ? a.confirm->title : "";
          g_pendingUI.confirmMessage =
              a.confirm->message ? a.confirm->message : "";
          g_pendingUI.confirmDialog.title = g_pendingUI.confirmTitle.c_str();
          g_pendingUI.confirmDialog.message =
              g_pendingUI.confirmMessage.c_str();
          g_pendingUI.selectedIndex = 0;
        }
        break;
      case NOVA_ACTION_SHOW_SPLIT:
        if (a.split) {
          g_pendingUI.active = true;
          g_pendingUI.type = NOVA_UI_SPLIT;
          g_pendingUI.ownerPlugin = owner;
          g_pendingUI.splitView = *a.split;
          // COPIE título:
          g_pendingUI.splitTitle = a.split->title ? a.split->title : "";
          g_pendingUI.splitView.title = g_pendingUI.splitTitle.c_str();
          // COPIE linhas e null out o ponteiro original:
          g_pendingUI.splitLines.clear();
          for (int j = 0; j < a.split->lineCount; j++)
            g_pendingUI.splitLines.push_back(
                a.split->lines[j] ? a.split->lines[j] : "");
          g_pendingUI.splitView.lines = nullptr;
          g_pendingUI.splitView.lineCount = (int)g_pendingUI.splitLines.size();
        }
        break;
      case NOVA_ACTION_CLOSE_SPLIT:
        if (g_pendingUI.type == NOVA_UI_SPLIT) pluginManagerClearPendingUI();
        break;
      case NOVA_ACTION_SHOW_NOTIFY:
        if (a.text) statusMsg = a.text;
        break;
      default:
        break;
    }
  }
  return statusMsg;
}

std::string pluginManagerFireEvent(NovaEvent* event, NovaBuffer* buffer,
                                   std::vector<std::string>& lines, int& curRow,
                                   int& curCol, bool& dirty,
                                   std::vector<NovaHighlight>& highlights,
                                   std::string& inlineText) {
  std::string lastMsg;
  for (auto& p : g_plugins) {
    if (event->type == NOVA_EVENT_COMMAND) {
      bool handles = false;
      for (auto& c : p.commands)
        if (c == event->command) {
          handles = true;
          break;
        }
      if (!handles) continue;
    }
    if (event->type == NOVA_EVENT_UI_RESULT) {
      g_pendingUI.active = false;
      g_pendingUI.type = NOVA_UI_NONE;
      if (&p != g_pendingUI.ownerPlugin) continue;
    }
    NovaResponse resp = p.fn_on_event(event, buffer);
    std::string msg = applyResponse(resp, lines, curRow, curCol, dirty,
                                    highlights, inlineText, &p);
    if (!msg.empty()) lastMsg = msg;
    p.fn_free_resp(&resp);
    // ADICIONE: UI_RESULT só vai para um plugin
    if (event->type == NOVA_EVENT_UI_RESULT) break;
  }
  return lastMsg;
}
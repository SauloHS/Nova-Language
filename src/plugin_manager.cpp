#include "plugin_manager.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <future>
#include <iostream>
#include <memory>
#include <optional>
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
static std::map<LoadedPlugin*, PluginRuntimeState> g_pluginStates;

struct AsyncPluginCall {
  NovaEvent event = {};
  NovaBuffer buffer = {};
  bool hasFilename = false;
  bool hasCommand = false;
  bool hasCommandArgs = false;
  bool hasUiSelectedText = false;
  std::string filename;
  std::string command;
  std::string commandArgs;
  std::string uiSelectedText;
  std::vector<std::string> lines;
  std::vector<char*> rawLines;

  AsyncPluginCall(NovaEvent* srcEvent, NovaBuffer* srcBuffer) {
    if (srcBuffer) {
      hasFilename = srcBuffer->filename != nullptr;
      if (hasFilename) filename = srcBuffer->filename;
      lines.reserve(srcBuffer->lineCount);
      rawLines.reserve(srcBuffer->lineCount);
      for (int i = 0; i < srcBuffer->lineCount; i++) {
        lines.push_back(srcBuffer->lines && srcBuffer->lines[i]
                            ? srcBuffer->lines[i]
                            : "");
      }
      for (auto& line : lines) rawLines.push_back(line.data());
      buffer.lines = rawLines.empty() ? nullptr : rawLines.data();
      buffer.lineCount = (int)lines.size();
      buffer.curRow = srcBuffer->curRow;
      buffer.curCol = srcBuffer->curCol;
      buffer.filename = hasFilename ? filename.c_str() : nullptr;
      buffer.dirty = srcBuffer->dirty;
    }

    if (srcEvent) {
      hasCommand = srcEvent->command != nullptr;
      hasCommandArgs = srcEvent->commandArgs != nullptr;
      hasUiSelectedText = srcEvent->uiSelectedText != nullptr;
      if (hasCommand) command = srcEvent->command;
      if (hasCommandArgs) commandArgs = srcEvent->commandArgs;
      if (hasUiSelectedText) uiSelectedText = srcEvent->uiSelectedText;
      event.type = srcEvent->type;
      event.key = srcEvent->key;
      event.command = hasCommand ? command.c_str() : nullptr;
      event.commandArgs = hasCommandArgs ? commandArgs.c_str() : nullptr;
      event.uiResultType = srcEvent->uiResultType;
      event.uiSelectedIndex = srcEvent->uiSelectedIndex;
      event.uiSelectedText =
          hasUiSelectedText ? uiSelectedText.c_str() : nullptr;
    }
  }
};

struct AsyncPluginTask {
  LoadedPlugin* plugin = nullptr;
  int bufferRevision = 0;
  std::shared_ptr<AsyncPluginCall> call;
  std::future<NovaResponse> future;
};

static std::vector<AsyncPluginTask> g_asyncTasks;

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

static bool pluginHandlesCommand(const LoadedPlugin& plugin,
                                 const char* command) {
  if (!command) return false;
  for (auto& c : plugin.commands)
    if (c == command) return true;
  return false;
}

static bool eventMatchesPlugin(const LoadedPlugin& plugin,
                               const NovaEvent& event) {
  if (event.type == NOVA_EVENT_COMMAND)
    return pluginHandlesCommand(plugin, event.command);
  return true;
}

static bool shouldDispatchAsync(const NovaEvent& event) {
  return event.type == NOVA_EVENT_COMMAND || event.type == NOVA_EVENT_UI_RESULT ||
         event.type == NOVA_EVENT_TICK;
}

void pluginManagerInit(const std::string& pluginDir) {
  auto files = listPluginFiles(pluginDir);
  g_plugins.reserve(files.size());
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
    p.execMutex = std::make_shared<std::mutex>();

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
    g_pluginStates[&g_plugins.back()] = PluginRuntimeState{};
    std::cerr << "plugin: loaded " << p.name << " v" << p.version << "\n";
  }
}

void pluginManagerShutdown() {
  for (auto& task : g_asyncTasks) {
    NovaResponse resp = task.future.get();
    if (task.plugin && task.plugin->fn_free_resp) task.plugin->fn_free_resp(&resp);
  }
  g_asyncTasks.clear();

  for (auto& p : g_plugins) {
    NovaEvent ev = {NOVA_EVENT_SHUTDOWN, 0, nullptr, nullptr, 0, 0, nullptr};
    NovaBuffer buf = {nullptr, 0, 0, 0, nullptr, 0};
    NovaResponse resp = p.fn_on_event(&ev, &buf);
    p.fn_free_resp(&resp);
    DLCLOSE(p.handle);
  }
  g_plugins.clear();
  g_pluginStates.clear();
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
  g_pendingUI.type = NOVA_UI_NONE;
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

static bool actionMutatesBuffer(const NovaAction& action) {
  switch (action.type) {
    case NOVA_ACTION_SET_LINE:
    case NOVA_ACTION_INSERT_LINE:
    case NOVA_ACTION_DELETE_LINE:
    case NOVA_ACTION_SET_CURSOR:
    case NOVA_ACTION_SET_DIRTY:
      return true;
    default:
      return false;
  }
}

// ── Apply response to editor state ───────────────────────────────────────────
static std::string applyResponse(NovaResponse& resp,
                                 std::vector<std::string>& lines, int& curRow,
                                 int& curCol, bool& dirty,
                                 std::vector<NovaHighlight>& highlights,
                                 std::string& inlineText, int& bufferRevision,
                                 bool allowBufferMutations,
                                 LoadedPlugin* owner) {
  std::string statusMsg;
  bool skippedMutation = false;
  auto markBufferChanged = [&]() {
    dirty = true;
    bufferRevision++;
  };

  for (int i = 0; i < resp.actionCount; i++) {
    auto& a = resp.actions[i];
    if (!allowBufferMutations && actionMutatesBuffer(a)) {
      skippedMutation = true;
      continue;
    }
    switch (a.type) {
      case NOVA_ACTION_SET_LINE:
        if (a.row >= 0 && a.row < (int)lines.size()) {
          lines[a.row] = a.text ? a.text : "";
          markBufferChanged();
        }
        break;
      case NOVA_ACTION_INSERT_LINE:
        if (a.row >= 0 && a.row <= (int)lines.size()) {
          lines.insert(lines.begin() + a.row, a.text ? a.text : "");
          markBufferChanged();
        }
        break;
      case NOVA_ACTION_DELETE_LINE:
        if (a.row >= 0 && a.row < (int)lines.size()) {
          lines.erase(lines.begin() + a.row);
          if (lines.empty()) lines.push_back("");
          markBufferChanged();
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
        markBufferChanged();
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
          // Default to modal unless plugin explicitly sets modal=0.
          // This avoids uninitialized garbage values accidentally enabling
          // non-modal behavior.
          g_pendingUI.splitView.modal = (g_pendingUI.splitView.modal == 0) ? 0 : 1;
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
  if (skippedMutation && statusMsg.empty() && owner) {
    statusMsg = owner->name + ": resultado ignorado, buffer mudou";
  }
  return statusMsg;
}

static std::string dispatchSyncEvent(LoadedPlugin& plugin, NovaEvent* event,
                                     NovaBuffer* buffer,
                                     std::vector<std::string>& lines,
                                     int& curRow, int& curCol, bool& dirty,
                                     std::vector<NovaHighlight>& highlights,
                                     std::string& inlineText,
                                     int& bufferRevision) {
  NovaResponse resp;
  {
    std::lock_guard<std::mutex> lock(*plugin.execMutex);
    resp = plugin.fn_on_event(event, buffer);
  }
  std::string msg =
      applyResponse(resp, lines, curRow, curCol, dirty, highlights, inlineText,
                    bufferRevision, true, &plugin);
  plugin.fn_free_resp(&resp);
  return msg;
}

static void startAsyncTask(LoadedPlugin& plugin,
                           std::shared_ptr<AsyncPluginCall> call,
                           int bufferRevision) {
  LoadedPlugin* pluginPtr = &plugin;
  auto execMutex = plugin.execMutex;
  g_pluginStates[pluginPtr].running = true;
  g_asyncTasks.push_back({pluginPtr, bufferRevision, call,
                          std::async(std::launch::async,
                                     [pluginPtr, execMutex, call]() {
                                       std::lock_guard<std::mutex> lock(
                                           *execMutex);
                                       return pluginPtr->fn_on_event(
                                           &call->event, &call->buffer);
                                     })});
}

std::string pluginManagerFireEvent(NovaEvent* event, NovaBuffer* buffer,
                                   std::vector<std::string>& lines, int& curRow,
                                   int& curCol, bool& dirty,
                                   std::vector<NovaHighlight>& highlights,
                                   std::string& inlineText,
                                   int& bufferRevision) {
  std::string lastMsg;

  if (event->type == NOVA_EVENT_UI_RESULT) {
    LoadedPlugin* target = g_pendingUI.ownerPlugin;
    pluginManagerClearPendingUI();
    if (target) {
      auto call = std::make_shared<AsyncPluginCall>(event, buffer);
      auto& state = g_pluginStates[target];
      if (state.running) {
        state.queuedEvent = QueuedPluginEvent{call, bufferRevision};
        state.droppedEvents++;
        lastMsg = "plugin: atualizando evento pendente de UI";
      } else {
        startAsyncTask(*target, call, bufferRevision);
        lastMsg = "plugin: processando resposta de UI";
      }
    }
    return lastMsg;
  }

  if (!shouldDispatchAsync(*event)) {
    for (auto& p : g_plugins) {
      if (!eventMatchesPlugin(p, *event)) continue;
      std::string msg =
          dispatchSyncEvent(p, event, buffer, lines, curRow, curCol, dirty,
                            highlights, inlineText, bufferRevision);
      if (!msg.empty()) lastMsg = msg;
    }
    return lastMsg;
  }

  for (auto& p : g_plugins) {
    if (!eventMatchesPlugin(p, *event)) continue;

    auto call = std::make_shared<AsyncPluginCall>(event, buffer);
    auto& state = g_pluginStates[&p];
    if (state.running) {
      state.queuedEvent = QueuedPluginEvent{call, bufferRevision};
      state.droppedEvents++;
      lastMsg = "plugin: " + p.name + " enfileirado (latest)";
    } else {
      startAsyncTask(p, call, bufferRevision);
      lastMsg = "plugin: executando " + p.name;
    }
  }

  return lastMsg;
}

std::string pluginManagerPoll(std::vector<std::string>& lines, int& curRow,
                              int& curCol, bool& dirty,
                              std::vector<NovaHighlight>& highlights,
                              std::string& inlineText, int& bufferRevision) {
  std::string lastMsg;

  for (size_t i = 0; i < g_asyncTasks.size();) {
    auto& task = g_asyncTasks[i];
    if (task.future.wait_for(std::chrono::seconds(0)) !=
        std::future_status::ready) {
      i++;
      continue;
    }

    NovaResponse resp = task.future.get();
    auto& state = g_pluginStates[task.plugin];
    state.running = false;
    bool allowBufferMutations = (task.bufferRevision == bufferRevision);
    std::string msg =
        applyResponse(resp, lines, curRow, curCol, dirty, highlights,
                      inlineText, bufferRevision, allowBufferMutations,
                      task.plugin);
    if (state.droppedEvents > 0 && msg.empty()) {
      msg = task.plugin->name + ": " + std::to_string(state.droppedEvents) +
            " evento(s) substituido(s)";
    }
    if (!msg.empty()) lastMsg = msg;
    if (task.plugin && task.plugin->fn_free_resp) task.plugin->fn_free_resp(&resp);
    if (state.queuedEvent.has_value()) {
      auto queued = *state.queuedEvent;
      state.queuedEvent.reset();
      state.droppedEvents = 0;
      startAsyncTask(*task.plugin, queued.call, queued.bufferRevision);
    } else {
      state.droppedEvents = 0;
    }
    g_asyncTasks.erase(g_asyncTasks.begin() + i);
  }

  return lastMsg;
}

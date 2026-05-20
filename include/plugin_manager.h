#pragma once
#include <memory>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "plugin.h"

#ifdef _WIN32
#include <ncurses/curses.h>
#else
#include <ncurses.h>
#endif

struct LoadedPlugin {
  void* handle;
  std::string name, version, author;
  std::vector<std::string> commands;
  const char* (*fn_info)();
  const char* (*fn_commands)();
  NovaResponse (*fn_on_event)(NovaEvent*, NovaBuffer*);
  void (*fn_free_resp)(NovaResponse*);
  std::shared_ptr<std::mutex> execMutex;
};

struct AsyncPluginCall;

struct QueuedPluginEvent {
  std::shared_ptr<AsyncPluginCall> call;
  int bufferRevision = 0;
};

struct PluginRuntimeState {
  bool running = false;
  std::optional<QueuedPluginEvent> queuedEvent;
  int droppedEvents = 0;
};

typedef enum {
  NOVA_UI_NONE = 0,
  NOVA_UI_POPUP = 1,
  NOVA_UI_INPUT = 2,
  NOVA_UI_CONFIRM = 3,
  NOVA_UI_SPLIT = 4,
} PendingUIType;

struct PendingUI {
  bool active = false;
  PendingUIType type = NOVA_UI_NONE;
  LoadedPlugin* ownerPlugin = nullptr;
  NovaPopupList popup = {};
  std::vector<std::string> items;
  int selectedIndex = 0;
  // ADICIONE ESTAS:
  std::string popupTitle;
  NovaInputDialog inputDialog = {};
  std::string inputText;
  NovaConfirmDialog confirmDialog = {};
  // ADICIONE ESTAS:
  std::string confirmTitle;
  std::string confirmMessage;
  NovaSplit splitView = {};
  std::vector<std::string> splitLines;
  // ADICIONE ESTA:
  std::string splitTitle;
};

extern PendingUI g_pendingUI;

void pluginManagerInit(const std::string& pluginDir);
void pluginManagerShutdown();
bool pluginManagerHasCommand(const std::string& cmd);
bool pluginManagerHasPendingUI();
PendingUI& pluginManagerGetPendingUI();
void pluginManagerClearPendingUI();

std::string pluginManagerFireEvent(NovaEvent* event, NovaBuffer* buffer,
                                   std::vector<std::string>& lines, int& curRow,
                                   int& curCol, bool& dirty,
                                   std::vector<NovaHighlight>& highlights,
                                   std::string& inlineText,
                                   int& bufferRevision);
std::string pluginManagerPoll(std::vector<std::string>& lines, int& curRow,
                              int& curCol, bool& dirty,
                              std::vector<NovaHighlight>& highlights,
                              std::string& inlineText, int& bufferRevision);

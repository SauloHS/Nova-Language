#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <chrono>

#include "include/plugin.h"

extern "C" {

const char* plugin_info() { return "async_test|1.0|chatgpt"; }

const char* plugin_commands() { return "async-test"; }

NovaResponse plugin_on_event(NovaEvent* event, NovaBuffer* buf) {
  (void)buf;
  NovaResponse resp = {nullptr, 0};

  if (event->type != NOVA_EVENT_COMMAND || !event->command) return resp;
  if (std::string(event->command) != "async-test") return resp;

  std::this_thread::sleep_for(std::chrono::seconds(3));

  NovaPopupList* popup = (NovaPopupList*)calloc(1, sizeof(NovaPopupList));
  static const char* items[] = {
      "Plugin terminou sem travar o editor",
      "Se voce conseguiu mover o cursor nesse meio tempo, funcionou",
      "OK",
  };

  popup->title = "Async test";
  popup->items = items;
  popup->itemStyles = nullptr;
  popup->itemCount = 3;
  popup->width = 60;
  popup->height = 8;
  popup->row = -1;
  popup->col = -1;
  popup->borderStyle = {NOVA_COLOR_BRIGHT_CYAN, NOVA_COLOR_NONE, 1, 0};
  popup->selectedStyle = {NOVA_COLOR_BLACK, NOVA_COLOR_BRIGHT_WHITE, 1, 0};

  resp.actions = (NovaAction*)calloc(1, sizeof(NovaAction));
  resp.actionCount = 1;
  resp.actions[0].type = NOVA_ACTION_SHOW_POPUP;
  resp.actions[0].popup = popup;
  return resp;
}

void nova_free_response(NovaResponse* resp) {
  if (!resp || !resp->actions) return;
  for (int i = 0; i < resp->actionCount; i++) {
    if (resp->actions[i].popup) free(resp->actions[i].popup);
  }
  free(resp->actions);
}

}  // extern "C"

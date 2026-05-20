#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>

#include "include/plugin.h"

extern "C" {
const char* plugin_info() { return "uppercase|1.0|saulo"; }
const char* plugin_commands() { return "up"; }
NovaResponse plugin_on_event(NovaEvent* event, NovaBuffer* buf) {
  NovaResponse resp = {nullptr, 0};
  if (event->type != NOVA_EVENT_COMMAND) return resp;
  if (!event->command) return resp;
  if (std::string(event->command) != "up") return resp;
  int totalActions = buf->lineCount + 1;
  resp.actions = (NovaAction*)calloc(totalActions, sizeof(NovaAction));

  resp.actionCount = totalActions;
  for (int i = 0; i < buf->lineCount; i++) {
    std::string line = buf->lines[i];
    std::transform(line.begin(), line.end(), line.begin(), ::toupper);
    resp.actions[i].type = NOVA_ACTION_SET_LINE;
    resp.actions[i].row = i;
    resp.actions[i].col = 0;
    resp.actions[i].text = strdup(line.c_str());
  }
  resp.actions[totalActions - 1].type = NOVA_ACTION_STATUS_MSG;
  resp.actions[totalActions - 1].text = strdup("Converted to uppercase!");
  return resp;
}
void nova_free_response(NovaResponse* resp) {
  if (!resp || !resp->actions) return;
  for (int i = 0; i < resp->actionCount; i++) {
    if (resp->actions[i].text) free(resp->actions[i].text);
  }
  free(resp->actions);
}
}
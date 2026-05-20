#include <cstdlib>
#include <cstring>

#include "include/plugin.h"
#include <string>

extern "C" {

const char* plugin_info() { return "hello|1.0|you"; }
const char* plugin_commands() { return "hello"; }

NovaResponse plugin_on_event(NovaEvent* event, NovaBuffer* buf) {
  NovaResponse resp = {nullptr, 0};
  if (event->type == NOVA_EVENT_COMMAND && event->command &&
      std::string(event->command) == "hello") {
    resp.actions = (NovaAction*)malloc(sizeof(NovaAction));
    resp.actionCount = 1;
    resp.actions[0].type = NOVA_ACTION_STATUS_MSG;
    resp.actions[0].row = 0;
    resp.actions[0].col = 0;
    resp.actions[0].text = (char*)"Hello from plugin!";
  }
  return resp;
}

void nova_free_response(NovaResponse* resp) {
  if (resp && resp->actions) free(resp->actions);
}
}
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <chrono>

#include "include/plugin.h"

// This plugin is intentionally "blocking": it sleeps inside plugin_on_event.
// With the async runtime, the editor should remain responsive.

static int g_seq = 0;

static NovaAction makeStatus(const std::string& msg) {
  NovaAction a = {};
  a.type = NOVA_ACTION_STATUS_MSG;
  a.text = strdup(msg.c_str());
  return a;
}

extern "C" {

const char* plugin_info() { return "async_job|1.0|chatgpt"; }

const char* plugin_commands() { return "async-ping,async-mutate"; }

NovaResponse plugin_on_event(NovaEvent* event, NovaBuffer* buf) {
  (void)buf;
  NovaResponse resp = {nullptr, 0};

  if (!event) return resp;
  if (event->type != NOVA_EVENT_COMMAND) return resp;
  if (!event->command) return resp;

  const std::string cmd = event->command;

  if (cmd == "async-ping") {
    int mySeq = ++g_seq;
    std::this_thread::sleep_for(std::chrono::seconds(2));
    resp.actions = (NovaAction*)calloc(1, sizeof(NovaAction));
    resp.actionCount = 1;
    resp.actions[0] =
        makeStatus("async-ping done (seq=" + std::to_string(mySeq) + ")");
    return resp;
  }

  if (cmd == "async-mutate") {
    int mySeq = ++g_seq;
    std::this_thread::sleep_for(std::chrono::seconds(3));

    // This action mutates the buffer. If the user edits the file while the
    // plugin is running, Nova may discard this mutation.
    resp.actions = (NovaAction*)calloc(2, sizeof(NovaAction));
    resp.actionCount = 2;

    resp.actions[0].type = NOVA_ACTION_SET_LINE;
    resp.actions[0].row = 0;
    resp.actions[0].col = 0;
    resp.actions[0].text = strdup(("// async-mutate applied (seq=" +
                                   std::to_string(mySeq) + ")")
                                      .c_str());

    resp.actions[1] = makeStatus(
        "async-mutate finished (if you edited meanwhile, SET_LINE may be ignored)");
    return resp;
  }

  return resp;
}

void nova_free_response(NovaResponse* resp) {
  if (!resp || !resp->actions) return;
  for (int i = 0; i < resp->actionCount; i++) {
    if (resp->actions[i].text) free(resp->actions[i].text);
    if (resp->actions[i].highlights) free(resp->actions[i].highlights);
    if (resp->actions[i].popup) free(resp->actions[i].popup);
    if (resp->actions[i].confirm) free(resp->actions[i].confirm);
    if (resp->actions[i].input) free(resp->actions[i].input);
    if (resp->actions[i].split) free(resp->actions[i].split);
  }
  free(resp->actions);
}

}  // extern "C"

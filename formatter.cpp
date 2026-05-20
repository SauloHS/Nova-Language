#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include "include/plugin.h"

static std::vector<std::string> splitLines(const std::string& s) {
  std::vector<std::string> out;
  std::stringstream ss(s);
  std::string ln;
  while (std::getline(ss, ln)) out.push_back(ln);
  return out;
}

static std::string trimLeft(const std::string& s) {
  size_t i = s.find_first_not_of(" \t");
  return (i == std::string::npos) ? "" : s.substr(i);
}

static std::vector<std::string> formatBuffer(char** lines, int count) {
  std::vector<std::string> out;
  int indent = 0;
  for (int i = 0; i < count; i++) {
    std::string line = trimLeft(lines[i]);
    // Decrease indent before lines that close a block
    if (!line.empty() && (line[0] == '}' || line[0] == ']' || line[0] == ')'))
      if (indent > 0) indent--;
    // Apply indent
    out.push_back(std::string(indent * 4, ' ') + line);
    // Increase indent after lines that open a block
    if (!line.empty() && line.back() == '{') indent++;
  }
  return out;
}

extern "C" {

const char* plugin_info() { return "formatter|1.0|nova"; }
const char* plugin_commands() { return "format"; }

NovaResponse plugin_on_event(NovaEvent* event, NovaBuffer* buf) {
  NovaResponse resp = {nullptr, 0};

  if (event->type != NOVA_EVENT_COMMAND || !event->command ||
      std::string(event->command) != "format")
    return resp;

  if (!buf->lines || buf->lineCount == 0) return resp;

  auto formatted = formatBuffer(buf->lines, buf->lineCount);

  // One SET_LINE per line + one STATUS_MSG
  int total = (int)formatted.size() + 1;
  resp.actions = (NovaAction*)malloc(sizeof(NovaAction) * total);
  resp.actionCount = total;

  for (int i = 0; i < (int)formatted.size(); i++) {
    resp.actions[i].type = NOVA_ACTION_SET_LINE;
    resp.actions[i].row = i;
    resp.actions[i].col = 0;
    // Allocate the string so it survives until nova_free_response
    char* txt = (char*)malloc(formatted[i].size() + 1);
    memcpy(txt, formatted[i].c_str(), formatted[i].size() + 1);
    resp.actions[i].text = txt;
  }

  // Status message
  resp.actions[total - 1].type = NOVA_ACTION_STATUS_MSG;
  resp.actions[total - 1].row = 0;
  resp.actions[total - 1].col = 0;
  resp.actions[total - 1].text = (char*)"Formatted!";

  return resp;
}

void nova_free_response(NovaResponse* resp) {
  if (!resp || !resp->actions) return;
  for (int i = 0; i < resp->actionCount; i++) {
    if (resp->actions[i].type == NOVA_ACTION_SET_LINE && resp->actions[i].text)
      free(resp->actions[i].text);
  }
  free(resp->actions);
}
}
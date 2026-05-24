#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include "include/plugin.h"

// ── State
// ─────────────────────────────────────────────────────────────────────
static bool g_splitOpen = false;
static int g_wordCount = 0;
static int g_menuStep =
    0;  // 0 = nothing, 1 = waiting popup, 2 = waiting confirm

// ── Helpers
// ───────────────────────────────────────────────────────────────────
static int countWords(char** lines, int count) {
  int words = 0;
  for (int i = 0; i < count; i++) {
    std::string line = lines[i];
    bool inWord = false;
    for (char c : line) {
      if (c == ' ' || c == '\t')
        inWord = false;
      else if (!inWord) {
        inWord = true;
        words++;
      }
    }
  }
  return words;
}

static NovaAction makeStatus(const std::string& msg) {
  NovaAction a = {};
  a.type = NOVA_ACTION_STATUS_MSG;
  a.text = strdup(msg.c_str());
  return a;
}

// ── plugin_info
// ───────────────────────────────────────────────────────────────
extern "C" {

const char* plugin_info() { return "testplugin|1.0|nova"; }

const char* plugin_commands() {
  // Registers 4 commands:
  // :demo      -- opens main menu popup
  // :stats     -- shows file stats in a split view
  // :highlight -- highlights all lines with TODO in red
  // :ghost     -- shows ghost text after the cursor
  return "demo,stats,highlight,ghost";
}

NovaResponse plugin_on_event(NovaEvent* event, NovaBuffer* buf) {
  NovaResponse resp = {nullptr, 0};

  // ── :ghost ────────────────────────────────────────────────────────────────
  if (event->type == NOVA_EVENT_COMMAND && event->command &&
      std::string(event->command) == "ghost") {
    resp.actions = (NovaAction*)calloc(2, sizeof(NovaAction));
    resp.actionCount = 2;
    resp.actions[0].type = NOVA_ACTION_SET_INLINE_TEXT;
    resp.actions[0].text = strdup("  <-- your cursor is here");
    resp.actions[1] =
        makeStatus("Ghost text set! Press :ghost again to clear.");
    return resp;
  }

  // ── :highlight ────────────────────────────────────────────────────────────
  if (event->type == NOVA_EVENT_COMMAND && event->command &&
      std::string(event->command) == "highlight") {
    // Find all lines containing "TODO"
    std::vector<NovaHighlight> hits;
    for (int i = 0; i < buf->lineCount; i++) {
      std::string line = buf->lines[i];
      size_t pos = 0;
      while ((pos = line.find("TODO", pos)) != std::string::npos) {
        NovaHighlight h = {};
        h.row = i;
        h.colStart = (int)pos;
        h.colEnd = (int)pos + 4;
        h.style = {NOVA_COLOR_BRIGHT_RED, NOVA_COLOR_NONE, 1, 1};
        hits.push_back(h);
        pos++;
      }
    }

    if (hits.empty()) {
      resp.actions = (NovaAction*)calloc(1, sizeof(NovaAction));
      resp.actionCount = 1;
      resp.actions[0] = makeStatus("No TODO found in file.");
      return resp;
    }

    resp.actions = (NovaAction*)calloc(2, sizeof(NovaAction));
    resp.actionCount = 2;

    // CLEAR first, then ADD
    resp.actions[0].type = NOVA_ACTION_CLEAR_HIGHLIGHTS;
    resp.actions[0].text = nullptr;
    resp.actions[1].type = NOVA_ACTION_ADD_HIGHLIGHT;
    resp.actions[1].text = nullptr;
    resp.actions[1].highlights =
        (NovaHighlight*)malloc(sizeof(NovaHighlight) * hits.size());
    resp.actions[1].highlightCount = (int)hits.size();
    for (int i = 0; i < (int)hits.size(); i++)
      resp.actions[1].highlights[i] = hits[i];

    std::string msg = "Highlighted " + std::to_string(hits.size()) + " TODO(s)";
    // Add status as third action
    resp.actions = (NovaAction*)realloc(resp.actions, sizeof(NovaAction) * 3);
    resp.actionCount = 3;
    memset(&resp.actions[2], 0, sizeof(NovaAction));
    resp.actions[2] = makeStatus(msg);
    return resp;
  }

  // ── :stats ────────────────────────────────────────────────────────────────
  if (event->type == NOVA_EVENT_COMMAND && event->command &&
      std::string(event->command) == "stats") {
    g_wordCount = countWords(buf->lines, buf->lineCount);
    int chars = 0;
    for (int i = 0; i < buf->lineCount; i++)
      chars += (int)std::string(buf->lines[i]).size();

    // Build split lines
    std::vector<std::string> statLines = {
        "",
        "  File: " + std::string(buf->filename ? buf->filename : "unknown"),
        "",
        "  Lines:      " + std::to_string(buf->lineCount),
        "  Words:      " + std::to_string(g_wordCount),
        "  Characters: " + std::to_string(chars),
        "  Modified:   " + std::string(buf->dirty ? "yes" : "no"),
        "  Cursor:     Ln " + std::to_string(buf->curRow + 1) + ", Col " +
            std::to_string(buf->curCol + 1),
        "",
        "  Press :stats again to close.",
    };

    if (g_splitOpen) {
      // Close split
      resp.actions = (NovaAction*)calloc(1, sizeof(NovaAction));
      resp.actionCount = 1;
      resp.actions[0].type = NOVA_ACTION_CLOSE_SPLIT;
      resp.actions[0].text = nullptr;
      g_splitOpen = false;
      return resp;
    }

    // Build per-line styles
    static NovaStyle lineStyles[10] = {
        {NOVA_COLOR_NONE, NOVA_COLOR_NONE, 0, 0},
        {NOVA_COLOR_BRIGHT_CYAN, NOVA_COLOR_NONE, 1, 0},
        {NOVA_COLOR_NONE, NOVA_COLOR_NONE, 0, 0},
        {NOVA_COLOR_BRIGHT_WHITE, NOVA_COLOR_NONE, 1, 0},
        {NOVA_COLOR_BRIGHT_WHITE, NOVA_COLOR_NONE, 1, 0},
        {NOVA_COLOR_BRIGHT_WHITE, NOVA_COLOR_NONE, 1, 0},
        {NOVA_COLOR_YELLOW, NOVA_COLOR_NONE, 1, 0},
        {NOVA_COLOR_GREEN, NOVA_COLOR_NONE, 1, 0},
        {NOVA_COLOR_NONE, NOVA_COLOR_NONE, 0, 0},
        {NOVA_COLOR_BRIGHT_BLACK, NOVA_COLOR_NONE, 0, 0},
    };

    // Allocate and copy lines
    const char** rawLines =
        (const char**)malloc(sizeof(const char*) * statLines.size());
    for (int i = 0; i < (int)statLines.size(); i++)
      rawLines[i] = strdup(statLines[i].c_str());

    NovaSplit* split = (NovaSplit*)malloc(sizeof(NovaSplit));
    split->title = "File Stats";
    split->titleStyle = {NOVA_COLOR_BRIGHT_CYAN, NOVA_COLOR_NONE, 1, 0};
    split->lines = rawLines;
    split->lineStyles = lineStyles;
    split->lineCount = (int)statLines.size();
    split->position = NOVA_SPLIT_RIGHT;
    split->size = 36;
    split->borderStyle = {NOVA_COLOR_BLUE, NOVA_COLOR_NONE, 1, 0};
    // Allow editing while the split is open
    split->modal = 0;

    resp.actions = (NovaAction*)calloc(1, sizeof(NovaAction));
    resp.actionCount = 1;
    resp.actions[0].type = NOVA_ACTION_SHOW_SPLIT;
    resp.actions[0].text = nullptr;
    resp.actions[0].split = split;
    g_splitOpen = true;
    return resp;
  }

  // ── :demo — main menu popup ───────────────────────────────────────────────
  if (event->type == NOVA_EVENT_COMMAND && event->command &&
      std::string(event->command) == "demo") {
    static const char* items[] = {"Show file stats (split view)",
                                  "Highlight all TODOs",
                                  "Set ghost text",
                                  "Ask me something (confirm dialog)",
                                  "Ask for input",
                                  "Cancel"};
    static NovaStyle itemStyles[] = {
        {NOVA_COLOR_BRIGHT_GREEN, NOVA_COLOR_NONE, 1, 0},
        {NOVA_COLOR_BRIGHT_RED, NOVA_COLOR_NONE, 1, 0},
        {NOVA_COLOR_BRIGHT_CYAN, NOVA_COLOR_NONE, 1, 0},
        {NOVA_COLOR_BRIGHT_YELLOW, NOVA_COLOR_NONE, 1, 0},
        {NOVA_COLOR_BRIGHT_MAGENTA, NOVA_COLOR_NONE, 1, 0},
        {NOVA_COLOR_BRIGHT_BLACK, NOVA_COLOR_NONE, 0, 0},
    };

    NovaPopupList* pop = (NovaPopupList*)malloc(sizeof(NovaPopupList));
    pop->title = "Nova Test Plugin";
    pop->titleStyle = {NOVA_COLOR_BRIGHT_CYAN, NOVA_COLOR_NONE, 1, 0};
    pop->items = items;
    pop->itemStyles = itemStyles;
    pop->itemCount = 6;
    pop->width = 44;
    pop->height = 12;
    pop->row = -1;
    pop->col = -1;
    pop->borderStyle = {NOVA_COLOR_BLUE, NOVA_COLOR_NONE, 1, 0};
    pop->selectedStyle = {NOVA_COLOR_BLACK, NOVA_COLOR_WHITE, 1, 0};

    resp.actions = (NovaAction*)calloc(1, sizeof(NovaAction));
    resp.actionCount = 1;
    resp.actions[0].type = NOVA_ACTION_SHOW_POPUP;
    resp.actions[0].text = nullptr;
    resp.actions[0].popup = pop;
    g_menuStep = 1;
    return resp;
  }

  // ── UI results ────────────────────────────────────────────────────────────
  if (event->type == NOVA_EVENT_UI_RESULT) {
    // Popup result from :demo
    if (g_menuStep == 1 && event->uiResultType == NOVA_UI_POPUP_SELECT) {
      g_menuStep = 0;
      int idx = event->uiSelectedIndex;

      if (idx == 0) {
        // Fake a :stats command
        NovaEvent fakeEv = {NOVA_EVENT_COMMAND, 0, "stats", "", 0, 0, nullptr};
        return plugin_on_event(&fakeEv, buf);
      }
      if (idx == 1) {
        NovaEvent fakeEv = {
            NOVA_EVENT_COMMAND, 0, "highlight", "", 0, 0, nullptr};
        return plugin_on_event(&fakeEv, buf);
      }
      if (idx == 2) {
        NovaEvent fakeEv = {NOVA_EVENT_COMMAND, 0, "ghost", "", 0, 0, nullptr};
        return plugin_on_event(&fakeEv, buf);
      }
      if (idx == 3) {
        // Show confirm dialog
        NovaConfirmDialog* cd =
            (NovaConfirmDialog*)malloc(sizeof(NovaConfirmDialog));
        cd->title = "Question";
        cd->titleStyle = {NOVA_COLOR_BRIGHT_YELLOW, NOVA_COLOR_NONE, 1, 0};
        cd->message = "Do you like the Nova editor?";
        cd->messageStyle = {NOVA_COLOR_WHITE, NOVA_COLOR_NONE, 0, 0};
        cd->confirmLabel = "Yes!";
        cd->cancelLabel = "Not yet";
        cd->borderStyle = {NOVA_COLOR_BLUE, NOVA_COLOR_NONE, 1, 0};

        resp.actions = (NovaAction*)calloc(1, sizeof(NovaAction));
        resp.actionCount = 1;
        resp.actions[0].type = NOVA_ACTION_SHOW_CONFIRM;
        resp.actions[0].text = nullptr;
        resp.actions[0].confirm = cd;
        g_menuStep = 2;
        return resp;
      }
      if (idx == 4) {
        // Show input dialog
        NovaInputDialog* id = (NovaInputDialog*)malloc(sizeof(NovaInputDialog));
        id->title = "Enter your name";
        id->titleStyle = {NOVA_COLOR_BRIGHT_MAGENTA, NOVA_COLOR_NONE, 1, 0};
        id->placeholder = "Your name here...";
        id->defaultValue = "";
        id->width = 40;
        id->row = -1;
        id->col = -1;
        id->borderStyle = {NOVA_COLOR_MAGENTA, NOVA_COLOR_NONE, 1, 0};
        id->inputStyle = {NOVA_COLOR_WHITE, NOVA_COLOR_NONE, 0, 0};

        resp.actions = (NovaAction*)calloc(1, sizeof(NovaAction));
        resp.actionCount = 1;
        resp.actions[0].type = NOVA_ACTION_SHOW_INPUT;
        resp.actions[0].text = nullptr;
        resp.actions[0].input = id;
        return resp;
      }
      // idx == 5: cancel
      resp.actions = (NovaAction*)calloc(1, sizeof(NovaAction));
      resp.actionCount = 1;
      resp.actions[0] = makeStatus("Cancelled.");
      return resp;
    }

    // Confirm result
    if (g_menuStep == 2) {
      g_menuStep = 0;
      if (event->uiResultType == NOVA_UI_CONFIRM_YES) {
        resp.actions = (NovaAction*)calloc(1, sizeof(NovaAction));
        resp.actionCount = 1;
        resp.actions[0] = makeStatus("Great! Glad you like it.");
      } else {
        resp.actions = (NovaAction*)calloc(1, sizeof(NovaAction));
        resp.actionCount = 1;
        resp.actions[0] = makeStatus("Fair enough. Keep exploring!");
      }
      return resp;
    }

    // Input result
    if (event->uiResultType == NOVA_UI_INPUT_CONFIRM && event->uiSelectedText) {
      std::string name = event->uiSelectedText;
      resp.actions = (NovaAction*)calloc(1, sizeof(NovaAction));
      resp.actionCount = 1;
      resp.actions[0] = makeStatus("Hello, " + name + "! Welcome to Nova.");
      return resp;
    }
    if (event->uiResultType == NOVA_UI_INPUT_CANCEL) {
      resp.actions = (NovaAction*)calloc(1, sizeof(NovaAction));
      resp.actionCount = 1;
      resp.actions[0] = makeStatus("Input cancelled.");
      return resp;
    }
  }

  return resp;
}

void nova_free_response(NovaResponse* resp) {
  if (!resp || !resp->actions) return;
  for (int i = 0; i < resp->actionCount; i++) {
    if (resp->actions[i].text) free(resp->actions[i].text);
    if (resp->actions[i].popup) free(resp->actions[i].popup);
    if (resp->actions[i].confirm) free(resp->actions[i].confirm);
    if (resp->actions[i].input) free(resp->actions[i].input);
    if (resp->actions[i].highlights) free(resp->actions[i].highlights);
    if (resp->actions[i].split) {
      // free the rawLines strings
      NovaSplit* s = resp->actions[i].split;
      if (s->lines)
        for (int j = 0; j < s->lineCount; j++)
          if (s->lines[j]) free((void*)s->lines[j]);
      free((void*)s->lines);
      free(s);
    }
  }
  free(resp->actions);
}

}  // extern "C"

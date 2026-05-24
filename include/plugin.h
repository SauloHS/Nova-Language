#pragma once
#include <stddef.h>

#ifdef _WIN32
#define NOVA_PLUGIN_EXPORT __declspec(dllexport)
#else
#define NOVA_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

// ── Colors
// ────────────────────────────────────────────────────────────────────
typedef enum {
  NOVA_COLOR_NONE = -1,
  NOVA_COLOR_BLACK = 0,
  NOVA_COLOR_RED = 1,
  NOVA_COLOR_GREEN = 2,
  NOVA_COLOR_YELLOW = 3,
  NOVA_COLOR_BLUE = 4,
  NOVA_COLOR_MAGENTA = 5,
  NOVA_COLOR_CYAN = 6,
  NOVA_COLOR_WHITE = 7,
  NOVA_COLOR_BRIGHT_BLACK = 8,
  NOVA_COLOR_BRIGHT_RED = 9,
  NOVA_COLOR_BRIGHT_GREEN = 10,
  NOVA_COLOR_BRIGHT_YELLOW = 11,
  NOVA_COLOR_BRIGHT_BLUE = 12,
  NOVA_COLOR_BRIGHT_MAGENTA = 13,
  NOVA_COLOR_BRIGHT_CYAN = 14,
  NOVA_COLOR_BRIGHT_WHITE = 15,
} NovaColorName;

typedef struct {
  int fg;         // NovaColorName, NOVA_COLOR_NONE = terminal default
  int bg;         // NovaColorName, NOVA_COLOR_NONE = transparent
  int bold;       // 1 = bold, 0 = normal
  int underline;  // 1 = underline, 0 = normal
} NovaStyle;

// ── Buffer
// ────────────────────────────────────────────────────────────────────
typedef struct {
  char** lines;
  int lineCount;
  int curRow;
  int curCol;
  const char* filename;
  int dirty;
} NovaBuffer;

// ── UI structs
// ────────────────────────────────────────────────────────────────

typedef struct {
  const char* title;
  NovaStyle titleStyle;
  const char** items;
  NovaStyle* itemStyles;  // per-item style, NULL = use default
  int itemCount;
  int width;   // 0 = auto
  int height;  // 0 = auto
  int row;     // -1 = centered
  int col;     // -1 = centered
  NovaStyle borderStyle;
  NovaStyle selectedStyle;
} NovaPopupList;

typedef struct {
  const char* title;
  NovaStyle titleStyle;
  const char* placeholder;
  const char* defaultValue;
  int width;  // 0 = auto
  int row;    // -1 = centered
  int col;    // -1 = centered
  NovaStyle borderStyle;
  NovaStyle inputStyle;
} NovaInputDialog;

typedef struct {
  const char* title;
  NovaStyle titleStyle;
  const char* message;
  NovaStyle messageStyle;
  const char* confirmLabel;  // NULL = "Yes"
  const char* cancelLabel;   // NULL = "No"
  NovaStyle borderStyle;
} NovaConfirmDialog;

typedef struct {
  const char* title;
  NovaStyle titleStyle;
  const char** lines;
  NovaStyle* lineStyles;  // per-line style, NULL = use default
  int lineCount;
  int position;  // NOVA_SPLIT_LEFT/RIGHT/BOTTOM
  int size;      // width or height in cells, 0 = auto (30%)
  NovaStyle borderStyle;
  // Input handling:
  // 1 = modal (default): while open, editor consumes keys and only the split
  //     close keys work.
  // 0 = non-modal: user can keep editing while split is visible.
  int modal;
} NovaSplit;

typedef struct {
  int row;
  int colStart;
  int colEnd;
  NovaStyle style;
} NovaHighlight;

// Split positions
#define NOVA_SPLIT_LEFT 0
#define NOVA_SPLIT_RIGHT 1
#define NOVA_SPLIT_BOTTOM 2

// ── Actions
// ───────────────────────────────────────────────────────────────────
typedef enum {
  NOVA_ACTION_NONE = 0,
  // Buffer
  NOVA_ACTION_SET_LINE = 1,
  NOVA_ACTION_INSERT_LINE = 2,
  NOVA_ACTION_DELETE_LINE = 3,
  NOVA_ACTION_SET_CURSOR = 4,
  NOVA_ACTION_STATUS_MSG = 5,
  NOVA_ACTION_SET_DIRTY = 6,
  // Highlights
  NOVA_ACTION_ADD_HIGHLIGHT = 7,
  NOVA_ACTION_CLEAR_HIGHLIGHTS = 8,
  // Inline text (ghost text)
  NOVA_ACTION_SET_INLINE_TEXT = 9,
  NOVA_ACTION_CLEAR_INLINE_TEXT = 10,
  // UI
  NOVA_ACTION_SHOW_POPUP = 11,
  NOVA_ACTION_SHOW_INPUT = 12,
  NOVA_ACTION_SHOW_CONFIRM = 13,
  NOVA_ACTION_SHOW_SPLIT = 14,
  NOVA_ACTION_CLOSE_SPLIT = 15,
  NOVA_ACTION_SHOW_NOTIFY = 16,
} NovaActionType;

typedef struct {
  NovaActionType type;
  int row;
  int col;
  char* text;
  NovaStyle style;
  // UI payloads — only one is used per action
  NovaPopupList* popup;
  NovaInputDialog* input;
  NovaConfirmDialog* confirm;
  NovaSplit* split;
  NovaHighlight* highlights;
  int highlightCount;
  int notifyDuration;  // seconds for SHOW_NOTIFY
} NovaAction;

typedef struct {
  NovaAction* actions;
  int actionCount;
} NovaResponse;

// ── Events
// ────────────────────────────────────────────────────────────────────
typedef enum {
  NOVA_EVENT_INIT = 0,
  NOVA_EVENT_SHUTDOWN = 1,
  NOVA_EVENT_SAVE = 2,
  NOVA_EVENT_KEY = 3,
  NOVA_EVENT_INSERT_CHAR = 4,
  NOVA_EVENT_CURSOR_MOVE = 5,
  NOVA_EVENT_COMMAND = 6,
  NOVA_EVENT_UI_RESULT = 7,  // result of a popup/input/confirm
  NOVA_EVENT_TICK = 8,       // periodic editor idle event
} NovaEventType;

// UI result types
#define NOVA_UI_POPUP_SELECT 0   // user selected an item
#define NOVA_UI_POPUP_CANCEL 1   // user pressed Esc
#define NOVA_UI_INPUT_CONFIRM 2  // user confirmed input
#define NOVA_UI_INPUT_CANCEL 3
#define NOVA_UI_CONFIRM_YES 4
#define NOVA_UI_CONFIRM_NO 5

typedef struct {
  NovaEventType type;
  int key;
  const char* command;
  const char* commandArgs;
  // UI result (for NOVA_EVENT_UI_RESULT)
  int uiResultType;            // NOVA_UI_*
  int uiSelectedIndex;         // for popup
  const char* uiSelectedText;  // for popup / input
} NovaEvent;

// ── Plugin exports
// ────────────────────────────────────────────────────────────
extern "C" {
NOVA_PLUGIN_EXPORT const char* plugin_info();
NOVA_PLUGIN_EXPORT const char* plugin_commands();
NOVA_PLUGIN_EXPORT NovaResponse plugin_on_event(NovaEvent* event,
                                                NovaBuffer* buffer);
NOVA_PLUGIN_EXPORT void nova_free_response(NovaResponse* response);
}

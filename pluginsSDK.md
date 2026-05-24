# Nova Editor — Plugin SDK

This document explains how to create plugins for the Nova editor.
Plugins are native shared libraries (`.so` on Linux, `.dll` on Windows)
that the editor loads at startup. They can react to events, modify
the buffer, add commands, create terminal UI elements, highlight text,
display split views, and interact with the user.

---

# Requirements

* A C or C++ compiler (`g++`, `clang++`, MinGW on Windows)
* The `plugin.h` header
* Basic knowledge of C or C++

---

# How plugins work

When the Nova editor starts, it scans the plugins directory and loads
every `.so` or `.dll` it finds.

For each library, Nova searches for four exported functions:

| Function             | Required | Description                        |
| -------------------- | -------- | ---------------------------------- |
| `plugin_info`        | Yes      | Returns plugin name/version/author |
| `plugin_commands`    | Yes      | Returns command list               |
| `plugin_on_event`    | Yes      | Main event handler                 |
| `nova_free_response` | Yes      | Frees plugin memory                |

The editor calls:

```cpp
plugin_on_event(NovaEvent* event, NovaBuffer* buf)
```

Your plugin returns a `NovaResponse` containing actions.

The editor then applies those actions.

Plugins never modify editor state directly.

`NOVA_EVENT_COMMAND`, `NOVA_EVENT_UI_RESULT`, and `NOVA_EVENT_TICK` run
asynchronously in the editor runtime. A plugin can block on HTTP, shell
commands, or other I/O in those events without freezing the editor UI.

Each plugin runs at most one async task at a time. If the same plugin receives
multiple async events while it is busy, Nova keeps only the latest pending
event (`latest-wins` policy).

If a delayed async response tries to modify the buffer after the user has
already edited it, Nova may discard the buffer-mutating actions from that
response to preserve editor state consistency.

---

# Plugin directory

Linux:

```text 
/usr/local/lib/nova/plugins/
```

Windows:

```text 
%USERPROFILE%\.local\lib\nova\plugins\
```

Or wherever:

```text 
$NOVA_STDLIB_PATH/plugins/
```

points to.

---

# Minimal plugin

```cpp 
#include "plugin.h"
#include <cstdlib>

extern "C" {

const char* plugin_info() {
    return "myplugin|1.0|yourname";
}

const char* plugin_commands() {
    return "";
}

NovaResponse plugin_on_event(NovaEvent* event, NovaBuffer* buf) {
    NovaResponse resp = {nullptr, 0};
    return resp;
}

void nova_free_response(NovaResponse* resp) {
    if (resp && resp->actions)
        free(resp->actions);
}

}
```

---

# Compiling

Linux:

```bash 
g++ -shared -fPIC -o myplugin.so myplugin.cpp
sudo cp myplugin.so /usr/local/lib/nova/plugins/
```

Windows:

```bash 
g++ -shared -o myplugin.dll myplugin.cpp
copy myplugin.dll %USERPROFILE%\.local\lib\nova\plugins\
```

---

# plugin_info

Must return:

```text 
"name|version|author"
```

Example:

```cpp 
const char* plugin_info() {
    return "formatter|1.0|alice";
}
```

---

# plugin_commands

Returns a comma-separated list of commands:

```cpp 
const char* plugin_commands() {
    return "format,lint,goto";
}
```

Commands become available as:

```text 
:format
:lint
:goto
```

No commands:

```cpp 
const char* plugin_commands() {
    return "";
}
```

---

# Events

Every interaction in Nova becomes an event.

Concurrency rules:

* `NOVA_EVENT_COMMAND`, `NOVA_EVENT_UI_RESULT`, and `NOVA_EVENT_TICK` are asynchronous
* `NOVA_EVENT_INIT`, `NOVA_EVENT_SHUTDOWN`, `NOVA_EVENT_SAVE`,
  `NOVA_EVENT_KEY`, `NOVA_EVENT_INSERT_CHAR`, and `NOVA_EVENT_CURSOR_MOVE` remain synchronous
* Plugins must keep their own global/static state thread-safe

---

# NovaEvent

```cpp 
typedef struct {
    NovaEventType type;

    int           key;

    const char*   command;
    const char*   commandArgs;

    int           uiResultType;
    int           uiSelectedIndex;
    const char*   uiSelectedText;

} NovaEvent;
```

---

# Event types

| Type                     | Description                    |
| ------------------------ | ------------------------------ |
| `NOVA_EVENT_INIT`        | Plugin loaded                  |
| `NOVA_EVENT_SHUTDOWN`    | Editor closing                 |
| `NOVA_EVENT_SAVE`        | File saved                     |
| `NOVA_EVENT_KEY`         | Key pressed                    |
| `NOVA_EVENT_INSERT_CHAR` | Insert-mode typing             |
| `NOVA_EVENT_CURSOR_MOVE` | Cursor moved                   |
| `NOVA_EVENT_COMMAND`     | User executed command          |
| `NOVA_EVENT_UI_RESULT`   | User interacted with plugin UI |
| `NOVA_EVENT_TICK`        | Periodic editor idle tick      |

---

# UI result events

`NOVA_EVENT_UI_RESULT` fires when the user:

* selects a popup item
* confirms/cancels a dialog
* submits input

---

# UI result types

| Type                    | Description              |
| ----------------------- | ------------------------ |
| `NOVA_UI_POPUP_SELECT`  | User selected popup item |
| `NOVA_UI_CONFIRM_YES`   | Confirmed dialog         |
| `NOVA_UI_CONFIRM_NO`    | Cancelled confirm dialog |
| `NOVA_UI_INPUT_CONFIRM` | Submitted input          |
| `NOVA_UI_INPUT_CANCEL`  | Cancelled input dialog   |

---

# Example: handling a command

```cpp 
if (event->type == NOVA_EVENT_COMMAND &&
    event->command &&
    std::string(event->command) == "hello")
{
    // handle command
}
```

For long-running command handlers, prefer returning UI/status/highlight actions
when the work completes. Do not assume the buffer is still unchanged by the
time your async response comes back.

---

# Example: handling UI results

```cpp 
if (event->type == NOVA_EVENT_UI_RESULT) {

    if (event->uiResultType == NOVA_UI_POPUP_SELECT) {

        int selected = event->uiSelectedIndex;

    }

    if (event->uiResultType == NOVA_UI_INPUT_CONFIRM) {

        std::string text = event->uiSelectedText;

    }
}
```

---

# The buffer

```cpp 
typedef struct {
    char**  lines;
    int     lineCount;

    int     curRow;
    int     curCol;

    const char* filename;
    int     dirty;

} NovaBuffer;
```

---

# Reading the buffer

```cpp 
for (int i = 0; i < buf->lineCount; i++) {
    std::string line = buf->lines[i];
}
```

---

# Actions

Plugins return actions to modify editor state.

---

# NovaAction

```cpp 
typedef struct {
    NovaActionType type;

    int row;
    int col;

    char* text;

    NovaPopupList* popup;
    NovaConfirmDialog* confirm;
    NovaInputDialog* input;
    NovaSplit* split;

    NovaHighlight* highlights;
    int highlightCount;

} NovaAction;
```

---

# Core action types

| Action                    | Description          |
| ------------------------- | -------------------- |
| `NOVA_ACTION_NONE`        | No-op                |
| `NOVA_ACTION_SET_LINE`    | Replace line         |
| `NOVA_ACTION_INSERT_LINE` | Insert line          |
| `NOVA_ACTION_DELETE_LINE` | Delete line          |
| `NOVA_ACTION_SET_CURSOR`  | Move cursor          |
| `NOVA_ACTION_STATUS_MSG`  | Status message       |
| `NOVA_ACTION_SET_DIRTY`   | Mark buffer modified |

---

# UI action types

| Action                        | Description            |
| ----------------------------- | ---------------------- |
| `NOVA_ACTION_SHOW_POPUP`      | Open popup menu        |
| `NOVA_ACTION_SHOW_CONFIRM`    | Show yes/no dialog     |
| `NOVA_ACTION_SHOW_INPUT`      | Show text input dialog |
| `NOVA_ACTION_SHOW_SPLIT`      | Open split panel       |
| `NOVA_ACTION_CLOSE_SPLIT`     | Close split panel      |
| `NOVA_ACTION_SET_INLINE_TEXT` | Show ghost text        |

---

# Highlight actions

| Action                         | Description       |
| ------------------------------ | ----------------- |
| `NOVA_ACTION_ADD_HIGHLIGHT`    | Add highlights    |
| `NOVA_ACTION_CLEAR_HIGHLIGHTS` | Remove highlights |

---

# Styling system

Nova UI components use `NovaStyle`.

---

# NovaStyle

```cpp 
typedef struct {
    NovaColor fg;
    NovaColor bg;

    int bold;
    int italic;

} NovaStyle;
```

---

# Example style

```cpp 
NovaStyle style = {
    NOVA_COLOR_BRIGHT_RED,
    NOVA_COLOR_NONE,
    1,
    0
};
```

---

# Popup menus

Popup menus allow interactive selection.

---

# NovaPopupList

```cpp 
typedef struct {
    const char* title;
    NovaStyle titleStyle;

    const char** items;
    NovaStyle* itemStyles;

    int itemCount;

    int width;
    int height;

    int row;
    int col;

    NovaStyle borderStyle;
    NovaStyle selectedStyle;

} NovaPopupList;
```

---

# Showing a popup

```cpp 
NovaPopupList* pop =
    (NovaPopupList*)malloc(sizeof(NovaPopupList));

pop->title = "Menu";

resp.actions[0].type = NOVA_ACTION_SHOW_POPUP;
resp.actions[0].popup = pop;
```

---

# Split views

Plugins can create side panels.

---

# NovaSplit

```cpp 
typedef struct {

    const char* title;
    NovaStyle titleStyle;

    const char** lines;
    NovaStyle* lineStyles;

    int lineCount;

    int position;
    int size;

    NovaStyle borderStyle;

    int modal; // 1 = modal (default), 0 = non-modal (editable)

} NovaSplit;
```

Set `modal = 0` for side panels that should remain visible while the user keeps
editing, such as diffs, diagnostics, git status, or issue details. Non-modal
splits are closed by the editor command `:close`; `Esc` is reserved for modal
splits and popups. When `:close` closes plugin UI, Nova sends a cancel UI result
to the owner plugin so it can stop periodic refreshes and avoid reopening the
split on the next tick.

---

# Split positions

| Constant            | Description |
| ------------------- | ----------- |
| `NOVA_SPLIT_LEFT`   | Left side   |
| `NOVA_SPLIT_RIGHT`  | Right side  |
| `NOVA_SPLIT_TOP`    | Top         |
| `NOVA_SPLIT_BOTTOM` | Bottom      |

---

# Opening a split

```cpp 
resp.actions[0].type = NOVA_ACTION_SHOW_SPLIT;
resp.actions[0].split = split;
```

---

# Closing a split

```cpp 
resp.actions[0].type = NOVA_ACTION_CLOSE_SPLIT;
```

Users can also close the current plugin UI with `:close`. Plugins that reopen
splits from `NOVA_EVENT_TICK` should treat cancel UI results as a signal to stop
refreshing.

---

# Confirm dialogs

---

# NovaConfirmDialog

```cpp 8
typedef struct {

    const char* title;
    NovaStyle titleStyle;

    const char* message;
    NovaStyle messageStyle;

    const char* confirmLabel;
    const char* cancelLabel;

    NovaStyle borderStyle;

} NovaConfirmDialog;
```

---

# Showing confirm dialog

```cpp 
resp.actions[0].type = NOVA_ACTION_SHOW_CONFIRM;
resp.actions[0].confirm = dialog;
```

---

# Input dialogs

---

# NovaInputDialog

```cpp 
typedef struct {

    const char* title;
    NovaStyle titleStyle;

    const char* placeholder;
    const char* defaultValue;

    int width;

    int row;
    int col;

    NovaStyle borderStyle;
    NovaStyle inputStyle;

} NovaInputDialog;
```

---

# Showing input dialog

```cpp 
resp.actions[0].type = NOVA_ACTION_SHOW_INPUT;
resp.actions[0].input = dialog;
```

---

# Inline ghost text

Ghost text appears after the cursor.

---

# Example

```cpp 
resp.actions[0].type = NOVA_ACTION_SET_INLINE_TEXT;
resp.actions[0].text = strdup(" suggested text");
```

---

# Highlights

Plugins can dynamically highlight text ranges.

---

# NovaHighlight

```cpp 
typedef struct {

    int row;

    int colStart;
    int colEnd;

    NovaStyle style;

} NovaHighlight;
```

---

# Adding highlights

```cpp 
resp.actions[0].type = NOVA_ACTION_ADD_HIGHLIGHT;
resp.actions[0].highlights = arr;
resp.actions[0].highlightCount = count;
```

---

# Clearing highlights

```cpp 
resp.actions[0].type = NOVA_ACTION_CLEAR_HIGHLIGHTS;
```

---

# Memory ownership

Anything allocated by the plugin must be freed in:

```cpp 
nova_free_response()
```

This includes:

* `text`
* popup structs
* input structs
* confirm structs
* split structs
* split lines
* highlight arrays

---

# Example free function

```cpp 
void nova_free_response(NovaResponse* resp) {

    if (!resp || !resp->actions)
        return;

    for (int i = 0; i < resp->actionCount; i++) {

        if (resp->actions[i].text)
            free(resp->actions[i].text);

        if (resp->actions[i].popup)
            free(resp->actions[i].popup);

        if (resp->actions[i].confirm)
            free(resp->actions[i].confirm);

        if (resp->actions[i].input)
            free(resp->actions[i].input);

        if (resp->actions[i].highlights)
            free(resp->actions[i].highlights);

        if (resp->actions[i].split) {

            NovaSplit* s = resp->actions[i].split;

            if (s->lines) {

                for (int j = 0; j < s->lineCount; j++)
                    if (s->lines[j])
                        free((void*)s->lines[j]);

                free((void*)s->lines);
            }

            free(s);
        }
    }

    free(resp->actions);
}
```

---

# Common mistakes

---

## Returning local string pointers

WRONG:

```cpp 
resp.actions[0].text = line.c_str();
```

CORRECT:

```cpp 
resp.actions[0].text = strdup(line.c_str());
```

---

## Modifying buffer directly

WRONG:

```cpp 
buf->lines[0] = (char*)"hello";
```

CORRECT:

```cpp 
resp.actions[0].type = NOVA_ACTION_SET_LINE;
resp.actions[0].text = strdup("hello");
```

---

## Forgetting extern "C"

Without:

```cpp 
extern "C"
```

the editor cannot find exported functions.

---

# Checklist

Before releasing:

* [ ] `plugin_info()` valid
* [ ] `plugin_commands()` valid
* [ ] all strings allocated
* [ ] everything freed
* [ ] no direct buffer modification
* [ ] all functions inside `extern "C"`
* [ ] compiled with `-shared`
* [ ] copied to plugins directory

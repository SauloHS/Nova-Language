#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <thread>
#include <chrono>
#include <vector>

#include "include/plugin.h"

enum WaitState {
  WAIT_NONE,
  WAIT_REPO_INPUT,
  WAIT_COMMIT_SCOPE,
  WAIT_COMMIT_MSG,
  WAIT_RELEASE_TAG,
  WAIT_RELEASE_CONFIRM,
  WAIT_ISSUE_SELECT,
};

static WaitState g_waitState = WAIT_NONE;
static std::string g_repoPath;
static std::string g_commitScope;
static std::string g_releaseTag;
static std::vector<int> g_issueNumbers;
static bool g_diffOpen = false;

static std::string trim(const std::string& input) {
  size_t start = input.find_first_not_of(" \t\r\n");
  if (start == std::string::npos) return "";
  size_t end = input.find_last_not_of(" \t\r\n");
  return input.substr(start, end - start + 1);
}

static std::string shellQuote(const std::string& input) {
  std::string out = "'";
  for (char c : input) {
    if (c == '\'')
      out += "'\\''";
    else
      out += c;
  }
  out += "'";
  return out;
}

static std::string dirnameOf(const std::string& path) {
  size_t pos = path.find_last_of("/\\");
  if (pos == std::string::npos) return ".";
  if (pos == 0) return "/";
  return path.substr(0, pos);
}

static std::string runCommand(const std::string& command, int* exitCode = nullptr) {
  std::string output;
  FILE* pipe = popen((command + " 2>&1").c_str(), "r");
  if (!pipe) {
    if (exitCode) *exitCode = -1;
    return "failed to run command";
  }

  char buffer[512];
  while (fgets(buffer, sizeof(buffer), pipe)) output += buffer;
  int rc = pclose(pipe);
  if (exitCode) *exitCode = rc;
  return output;
}

static std::string inRepoCommand(const std::string& repo,
                                 const std::string& command) {
  return "cd " + shellQuote(repo) + " && " + command;
}

static std::string detectRepo(NovaBuffer* buf) {
  if (!g_repoPath.empty()) return g_repoPath;
  std::string base = ".";
  if (buf && buf->filename && buf->filename[0]) base = dirnameOf(buf->filename);
  int rc = 0;
  std::string repo =
      trim(runCommand("git -C " + shellQuote(base) + " rev-parse --show-toplevel", &rc));
  if (rc == 0 && !repo.empty()) return repo;
  return base;
}

static std::string currentFile(NovaBuffer* buf) {
  if (buf && buf->filename) return buf->filename;
  return "";
}

static std::vector<std::string> splitLines(const std::string& text) {
  std::vector<std::string> lines;
  std::stringstream stream(text);
  std::string line;
  while (std::getline(stream, line)) lines.push_back(line);
  if (lines.empty()) lines.push_back("");
  return lines;
}

static NovaStyle styleForDiffLine(const std::string& line) {
  if (line.rfind("+", 0) == 0 && line.rfind("+++", 0) != 0)
    return {NOVA_COLOR_BRIGHT_GREEN, NOVA_COLOR_NONE, 1, 0};
  if (line.rfind("-", 0) == 0 && line.rfind("---", 0) != 0)
    return {NOVA_COLOR_BRIGHT_RED, NOVA_COLOR_NONE, 1, 0};
  if (line.rfind("@@", 0) == 0)
    return {NOVA_COLOR_BRIGHT_CYAN, NOVA_COLOR_NONE, 1, 0};
  if (line.rfind("diff ", 0) == 0 || line.rfind("index ", 0) == 0 ||
      line.rfind("---", 0) == 0 || line.rfind("+++", 0) == 0)
    return {NOVA_COLOR_YELLOW, NOVA_COLOR_NONE, 1, 0};
  return {NOVA_COLOR_NONE, NOVA_COLOR_NONE, 0, 0};
}

static NovaAction makeStatus(const std::string& message) {
  NovaAction action = {};
  action.type = NOVA_ACTION_STATUS_MSG;
  action.text = strdup(message.c_str());
  return action;
}

static NovaAction makeCloseSplit() {
  NovaAction action = {};
  action.type = NOVA_ACTION_CLOSE_SPLIT;
  return action;
}

static NovaAction makeInput(const std::string& title,
                            const std::string& defaultValue = "") {
  NovaInputDialog* input =
      (NovaInputDialog*)calloc(1, sizeof(NovaInputDialog));
  input->title = strdup(title.c_str());
  input->defaultValue = strdup(defaultValue.c_str());
  input->width = 64;
  input->row = -1;
  input->col = -1;
  input->borderStyle = {NOVA_COLOR_BLUE, NOVA_COLOR_NONE, 1, 0};
  input->inputStyle = {NOVA_COLOR_WHITE, NOVA_COLOR_NONE, 0, 0};

  NovaAction action = {};
  action.type = NOVA_ACTION_SHOW_INPUT;
  action.input = input;
  return action;
}

static NovaAction makeConfirm(const std::string& title,
                              const std::string& message) {
  NovaConfirmDialog* confirm =
      (NovaConfirmDialog*)calloc(1, sizeof(NovaConfirmDialog));
  confirm->title = strdup(title.c_str());
  confirm->message = strdup(message.c_str());
  confirm->confirmLabel = strdup("Yes");
  confirm->cancelLabel = strdup("No");
  confirm->borderStyle = {NOVA_COLOR_BLUE, NOVA_COLOR_NONE, 1, 0};

  NovaAction action = {};
  action.type = NOVA_ACTION_SHOW_CONFIRM;
  action.confirm = confirm;
  return action;
}

static NovaAction makePopup(const std::string& title,
                            const std::vector<std::string>& items) {
  NovaPopupList* popup = (NovaPopupList*)calloc(1, sizeof(NovaPopupList));
  popup->title = strdup(title.c_str());
  popup->itemCount = (int)items.size();
  popup->items = (const char**)calloc(items.size(), sizeof(char*));
  popup->width = 72;
  popup->height = std::min(18, std::max(6, (int)items.size() + 4));
  popup->row = -1;
  popup->col = -1;
  popup->borderStyle = {NOVA_COLOR_BLUE, NOVA_COLOR_NONE, 1, 0};
  popup->selectedStyle = {NOVA_COLOR_BLACK, NOVA_COLOR_BRIGHT_WHITE, 1, 0};

  for (size_t index = 0; index < items.size(); index++)
    popup->items[index] = strdup(items[index].c_str());

  NovaAction action = {};
  action.type = NOVA_ACTION_SHOW_POPUP;
  action.popup = popup;
  return action;
}

static NovaAction makeSplit(const std::string& title,
                            const std::vector<std::string>& lines,
                            bool modal = false) {
  NovaSplit* split = (NovaSplit*)calloc(1, sizeof(NovaSplit));
  split->title = strdup(title.c_str());
  split->titleStyle = {NOVA_COLOR_BRIGHT_CYAN, NOVA_COLOR_NONE, 1, 0};
  split->lineCount = (int)lines.size();
  split->lines = (const char**)calloc(lines.size(), sizeof(char*));
  split->lineStyles = (NovaStyle*)calloc(lines.size(), sizeof(NovaStyle));
  split->position = NOVA_SPLIT_RIGHT;
  split->size = 48; // Changed from 72 to 48 for a smaller split view
  split->borderStyle = {NOVA_COLOR_BLUE, NOVA_COLOR_NONE, 1, 0};
  split->modal = modal ? 1 : 0;

  for (size_t index = 0; index < lines.size(); index++) {
    split->lines[index] = strdup(lines[index].c_str());
    split->lineStyles[index] = styleForDiffLine(lines[index]);
  }

  NovaAction action = {};
  action.type = NOVA_ACTION_SHOW_SPLIT;
  action.split = split;
  return action;
}

static NovaResponse singleAction(NovaAction action) {
  NovaResponse response = {};
  response.actions = (NovaAction*)calloc(1, sizeof(NovaAction));
  response.actionCount = 1;
  response.actions[0] = action;
  return response;
}

static std::vector<std::string> buildDiffLines(NovaBuffer* buf) {
  std::string repo = detectRepo(buf);
  std::string file = currentFile(buf);
  std::vector<std::string> lines;

  int statusRc = 0;
  std::string branch =
      trim(runCommand("git -C " + shellQuote(repo) + " branch --show-current", &statusRc));
  std::string status =
      trim(runCommand("git -C " + shellQuote(repo) + " status --short " +
                          shellQuote(file),
                      nullptr));

  lines.push_back("Repo: " + repo);
  lines.push_back("Branch: " + (branch.empty() ? "(detached)" : branch));
  lines.push_back("File: " + (file.empty() ? "(unknown)" : file));
  lines.push_back(status.empty() ? "Status: clean since last commit"
                                 : "Status: " + status);
  lines.push_back("");

  int rc = 0;
  std::string diff =
      runCommand("git -C " + shellQuote(repo) + " diff -- " + shellQuote(file), &rc);
  if (rc != 0) {
    lines.push_back("git diff failed:");
    for (auto& line : splitLines(diff)) lines.push_back(line);
    return lines;
  }
  if (trim(diff).empty()) {
    lines.push_back("No diff for current file.");
    return lines;
  }
  for (auto& line : splitLines(diff)) lines.push_back(line);
  return lines;
}

static NovaResponse showDiff(NovaBuffer* buf) {
  return singleAction(makeSplit("Git Diff", buildDiffLines(buf), false));
}

static NovaResponse connectGithub() {
  int rc = 0;
  std::string status = runCommand("gh auth status", &rc);
  if (rc == 0) return singleAction(makeStatus("GitHub connected via gh CLI"));

  std::vector<std::string> lines = {
      "GitHub is not connected.",
      "",
      "This plugin uses the official GitHub CLI (`gh`).",
      "",
      "Recommended setup:",
      "  gh auth login",
      "",
      "Alternative non-interactive setup:",
      "  export GH_TOKEN=...",
      "",
      "Current gh output:",
  };
  for (auto& line : splitLines(status)) lines.push_back(line);
  return singleAction(makeSplit("GitHub Connect", lines, false));
}

static NovaResponse configureRepo(NovaBuffer* buf) {
  g_waitState = WAIT_REPO_INPUT;
  return singleAction(makeInput("Repo path", detectRepo(buf)));
}

static NovaResponse startCommit() {
  g_waitState = WAIT_COMMIT_SCOPE;
  return singleAction(makePopup("Commit Scope",
                                {"Current file", "All repo changes", "Cancel"}));
}

static NovaResponse startRelease() {
  g_waitState = WAIT_RELEASE_TAG;
  return singleAction(makeInput("Release tag", "v0.1.0"));
}

static NovaResponse listIssues(NovaBuffer* buf) {
  std::string repo = detectRepo(buf);
  int rc = 0;
  std::string output =
      runCommand(inRepoCommand(
                     repo,
                     "gh issue list --limit 30 --json number,title --jq "
                     "'.[] | \"\\(.number)\\t\\(.title)\"'"),
                 &rc);
  if (rc != 0) return singleAction(makeStatus("gh issue list failed: " + trim(output)));

  g_issueNumbers.clear();
  std::vector<std::string> items;
  for (auto& line : splitLines(output)) {
    size_t tab = line.find('\t');
    if (tab == std::string::npos) continue;
    int number = std::atoi(line.substr(0, tab).c_str());
    std::string title = line.substr(tab + 1);
    g_issueNumbers.push_back(number);
    items.push_back("#" + std::to_string(number) + " " + title);
  }

  if (items.empty()) return singleAction(makeStatus("No open GitHub issues."));
  g_waitState = WAIT_ISSUE_SELECT;
  return singleAction(makePopup("GitHub Issues", items));
}

static NovaResponse viewIssue(NovaBuffer* buf, int issueNumber) {
  std::string repo = detectRepo(buf);
  int rc = 0;
  std::string output =
      runCommand(inRepoCommand(
                     repo,
                     "gh issue view " + std::to_string(issueNumber) +
                         " --json number,title,state,author,body,url --jq "
                         "'\"#\\(.number) \\(.title)\\nState: \\(.state)\\nAuthor: "
                         "\\(.author.login)\\nURL: \\(.url)\\n\\n\\(.body)\"'"),
                 &rc);
  if (rc != 0) return singleAction(makeStatus("gh issue view failed: " + trim(output)));
  return singleAction(makeSplit("Issue #" + std::to_string(issueNumber),
                                splitLines(output), false));
}

static NovaResponse runCommit(NovaBuffer* buf, const std::string& message) {
  std::string repo = detectRepo(buf);
  std::string file = currentFile(buf);
  std::string addCommand = "git -C " + shellQuote(repo) + " add ";
  addCommand += (g_commitScope == "all") ? "-A" : "-- " + shellQuote(file);

  int addRc = 0;
  std::string addOutput = runCommand(addCommand, &addRc);
  if (addRc != 0) return singleAction(makeStatus("git add failed: " + trim(addOutput)));

  int commitRc = 0;
  std::string commitOutput =
      runCommand("git -C " + shellQuote(repo) + " commit -m " + shellQuote(message),
                 &commitRc);
  if (commitRc != 0)
    return singleAction(makeStatus("git commit failed: " + trim(commitOutput)));

  return singleAction(makeStatus("Commit created: " + message));
}

static NovaResponse runRelease(NovaBuffer* buf) {
  std::string repo = detectRepo(buf);
  int rc = 0;
  std::string output =
      runCommand(inRepoCommand(repo,
                               "git tag " + shellQuote(g_releaseTag) +
                                   " 2>/dev/null || true; git push origin " +
                                   shellQuote(g_releaseTag) +
                                   "; gh release create " +
                                   shellQuote(g_releaseTag) +
                                   " --generate-notes"),
                 &rc);
  if (rc != 0) return singleAction(makeStatus("release failed: " + trim(output)));
  return singleAction(makeStatus("Release created: " + g_releaseTag));
}

extern "C" {

const char* plugin_info() { return "github_workflow|1.0|nova"; }

const char* plugin_commands() {
  return "gh-connect,repo,diff,commit,release,issues,diffs";
}

NovaResponse plugin_on_event(NovaEvent* event, NovaBuffer* buf) {
  NovaResponse empty = {nullptr, 0};
  if (!event) return empty;

  if (event->type == NOVA_EVENT_TICK) {
    if (!g_diffOpen) return empty;
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    
    // When refreshing diff, also clear any existing highlights (from :diffs)
    NovaAction clearHighlights = {};
    clearHighlights.type = NOVA_ACTION_CLEAR_HIGHLIGHTS;
    NovaAction showSplit = makeSplit("Git Diff", buildDiffLines(buf), false);

    return createMultiActionResponse({clearHighlights, showSplit});
  }

  if (event->type == NOVA_EVENT_UI_RESULT) {
    if ((event->uiResultType == NOVA_UI_POPUP_CANCEL ||
         event->uiResultType == NOVA_UI_INPUT_CANCEL) &&
        g_waitState == WAIT_NONE) {
      g_diffOpen = false;
      return empty;
    }

    if (event->uiResultType == NOVA_UI_POPUP_CANCEL ||
        event->uiResultType == NOVA_UI_INPUT_CANCEL) {
      g_waitState = WAIT_NONE;
      g_diffOpen = false;
      return singleAction(makeStatus("Cancelled."));
    }

    if (g_waitState == WAIT_REPO_INPUT &&
        event->uiResultType == NOVA_UI_INPUT_CONFIRM && event->uiSelectedText) {
      g_repoPath = trim(event->uiSelectedText);
      g_waitState = WAIT_NONE;
      return singleAction(makeStatus("Repo configured: " + g_repoPath));
    }

    if (g_waitState == WAIT_COMMIT_SCOPE &&
        event->uiResultType == NOVA_UI_POPUP_SELECT) {
      if (event->uiSelectedIndex == 2) {
        g_waitState = WAIT_NONE;
        return singleAction(makeStatus("Commit cancelled."));
      }
      g_commitScope = (event->uiSelectedIndex == 1) ? "all" : "file";
      g_waitState = WAIT_COMMIT_MSG;
      return singleAction(makeInput("Commit message"));
    }

    if (g_waitState == WAIT_COMMIT_MSG &&
        event->uiResultType == NOVA_UI_INPUT_CONFIRM && event->uiSelectedText) {
      std::string message = trim(event->uiSelectedText);
      g_waitState = WAIT_NONE;
      if (message.empty()) return singleAction(makeStatus("Empty commit message."));
      return runCommit(buf, message);
    }

    if (g_waitState == WAIT_RELEASE_TAG &&
        event->uiResultType == NOVA_UI_INPUT_CONFIRM && event->uiSelectedText) {
      g_releaseTag = trim(event->uiSelectedText);
      g_waitState = WAIT_RELEASE_CONFIRM;
      return singleAction(makeConfirm(
          "Create Release",
          "Create/push tag " + g_releaseTag + " and publish GitHub release?"));
    }

    if (g_waitState == WAIT_RELEASE_CONFIRM) {
      g_waitState = WAIT_NONE;
      if (event->uiResultType != NOVA_UI_CONFIRM_YES)
        return singleAction(makeStatus("Release cancelled."));
      return runRelease(buf);
    }

    if (g_waitState == WAIT_ISSUE_SELECT &&
        event->uiResultType == NOVA_UI_POPUP_SELECT) {
      g_waitState = WAIT_NONE;
      int index = event->uiSelectedIndex;
      if (index < 0 || index >= (int)g_issueNumbers.size())
        return singleAction(makeStatus("Invalid issue selection."));
      return viewIssue(buf, g_issueNumbers[index]);
    }

    return empty;
  }

// Helper to create a multi-action response
static NovaResponse createMultiActionResponse(const std::vector<NovaAction>& actions) {
    NovaResponse resp = {nullptr, 0};
    resp.actionCount = (int)actions.size();
    resp.actions = (NovaAction*)calloc(actions.size(), sizeof(NovaAction));
    for (size_t i = 0; i < actions.size(); ++i) {
        resp.actions[i] = actions[i];
    }
    return resp;
}

  if (event->type != NOVA_EVENT_COMMAND || !event->command) return empty;
  std::string command = event->command;

  if (command == "gh-connect") return connectGithub();
  if (command == "repo") return configureRepo(buf);
  if (command == "diff") {
    g_diffOpen = !g_diffOpen;
    if (g_diffOpen) {
      // Opening split view: clear existing highlights and show split
      NovaAction clearHighlights = {};
      clearHighlights.type = NOVA_ACTION_CLEAR_HIGHLIGHTS;
      NovaAction showSplit = makeSplit("Git Diff", buildDiffLines(buf), false);
      
      return createMultiActionResponse({clearHighlights, showSplit});
    } else {
      // Closing split view: just close it
      return singleAction(makeCloseSplit());
    }
  }
  if (command == "diffs") {
    // If diff split is open, close it before showing highlights
    if (g_diffOpen) {
        g_diffOpen = false; // Set to false to prevent NOVA_EVENT_TICK from reopening
        NovaAction closeSplit = makeCloseSplit();
        NovaResponse highlightResp = highlightDiffs(buf); // This already clears highlights and adds new ones.
        
        // Combine closeSplit and highlightResp's actions
        std::vector<NovaAction> combinedActions;
        combinedActions.push_back(closeSplit);
        for (int k = 0; k < highlightResp.actionCount; ++k) {
            combinedActions.push_back(highlightResp.actions[k]);
        }
        // IMPORTANT: Free the original highlightResp.actions after copying
        free(highlightResp.actions); 
        return createMultiActionResponse(combinedActions);
    }
    // If split view not open, just highlight
    return highlightDiffs(buf);
  }
  if (command == "commit") return startCommit();
  if (command == "release") return startRelease();
  if (command == "issues") return listIssues(buf);

  return empty;
}

void nova_free_response(NovaResponse* response) {
  if (!response || !response->actions) return;
  for (int i = 0; i < response->actionCount; i++) {
    NovaAction& action = response->actions[i];
    if (action.text) free(action.text);
    if (action.popup) {
      if (action.popup->title) free((void*)action.popup->title);
      if (action.popup->items) {
        for (int item = 0; item < action.popup->itemCount; item++)
          if (action.popup->items[item]) free((void*)action.popup->items[item]);
        free((void*)action.popup->items);
      }
      free(action.popup);
    }
    if (action.input) {
      if (action.input->title) free((void*)action.input->title);
      if (action.input->defaultValue) free((void*)action.input->defaultValue);
      free(action.input);
    }
    if (action.confirm) {
      if (action.confirm->title) free((void*)action.confirm->title);
      if (action.confirm->message) free((void*)action.confirm->message);
      if (action.confirm->confirmLabel) free((void*)action.confirm->confirmLabel);
      if (action.confirm->cancelLabel) free((void*)action.confirm->cancelLabel);
      free(action.confirm);
    }
    if (action.split) {
      if (action.split->title) free((void*)action.split->title);
      if (action.split->lines) {
        for (int line = 0; line < action.split->lineCount; line++)
          if (action.split->lines[line]) free((void*)action.split->lines[line]);
        free((void*)action.split->lines);
      }
      if (action.split->lineStyles) free(action.split->lineStyles);
      free(action.split);
    }
    if (action.highlights) free(action.highlights);
  }
  free(response->actions);
}

}  // extern "C"

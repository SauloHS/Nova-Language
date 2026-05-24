#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "include/plugin.h"

static void logMsg(const std::string& msg) {
  std::ofstream f("/tmp/nova_plugin.log", std::ios::app);
  if (f) f << msg << "\n";
}

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
static bool g_diffHighlightsOn = false;
static bool g_diffLinesOpen = false;

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

// Writes buf->lines to a temp file and returns a unified diff against HEAD.
// This captures unsaved changes — `git diff` on disk would miss them.
// Returns the diff string (empty if no changes or on error).
// On error, if exitCode != nullptr it is set to non-zero.
static std::string diffFromBuffer(NovaBuffer* buf, const std::string& repo,
                                  int* exitCode = nullptr) {
  if (!buf || !buf->filename || !buf->filename[0]) {
    if (exitCode) *exitCode = -1;
    return "";
  }

  // Write buffer to a temp file
  char tmpPath[] = "/tmp/nova_diff_XXXXXX";
  int fd = mkstemp(tmpPath);
  if (fd < 0) {
    if (exitCode) *exitCode = -1;
    return "";
  }
  for (int i = 0; i < buf->lineCount; i++) {
    if (buf->lines[i]) {
      write(fd, buf->lines[i], strlen(buf->lines[i]));
    }
    write(fd, "\n", 1);
  }
  close(fd);

  // Relative path of the file inside the repo (for git show HEAD:<path>)
  std::string relPath = buf->filename;
  if (relPath.rfind(repo, 0) == 0 && relPath.size() > repo.size())
    relPath = relPath.substr(repo.size() + 1);  // strip leading "repo/"

  // diff: compare HEAD version with the temp file
  // --label makes the output look like a normal git diff
  std::string cmd =
      "git -C " + shellQuote(repo) + " show HEAD:" + shellQuote(relPath) +
      " 2>/dev/null | diff -u --label a/" + shellQuote(relPath) +
      " --label b/" + shellQuote(relPath) + " - " + shellQuote(tmpPath);

  int rc = 0;
  std::string result;
  FILE* pipe = popen(cmd.c_str(), "r");
  if (!pipe) {
    unlink(tmpPath);
    if (exitCode) *exitCode = -1;
    return "";
  }
  char buffer[512];
  while (fgets(buffer, sizeof(buffer), pipe)) result += buffer;
  rc = pclose(pipe);
  unlink(tmpPath);

  // diff exits 1 when there are differences (normal), 2 on error.
  // pclose() returns the raw waitpid status — use WEXITSTATUS to get the
  // actual exit code.
  int exitStatus = WIFEXITED(rc) ? WEXITSTATUS(rc) : 2;
  if (exitCode)
    *exitCode = (exitStatus == 0 || exitStatus == 1) ? 0 : exitStatus;
  return result;
}

static std::string runCommand(const std::string& command,
                              int* exitCode = nullptr) {
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
  std::string repo = trim(runCommand(
      "git -C " + shellQuote(base) + " rev-parse --show-toplevel", &rc));
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

static std::string g_inputTitle;
static std::string g_inputPlaceholder;
static std::string g_inputDefault;

static NovaAction makeInput(const std::string& title,
                            const std::string& defaultValue = "",
                            const std::string& placeholder = "") {
  g_inputTitle = title;
  g_inputPlaceholder = placeholder;
  g_inputDefault = defaultValue;

  NovaInputDialog* input = (NovaInputDialog*)calloc(1, sizeof(NovaInputDialog));
  input->title = g_inputTitle.c_str();
  input->placeholder = g_inputPlaceholder.c_str();
  input->defaultValue = g_inputDefault.c_str();
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
  split->size = 72;
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
  std::string branch = trim(runCommand(
      "git -C " + shellQuote(repo) + " branch --show-current", &statusRc));
  std::string status = trim(runCommand(
      "git -C " + shellQuote(repo) + " status --short " + shellQuote(file),
      nullptr));

  lines.push_back("Repo: " + repo);
  lines.push_back("Branch: " + (branch.empty() ? "(detached)" : branch));
  lines.push_back("File: " + (file.empty() ? "(unknown)" : file));
  lines.push_back(status.empty() ? "Status: clean since last commit"
                                 : "Status: " + status);
  lines.push_back("");

  int rc = 0;
  std::string diff = diffFromBuffer(buf, repo, &rc);
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

// ── Inline diff highlights
// ──────────────────────────────────────────────────── Parses `git diff`
// unified output and maps +/- lines back to buffer row positions so the editor
// highlights them in-place.
//
// Strategy:
//   - @@ -old,n +new,m @@ tells us the starting buffer row for new content.
//   - '+' lines   → bright green  (added/modified in working tree)
//   - '-' lines   → bright red    (removed since last commit; shown at the
//                                  nearest surrounding context row)
//   - context lines advance the new-side counter without generating highlights.
static std::vector<NovaHighlight> buildInlineHighlights(NovaBuffer* buf) {
  std::string repo = detectRepo(buf);
  std::string file = currentFile(buf);
  std::vector<NovaHighlight> highlights;

  int rc = 0;
  std::string diff = diffFromBuffer(buf, repo, &rc);
  if (rc != 0 || trim(diff).empty()) return highlights;

  int newRow = 0;   // 0-based row in the current buffer
  int lastRow = 0;  // last valid row we've seen (for anchoring '-' lines)

  for (auto& line : splitLines(diff)) {
    // Hunk header: @@ -old,n +new,m @@
    if (line.rfind("@@", 0) == 0) {
      // Parse the +new line number
      size_t plus = line.find('+');
      if (plus != std::string::npos) {
        int startLine = std::atoi(line.c_str() + plus + 1);
        if (startLine > 0) newRow = startLine - 1;  // convert to 0-based
      }
      // Highlight the hunk header row as cyan
      if (lastRow < buf->lineCount) {
        NovaHighlight h = {};
        h.row = lastRow;
        h.colStart = 0;
        h.colEnd = buf->lines[lastRow] ? (int)strlen(buf->lines[lastRow]) : 1;
        h.style = {NOVA_COLOR_BRIGHT_CYAN, NOVA_COLOR_NONE, 1, 0};
        highlights.push_back(h);
      }
      continue;
    }

    // Skip file header lines
    if (line.rfind("diff ", 0) == 0 || line.rfind("index ", 0) == 0 ||
        line.rfind("--- ", 0) == 0 || line.rfind("+++ ", 0) == 0)
      continue;

    if (line.rfind("+", 0) == 0) {
      // Added line — highlight the buffer row
      if (newRow < buf->lineCount) {
        NovaHighlight h = {};
        h.row = newRow;
        h.colStart = 0;
        h.colEnd = buf->lines[newRow] ? (int)strlen(buf->lines[newRow]) : 1;
        h.style = {NOVA_COLOR_BRIGHT_GREEN, NOVA_COLOR_NONE, 1, 0};
        highlights.push_back(h);
        lastRow = newRow;
      }
      newRow++;
    } else if (line.rfind("-", 0) == 0) {
      // Removed line — anchor to surrounding context row
      int anchor = std::min(lastRow, buf->lineCount - 1);
      if (anchor >= 0) {
        NovaHighlight h = {};
        h.row = anchor;
        h.colStart = 0;
        h.colEnd = buf->lines[anchor] ? (int)strlen(buf->lines[anchor]) : 1;
        h.style = {NOVA_COLOR_BRIGHT_RED, NOVA_COLOR_NONE, 1, 0};
        // Only push if not already marked green (added wins over removed)
        bool alreadyGreen = false;
        for (auto& existing : highlights)
          if (existing.row == anchor &&
              existing.style.fg == NOVA_COLOR_BRIGHT_GREEN)
            alreadyGreen = true;
        if (!alreadyGreen) highlights.push_back(h);
      }
      // '-' lines don't advance the new-side counter
    } else {
      // Context line — advance new-side counter
      lastRow = newRow;
      newRow++;
    }
  }

  return highlights;
}

static NovaAction makeHighlights(const std::vector<NovaHighlight>& hl) {
  NovaAction action = {};
  if (hl.empty()) {
    action.type = NOVA_ACTION_CLEAR_HIGHLIGHTS;
    return action;
  }
  action.type = NOVA_ACTION_ADD_HIGHLIGHT;
  action.highlightCount = (int)hl.size();
  action.highlights = (NovaHighlight*)malloc(hl.size() * sizeof(NovaHighlight));
  for (size_t i = 0; i < hl.size(); i++) action.highlights[i] = hl[i];
  return action;
}

// ── showDiffLines context
// ───────────────────────────────────────────────────── Finds the diff hunk
// closest to the cursor and shows what those lines looked like before (the '-'
// side) in a non-modal split on the right.
static std::vector<std::string> buildDiffLinesContext(NovaBuffer* buf) {
  std::string repo = detectRepo(buf);
  std::string file = currentFile(buf);
  std::vector<std::string> result;

  int rc = 0;
  std::string diff = diffFromBuffer(buf, repo, &rc);
  if (rc != 0) {
    result.push_back("git diff failed.");
    return result;
  }
  if (trim(diff).empty()) {
    result.push_back("No changes since last commit.");
    return result;
  }

  // Collect hunks: {newStartRow, vector of lines in that hunk}
  struct Hunk {
    int newStartRow;
    std::vector<std::string> lines;
  };
  std::vector<Hunk> hunks;

  int currentNewRow = 0;
  Hunk* current = nullptr;

  for (auto& line : splitLines(diff)) {
    if (line.rfind("@@", 0) == 0) {
      hunks.push_back({0, {}});
      current = &hunks.back();
      size_t plus = line.find('+');
      if (plus != std::string::npos) {
        int startLine = std::atoi(line.c_str() + plus + 1);
        currentNewRow = (startLine > 0) ? startLine - 1 : 0;
      }
      current->newStartRow = currentNewRow;
      current->lines.push_back(line);
      continue;
    }
    if (!current) continue;
    if (line.rfind("diff ", 0) == 0 || line.rfind("index ", 0) == 0 ||
        line.rfind("--- ", 0) == 0 || line.rfind("+++ ", 0) == 0)
      continue;

    current->lines.push_back(line);
    if (line.rfind("+", 0) == 0)
      currentNewRow++;
    else if (line.rfind("-", 0) != 0)
      currentNewRow++;  // context
  }

  if (hunks.empty()) {
    result.push_back("No hunks found.");
    return result;
  }

  // Find hunk whose newStartRow is closest to the cursor
  int cursorRow = buf ? buf->curRow : 0;
  int bestIdx = 0;
  int bestDist = std::abs(hunks[0].newStartRow - cursorRow);
  for (int i = 1; i < (int)hunks.size(); i++) {
    int dist = std::abs(hunks[i].newStartRow - cursorRow);
    if (dist < bestDist) {
      bestDist = dist;
      bestIdx = i;
    }
  }

  const Hunk& best = hunks[bestIdx];
  result.push_back("Nearest changed region (line ~" +
                   std::to_string(best.newStartRow + 1) + "):");
  result.push_back("");

  // Show only the '-' (before) lines plus context, skip '+' lines
  for (auto& line : best.lines) {
    if (line.rfind("+", 0) == 0 && line.rfind("+++", 0) != 0) continue;
    result.push_back(line);
  }

  return result;
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
  return singleAction(makePopup(
      "Commit Scope", {"Current file", "All repo changes", "Cancel"}));
}

static NovaResponse startRelease() {
  g_waitState = WAIT_RELEASE_TAG;
  return singleAction(makeInput("Release tag", "v0.1.0"));
}

static NovaResponse listIssues(NovaBuffer* buf) {
  std::string repo = detectRepo(buf);
  int rc = 0;
  std::string output = runCommand(
      inRepoCommand(repo,
                    "gh issue list --limit 30 --json number,title --jq "
                    "'.[] | \"\\(.number)\\t\\(.title)\"'"),
      &rc);
  if (rc != 0)
    return singleAction(makeStatus("gh issue list failed: " + trim(output)));

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
  std::string output = runCommand(
      inRepoCommand(
          repo, "gh issue view " + std::to_string(issueNumber) +
                    " --json number,title,state,author,body,url --jq "
                    "'\"#\\(.number) \\(.title)\\nState: \\(.state)\\nAuthor: "
                    "\\(.author.login)\\nURL: \\(.url)\\n\\n\\(.body)\"'"),
      &rc);
  if (rc != 0)
    return singleAction(makeStatus("gh issue view failed: " + trim(output)));
  return singleAction(makeSplit("Issue #" + std::to_string(issueNumber),
                                splitLines(output), false));
}

static NovaResponse runCommit(NovaBuffer* buf, const std::string& message) {
  std::string repo = detectRepo(buf);
  std::string file = currentFile(buf);
  logMsg("runCommit: repo=" + repo + " file=" + file +
         " scope=" + g_commitScope + " msg=" + message);

  std::string addCommand = "git -C " + shellQuote(repo) + " add ";
  addCommand += (g_commitScope == "all") ? "-A" : "-- " + shellQuote(file);
  logMsg("runCommit: add cmd=" + addCommand);

  int addRc = 0;
  std::string addOutput = runCommand(addCommand, &addRc);
  logMsg("runCommit: add rc=" + std::to_string(addRc) +
         " out=" + trim(addOutput));
  if (addRc != 0)
    return singleAction(makeStatus("git add failed: " + trim(addOutput)));

  int commitRc = 0;
  std::string commitOutput = runCommand(
      "git -C " + shellQuote(repo) + " commit -m " + shellQuote(message),
      &commitRc);
  logMsg("runCommit: commit rc=" + std::to_string(commitRc) +
         " out=" + trim(commitOutput));
  if (commitRc != 0)
    return singleAction(makeStatus("git commit failed: " + trim(commitOutput)));

  int pushRc = 0;
  std::string pushOutput =
      runCommand("git -C " + shellQuote(repo) + " push 2>&1", &pushRc);
  logMsg("runCommit: push rc=" + std::to_string(pushRc) +
         " out=" + trim(pushOutput));
  if (pushRc != 0)
    return singleAction(makeStatus("Push failed: " + trim(pushOutput)));

  return singleAction(makeStatus("Pushed: " + trim(pushOutput)));
}

static NovaResponse runRelease(NovaBuffer* buf) {
  std::string repo = detectRepo(buf);
  int rc = 0;
  std::string output = runCommand(
      inRepoCommand(repo, "git tag " + shellQuote(g_releaseTag) +
                              " 2>/dev/null || true; git push origin " +
                              shellQuote(g_releaseTag) +
                              "; gh release create " +
                              shellQuote(g_releaseTag) + " --generate-notes"),
      &rc);
  if (rc != 0)
    return singleAction(makeStatus("release failed: " + trim(output)));
  return singleAction(makeStatus("Release created: " + g_releaseTag));
}

extern "C" {

const char* plugin_info() { return "github_workflow|1.0|nova"; }

const char* plugin_commands() {
  return "gh-connect,repo,diff,showDiffLines,commit,release,issues";
}

NovaResponse plugin_on_event(NovaEvent* event, NovaBuffer* buf) {
  NovaResponse empty = {nullptr, 0};
  if (!event) return empty;

  if (event->type == NOVA_EVENT_TICK) {
    // Não fazer nada enquanto aguarda input/popup/confirm do usuário —
    // o latest-wins policy descartaria o UI_RESULT se o TICK emitir ao mesmo
    // tempo.
    if (g_waitState != WAIT_NONE) return empty;

    // Refresh diff highlights
    if (g_diffHighlightsOn) {
      std::this_thread::sleep_for(std::chrono::milliseconds(150));
      NovaResponse resp = {};
      resp.actions = (NovaAction*)calloc(2, sizeof(NovaAction));
      resp.actionCount = 2;
      resp.actions[0].type = NOVA_ACTION_CLEAR_HIGHLIGHTS;
      resp.actions[1] = makeHighlights(buildInlineHighlights(buf));
      return resp;
    }
    // Refresh showDiffLines split
    if (g_diffLinesOpen) {
      std::this_thread::sleep_for(std::chrono::milliseconds(300));
      auto lines = buildDiffLinesContext(buf);
      NovaSplit* split = (NovaSplit*)calloc(1, sizeof(NovaSplit));
      split->title = strdup("Before (nearest change)");
      split->titleStyle = {NOVA_COLOR_BRIGHT_CYAN, NOVA_COLOR_NONE, 1, 0};
      split->lineCount = (int)lines.size();
      split->lines = (const char**)calloc(lines.size(), sizeof(char*));
      split->lineStyles = (NovaStyle*)calloc(lines.size(), sizeof(NovaStyle));
      split->position = NOVA_SPLIT_RIGHT;
      split->size = 60;
      split->borderStyle = {NOVA_COLOR_BLUE, NOVA_COLOR_NONE, 1, 0};
      split->modal = 0;
      for (size_t i = 0; i < lines.size(); i++) {
        split->lines[i] = strdup(lines[i].c_str());
        split->lineStyles[i] = styleForDiffLine(lines[i]);
      }
      NovaAction action = {};
      action.type = NOVA_ACTION_SHOW_SPLIT;
      action.split = split;
      return singleAction(action);
    }
    return empty;
  }

  if (event->type == NOVA_EVENT_UI_RESULT) {
    logMsg(
        "UI_RESULT: type=" + std::to_string(event->uiResultType) +
        " waitState=" + std::to_string(g_waitState) +
        " idx=" + std::to_string(event->uiSelectedIndex) + " text=" +
        std::string(event->uiSelectedText ? event->uiSelectedText : "(null)"));
    if ((event->uiResultType == NOVA_UI_POPUP_CANCEL ||
         event->uiResultType == NOVA_UI_INPUT_CANCEL) &&
        g_waitState == WAIT_NONE) {
      g_diffLinesOpen = false;
      if (g_diffHighlightsOn) {
        g_diffHighlightsOn = false;
        NovaAction a = {};
        a.type = NOVA_ACTION_CLEAR_HIGHLIGHTS;
        return singleAction(a);
      }
      return empty;
    }

    if (event->uiResultType == NOVA_UI_POPUP_CANCEL ||
        event->uiResultType == NOVA_UI_INPUT_CANCEL) {
      g_waitState = WAIT_NONE;
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
      logMsg("COMMIT_SCOPE: index=" + std::to_string(event->uiSelectedIndex));
      if (event->uiSelectedIndex == 2) {
        g_waitState = WAIT_NONE;
        return singleAction(makeStatus("Commit cancelled."));
      }
      g_commitScope = (event->uiSelectedIndex == 1) ? "all" : "file";
      g_waitState = WAIT_COMMIT_MSG;
      logMsg("COMMIT_SCOPE: scope=" + g_commitScope + ", showing input");
      return singleAction(makeInput("Commit message"));
    }

    if (g_waitState == WAIT_COMMIT_MSG) {
      logMsg("COMMIT_MSG: uiResultType=" + std::to_string(event->uiResultType) +
             " text=" +
             std::string(event->uiSelectedText ? event->uiSelectedText
                                               : "(null)"));
      if (event->uiResultType == NOVA_UI_INPUT_CONFIRM &&
          event->uiSelectedText) {
        std::string message = trim(event->uiSelectedText);
        g_waitState = WAIT_NONE;
        if (message.empty())
          return singleAction(makeStatus("Empty commit message."));
        return runCommit(buf, message);
      }
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

  if (event->type != NOVA_EVENT_COMMAND || !event->command) return empty;
  std::string command = event->command;

  if (command == "gh-connect") return connectGithub();
  if (command == "repo") return configureRepo(buf);

  if (command == "diff") {
    g_diffHighlightsOn = !g_diffHighlightsOn;
    if (!g_diffHighlightsOn) {
      // Desligar: limpar highlights
      NovaResponse resp = {};
      resp.actions = (NovaAction*)calloc(1, sizeof(NovaAction));
      resp.actionCount = 1;
      resp.actions[0].type = NOVA_ACTION_CLEAR_HIGHLIGHTS;
      return resp;
    }
    // Ligar: aplicar highlights nas linhas modificadas
    auto hlVec = buildInlineHighlights(buf);
    NovaResponse resp = {};
    resp.actions = (NovaAction*)calloc(2, sizeof(NovaAction));
    resp.actionCount = 2;
    resp.actions[0].type = NOVA_ACTION_CLEAR_HIGHLIGHTS;
    resp.actions[1] = makeHighlights(hlVec);
    return resp;
  }

  if (command == "showDiffLines") {
    g_diffLinesOpen = !g_diffLinesOpen;
    if (!g_diffLinesOpen) return singleAction(makeCloseSplit());

    auto lines = buildDiffLinesContext(buf);
    NovaSplit* split = (NovaSplit*)calloc(1, sizeof(NovaSplit));
    split->title = strdup("Before (nearest change)");
    split->titleStyle = {NOVA_COLOR_BRIGHT_CYAN, NOVA_COLOR_NONE, 1, 0};
    split->lineCount = (int)lines.size();
    split->lines = (const char**)calloc(lines.size(), sizeof(char*));
    split->lineStyles = (NovaStyle*)calloc(lines.size(), sizeof(NovaStyle));
    split->position = NOVA_SPLIT_RIGHT;
    split->size = 60;
    split->borderStyle = {NOVA_COLOR_BLUE, NOVA_COLOR_NONE, 1, 0};
    split->modal =
        0;  // non-modal: continua editando enquanto o split está aberto
    for (size_t i = 0; i < lines.size(); i++) {
      split->lines[i] = strdup(lines[i].c_str());
      split->lineStyles[i] = styleForDiffLine(lines[i]);
    }
    NovaAction action = {};
    action.type = NOVA_ACTION_SHOW_SPLIT;
    action.split = split;
    return singleAction(action);
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
      // title/placeholder/defaultValue apontam para globals g_input*,
      // não foram alocados com strdup — não liberar.
      free(action.input);
    }
    if (action.confirm) {
      if (action.confirm->title) free((void*)action.confirm->title);
      if (action.confirm->message) free((void*)action.confirm->message);
      if (action.confirm->confirmLabel)
        free((void*)action.confirm->confirmLabel);
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
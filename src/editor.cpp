#include "editor.h"

#ifdef _WIN32
#include <ncurses/curses.h>
#else
#include <ncurses.h>
#endif
#include <unistd.h>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

int compileFile(const std::vector<std::string>& sourceFiles,
                const std::string& outputFileArg, int optLevel);

static const std::vector<std::string> CTRL_KW = {
    "fn",        "let",     "mut", "impl",  "self", "return",
    "if",        "else",    "for", "while", "in",   "struct",
    "namespace", "include", "as",  "asm",   "ir"};
static bool isCtrlKw(const std::string& w) {
  return std::find(CTRL_KW.begin(), CTRL_KW.end(), w) != CTRL_KW.end();
}

static const std::vector<std::string> TYPE_KW = {
    "i32", "i64", "f32", "f64", "char", "string", "str", "bool", "void"};
static bool isTypeKw(const std::string& w) {
  return std::find(TYPE_KW.begin(), TYPE_KW.end(), w) != TYPE_KW.end();
}

static const std::vector<std::string> LITERAL_KW = {"true", "false"};
static bool isLiteralKw(const std::string& w) {
  return std::find(LITERAL_KW.begin(), LITERAL_KW.end(), w) != LITERAL_KW.end();
}

enum NcColors {
  COL_NORMAL = 1,
  COL_KEYWORD = 2,
  COL_TYPE = 3,
  COL_NUMBER = 4,
  COL_STRING = 5,
  COL_COMMENT = 6,
  COL_LINENO = 7,
  COL_STATUSBAR = 8,
  COL_STATUSER = 9,
  COL_DIALOG = 10,
  COL_DIGINPUT = 11,
  COL_LITERAL = 12,   // true/false → magenta
  COL_CHAR = 13,      // char literals → verde
  COL_OPERATOR = 14,  // operadores → ciano escuro
};



static void initEditorColors() {
  start_color();
  use_default_colors();

  init_pair(COL_NORMAL, COLOR_WHITE, -1);
  init_pair(COL_KEYWORD, COLOR_BLUE, -1);
  init_pair(COL_TYPE, COLOR_CYAN, -1);
  init_pair(COL_NUMBER, COLOR_YELLOW, -1);
  init_pair(COL_STRING, COLOR_YELLOW, -1);
  init_pair(COL_COMMENT, COLOR_BLACK, -1);
  init_pair(COL_LINENO, COLOR_BLUE, -1);
  init_pair(COL_STATUSBAR, COLOR_WHITE, COLOR_BLUE);
  init_pair(COL_STATUSER, COLOR_WHITE, COLOR_RED);
  init_pair(COL_DIALOG, COLOR_WHITE, COLOR_BLUE);
  init_pair(COL_DIGINPUT, COLOR_WHITE, COLOR_BLACK);
  init_pair(COL_LITERAL, COLOR_MAGENTA, -1);
  init_pair(COL_CHAR, COLOR_GREEN, -1);
  init_pair(COL_OPERATOR, COLOR_CYAN, -1);
}

// ── Renderiza uma linha com syntax highlight
// ──────────────────────────────────
static void renderLine(WINDOW* win, int y, int xOff, const std::string& line,
                       int scrollX) {
  wmove(win, y, xOff);
  wclrtoeol(win);

  std::string vis = (scrollX < (int)line.size()) ? line.substr(scrollX) : "";
  int maxw = getmaxx(win) - xOff;
  if ((int)vis.size() > maxw) vis = vis.substr(0, maxw);

  size_t i = 0;
  while (i < vis.size()) {
    // Comentário
    if (i + 1 < vis.size() && vis[i] == '/' && vis[i + 1] == '/') {
      wattron(win, COLOR_PAIR(COL_COMMENT) | A_BOLD);
      waddstr(win, vis.substr(i).c_str());
      wattroff(win, COLOR_PAIR(COL_COMMENT) | A_BOLD);
      break;
    }

    // #include <header.nh>
    if (vis[i] == '#') {
      wattron(win, COLOR_PAIR(COL_KEYWORD) | A_BOLD);
      waddch(win, vis[i++]);
      // consome "include"
      std::string word;
      while (i < vis.size() && std::isalpha((unsigned char)vis[i]))
        word += vis[i++];
      waddstr(win, word.c_str());
      wattroff(win, COLOR_PAIR(COL_KEYWORD) | A_BOLD);
      // espaço
      while (i < vis.size() && vis[i] == ' ') {
        waddch(win, vis[i++]);
      }
      // <path.nh> ou "path.nh"
      if (i < vis.size() && (vis[i] == '<' || vis[i] == '"')) {
        char close = (vis[i] == '<') ? '>' : '"';
        wattron(win, COLOR_PAIR(COL_STRING) | A_BOLD);
        waddch(win, vis[i++]);
        while (i < vis.size() && vis[i] != close) waddch(win, vis[i++]);
        if (i < vis.size()) waddch(win, vis[i++]);
        wattroff(win, COLOR_PAIR(COL_STRING) | A_BOLD);
      }
      continue;
    }

    // String literal
    if (vis[i] == '"') {
      wattron(win, COLOR_PAIR(COL_STRING) | A_BOLD);
      waddch(win, vis[i++]);
      while (i < vis.size() && vis[i] != '"') {
        if (vis[i] == '\\' && i + 1 < vis.size()) {
          waddch(win, vis[i++]);
        }
        waddch(win, vis[i++]);
      }
      if (i < vis.size()) waddch(win, vis[i++]);
      wattroff(win, COLOR_PAIR(COL_STRING) | A_BOLD);
      continue;
    }

    // Char literal
    if (vis[i] == '\'') {
      wattron(win, COLOR_PAIR(COL_CHAR) | A_BOLD);
      waddch(win, vis[i++]);
      if (i < vis.size() && vis[i] == '\\' && i + 1 < vis.size()) {
        waddch(win, vis[i++]);
      }
      if (i < vis.size()) waddch(win, vis[i++]);
      if (i < vis.size() && vis[i] == '\'') waddch(win, vis[i++]);
      wattroff(win, COLOR_PAIR(COL_CHAR) | A_BOLD);
      continue;
    }

    // Número
    if (std::isdigit((unsigned char)vis[i])) {
      wattron(win, COLOR_PAIR(COL_NUMBER) | A_BOLD);
      while (i < vis.size() &&
             (std::isdigit((unsigned char)vis[i]) || vis[i] == '.'))
        waddch(win, vis[i++]);
      wattroff(win, COLOR_PAIR(COL_NUMBER) | A_BOLD);
      continue;
    }

    // Palavra
    if (std::isalpha((unsigned char)vis[i]) || vis[i] == '_') {
      std::string word;
      while (i < vis.size() &&
             (std::isalnum((unsigned char)vis[i]) || vis[i] == '_'))
        word += vis[i++];
      if (isTypeKw(word)) {
        wattron(win, COLOR_PAIR(COL_TYPE) | A_BOLD);
        waddstr(win, word.c_str());
        wattroff(win, COLOR_PAIR(COL_TYPE) | A_BOLD);
      } else if (isCtrlKw(word)) {
        wattron(win, COLOR_PAIR(COL_KEYWORD) | A_BOLD);
        waddstr(win, word.c_str());
        wattroff(win, COLOR_PAIR(COL_KEYWORD) | A_BOLD);
      } else if (isLiteralKw(word)) {
        wattron(win, COLOR_PAIR(COL_LITERAL) | A_BOLD);
        waddstr(win, word.c_str());
        wattroff(win, COLOR_PAIR(COL_LITERAL) | A_BOLD);
      } else {
        wattron(win, COLOR_PAIR(COL_NORMAL));
        waddstr(win, word.c_str());
        wattroff(win, COLOR_PAIR(COL_NORMAL));
      }
      continue;
    }

    // Operadores
    if (std::string("+-*/=%&|<>!:.").find(vis[i]) != std::string::npos) {
      wattron(win, COLOR_PAIR(COL_OPERATOR) | A_BOLD);
      waddch(win, (unsigned char)vis[i++]);
      wattroff(win, COLOR_PAIR(COL_OPERATOR) | A_BOLD);
      continue;
    }

    // Normal
    wattron(win, COLOR_PAIR(COL_NORMAL));
    waddch(win, (unsigned char)vis[i++]);
    wattroff(win, COLOR_PAIR(COL_NORMAL));
  }
}

// ── Dialog de input de texto (nome do arquivo / caminho) ─────────────────────
// Retorna o texto digitado, ou "" se o usuário cancelou com Esc.
static std::string inputDialog(const std::string& prompt,
                               const std::string& preset) {
  int maxy, maxx;
  getmaxyx(stdscr, maxy, maxx);

  const int dw = std::min(maxx - 4, 72);
  const int dh = 5;
  const int dy = maxy / 2 - dh / 2;
  const int dx = maxx / 2 - dw / 2;

  WINDOW* dlgWin = newwin(dh, dw, dy, dx);
  keypad(dlgWin, TRUE);

  std::string input = preset;
  int cursor = (int)input.size();

  auto drawDialog = [&]() {
    werase(dlgWin);
    wattron(dlgWin, COLOR_PAIR(COL_DIALOG) | A_BOLD);
    box(dlgWin, 0, 0);  // borda dupla
    // Título
    std::string title = "  " + prompt + "  ";
    mvwaddstr(dlgWin, 0, (dw - (int)title.size()) / 2, title.c_str());
    wattroff(dlgWin, COLOR_PAIR(COL_DIALOG) | A_BOLD);

    // Campo de input
    int fieldW = dw - 4;
    int fieldX = 2;
    int fieldY = 2;
    wattron(dlgWin, COLOR_PAIR(COL_DIGINPUT));
    std::string field(fieldW, ' ');
    mvwaddstr(dlgWin, fieldY, fieldX, field.c_str());

    // Calcula scroll do input para mostrar o cursor
    int scrollI = 0;
    if (cursor >= fieldW) scrollI = cursor - fieldW + 1;
    std::string visible = input.substr(scrollI);
    if ((int)visible.size() > fieldW) visible = visible.substr(0, fieldW);
    mvwaddstr(dlgWin, fieldY, fieldX, visible.c_str());
    wattroff(dlgWin, COLOR_PAIR(COL_DIGINPUT));

    // Hint
    wattron(dlgWin, COLOR_PAIR(COL_COMMENT));
    mvwaddstr(dlgWin, dh - 2, 2, " Enter: confirm   Esc: cancel ");
    wattroff(dlgWin, COLOR_PAIR(COL_COMMENT));

    // Cursor
    int curScreenX = fieldX + (cursor - scrollI);
    wmove(dlgWin, fieldY, curScreenX);
    wrefresh(dlgWin);
  };

  drawDialog();

  while (true) {
    int ch = wgetch(dlgWin);
    if (ch == 27) {  // Esc — cancela
      delwin(dlgWin);
      return "";
    }
    if (ch == '\n' || ch == KEY_ENTER) {
      delwin(dlgWin);
      return input;
    }
    if (ch == KEY_LEFT && cursor > 0) {
      cursor--;
    } else if (ch == KEY_RIGHT && cursor < (int)input.size()) {
      cursor++;
    } else if (ch == KEY_HOME) {
      cursor = 0;
    } else if (ch == KEY_END) {
      cursor = (int)input.size();
    } else if ((ch == KEY_BACKSPACE || ch == 127) && cursor > 0) {
      input.erase(cursor - 1, 1);
      cursor--;
    } else if (ch == KEY_DC && cursor < (int)input.size()) {
      input.erase(cursor, 1);
    } else if (ch >= 32 && ch < 127) {
      input.insert(cursor, 1, (char)ch);
      cursor++;
    }
    drawDialog();
  }
}

// ── Retorna o diretório atual
// ─────────────────────────────────────────────────
static std::string currentDir() {
  char buf[4096];
  if (getcwd(buf, sizeof(buf))) return std::string(buf) + "/";
  return "";
}

// ── Editor principal
// ──────────────────────────────────────────────────────────
void runEditor(const std::string& editFile, const std::string& outputFileArg,
               int optLevel) {
  std::string filename = editFile;

  initscr();
  set_escdelay(10);
  raw();
  keypad(stdscr, TRUE);
  noecho();
  curs_set(1);
  initEditorColors();

  if (filename.empty()) {
    std::string preset = currentDir() + "untitled.npp";
    std::string result = inputDialog(" Save as ", preset);
    if (result.empty()) {
      endwin();
      return;
    }
    filename = result;
  }

  std::vector<std::string> lines;
  {
    std::ifstream f(filename);
    if (f) {
      std::string ln;
      while (std::getline(f, ln)) lines.push_back(ln);
    }
  }
  if (lines.empty()) lines.push_back("");

  int maxy, maxx;
  getmaxyx(stdscr, maxy, maxx);

  const int LINENO_WIDTH = 6;
  WINDOW* statusWin = newwin(1, maxx, 0, 0);
  WINDOW* editorWin = newwin(maxy - 2, maxx, 1, 0);
  WINDOW* footerWin = newwin(1, maxx, maxy - 1, 0);
  keypad(editorWin, TRUE);
  wtimeout(editorWin, 500);  // retorna ERR a cada 500ms se não houver input

  int curRow = 0, curCol = 0;
  int scrollRow = 0, scrollX = 0;
  bool dirty = false;
  std::string statusMsg;
  bool statusIsError = false;
  struct EcheckError {
    int line, col;
    std::string msg;
  };
  std::vector<EcheckError> echeckErrors;
  time_t lastEcheck = 0;

  // ── Modo Vim ──────────────────────────────────────────────────────
  enum class Mode { Normal, Insert, Command, Visual, VisualLine };
  Mode mode = Mode::Normal;
  std::string cmdBuffer;  // acumula teclas em normal (ex: "dd", "gg")
  std::string cmdLine;    // acumula linha de comando (:w, :q, :wq)

  // ── Visual mode ───────────────────────────────────────────────
  int selAnchorRow = 0, selAnchorCol = 0;
  std::string yankBuffer;  // buffer interno de cópia

  auto clampCursor = [&]() {
    if (curRow < 0) curRow = 0;
    if (curRow >= (int)lines.size()) curRow = (int)lines.size() - 1;
    int maxCol = (int)lines[curRow].size();
    if (mode == Mode::Normal && maxCol > 0) maxCol--;
    if (curCol > maxCol) curCol = maxCol;
    if (curCol < 0) curCol = 0;
  };

  auto wordForward = [&]() {
    auto& l = lines[curRow];
    // pula caracteres de palavra
    while (curCol < (int)l.size() &&
           (std::isalnum((unsigned char)l[curCol]) || l[curCol] == '_'))
      curCol++;
    // pula espaços
    while (curCol < (int)l.size() && l[curCol] == ' ') curCol++;
  };

  auto wordBack = [&]() {
    auto& l = lines[curRow];
    if (curCol > 0) curCol--;
    while (curCol > 0 && l[curCol] == ' ') curCol--;
    while (curCol > 0 &&
           (std::isalnum((unsigned char)l[curCol - 1]) || l[curCol - 1] == '_'))
      curCol--;
  };

  auto yankToClipboard = [&](const std::string& text) {
    yankBuffer = text;
    const char* cmds[] = {"xclip -selection clipboard 2>/dev/null",
                          "xsel --clipboard --input 2>/dev/null",
                          "clip.exe 2>/dev/null", nullptr};
    for (int i = 0; cmds[i]; i++) {
      FILE* p = popen(cmds[i], "w");
      if (p) {
        fwrite(text.c_str(), 1, text.size(), p);
        if (pclose(p) == 0) break;
      }
    }
  };

  auto getSelection = [&]() -> std::string {
    int r1 = std::min(curRow, selAnchorRow);
    int r2 = std::max(curRow, selAnchorRow);
    if (mode == Mode::VisualLine) {
      std::string out;
      for (int r = r1; r <= r2; r++) {
        out += lines[r];
        if (r < r2) out += "\n";
      }
      return out;
    }
    // Visual char
    int c1, c2;
    if (curRow < selAnchorRow ||
        (curRow == selAnchorRow && curCol <= selAnchorCol)) {
      c1 = curCol;
      c2 = selAnchorCol;
      r1 = curRow;
      r2 = selAnchorRow;
    } else {
      c1 = selAnchorCol;
      c2 = curCol;
      r1 = selAnchorRow;
      r2 = curRow;
    }
    if (r1 == r2) return lines[r1].substr(c1, c2 - c1 + 1);
    std::string out = lines[r1].substr(c1) + "\n";
    for (int r = r1 + 1; r < r2; r++) out += lines[r] + "\n";
    out += lines[r2].substr(0, c2 + 1);
    return out;
  };

  // ── Redraw ────────────────────────────────────────────────────────
  auto redraw = [&]() {
    getmaxyx(stdscr, maxy, maxx);
    wresize(statusWin, 1, maxx);
    wresize(editorWin, maxy - 2, maxx);
    wresize(footerWin, 1, maxx);
    mvwin(footerWin, maxy - 1, 0);

    int editorH = maxy - 2;
    int editorW = maxx - LINENO_WIDTH;

    if (curRow < scrollRow) scrollRow = curRow;
    if (curRow >= scrollRow + editorH) scrollRow = curRow - editorH + 1;
    if (curCol < scrollX) scrollX = curCol;
    if (curCol >= scrollX + editorW) scrollX = curCol - editorW + 1;

    // ── Status bar ────────────────────────────────────────────────
    werase(statusWin);
    // Verifica se há erro na linha atual
    std::string right = statusMsg;
    bool lineHasError = false;
    for (auto& e : echeckErrors) {
      if (e.line == curRow + 1) {
        right = "  ⚠ " + e.msg;
        lineHasError = true;
        break;
      }
    }
    right += "   ";
    int scol = (statusIsError || lineHasError) ? COL_STATUSER : COL_STATUSBAR;
    wattron(statusWin, COLOR_PAIR(scol) | A_BOLD);
    std::string modeStr = (mode == Mode::Insert)       ? " INSERT "
                          : (mode == Mode::Command)    ? " COMMAND "
                          : (mode == Mode::Visual)     ? " VISUAL "
                          : (mode == Mode::VisualLine) ? " V-LINE "
                                                       : " NORMAL ";
    std::string left = modeStr + (dirty ? "● " : "  ") + filename;
    int spaces = maxx - (int)left.size() - (int)right.size();
    if (spaces < 1) spaces = 1;
    std::string bar = left + std::string(spaces, ' ') + right;
    bar.resize(maxx, ' ');
    waddstr(statusWin, bar.c_str());
    wattroff(statusWin, COLOR_PAIR(scol) | A_BOLD);
    wrefresh(statusWin);

    // ── Linhas do editor ──────────────────────────────────────────
    werase(editorWin);
    for (int r = 0; r < editorH; r++) {
      int lineIdx = scrollRow + r;
      if (lineIdx < (int)lines.size()) {
        // Número da linha — vermelho se tiver erro
        bool hasErr = false;
        for (auto& e : echeckErrors)
          if (e.line == lineIdx + 1) {
            hasErr = true;
            break;
          }

        if (hasErr)
          wattron(editorWin, COLOR_PAIR(COL_STATUSER) | A_BOLD);
        else
          wattron(editorWin, COLOR_PAIR(COL_LINENO) | A_BOLD);
        char lnbuf[16];
        snprintf(lnbuf, sizeof(lnbuf), "%4d  ", lineIdx + 1);
        mvwaddstr(editorWin, r, 0, lnbuf);
        if (hasErr)
          wattroff(editorWin, COLOR_PAIR(COL_STATUSER) | A_BOLD);
        else
          wattroff(editorWin, COLOR_PAIR(COL_LINENO) | A_BOLD);

        renderLine(editorWin, r, LINENO_WIDTH, lines[lineIdx], scrollX);

        // Highlight visual
        if (mode == Mode::Visual || mode == Mode::VisualLine) {
          int r1 = std::min(curRow, selAnchorRow);
          int r2 = std::max(curRow, selAnchorRow);
          if (lineIdx >= r1 && lineIdx <= r2) {
            int hx, hw;
            if (mode == Mode::VisualLine) {
              hx = LINENO_WIDTH;
              hw = getmaxx(editorWin) - LINENO_WIDTH;
            } else {
              int c1, c2;
              if (curRow < selAnchorRow ||
                  (curRow == selAnchorRow && curCol <= selAnchorCol)) {
                c1 = curCol;
                c2 = selAnchorCol;
              } else {
                c1 = selAnchorCol;
                c2 = curCol;
              }
              if (lineIdx == r1 && lineIdx == r2) {
                hx = LINENO_WIDTH + c1 - scrollX;
                hw = c2 - c1 + 1;
              } else if (lineIdx == r1) {
                int cc = (r1 == curRow) ? curCol : selAnchorCol;
                hx = LINENO_WIDTH + cc - scrollX;
                hw = getmaxx(editorWin) - hx;
              } else if (lineIdx == r2) {
                int cc = (r2 == curRow) ? curCol : selAnchorCol;
                hx = LINENO_WIDTH;
                hw = cc - scrollX + 1;
              } else {
                hx = LINENO_WIDTH;
                hw = getmaxx(editorWin) - LINENO_WIDTH;
              }
            }
            if (hx < LINENO_WIDTH) {
              hw -= (LINENO_WIDTH - hx);
              hx = LINENO_WIDTH;
            }
            if (hw > 0) mvwchgat(editorWin, r, hx, hw, A_REVERSE, 0, NULL);
          }
        }

        // Sublinha a coluna do erro
        for (auto& e : echeckErrors) {
          if (e.line == lineIdx + 1) {
            int errX = LINENO_WIDTH + std::max(0, e.col - 1 - scrollX);
            int endX = getmaxx(editorWin);
            if (errX < endX)
              mvwchgat(editorWin, r, errX, endX - errX, A_UNDERLINE,
                       COL_STATUSER, NULL);
            break;
          }
        }
      } else {
        wattron(editorWin, COLOR_PAIR(COL_LINENO) | A_BOLD);
        mvwaddstr(editorWin, r, 0, "    ~ ");
        wattroff(editorWin, COLOR_PAIR(COL_LINENO) | A_BOLD);
      }
    }

    int cursorY = curRow - scrollRow;
    int cursorX = LINENO_WIDTH + (curCol - scrollX);

    char charUnder = ' ';
    if (curCol < (int)lines[curRow].size()) charUnder = lines[curRow][curCol];

    if (mode == Mode::Insert)
      mvwaddch(editorWin, cursorY, cursorX, charUnder | A_REVERSE);
    else
      mvwaddch(editorWin, cursorY, cursorX, charUnder | A_UNDERLINE | A_BOLD);

    wmove(editorWin, cursorY, cursorX);
    wrefresh(editorWin);

    // ── Footer ────────────────────────────────────────────────────
    werase(footerWin);
    wattron(footerWin, A_REVERSE | A_BOLD);
    char pos[64];
    snprintf(pos, sizeof(pos), " Ln %d, Col %d ", curRow + 1, curCol + 1);
    std::string left2, right2;
    if (mode == Mode::Command) {
      left2 = ":" + cmdLine;
    } else if (mode == Mode::Insert) {
      left2 = "  i insert   Esc normal mode";
    } else if (mode == Mode::Visual || mode == Mode::VisualLine) {
      left2 = "  y yank   Esc cancel";
    } else {
      left2 =
          "  i/a insert   o new line   dd del line   w/b word   gg/G top/bot   "
          ":w :q :wq";
    }
    right2 = std::string(pos);
    int fspaces = maxx - (int)left2.size() - (int)right2.size();
    if (fspaces < 0) fspaces = 0;
    std::string footer = left2 + std::string(fspaces, ' ') + right2;
    footer.resize(maxx, ' ');
    waddstr(footerWin, footer.c_str());
    wattroff(footerWin, A_REVERSE | A_BOLD);
    wrefresh(footerWin);
  };

  // ── Save ──────────────────────────────────────────────────────────
  auto saveFile = [&]() -> bool {
    std::ofstream f(filename);
    if (!f) {
      statusMsg = "Error: could not save " + filename;
      statusIsError = true;
      return false;
    }
    for (size_t i = 0; i < lines.size(); i++) {
      f << lines[i];
      if (i + 1 < lines.size()) f << "\n";
    }
    dirty = false;
    return true;
  };

  // ── Save & Compile ────────────────────────────────────────────────
  auto saveAndCompile = [&]() -> bool {
    if (!saveFile()) return false;
    endwin();
    int ret = compileFile({filename}, outputFileArg, optLevel);
    initscr();
    raw();
    keypad(stdscr, TRUE);
    noecho();
    curs_set(0);
    initEditorColors();
    if (ret == 0) {
      statusMsg = "Compiled OK";
      statusIsError = false;
    } else {
      statusMsg = "Compile error — check output above";
      statusIsError = true;
    }
    return ret == 0;
  };

  redraw();

  auto runEcheck = [&]() {
    statusMsg = "checking...";
    redraw();

    // No MinGW/Windows, usa TEMP do sistema
    const char* tmpEnv = getenv("TEMP");
    if (!tmpEnv) tmpEnv = getenv("TMP");
    if (!tmpEnv) tmpEnv = "C:\\Temp";
    std::string tmpDir = tmpEnv;

    std::string pid = std::to_string(getpid());
    std::string tmpFile = tmpDir + "\\nova_echeck_" + pid + ".npp";
    std::string tmpOut = tmpDir + "\\nova_echeck_out_" + pid + ".txt";

#ifdef _WIN32
    std::string cmd =
        "n++.exe --echeck \"" + tmpFile + "\" > \"" + tmpOut + "\" 2>nul";
#else
    std::string tmpFile = "/tmp/nova_echeck_" + pid + ".npp";
    std::string tmpOut = "/tmp/nova_echeck_out_" + pid + ".txt";
    std::string cmd =
        "n++ --echeck " + tmpFile + " > " + tmpOut + " 2>/dev/null";
#endif

    {
      std::ofstream f(tmpFile);
      for (size_t i = 0; i < lines.size(); i++) {
        f << lines[i];
        if (i + 1 < lines.size()) f << "\n";
      }
    }
    system(cmd.c_str());
    echeckErrors.clear();
    std::ifstream f(tmpOut);
    std::string ln;
    while (std::getline(f, ln)) {
      size_t p1 = ln.find(':');
      size_t p2 = ln.find(':', p1 + 1);
      size_t p3 = ln.find(':', p2 + 1);
      if (p1 == std::string::npos || p2 == std::string::npos ||
          p3 == std::string::npos)
        continue;
      EcheckError e;
      e.line = std::stoi(ln.substr(0, p1));
      e.col = std::stoi(ln.substr(p1 + 1, p2 - p1 - 1));
      e.msg = ln.substr(p3 + 1);
      echeckErrors.push_back(e);
    }
    std::remove(tmpFile.c_str());
    std::remove(tmpOut.c_str());
  };

  while (true) {
    int ch = wgetch(editorWin);
    if (ch == ERR) {
      time_t now = time(nullptr);
      if (dirty && now - lastEcheck >= 1) {
        runEcheck();
        lastEcheck = now;
      }
      redraw();
      continue;
    }
    clampCursor();
    int lineLen = (int)lines[curRow].size();

    // ── Ctrl sempre funciona independente do modo ─────────────────
    if (ch == ('s' & 0x1f)) {
      saveAndCompile();
      runEcheck();
      redraw();
      continue;
    }
    if (ch == ('x' & 0x1f)) {
      saveAndCompile();
      endwin();
      return;
    }
    if (ch == ('q' & 0x1f)) {
      endwin();
      return;
    }
    if (ch == ('r' & 0x1f)) {
      std::string newName = inputDialog(" Save as ", filename);
      if (!newName.empty()) {
        filename = newName;
        dirty = true;
        statusMsg = "Path changed — ^S to save";
        statusIsError = false;
      }
      redraw();
      continue;
    }

    // ── Setas sempre funcionam ────────────────────────────────────
    if (ch == KEY_UP) {
      if (curRow > 0) {
        curRow--;
        clampCursor();
      }
      redraw();
      continue;
    }
    if (ch == KEY_DOWN) {
      if (curRow < (int)lines.size() - 1) {
        curRow++;
        clampCursor();
      }
      redraw();
      continue;
    }
    if (ch == KEY_LEFT) {
      if (curCol > 0)
        curCol--;
      else if (curRow > 0) {
        curRow--;
        curCol = (int)lines[curRow].size();
      }
      redraw();
      continue;
    }
    if (ch == KEY_RIGHT) {
      if (curCol < lineLen)
        curCol++;
      else if (curRow < (int)lines.size() - 1) {
        curRow++;
        curCol = 0;
      }
      redraw();
      continue;
    }
    if (ch == KEY_HOME) {
      curCol = 0;
      redraw();
      continue;
    }
    if (ch == KEY_END) {
      curCol = lineLen;
      redraw();
      continue;
    }
    if (ch == KEY_PPAGE) {
      curRow = std::max(0, curRow - (maxy - 2));
      clampCursor();
      redraw();
      continue;
    }
    if (ch == KEY_NPAGE) {
      curRow = std::min((int)lines.size() - 1, curRow + (maxy - 2));
      clampCursor();
      redraw();
      continue;
    }

    // ══════════════════════════════════════════════════════════════
    // MODO INSERT
    // ══════════════════════════════════════════════════════════════
    if (mode == Mode::Insert) {
      if (ch == 27) {  // Esc → Normal
        mode = Mode::Normal;
        if (curCol > 0) curCol--;
        clampCursor();
      } else if (ch == KEY_BACKSPACE || ch == 127) {
        if (curCol > 0) {
          lines[curRow].erase(curCol - 1, 1);
          curCol--;
          dirty = true;
        } else if (curRow > 0) {
          curCol = (int)lines[curRow - 1].size();
          lines[curRow - 1] += lines[curRow];
          lines.erase(lines.begin() + curRow);
          curRow--;
          dirty = true;
        }
      } else if (ch == KEY_DC) {
        if (curCol < (int)lines[curRow].size()) {
          lines[curRow].erase(curCol, 1);
          dirty = true;
        } else if (curRow < (int)lines.size() - 1) {
          lines[curRow] += lines[curRow + 1];
          lines.erase(lines.begin() + curRow + 1);
          dirty = true;
        }
      } else if (ch == '\n' || ch == KEY_ENTER) {
        std::string rest = lines[curRow].substr(curCol);
        lines[curRow] = lines[curRow].substr(0, curCol);
        lines.insert(lines.begin() + curRow + 1, rest);
        curRow++;
        curCol = 0;
        dirty = true;
      } else if (ch == '\t') {
        lines[curRow].insert(curCol, "    ");
        curCol += 4;
        dirty = true;
      } else if (ch >= 32 && ch < 127) {
        lines[curRow].insert(curCol, 1, (char)ch);
        curCol++;
        dirty = true;
        statusMsg = "";
        statusIsError = false;
      }
      if (dirty) {
        time_t now = time(nullptr);
        if (now - lastEcheck >= 1) {
          runEcheck();
          lastEcheck = now;
        }
      }
      redraw();
      continue;
    }
    // ══════════════════════════════════════════════════════════════
    // MODO VISUAL / VISUAL LINE
    // ══════════════════════════════════════════════════════════════
    if (mode == Mode::Visual || mode == Mode::VisualLine) {
      if (ch == 27) {  // Esc — cancela
        mode = Mode::Normal;
        redraw();
        continue;
      }
      // Navegação hjkl e setas estendem a seleção
      if (ch == 'h' || ch == KEY_LEFT) {
        if (curCol > 0) curCol--;
        redraw();
        continue;
      }
      if (ch == 'l' || ch == KEY_RIGHT) {
        if (curCol < (int)lines[curRow].size() - 1) curCol++;
        redraw();
        continue;
      }
      if (ch == 'j' || ch == KEY_DOWN) {
        if (curRow < (int)lines.size() - 1) {
          curRow++;
          clampCursor();
        }
        redraw();
        continue;
      }
      if (ch == 'k' || ch == KEY_UP) {
        if (curRow > 0) {
          curRow--;
          clampCursor();
        }
        redraw();
        continue;
      }
      if (ch == 'w') {
        wordForward();
        clampCursor();
        redraw();
        continue;
      }
      if (ch == 'b') {
        wordBack();
        clampCursor();
        redraw();
        continue;
      }
      if (ch == '0') {
        curCol = 0;
        redraw();
        continue;
      }
      if (ch == '$') {
        curCol = std::max(0, (int)lines[curRow].size() - 1);
        redraw();
        continue;
      }
      if (ch == 'G') {
        curRow = (int)lines.size() - 1;
        clampCursor();
        redraw();
        continue;
      }
      if (ch == 'y') {  // yank
        std::string sel = getSelection();
        yankToClipboard(sel);
        statusMsg = "yanked " +
                    std::to_string(std::abs(curRow - selAnchorRow) + 1) +
                    " line(s)";
        mode = Mode::Normal;
        redraw();
        continue;
      }
      redraw();
      continue;
    }
    // ══════════════════════════════════════════════════════════════
    // MODO COMMAND (:)
    // ══════════════════════════════════════════════════════════════
    if (mode == Mode::Command) {
      if (ch == 27) {
        mode = Mode::Normal;
        cmdLine = "";
      } else if (ch == '\n' || ch == KEY_ENTER) {
        if (cmdLine == "w") {
          saveFile();
          statusMsg = "Saved";
          statusIsError = false;
        } else if (cmdLine == "q") {
          if (!dirty) {
            endwin();
            return;
          }
          statusMsg = "Unsaved changes — :w first, or :q!";
          statusIsError = true;
        } else if (cmdLine == "q!") {
          endwin();
          return;
        } else if (cmdLine == "wq" || cmdLine == "x") {
          saveAndCompile();
          endwin();
          return;
        } else {
          statusMsg = "Unknown command: " + cmdLine;
          statusIsError = true;
        }
        mode = Mode::Normal;
        cmdLine = "";
      } else if (ch == KEY_BACKSPACE || ch == 127) {
        if (!cmdLine.empty())
          cmdLine.pop_back();
        else
          mode = Mode::Normal;
      } else if (ch >= 32 && ch < 127) {
        cmdLine += (char)ch;
      }
      redraw();
      continue;
    }

    if (ch == 'v') {
      mode = Mode::Visual;
      selAnchorRow = curRow;
      selAnchorCol = curCol;
      cmdBuffer = "";
      redraw();
      continue;
    }
    if (ch == 'V') {
      mode = Mode::VisualLine;
      selAnchorRow = curRow;
      selAnchorCol = 0;
      cmdBuffer = "";
      redraw();
      continue;
    }
    if (ch == 'y' && ch == 'y') {  // yy — linha inteira no normal
      yankToClipboard(lines[curRow] + "\n");
      statusMsg = "yanked line";
      redraw();
      continue;
    }
    // ══════════════════════════════════════════════════════════════
    // MODO NORMAL
    // ══════════════════════════════════════════════════════════════
    if (ch == ':') {
      mode = Mode::Command;
      cmdLine = "";
      redraw();
      continue;
    }
    if (ch == 'i') {
      mode = Mode::Insert;
      cmdBuffer = "";
      redraw();
      continue;
    }
    if (ch == 'a') {
      if (curCol < (int)lines[curRow].size()) curCol++;
      mode = Mode::Insert;
      cmdBuffer = "";
      redraw();
      continue;
    }
    if (ch == 'o') {
      lines.insert(lines.begin() + curRow + 1, "");
      curRow++;
      curCol = 0;
      dirty = true;
      mode = Mode::Insert;
      cmdBuffer = "";
      redraw();
      continue;
    }
    if (ch == 'O') {
      lines.insert(lines.begin() + curRow, "");
      curCol = 0;
      dirty = true;
      mode = Mode::Insert;
      cmdBuffer = "";
      redraw();
      continue;
    }

    if (ch == 'h') {
      if (curCol > 0) curCol--;
      redraw();
      continue;
    }
    if (ch == 'l') {
      if (curCol < (int)lines[curRow].size() - 1) curCol++;
      redraw();
      continue;
    }
    if (ch == 'j') {
      if (curRow < (int)lines.size() - 1) {
        curRow++;
        clampCursor();
      }
      redraw();
      continue;
    }
    if (ch == 'k') {
      if (curRow > 0) {
        curRow--;
        clampCursor();
      }
      redraw();
      continue;
    }
    if (ch == '0') {
      curCol = 0;
      redraw();
      continue;
    }
    if (ch == '$') {
      curCol = std::max(0, (int)lines[curRow].size() - 1);
      redraw();
      continue;
    }
    if (ch == 'w') {
      wordForward();
      clampCursor();
      redraw();
      continue;
    }
    if (ch == 'b') {
      wordBack();
      clampCursor();
      redraw();
      continue;
    }
    if (ch == 'G') {
      curRow = (int)lines.size() - 1;
      clampCursor();
      redraw();
      continue;
    }
    if (ch == 'x') {
      if (!lines[curRow].empty() && curCol < (int)lines[curRow].size()) {
        lines[curRow].erase(curCol, 1);
        dirty = true;
        clampCursor();
      }
      redraw();
      continue;
    }

    // Comandos com buffer (dd, gg)
    cmdBuffer += (char)ch;
    if (cmdBuffer == "dd") {
      lines.erase(lines.begin() + curRow);
      if (lines.empty()) lines.push_back("");
      clampCursor();
      dirty = true;
      cmdBuffer = "";
    } else if (cmdBuffer == "gg") {
      curRow = 0;
      curCol = 0;
      cmdBuffer = "";
    } else if (cmdBuffer == "yy") {
      yankToClipboard(lines[curRow] + "\n");
      statusMsg = "yanked line";
      cmdBuffer = "";
    } else if (cmdBuffer.size() >= 2) {
      cmdBuffer = "";
    }

    if (dirty) {
      time_t now = time(nullptr);
      if (now - lastEcheck >= 1) {
        runEcheck();
        lastEcheck = now;
      }
    }

    redraw();
  }
}
#pragma once
#include <string>
#include <iostream>
#include <sstream>
#include <vector>

// Códigos ANSI
#define RED     "\033[1;31m"
#define YELLOW  "\033[1;33m"
#define CYAN    "\033[1;36m"
#define BOLD    "\033[1m"
#define DIM     "\033[2m"
#define RESET   "\033[0m"

// Modo echeck: coleta erros em vez de encerrar
inline bool g_echeckMode = false;
struct EcheckError {
  int line, col;
  std::string message;
};
inline std::vector<EcheckError> g_echeckErrors;
// ── Exibe erro estilo GCC/Clang e encerra o programa ─────────────────────────
// tokenLen: tamanho do token para os ^^^^
// hint: mensagem opcional em azul/cyan abaixo do ^^^^  (ex: "did you mean 'foo'?")
inline void reportError(const std::string& filename, int line, int col,
                        const std::string& message, const std::string& sourceLine,
                        int tokenLen = 1, const std::string& hint = "") {
    std::cerr << BOLD << filename << ":" << line << ":" << col << ": "
              << RED  << "error: " << RESET << BOLD << message << RESET << "\n";

    if (!sourceLine.empty()) {
        std::string lineStr = std::to_string(line);
        std::cerr << DIM << "  " << lineStr << " │ " << RESET << sourceLine << "\n";

        // Alinha a seta com o conteúdo da linha
        std::string pad(lineStr.size() + 3, ' ');
        std::cerr << DIM << pad << "│ " << RESET;

        // Espaços até a coluna (respeita tabs como 1 char)
        for (int i = 1; i < col; i++) std::cerr << ' ';

        // Os ^^^^ em vermelho
        std::cerr << RED;
        for (int i = 0; i < tokenLen; i++) std::cerr << '^';
        std::cerr << RESET << "\n";

        // Hint opcional em ciano abaixo dos ^^^^
        if (!hint.empty()) {
            std::cerr << DIM << pad << "│ " << RESET;
            for (int i = 1; i < col; i++) std::cerr << ' ';
            std::cerr << CYAN << hint << RESET << "\n";
        }
    }

    std::cerr << "\n";
    if (g_echeckMode) {
      g_echeckErrors.push_back({line, col, message});
      return;
    }
    exit(1);
}

// ── Emite warning (sem encerrar) ──────────────────────────────────────────────
inline void reportWarning(const std::string& filename, int line, int col,
                          const std::string& message, const std::string& sourceLine,
                          int tokenLen = 1, const std::string& hint = "") {
    std::cerr << BOLD << filename << ":" << line << ":" << col << ": "
              << YELLOW << "warning: " << RESET << BOLD << message << RESET << "\n";

    if (!sourceLine.empty()) {
        std::string lineStr = std::to_string(line);
        std::cerr << DIM << "  " << lineStr << " │ " << RESET << sourceLine << "\n";

        std::string pad(lineStr.size() + 3, ' ');
        std::cerr << DIM << pad << "│ " << RESET;
        for (int i = 1; i < col; i++) std::cerr << ' ';
        std::cerr << YELLOW;
        for (int i = 0; i < tokenLen; i++) std::cerr << '^';
        std::cerr << RESET << "\n";

        if (!hint.empty()) {
            std::cerr << DIM << pad << "│ " << RESET;
            for (int i = 1; i < col; i++) std::cerr << ' ';
            std::cerr << CYAN << hint << RESET << "\n";
        }
    }
    std::cerr << "\n";
}

// ── Formata e exibe erros de linker de forma legível ─────────────────────────
// Recebe a saída bruta do linker e a transforma em mensagens amigáveis.
// Deve ser chamado em main.cpp após system(linkCmd) retornar != 0.
inline void reportLinkerError(const std::string& rawOutput,
                               const std::string& outputFile,
                               const std::vector<std::string>& sourceFiles) {
    // Cabeçalho
    std::cerr << "\n" << BOLD << RED << "  ✗ linking failed" << RESET << "\n\n";

    if (rawOutput.empty()) {
        std::cerr << BOLD << "    note: " << RESET
                  << "the linker produced no output — make sure clang/gcc is installed.\n\n";
        return;
    }

    // Processa linha por linha
    std::istringstream ss(rawOutput);
    std::string ln;
    bool anyPrinted = false;

    while (std::getline(ss, ln)) {
        if (ln.empty()) continue;

        // ── Undefined symbol / undefined reference ─────────────────────────
        if (ln.find("undefined reference to") != std::string::npos ||
            ln.find("undefined symbol") != std::string::npos ||
            ln.find("unresolved external") != std::string::npos) {

            // Extrai o nome do símbolo entre aspas
            std::string sym;
            size_t q1 = ln.find('`');
            size_t q2 = ln.rfind('\'');
            size_t qq1 = ln.find('"');
            size_t qq2 = ln.rfind('"');
            if (q1 != std::string::npos && q2 != std::string::npos && q2 > q1)
                sym = ln.substr(q1 + 1, q2 - q1 - 1);
            else if (qq1 != std::string::npos && qq2 != std::string::npos && qq2 > qq1)
                sym = ln.substr(qq1 + 1, qq2 - qq1 - 1);

            std::cerr << BOLD << "  error: " << RESET
                      << "function or symbol ";
            if (!sym.empty())
                std::cerr << BOLD << "'" << sym << "'" << RESET << " ";
            std::cerr << "is used but never defined.\n";

            if (!sym.empty()) {
                std::cerr << CYAN << "    hint: " << RESET
                          << "did you forget to implement '" << sym
                          << "' in one of your .npp files, or to pass that file to n++?\n";
                std::cerr << CYAN << "    hint: " << RESET
                          << "if this is a stdlib function, check that the matching "
                          << "#include <...> is present in your source.\n";
            }
            std::cerr << "\n";
            anyPrinted = true;
            continue;
        }

        // ── Multiple definition / duplicate symbol ─────────────────────────
        if (ln.find("multiple definition") != std::string::npos ||
            ln.find("duplicate symbol") != std::string::npos ||
            ln.find("already defined") != std::string::npos) {

            std::string sym;
            size_t q1 = ln.find('`'); size_t q2 = ln.rfind('\'');
            if (q1 != std::string::npos && q2 > q1)
                sym = ln.substr(q1 + 1, q2 - q1 - 1);

            std::cerr << BOLD << "  error: " << RESET
                      << "function or variable ";
            if (!sym.empty())
                std::cerr << BOLD << "'" << sym << "'" << RESET << " ";
            std::cerr << "is defined more than once across your source files.\n";
            std::cerr << CYAN << "    hint: " << RESET
                      << "each function can only be defined in one .npp file. "
                      << "Use a .nh header to share the signature.\n\n";
            anyPrinted = true;
            continue;
        }

        // ── Cannot find / No such file (library) ──────────────────────────
        if (ln.find("cannot find") != std::string::npos ||
            ln.find("no such file") != std::string::npos ||
            ln.find("library not found") != std::string::npos) {

            std::cerr << BOLD << "  error: " << RESET
                      << "the linker could not find a required library or object file.\n";
            std::cerr << DIM  << "    " << ln << RESET << "\n";
            std::cerr << CYAN << "    hint: " << RESET
                      << "make sure all .npp files are passed to n++ and that "
                      << "your NOVA_STDLIB_PATH is set correctly.\n\n";
            anyPrinted = true;
            continue;
        }

        // ── MinGW / Windows cross-compile not installed ────────────────────
        if (ln.find("x86_64-w64-mingw32") != std::string::npos &&
            (ln.find("not found") != std::string::npos ||
             ln.find("No such file") != std::string::npos ||
             ln.find("command not found") != std::string::npos)) {

            std::cerr << BOLD << "  error: " << RESET
                      << "cross-compilation to Windows requires the MinGW toolchain.\n";
            std::cerr << CYAN << "    hint: " << RESET
                      << "on Ubuntu/Debian: sudo apt install mingw-w64\n";
            std::cerr << CYAN << "    hint: " << RESET
                      << "on Arch: sudo pacman -S mingw-w64-gcc\n\n";
            anyPrinted = true;
            continue;
        }

        // ── Entry point missing (no main) ─────────────────────────────────
        if (ln.find("entry point") != std::string::npos ||
            ln.find("_start") != std::string::npos ||
            (ln.find("main") != std::string::npos &&
             (ln.find("undefined") != std::string::npos ||
              ln.find("not defined") != std::string::npos))) {

            std::cerr << BOLD << "  error: " << RESET
                      << "no 'main' function found — every executable must define 'int main()'.\n";
            std::cerr << CYAN << "    hint: " << RESET
                      << "add 'int main() { ... }' to one of your .npp files.\n";
            std::cerr << CYAN << "    hint: " << RESET
                      << "if you are building a library (.so/.dll/.a), "
                      << "this error is expected — the output file name may be wrong.\n\n";
            anyPrinted = true;
            continue;
        }

        // ── ELF/COFF format mismatch ───────────────────────────────────────
        if (ln.find("file format") != std::string::npos ||
            ln.find("incompatible") != std::string::npos) {
            std::cerr << BOLD << "  error: " << RESET
                      << "object file format mismatch — mixing Windows and Linux targets.\n";
            std::cerr << CYAN << "    hint: " << RESET
                      << "compile ALL source files with the same target. "
                      << "Use '-o output.exe' to target Windows for ALL files.\n\n";
            anyPrinted = true;
            continue;
        }

        // ── Linha genérica: imprime com recuo se parecer relevante ─────────
        if (ln.find("error:") != std::string::npos ||
            ln.find("Error:") != std::string::npos) {
            std::cerr << BOLD << "  error: " << RESET << DIM << ln << RESET << "\n\n";
            anyPrinted = true;
        }
    }

    // Se não reconheceu nenhum padrão, dump da saída raw resumida
    if (!anyPrinted) {
        std::cerr << BOLD << "  linker output:\n" << RESET;
        std::istringstream ss2(rawOutput);
        int printed = 0;
        while (std::getline(ss2, ln) && printed < 20) {
            if (!ln.empty()) {
                std::cerr << DIM << "    " << ln << RESET << "\n";
                printed++;
            }
        }
        if (printed == 20)
            std::cerr << DIM << "    ... (truncated)\n" << RESET;
        std::cerr << "\n";
    }

    std::cerr << BOLD << "  note: " << RESET
              << "compiled objects were: ";
    for (size_t i = 0; i < sourceFiles.size(); i++) {
        if (i > 0) std::cerr << ", ";
        std::cerr << sourceFiles[i];
    }
    std::cerr << "\n";
    std::cerr << BOLD << "  note: " << RESET
              << "target output was: " << outputFile << "\n\n";
}
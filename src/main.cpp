#include <iostream>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <cstring>
#include <string>
#include "../include/editor.h"
#include <vector>
#include <set>
#include <algorithm>
#include <unistd.h>   // getcwd
#include "../include/ast.h"
#include "../include/codegen.h"
#include "../include/error.h"
#include "../include/analysis.h"

ProgramNode parseProgram(const std::string& source, const std::string& filename);

// ── Cores ANSI (para output fora do editor) ───────────────────────────────────
#define ANSI_RESET   "\033[0m"
#define ANSI_BOLD    "\033[1m"
#define ANSI_RED     "\033[1;31m"
#define ANSI_YELLOW  "\033[1;33m"
#define ANSI_CYAN    "\033[1;36m"
#define ANSI_BLUE    "\033[1;34m"
#define ANSI_GRAY    "\033[0;90m"
#define ANSI_WHITE   "\033[1;37m"

static void printError(const std::string& msg) {
    std::cerr << ANSI_RED << "  ✗ error: " << ANSI_RESET << ANSI_BOLD << msg << ANSI_RESET << "\n";
}

// ── --help ────────────────────────────────────────────────────────────────────
static void printHelp() {
    std::cout << ANSI_BOLD << "Nova Compiler - Beta 2.2.0 \n" << ANSI_RESET;
    std::cout << "\n";
    std::cout << ANSI_BOLD << "Usage:\n" << ANSI_RESET;
    std::cout << "  n++ <file.npp> [file2.npp ...] [options]\n";
    std::cout << "  n++ -w [file.npp]\n";
    std::cout << "  n++ --version\n";
    std::cout << "  n++ --help\n";
    std::cout << "\n";
    std::cout << ANSI_BOLD << "Options:\n" << ANSI_RESET;
    std::cout << ANSI_CYAN << "  -o <out>      " << ANSI_RESET << "Set output file name (.exe/.dll → Windows PE32+)\n";
    std::cout << ANSI_CYAN << "  -O2           " << ANSI_RESET << "Enable level 2 optimizations\n";
    std::cout << ANSI_CYAN << "  -O3           " << ANSI_RESET << "Enable level 3 optimizations (aggressive)\n";
    std::cout << ANSI_CYAN << "  -w [file]     " << ANSI_RESET << "Open interactive editor in terminal\n";
    std::cout << ANSI_CYAN << "  --version     " << ANSI_RESET << "Show compiler version\n";
    std::cout << ANSI_CYAN << "  --help        " << ANSI_RESET << "Show this help message\n";
    std::cout << "\n";
    std::cout << ANSI_BOLD << "Examples:\n" << ANSI_RESET;
    std::cout << ANSI_GRAY << "  n++ hello.npp\n";
    std::cout << "  n++ hello.npp -o hello -O2\n";
    std::cout << "  n++ hello.npp -o hello.exe        # cross-compile to Windows\n";
    std::cout << "  n++ hello.npp -o hello.dll        # Windows DLL\n";
    std::cout << "  n++ -w                            # new file\n";
    std::cout << "  n++ -w existing.npp               # edit existing file\n" << ANSI_RESET;
    std::cout << "\n";
    std::cout << ANSI_BOLD << "Editor shortcuts (-w mode):\n" << ANSI_RESET;
    std::cout << ANSI_GRAY << "  Ctrl+S   Save and compile\n";
    std::cout << "  Ctrl+Q   Quit without compiling\n";
    std::cout << "  Ctrl+X   Save, compile and exit\n";
    std::cout << "  Arrow keys / Home / End / PgUp / PgDn to navigate\n" << ANSI_RESET;
    std::cout << "\n";
}

// ── Detecta se o output alvo é Windows ───────────────────────────────────────
static bool isWindowsTarget(const std::string& outFile) {
    auto dot = outFile.rfind('.');
    if (dot == std::string::npos) return false;
    std::string ext = outFile.substr(dot);
    return (ext == ".exe" || ext == ".dll");
}

// ── Link command builder ──────────────────────────────────────────────────────
// objFiles: lista de todos os .o gerados
static std::string buildLinkCmd(const std::vector<std::string>& objFiles,
                                const std::string& outFile) {
    std::string ext;
    auto dot = outFile.rfind('.');
    if (dot != std::string::npos) ext = outFile.substr(dot);

    std::string objs;
    for (auto& o : objFiles) objs += " " + o;

    // ── Alvo Windows: .exe ou .dll → usa MinGW (gera PE32+)
    if (ext == ".exe" || ext == ".dll") {
        std::string cmd = "x86_64-w64-mingw32-gcc -m64" + objs;
        if (ext == ".dll") cmd += " -shared";
        cmd += " -o " + outFile;
        cmd += " -lmingw32 -lmingwex -lmsvcrt";
        return cmd;
    }

    // ── Alvo host (Linux/macOS)

    #ifdef _WIN32
        std::string cmd = "gcc -m64" + objs;
    #else
        std::string cmd = "clang -m64" + objs;
    #endif
    if (ext == ".so" || ext == ".dylib") {
        cmd += " -shared";
    } else if (ext == ".a") {
        return "ar rcs " + outFile + objs;
    }
    cmd += " -o " + outFile;
    return cmd;
}

// ── Detecta stdlib includes nos arquivos fonte ────────────────────────────────
// Varre cada .npp buscando linhas "#include <nome.nh>" e retorna os .npp
// correspondentes em NOVA_STDLIB_PATH que ainda não estão na lista de arquivos.
static std::vector<std::string> detectStdlibModules(
        const std::vector<std::string>& sourceFiles) {

    const char* envPath = std::getenv("NOVA_STDLIB_PATH");
    std::string stdlibDir = envPath ? std::string(envPath) : "/usr/local/lib/nova";

    std::vector<std::string> result;
    std::set<std::string> added; // evita duplicatas

    // Pré-popula com os arquivos já na lista (não queremos readicionar)
    for (auto& s : sourceFiles) added.insert(s);

    for (auto& src : sourceFiles) {
        std::ifstream f(src);
        if (!f) continue;
        std::string line;
        while (std::getline(f, line)) {
            // Busca padrão: #include <nome.nh>
            size_t hash = line.find('#');
            if (hash == std::string::npos) continue;
            size_t inc = line.find("include", hash + 1);
            if (inc == std::string::npos) continue;
            size_t lt = line.find('<', inc + 7);
            size_t gt = line.find('>', lt == std::string::npos ? 0 : lt + 1);
            if (lt == std::string::npos || gt == std::string::npos) continue;

            std::string headerName = line.substr(lt + 1, gt - lt - 1);
            // Troca extensão .nh → .npp
            std::string moduleName = headerName;
            if (moduleName.size() > 3 &&
                moduleName.substr(moduleName.size() - 3) == ".nh")
                moduleName = moduleName.substr(0, moduleName.size() - 3) + ".npp";

            std::string modulePath = stdlibDir + "/" + moduleName;

            // Só adiciona se o arquivo existir e ainda não estiver na lista
            if (added.count(modulePath) == 0) {
                std::ifstream check(modulePath);
                if (check.good()) {
                    result.push_back(modulePath);
                    added.insert(modulePath);
                }
            }
        }
    }
    return result;
}

// ── Compila um único arquivo .npp para .o ─────────────────────────────────────
static std::string compileSingleFile(const std::string& srcFile, int optLevel,
                                     bool winTarget, const std::string& objOut) {
    std::ifstream file(srcFile);
    if (!file) { printError("file '" + srcFile + "' not found"); return ""; }
    std::stringstream buf;
    buf << file.rdbuf();

    ProgramNode program = parseProgram(buf.str(), srcFile);
    runAnalysis(program, srcFile);
    codegenProgram(program, objOut, srcFile, optLevel, winTarget);
    return objOut;
}

// Remove códigos ANSI da string
auto stripAnsi = [](const std::string& s) -> std::string {
  std::string out;
  size_t i = 0;
  while (i < s.size()) {
    if (s[i] == '\033' && i + 1 < s.size() && s[i + 1] == '[') {
      i += 2;
      while (i < s.size() && s[i] != 'm') i++;
      if (i < s.size()) i++;
    } else {
      out += s[i++];
    }
  }
  return out;
};
// ── Compilação silenciosa (sem output a não ser erros/warnings) ───────────────
int compileFile(const std::vector<std::string>& sourceFiles,
                       const std::string& outputFileArg, int optLevel) {
    if (sourceFiles.empty()) { printError("no source files specified"); return 1; }

    // Determina o nome de saída
    std::string outputFile = outputFileArg;
    if (outputFile.empty()) {
        // Usa o nome do primeiro arquivo sem extensão
        outputFile = sourceFiles[0];
        auto dot = outputFile.rfind('.');
        if (dot != std::string::npos) outputFile = outputFile.substr(0, dot);
    }

    bool winTarget = isWindowsTarget(outputFile);

    // Detecta e injeta automaticamente os .npp da stdlib referenciados via #include <...>
    std::vector<std::string> allFiles = sourceFiles;
    std::vector<std::string> stdlibModules = detectStdlibModules(sourceFiles);
    for (auto& m : stdlibModules)
        allFiles.push_back(m);

    // Compila cada arquivo fonte para seu próprio .o
    std::vector<std::string> objFiles;
    bool projectHasMain = false;
    for (auto& src : allFiles) {
        // .o fica em /tmp para não poluir o diretório do usuário
        std::string base = src;
        // Remove path e extensão para o nome do .o
        auto slash = base.rfind('/');
        if (slash != std::string::npos) base = base.substr(slash + 1);
        auto dot = base.rfind('.');
        if (dot != std::string::npos) base = base.substr(0, dot);
        #ifdef _WIN32
            std::string objPath = std::string(getenv("TEMP") ? getenv("TEMP") : "C:/tmp") + "/nova_" + base + "_" + std::to_string(objFiles.size()) + ".o";
        #else
            std::string objPath = "/tmp/nova_" + base + "_" + std::to_string(objFiles.size()) + ".o";
        #endif

        std::string obj = compileSingleFile(src, optLevel, winTarget, objPath);
        if (obj.empty()) return 1;
        objFiles.push_back(obj);

        // Checa se este arquivo continha main (lendo o fonte novamente é caro;
        // compileSingleFile já rodou — o codegen sabe via isLibraryFile)
        // Mais simples: fazemos uma passagem rápida pelo fonte buscando "void main" ou "int main"
        {
            std::ifstream f(src);
            std::string content((std::istreambuf_iterator<char>(f)),
                                 std::istreambuf_iterator<char>());
            if (content.find("main") != std::string::npos)
                projectHasMain = true;
        }
    }

    // Emite warnings de funções definidas mas nunca chamadas em todo o projeto
    flushFunctionWarnings(projectHasMain);

    // Linka todos os .o juntos — captura stderr do linker para exibir erros amigáveis
    std::string linkCmd = buildLinkCmd(objFiles, outputFile);

    // Redireciona stderr do linker para arquivo temporário
    // Linha 242-243, troca por:
    #ifdef _WIN32
        std::string linkerErrFile = std::string(getenv("TEMP") ? getenv("TEMP") : "C:/tmp") + "/nova_linker_" + std::to_string(getpid()) + ".txt";
        std::string fullCmd = "C:/msys64/mingw64/bin/gcc.exe -m64";
        for (auto& o : objFiles) fullCmd += " " + o;
        // rebuild linkCmd com caminho absoluto
        std::string linkCmd2 = fullCmd + " -o " + outputFile;
        int ret = system((linkCmd2 + " 2>" + linkerErrFile).c_str());
    #else
        std::string linkerErrFile = "/tmp/nova_linker_" + std::to_string(getpid()) + ".txt";
        int ret = system((linkCmd + " 2>" + linkerErrFile).c_str());
    #endif

    // Remove os .o temporários
    for (auto& o : objFiles) std::remove(o.c_str());

    if (ret != 0) {
        std::string linkerOutput;
        {
            std::ifstream ef(linkerErrFile);
            if (ef) { std::stringstream eb; eb << ef.rdbuf(); linkerOutput = eb.str(); }
        }
        std::remove(linkerErrFile.c_str());
        reportLinkerError(linkerOutput, outputFile, allFiles);
        return 1;
    }
    std::remove(linkerErrFile.c_str());
    return 0;
}

// ═════════════════════════════════════════════════════════════════════════════
// MAIN
// ═════════════════════════════════════════════════════════════════════════════
int main(int argc, char* argv[]) {
    if (argc < 2) { std::cout << "\033[1;31mFatal Error:\033[0m No file specified\n"; return 1; }
    if (std::string(argv[1]) == "--echeck") {
      if (argc < 3) {
        std::cerr << "usage: n++ --echeck file.npp\n";
        return 1;
      }
      std::string file = argv[2];
      std::string tmpErr =
          "/tmp/nova_echeck_" + std::to_string(getpid()) + ".txt";
      std::string cmd = "n++ " + file + " -o /tmp/nova_echeck_out_" +
                        std::to_string(getpid()) + " 2>" + tmpErr;
      system(cmd.c_str());
      std::ifstream ef(tmpErr);
      std::string ln;
      while (std::getline(ef, ln)) {
        // Formato: file:line:col: error: mensagem
        // Extrai só o que precisamos
        size_t p1 = ln.find(':');
        if (p1 == std::string::npos) continue;
        size_t p2 = ln.find(':', p1 + 1);
        if (p2 == std::string::npos) continue;
        size_t p3 = ln.find(':', p2 + 1);
        if (p3 == std::string::npos) continue;
        std::string lineN = ln.substr(p1 + 1, p2 - p1 - 1);
        std::string colN = ln.substr(p2 + 1, p3 - p2 - 1);
        size_t errPos = ln.find("error: ", p3);
        if (errPos == std::string::npos) continue;
        std::string msg = ln.substr(errPos + 7);
        std::cout << lineN << ":" << colN << ":error:" << stripAnsi(msg)
                  << "\n";
      }
      std::remove(tmpErr.c_str());
      return 0;
    }

    if (std::string(argv[1]) == "--version") {
        std::cout << ANSI_BOLD << "Nova Compiler" << ANSI_RESET 
          << " - Beta 2.2.0 (Build " << NOVA_BUILD_VERSION << ")\n";
        return 0;
    }
    if (std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h") {
        printHelp(); return 0;
    }

    std::vector<std::string> sourceFiles;
    std::string outputFile;
    int  optLevel   = 0;
    bool editorMode = false;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if      (arg == "-O2") { optLevel = 2; }
        else if (arg == "-O3") { optLevel = 3; }
        else if (arg == "-o") {
            if (i + 1 >= argc) { printError("'-o' requires an output filename"); return 1; }
            outputFile = argv[++i];
        } else if (arg == "-w") {
            editorMode = true;
        } else if (arg[0] == '-') {
            printError("unknown argument '" + arg + "'");
            return 1;
        } else {
            sourceFiles.push_back(arg);
        }
    }

    if (editorMode) {
        std::string editFile = sourceFiles.empty() ? "" : sourceFiles[0];
        runEditor(editFile, outputFile, optLevel);
        return 0;
    }

    if (sourceFiles.empty()) { printError("no source file specified"); return 1; }
    return compileFile(sourceFiles, outputFile, optLevel);
}

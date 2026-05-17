#include "ui.h"
#include "project.h"
#include "builder.h"
#include "package.h"
#include "downloader.h"

#include <iostream>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <algorithm>
#include <cstring>

namespace fs = std::filesystem;

#ifndef ORBIT_VERSION
  #define ORBIT_VERSION "0.1.0"
#endif

// ── Helpers ───────────────────────────────────────────────────────────────────

static void print_usage() {
    ui::header("Orbit " ORBIT_VERSION, "Build system & package manager for the Nova language");

    printf("  Usage: orbit <command> [options]\n\n");

    printf("  Commands:\n\n");

    auto cmd = [](const char* name, const char* desc) {
        printf("    \033[1;36m%-18s\033[0m %s\n", name, desc);
    };

    cmd("init [name]",       "Create a new Nova project");
    cmd("init --lib [name]", "Create a new Nova library");
    cmd("build",             "Build the project (debug)");
    cmd("build --release",   "Build with optimizations");
    cmd("run [args...]",     "Build and run the project");
    cmd("check",             "Check project without building");
    cmd("fetch",             "Download all dependencies (no build)");
    cmd("pack",              "Package project as .orbit file");
    cmd("add <dep>",         "Add a dependency (local path or URL)");
    cmd("remove <dep>",      "Remove a dependency");
    cmd("clean",             "Remove build artifacts");
    cmd("info",              "Show project information");
    cmd("deps",              "Show dependency tree");
    cmd("version",           "Show orbit version");
    cmd("help",              "Show this help");

    printf("\n  Options:\n\n");
    printf("    \033[1;36m%-18s\033[0m %s\n", "--release",   "Use release profile");
    printf("    \033[1;36m%-18s\033[0m %s\n", "--prefix DIR","Installation prefix (init)");
    printf("\n");
}

static void print_version() {
    printf("\033[1;36morbit\033[0m %s  (Nova build system)\n", ORBIT_VERSION);
}

// Find project root by traversing up from cwd
static std::string find_project_root() {
    fs::path p = fs::current_path();
    for (int i = 0; i < 10; i++) {
        if (fs::exists(p / "orbit.toml")) return p.string();
        fs::path parent = p.parent_path();
        if (parent == p) break;
        p = parent;
    }
    return ""; // not found
}

// ── Command: init ─────────────────────────────────────────────────────────────

static int cmd_init(const std::vector<std::string>& args) {
    bool is_lib   = false;
    std::string name;

    for (const auto& a : args) {
        if (a == "--lib")  is_lib = true;
        else if (a[0] != '-') name = a;
    }

    // Determine target directory and name
    fs::path target;
    if (name.empty()) {
        // Use current directory name as project name
        target = fs::current_path();
        name   = target.filename().string();
    } else {
        target = fs::current_path() / name;
    }

    std::string type = is_lib ? "library" : "binary";

    ui::header("orbit init", "Creating new " + type + " project: " + name);

    // Check if already initialized
    if (fs::exists(target / "orbit.toml")) {
        ui::warn("orbit.toml already exists. Skipping manifest creation.");
    } else {
        // Create directory structure
        if (!fs::exists(target)) {
            fs::create_directories(target);
            ui::info("Created directory: " + target.string());
        }
        fs::create_directories(target / "src");
        fs::create_directories(target / "include");

        // Write manifest
        write_default_manifest(target.string(), name, type);
        ui::success("Created orbit.toml");
    }

    // Create example source file
    fs::path src_file;
    if (is_lib) {
        src_file = target / "src" / "lib.npp";
    } else {
        src_file = target / "src" / "main.npp";
    }

    if (!fs::exists(src_file)) {
        std::ofstream f(src_file);
        if (is_lib) {
            f << "// " << name << " — Nova library\n\n";
            f << "fn add(a: i64, b: i64) -> i64 {\n";
            f << "    return a + b;\n";
            f << "}\n";
        } else {
            f << "#include <stdio.nh>\n\n";
            f << "fn main() {\n";
            f << "    std::printf(\"Hello from " << name << "!\\n\");\n";
            f << "}\n";
        }
        ui::success("Created " + src_file.string());
    }

    // Create .gitignore
    fs::path gitignore = target / ".gitignore";
    if (!fs::exists(gitignore)) {
        std::ofstream f(gitignore);
        f << "build/\n.orbit/\n*.orbit\n";
        ui::success("Created .gitignore");
    }

    ui::blank();
    ui::divider();
    printf("\n");
    ui::print_kv("Project:", name);
    ui::print_kv("Type:",    type);
    ui::print_kv("Path:",    target.string());
    printf("\n");

    printf("  \033[0;90mGet started:\033[0m\n\n");
    if (name != fs::current_path().filename().string()) {
        printf("    \033[1;36mcd %s\033[0m\n", name.c_str());
    }
    printf("    \033[1;36morbit build\033[0m\n");
    if (!is_lib) {
        printf("    \033[1;36morbit run\033[0m\n");
    }
    printf("\n");

    return 0;
}

// ── Command: build ────────────────────────────────────────────────────────────

static int cmd_build(const std::vector<std::string>& args) {
    bool release = false;
    for (const auto& a : args)
        if (a == "--release") release = true;

    std::string root = find_project_root();
    if (root.empty()) {
        ui::error("Not inside a Nova project. Run 'orbit init' to create one.");
        return 1;
    }

    Manifest m;
    try {
        m = load_manifest(root);
    } catch (const std::exception& e) {
        ui::error(e.what());
        return 1;
    }

    BuildMode mode = release ? BuildMode::Release : BuildMode::Dev;
    std::string profile_name = release ? "release" : "dev";

    ui::header("orbit build",
               m.name + " v" + m.version + "  [" + profile_name + "]");

    BuildResult res;
    try {
        res = build_project(m, mode);
    } catch (const std::exception& e) {
        ui::error(e.what());
        return 1;
    }

    ui::blank();
    ui::divider();
    if (res.success) {
        ui::success("Build finished in " +
                    std::to_string((int)(res.elapsed_secs * 1000)) + "ms");
        ui::print_kv("Output:", res.output_path);
    } else {
        ui::error("Build failed (exit code " + std::to_string(res.exit_code) + ")");
    }
    printf("\n");

    return res.success ? 0 : 1;
}

// ── Command: run ──────────────────────────────────────────────────────────────

static int cmd_run(const std::vector<std::string>& args) {
    bool release = false;
    std::vector<std::string> run_args;
    bool past_double_dash = false;

    for (const auto& a : args) {
        if (a == "--") { past_double_dash = true; continue; }
        if (past_double_dash) run_args.push_back(a);
        else if (a == "--release") release = true;
        else run_args.push_back(a);
    }

    // Build first
    std::vector<std::string> build_args;
    if (release) build_args.push_back("--release");
    int ret = cmd_build(build_args);
    if (ret != 0) return ret;

    std::string root = find_project_root();
    Manifest m = load_manifest(root);

    if (m.is_library()) {
        ui::warn("Libraries cannot be run directly. Build succeeded.");
        return 0;
    }

    ui::blank();
    ui::info("Running " + m.name + "...");
    ui::divider();
    ui::blank();

    return run_project(m, run_args);
}

// ── Command: check ────────────────────────────────────────────────────────────

static int cmd_check(const std::vector<std::string>& /*args*/) {
    std::string root = find_project_root();
    if (root.empty()) {
        ui::error("Not inside a Nova project.");
        return 1;
    }
    try {
        Manifest m = load_manifest(root);
        ui::header("orbit check", m.name + " v" + m.version);
        ui::print_kv("Project root:",  m.project_root);
        ui::print_kv("Entry point:",   m.entry_point);
        ui::print_kv("Type:",          m.type);
        ui::print_kv("Output:",        m.output_name);
        ui::print_kv("Dependencies:",  std::to_string(m.dependencies.size()));
        ui::blank();

        // Check entry point exists
        if (!fs::exists(fs::path(root) / m.entry_point)) {
            ui::warn("Entry point not found: " + m.entry_point);
        } else {
            ui::success("Entry point found");
        }

        // Check n++ is in PATH
        if (std::system("command -v n++ >/dev/null 2>&1") == 0) {
            ui::success("n++ compiler found");
        } else {
            ui::warn("n++ compiler not found in PATH");
        }

        ui::success("Manifest is valid");
    } catch (const std::exception& e) {
        ui::error(e.what());
        return 1;
    }
    printf("\n");
    return 0;
}

// ── Command: pack ─────────────────────────────────────────────────────────────

static int cmd_pack(const std::vector<std::string>& /*args*/) {
    std::string root = find_project_root();
    if (root.empty()) {
        ui::error("Not inside a Nova project.");
        return 1;
    }

    Manifest m;
    try {
        m = load_manifest(root);
    } catch (const std::exception& e) {
        ui::error(e.what());
        return 1;
    }

    ui::header("orbit pack", "Packaging " + m.name + " v" + m.version);

    try {
        std::string out = pack_project(m, root);
        ui::success("Package created: " + out);
        ui::blank();
        ui::info("Share this file or host it at a URL to distribute your package.");
        ui::info("Users can add it with:");
        printf("\n    \033[1;36morbit add %s = { url = \"https://…/%s\" }\033[0m\n\n",
               m.name.c_str(),
               fs::path(out).filename().string().c_str());
    } catch (const std::exception& e) {
        ui::error(e.what());
        return 1;
    }
    return 0;
}

// ── Command: add ─────────────────────────────────────────────────────────────

static int cmd_add(const std::vector<std::string>& args) {
    if (args.empty()) {
        ui::error("Usage: orbit add <name> <path-or-url>");
        ui::blank();
        printf("  Examples:\n");
        printf("    \033[1;36morbit add GML ./GML-1.0.0.orbit\033[0m\n");
        printf("    \033[1;36morbit add GML https://example.com/GML.orbit\033[0m\n");
        printf("  Or let orbit infer the name from the filename:\n");
        printf("    \033[1;36morbit add ./GML-1.0.0.orbit\033[0m\n\n");
        return 1;
    }

    std::string root = find_project_root();
    if (root.empty()) {
        ui::error("Not inside a Nova project.");
        return 1;
    }

    std::string dep_name;
    std::string dep_source;

    // Smart single-arg mode: if the argument looks like a path or URL,
    // infer the package name from the filename stem.
    //   orbit add ./GML-1.0.0.orbit   -> name=GML
    //   orbit add https://.../foo.orbit -> name=foo
    if (args.size() == 1) {
        dep_source = args[0];
        bool looks_like_path = (!dep_source.empty()) &&
            (dep_source[0] == '.' || dep_source[0] == '/' ||
             dep_source.find("://") != std::string::npos);
        if (!looks_like_path) {
            ui::error("'" + dep_source + "' is not a path or URL.");
            ui::blank();
            printf("  Did you mean:\n");
            printf("    \033[1;36morbit add %s <path-or-url>\033[0m\n\n", dep_source.c_str());
            printf("  Or to infer the name automatically:\n");
            printf("    \033[1;36morbit add ./%s.orbit\033[0m\n\n", dep_source.c_str());
            return 1;
        }
        // Strip extension (.orbit) then version suffix (-1.0.0)
        fs::path p(dep_source);
        std::string stem = p.stem().string();
        size_t dash = stem.find('-');
        if (dash != std::string::npos && dash + 1 < stem.size() &&
            std::isdigit((unsigned char)stem[dash + 1])) {
            stem = stem.substr(0, dash);
        }
        dep_name = stem;
        ui::info("Package name inferred as: \033[1m" + dep_name + "\033[0m");
    } else {
        dep_name   = args[0];
        dep_source = args[1];
    }


    // Read current orbit.toml
    fs::path toml_path = fs::path(root) / "orbit.toml";
    std::ifstream f_in(toml_path);
    std::ostringstream buf;
    buf << f_in.rdbuf();
    std::string content = buf.str();
    f_in.close();

    // Check if already added
    if (content.find("\"" + dep_name + "\"") != std::string::npos ||
        content.find(dep_name + " =")  != std::string::npos) {
        ui::warn("Dependency '" + dep_name + "' already exists in orbit.toml.");
        ui::info("Edit orbit.toml manually to update it.");
        return 1;
    }

    // Build the TOML entry
    std::string entry;
    bool is_url = dep_source.find("://") != std::string::npos;
    if (is_url) {
        entry = dep_name + " = { url = \"" + dep_source + "\" }\n";
    } else {
        entry = dep_name + " = { local = \"" + dep_source + "\" }\n";
    }

    // Append to [dependencies] section
    size_t dep_pos = content.find("[dependencies]");
    if (dep_pos == std::string::npos) {
        content += "\n[dependencies]\n" + entry;
    } else {
        // Insert after [dependencies] header (and any existing entries)
        // Find next section or end of file
        size_t next_section = content.find("\n[", dep_pos + 1);
        if (next_section == std::string::npos) {
            content += entry;
        } else {
            content.insert(next_section, "\n" + entry);
        }
    }

    std::ofstream f_out(toml_path);
    f_out << content;
    f_out.close();

    ui::success("Added dependency: " + dep_name);
    if (is_url) {
        ui::print_kv("URL:", dep_source);
    } else {
        ui::print_kv("Path:", dep_source);
    }
    ui::info("Run 'orbit build' to fetch and compile.");
    printf("\n");
    return 0;
}

// ── Command: remove ───────────────────────────────────────────────────────────

static int cmd_remove(const std::vector<std::string>& args) {
    if (args.empty()) {
        ui::error("Usage: orbit remove <name>");
        return 1;
    }

    std::string root = find_project_root();
    if (root.empty()) {
        ui::error("Not inside a Nova project.");
        return 1;
    }

    std::string dep_name = args[0];
    fs::path toml_path   = fs::path(root) / "orbit.toml";

    std::ifstream f_in(toml_path);
    std::string line;
    std::vector<std::string> lines;
    bool found = false;
    while (std::getline(f_in, line)) {
        // Remove lines that start with dep_name =
        std::string trimmed = line;
        size_t a = trimmed.find_first_not_of(" \t");
        if (a != std::string::npos) trimmed = trimmed.substr(a);
        if (trimmed.find(dep_name + " =") == 0 ||
            trimmed.find("\"" + dep_name + "\"") == 0) {
            found = true;
            continue; // skip this line
        }
        lines.push_back(line);
    }
    f_in.close();

    if (!found) {
        ui::warn("Dependency '" + dep_name + "' not found in orbit.toml.");
        return 1;
    }

    std::ofstream f_out(toml_path);
    for (const auto& l : lines) f_out << l << "\n";

    // Remove installed package directory
    fs::path dep_dir = fs::path(root) / ".orbit" / "deps" / dep_name;
    if (fs::exists(dep_dir)) {
        fs::remove_all(dep_dir);
        ui::info("Removed cached package: " + dep_name);
    }

    ui::success("Removed dependency: " + dep_name);
    printf("\n");
    return 0;
}

// ── Command: clean ────────────────────────────────────────────────────────────

static int cmd_clean(const std::vector<std::string>& /*args*/) {
    std::string root = find_project_root();
    if (root.empty()) {
        ui::error("Not inside a Nova project.");
        return 1;
    }

    Manifest m;
    try {
        m = load_manifest(root);
    } catch (const std::exception& e) {
        ui::error(e.what());
        return 1;
    }

    ui::header("orbit clean", "Cleaning " + m.name);

    fs::path build = fs::path(root) / "build";
    if (fs::exists(build)) {
        fs::remove_all(build);
        ui::success("Removed build/");
    } else {
        ui::info("Nothing to clean.");
    }
    printf("\n");
    return 0;
}

// ── Command: info ─────────────────────────────────────────────────────────────

static int cmd_info(const std::vector<std::string>& /*args*/) {
    std::string root = find_project_root();
    if (root.empty()) {
        ui::error("Not inside a Nova project.");
        return 1;
    }

    Manifest m;
    try {
        m = load_manifest(root);
    } catch (const std::exception& e) {
        ui::error(e.what());
        return 1;
    }

    ui::header("orbit info", m.name + " v" + m.version);
    ui::print_kv("Name:",         m.name);
    ui::print_kv("Version:",      m.version);
    ui::print_kv("Author:",       m.author.empty() ? "(not set)" : m.author);
    ui::print_kv("Description:",  m.description.empty() ? "(not set)" : m.description);
    ui::print_kv("License:",      m.license);
    ui::print_kv("Type:",         m.type);
    ui::print_kv("Entry point:",  m.entry_point);
    ui::print_kv("Output:",       m.output_name);
    ui::print_kv("Project root:", m.project_root);
    printf("\n");

    if (!m.dependencies.empty()) {
        ui::divider();
        printf("  \033[1mDependencies (%zu)\033[0m\n\n", m.dependencies.size());
        for (const auto& d : m.dependencies) {
            if (d.is_url()) {
                ui::print_kv("  " + d.name + ":", "(url) " + d.url);
            } else if (d.is_local()) {
                ui::print_kv("  " + d.name + ":", "(local) " + d.local_path);
            } else {
                ui::print_kv("  " + d.name + ":", d.version);
            }
        }
        printf("\n");
    }

    return 0;
}

// ── Command: deps ─────────────────────────────────────────────────────────────

// Print one dependency row, then recursively print its transitive deps
static void print_dep_tree(const std::string& dep_root,
                            const std::string& name,
                            const std::string& version,
                            const std::string& source_label,
                            int depth,
                            std::vector<std::string>& printed) {
    // Cycle / already-shown guard
    for (const auto& s : printed) if (s == name) return;
    printed.push_back(name);

    bool installed = fs::exists(fs::path(dep_root) / name / "orbit_pkg.json");

    std::string indent(depth * 4, ' ');
    std::string branch = (depth == 0) ? "  " : indent + "└─ ";

    printf("%s\033[1m%-20s\033[0m  %s",
           branch.c_str(), name.c_str(),
           installed ? "\033[1;32m✓\033[0m" : "\033[1;33m○ not fetched\033[0m");

    if (!version.empty())
        printf("  \033[0;90mv%s\033[0m", version.c_str());

    if (!source_label.empty())
        printf("  \033[0;90m%s\033[0m", source_label.c_str());

    printf("\n");

    if (installed) {
        try {
            PackageInfo pkg = read_package_info(
                (fs::path(dep_root) / name).string());

            // Stats line
            printf("%s    \033[0;90m%zu source%s  %zu header%s",
                   indent.c_str(),
                   pkg.sources.size(), pkg.sources.size() != 1 ? "s" : "",
                   pkg.headers.size(), pkg.headers.size() != 1 ? "s" : "");
            if (!pkg.description.empty())
                printf("  —  %s", pkg.description.c_str());
            printf("\033[0m\n");

            // Recurse into transitive deps
            for (const auto& td : pkg.dependencies) {
                std::string lbl = td.is_url() ? td.url : td.local_path;
                print_dep_tree(dep_root, td.name, td.version,
                               lbl, depth + 1, printed);
            }
        } catch (...) {}
    }
}

static int cmd_deps(const std::vector<std::string>& /*args*/) {
    std::string root = find_project_root();
    if (root.empty()) { ui::error("Not inside a Nova project."); return 1; }

    Manifest m;
    try { m = load_manifest(root); }
    catch (const std::exception& e) { ui::error(e.what()); return 1; }

    ui::header("orbit deps", m.name + " v" + m.version + " — dependency tree");

    if (m.dependencies.empty()) {
        ui::info("No dependencies declared.");
        printf("\n");
        return 0;
    }

    std::string dep_root = (fs::path(root) / ".orbit" / "deps").string();
    std::vector<std::string> printed;

    for (const auto& d : m.dependencies) {
        std::string lbl = d.is_url() ? d.url : (d.is_local() ? d.local_path : "");
        print_dep_tree(dep_root, d.name, d.version, lbl, 0, printed);
    }
    printf("\n");
    return 0;
}

// ── Command: fetch ────────────────────────────────────────────────────────────
// Install all deps without building (like `cargo fetch`)

static int cmd_fetch(const std::vector<std::string>& /*args*/) {
    std::string root = find_project_root();
    if (root.empty()) { ui::error("Not inside a Nova project."); return 1; }

    Manifest m;
    try { m = load_manifest(root); }
    catch (const std::exception& e) { ui::error(e.what()); return 1; }

    ui::header("orbit fetch", "Fetching dependencies for " + m.name);

    if (m.dependencies.empty()) {
        ui::info("No dependencies to fetch.");
        printf("\n");
        return 0;
    }

    try {
        auto sources = resolve_dependencies(m);
        ui::blank();
        ui::divider();
        ui::success("All dependencies ready  (" +
                    std::to_string(sources.size()) + " source file" +
                    (sources.size() != 1 ? "s" : "") + " collected)");
    } catch (const std::exception& e) {
        ui::error(e.what());
        return 1;
    }
    printf("\n");
    return 0;
}

// ── main ──────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    ui::init();

    if (argc < 2) {
        print_usage();
        return 0;
    }

    std::string command = argv[1];
    std::vector<std::string> rest;
    for (int i = 2; i < argc; i++) rest.push_back(argv[i]);

    // Normalize aliases
    if (command == "--help" || command == "-h") command = "help";
    if (command == "--version" || command == "-v") command = "version";

    if (command == "help")    { print_usage();   return 0; }
    if (command == "version") { print_version(); return 0; }
    if (command == "init")    return cmd_init(rest);
    if (command == "build")   return cmd_build(rest);
    if (command == "run")     return cmd_run(rest);
    if (command == "check")   return cmd_check(rest);
    if (command == "fetch")   return cmd_fetch(rest);
    if (command == "pack")    return cmd_pack(rest);
    if (command == "add")     return cmd_add(rest);
    if (command == "remove")  return cmd_remove(rest);
    if (command == "clean")   return cmd_clean(rest);
    if (command == "info")    return cmd_info(rest);
    if (command == "deps")    return cmd_deps(rest);

    ui::error("Unknown command: " + command);
    printf("\n  Run \033[1;36morbit help\033[0m for a list of commands.\n\n");
    return 1;
}

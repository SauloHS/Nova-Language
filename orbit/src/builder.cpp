#include "builder.h"
#include "package.h"
#include "downloader.h"
#include "ui.h"
#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <chrono>
#include <cstdlib>
#include <algorithm>
#include <fstream>

namespace fs = std::filesystem;

static fs::path orbit_dir(const Manifest& m) { return fs::path(m.project_root) / ".orbit"; }
static fs::path deps_dir (const Manifest& m) { return orbit_dir(m) / "deps"; }
static fs::path build_dir(const Manifest& m) { return fs::path(m.project_root) / "build"; }

// Collect all .npp files under a directory
static void collect_sources(const fs::path& dir, std::vector<std::string>& out) {
    if (!fs::exists(dir)) return;
    for (const auto& e : fs::recursive_directory_iterator(dir))
        if (e.is_regular_file() && e.path().extension() == ".npp")
            out.push_back(e.path().string());
}

// Copy all .nh headers from dep install dir into the project's src/ directory
// so that #include "gml.nh" works relative to the source file.
// We use a dedicated .orbit/headers/ folder and symlink/copy from there.
static void install_headers(const fs::path& dep_install,
                             const fs::path& headers_out) {
    if (!fs::exists(dep_install)) return;
    fs::create_directories(headers_out);
    for (const auto& e : fs::recursive_directory_iterator(dep_install)) {
        if (!e.is_regular_file()) continue;
        if (e.path().extension() != ".nh") continue;
        fs::path dst = headers_out / e.path().filename();
        fs::copy_file(e.path(), dst, fs::copy_options::overwrite_existing);
    }
}

// ── resolve_dependencies ──────────────────────────────────────────────────────

std::vector<std::string> resolve_dependencies(const Manifest& manifest) {
    std::vector<std::string> extra_sources;
    if (manifest.dependencies.empty()) return extra_sources;

    fs::path dep_root    = deps_dir(manifest);
    // Central header directory: .orbit/headers/
    // The compiler is invoked with sources relative to src/, so we copy
    // all dep headers here AND into src/ so #include "x.nh" resolves.
    fs::path headers_dir = orbit_dir(manifest) / "headers";
    fs::create_directories(dep_root);
    fs::create_directories(headers_dir);

    std::vector<std::string> already_seen;
    int total = (int)manifest.dependencies.size();
    int idx   = 0;

    for (const auto& dep : manifest.dependencies) {
        idx++;
        ui::step(idx, total, "Resolving: " + dep.name);

        fs::path dep_install = dep_root / dep.name;

        // ── Local ────────────────────────────────────────────────────────────
        if (dep.is_local()) {
            fs::path lp = fs::path(manifest.project_root) / dep.local_path;
            if (!fs::exists(lp))
                throw std::runtime_error(
                    "Local dependency '" + dep.name + "' not found at: " + lp.string());

            if (fs::is_directory(lp)) {
                collect_sources(lp, extra_sources);
                install_headers(lp, headers_dir);
                ui::success("  Local dir: " + lp.string());
            } else if (lp.extension() == ".orbit") {
                if (!fs::exists(dep_install / "orbit_pkg.json")) {
                    fs::create_directories(dep_install);
                    PackageInfo pkg = unpack_package(lp.string(), dep_install.string());
                    ui::success("  Unpacked: " + dep.name);
                    install_transitive_deps(pkg, dep_root.string(), already_seen);
                }
                collect_sources(dep_install, extra_sources);
                install_headers(dep_install, headers_dir);
                ui::success("  Ready: " + dep.name);
            } else {
                throw std::runtime_error(
                    "Local dep '" + dep.name + "' must be a directory or .orbit file.");
            }
            already_seen.push_back(dep.name);

        // ── URL ──────────────────────────────────────────────────────────────
        } else if (dep.is_url()) {
            fs::path cached = dep_root / (dep.name + ".orbit");
            if (!fs::exists(dep_install / "orbit_pkg.json")) {
                if (!fs::exists(cached)) {
                    ui::info("  Downloading: " + dep.url);
                    download_file(dep.url, cached.string());
                }
                fs::create_directories(dep_install);
                PackageInfo pkg = unpack_package(cached.string(), dep_install.string());
                ui::success("  Installed: " + dep.name +
                            (pkg.version.empty() ? "" : " v" + pkg.version));
                install_transitive_deps(pkg, dep_root.string(), already_seen);
            }
            collect_sources(dep_install, extra_sources);
            install_headers(dep_install, headers_dir);
            already_seen.push_back(dep.name);

        } else {
            ui::warn("Dep '" + dep.name + "' has no 'local' or 'url'. Skipping.");
        }
    }

    // Collect sources from any transitively-installed deps
    for (const auto& name : already_seen) {
        bool in_manifest = false;
        for (const auto& d : manifest.dependencies)
            if (d.name == name) { in_manifest = true; break; }
        if (in_manifest) continue;
        fs::path p = dep_root / name;
        if (fs::exists(p)) {
            collect_sources(p, extra_sources);
            install_headers(p, headers_dir);
        }
    }

    // Also copy all dep headers into src/ so that relative #include "x.nh"
    // from within src/main.npp finds them next to the source file.
    fs::path src_dir = fs::path(manifest.project_root) / "src";
    fs::create_directories(src_dir);
    for (const auto& e : fs::directory_iterator(headers_dir)) {
        if (e.is_regular_file() && e.path().extension() == ".nh") {
            fs::path dst = src_dir / e.path().filename();
            fs::copy_file(e.path(), dst, fs::copy_options::overwrite_existing);
        }
    }

    return extra_sources;
}

// ── build_project ─────────────────────────────────────────────────────────────

BuildResult build_project(const Manifest& manifest, BuildMode mode) {
    BuildResult result;
    auto t_start = std::chrono::steady_clock::now();

    const BuildProfile& profile = (mode == BuildMode::Release)
        ? manifest.release : manifest.dev;

    fs::path root(manifest.project_root);
    fs::path entry = root / manifest.entry_point;

    if (!fs::exists(entry))
        throw std::runtime_error(
            "Entry point not found: " + entry.string() +
            "\n  Check 'entry' in [build] of orbit.toml");

    std::vector<std::string> dep_sources = resolve_dependencies(manifest);

    fs::path build = build_dir(manifest);
    fs::create_directories(build);

    std::string out_name = manifest.output_name.empty() ? manifest.name
                                                        : manifest.output_name;
    fs::path out_path = build / out_name;
    result.output_path = out_path.string();

    // ── Assemble n++ command ──────────────────────────────────────────────────
    std::ostringstream cmd;
    cmd << "n++";
    cmd << " \"" << entry.string() << "\"";

    for (const auto& src : manifest.sources) {
        fs::path p = root / src;
        if (fs::exists(p)) cmd << " \"" << p.string() << "\"";
        else ui::warn("Extra source not found, skipping: " + src);
    }

    for (const auto& src : dep_sources)
        cmd << " \"" << src << "\"";

    cmd << " -o \"" << out_path.string() << "\"";

    if (profile.opt_level >= 2)
        cmd << " -O" << profile.opt_level;

    fs::path err_log = build / "orbit_build.log";
    cmd << " 2>\"" << err_log.string() << "\"";

    std::string full_cmd = cmd.str();
    ui::info("Invoking compiler:");
    printf("  \033[0;90m%s\033[0m\n", full_cmd.c_str());
    ui::blank();

    int ret = std::system(full_cmd.c_str());

    auto t_end = std::chrono::steady_clock::now();
    result.elapsed_secs = std::chrono::duration<double>(t_end - t_start).count();
    result.exit_code    = ret;
    result.success      = (ret == 0);

    if (fs::exists(err_log)) {
        std::ifstream lf(err_log);
        std::ostringstream lb; lb << lf.rdbuf();
        std::string content = lb.str();
        if (!content.empty()) {
            std::istringstream ls(content);
            std::string line;
            while (std::getline(ls, line)) {
                if (line.find("error") != std::string::npos ||
                    line.find("Error") != std::string::npos) ui::error(line);
                else if (line.find("warning") != std::string::npos ||
                         line.find("Warning") != std::string::npos) ui::warn(line);
                else printf("  %s\n", line.c_str());
            }
        }
    }

    return result;
}

// ── run_project ───────────────────────────────────────────────────────────────

int run_project(const Manifest& manifest, const std::vector<std::string>& args) {
    fs::path out = build_dir(manifest) / manifest.output_name;
    if (!fs::exists(out))
        throw std::runtime_error(
            "Binary not found: " + out.string() +
            "\n  Run 'orbit build' first.");
    std::ostringstream cmd;
    cmd << "\"" << out.string() << "\"";
    for (const auto& a : args) cmd << " \"" << a << "\"";
    return std::system(cmd.str().c_str());
}
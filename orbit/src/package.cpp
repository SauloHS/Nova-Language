#include "package.h"
#include "downloader.h"
#include "ui.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <cstdlib>
#include <algorithm>
#include <cstdio>
#include <iostream>

namespace fs = std::filesystem;

// ═════════════════════════════════════════════════════════════════════════════
// Minimal JSON builder / parser
// ═════════════════════════════════════════════════════════════════════════════

static std::string json_esc(const std::string& s) {
    std::string o;
    for (char c : s) {
        if      (c == '"')  o += "\\\"";
        else if (c == '\\') o += "\\\\";
        else if (c == '\n') o += "\\n";
        else if (c == '\r') o += "\\r";
        else                o += c;
    }
    return o;
}

static std::string json_str_arr(const std::vector<std::string>& v) {
    std::string o = "[\n";
    for (size_t i = 0; i < v.size(); i++) {
        o += "    \"" + json_esc(v[i]) + "\"";
        if (i + 1 < v.size()) o += ",";
        o += "\n";
    }
    o += "  ]";
    return o;
}

// ── JSON parser helpers ───────────────────────────────────────────────────────

// Read the raw value (string or number) for a top-level key in flat JSON.
static std::string jget(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    size_t p = json.find(needle);
    if (p == std::string::npos) return "";
    p = json.find(':', p + needle.size());
    if (p == std::string::npos) return "";
    // skip whitespace
    while (p < json.size() && (json[++p] == ' ' || json[p] == '\t' || json[p] == '\n'));
    if (p >= json.size()) return "";
    if (json[p] == '"') {
        size_t e = p + 1;
        while (e < json.size() && !(json[e] == '"' && json[e-1] != '\\')) e++;
        return json.substr(p + 1, e - p - 1);
    }
    // bare value (number / bool)
    size_t e = p;
    while (e < json.size() && json[e] != ',' && json[e] != '\n' && json[e] != '}') e++;
    std::string v = json.substr(p, e - p);
    while (!v.empty() && (v.back() == ' ' || v.back() == '\r')) v.pop_back();
    return v;
}

// Extract JSON array of strings for a key.
static std::vector<std::string> jget_str_arr(const std::string& json, const std::string& key) {
    std::vector<std::string> result;
    std::string needle = "\"" + key + "\"";
    size_t p = json.find(needle);
    if (p == std::string::npos) return result;
    p = json.find('[', p);
    if (p == std::string::npos) return result;
    size_t e = json.find(']', p);
    if (e == std::string::npos) return result;
    std::string inner = json.substr(p + 1, e - p - 1);
    size_t i = 0;
    while ((i = inner.find('"', i)) != std::string::npos) {
        size_t j = i + 1;
        while (j < inner.size() && !(inner[j] == '"' && inner[j-1] != '\\')) j++;
        result.push_back(inner.substr(i + 1, j - i - 1));
        i = j + 1;
    }
    return result;
}

// Extract array of JSON objects for key. Returns vector of raw object strings.
static std::vector<std::string> jget_obj_arr(const std::string& json, const std::string& key) {
    std::vector<std::string> result;
    std::string needle = "\"" + key + "\"";
    size_t p = json.find(needle);
    if (p == std::string::npos) return result;
    p = json.find('[', p);
    if (p == std::string::npos) return result;
    // Walk character by character tracking depth
    int depth = 0;
    bool in_obj = false;
    size_t obj_start = 0;
    for (size_t i = p; i < json.size(); i++) {
        char c = json[i];
        if (c == '[' && i == p) { depth = 1; continue; }
        if (c == '[') depth++;
        if (c == ']') { depth--; if (depth == 0) break; }
        if (c == '{' && depth == 1) { in_obj = true; obj_start = i; }
        if (c == '}' && depth == 1 && in_obj) {
            result.push_back(json.substr(obj_start, i - obj_start + 1));
            in_obj = false;
        }
    }
    return result;
}

// ═════════════════════════════════════════════════════════════════════════════
// write_package_info
// ═════════════════════════════════════════════════════════════════════════════

void write_package_info(const std::string& dir, const PackageInfo& info) {
    fs::path p = fs::path(dir) / "orbit_pkg.json";
    std::ofstream f(p);
    if (!f) throw std::runtime_error("Cannot write orbit_pkg.json to: " + dir);

    f << "{\n";
    f << "  \"name\":         \"" << json_esc(info.name)        << "\",\n";
    f << "  \"version\":      \"" << json_esc(info.version)     << "\",\n";
    f << "  \"author\":       \"" << json_esc(info.author)      << "\",\n";
    f << "  \"description\":  \"" << json_esc(info.description) << "\",\n";
    f << "  \"license\":      \"" << json_esc(info.license)     << "\",\n";
    f << "  \"nova_version\": \"" << json_esc(info.nova_version)<< "\",\n";
    f << "  \"sources\":  "       << json_str_arr(info.sources) << ",\n";
    f << "  \"headers\":  "       << json_str_arr(info.headers) << ",\n";

    // Dependencies array
    f << "  \"dependencies\": [\n";
    for (size_t i = 0; i < info.dependencies.size(); i++) {
        const auto& d = info.dependencies[i];
        f << "    {\n";
        f << "      \"name\":        \"" << json_esc(d.name)        << "\",\n";
        f << "      \"version\":     \"" << json_esc(d.version)     << "\",\n";
        f << "      \"url\":         \"" << json_esc(d.url)         << "\",\n";
        f << "      \"local_path\":  \"" << json_esc(d.local_path)  << "\",\n";
        f << "      \"description\": \"" << json_esc(d.description) << "\"\n";
        f << "    }";
        if (i + 1 < info.dependencies.size()) f << ",";
        f << "\n";
    }
    f << "  ]\n";
    f << "}\n";
}

// ═════════════════════════════════════════════════════════════════════════════
// read_package_info
// ═════════════════════════════════════════════════════════════════════════════

PackageInfo read_package_info(const std::string& dir) {
    fs::path p = fs::path(dir) / "orbit_pkg.json";
    if (!fs::exists(p))
        throw std::runtime_error("orbit_pkg.json not found in: " + dir);

    std::ifstream f(p);
    std::ostringstream buf;
    buf << f.rdbuf();
    std::string json = buf.str();

    PackageInfo info;
    info.name         = jget(json, "name");
    info.version      = jget(json, "version");
    info.author       = jget(json, "author");
    info.description  = jget(json, "description");
    info.license      = jget(json, "license");
    info.nova_version = jget(json, "nova_version");
    info.sources      = jget_str_arr(json, "sources");
    info.headers      = jget_str_arr(json, "headers");

    for (const auto& obj : jget_obj_arr(json, "dependencies")) {
        PkgDependency d;
        d.name        = jget(obj, "name");
        d.version     = jget(obj, "version");
        d.url         = jget(obj, "url");
        d.local_path  = jget(obj, "local_path");
        d.description = jget(obj, "description");
        if (!d.name.empty())
            info.dependencies.push_back(d);
    }
    return info;
}

// ═════════════════════════════════════════════════════════════════════════════
// pack_project
// ═════════════════════════════════════════════════════════════════════════════

std::string pack_project(const Manifest& manifest, const std::string& out_dir) {
    fs::path root(manifest.project_root);
    fs::path stage = root / ".orbit_stage";

    if (fs::exists(stage)) fs::remove_all(stage);
    fs::create_directories(stage / "src");
    fs::create_directories(stage / "include");

    PackageInfo info;
    info.name         = manifest.name;
    info.version      = manifest.version;
    info.author       = manifest.author;
    info.description  = manifest.description;
    info.license      = manifest.license;
    info.nova_version = manifest.nova_version;

    // ── Collect .npp sources ──────────────────────────────────────────────────
    fs::path src_dir = root / "src";
    if (fs::exists(src_dir)) {
        for (const auto& entry : fs::recursive_directory_iterator(src_dir)) {
            if (!entry.is_regular_file()) continue;
            if (entry.path().extension() != ".npp") continue;
            std::string rel = fs::relative(entry.path(), root).string();
            info.sources.push_back(rel);
            fs::path dst = stage / rel;
            fs::create_directories(dst.parent_path());
            fs::copy_file(entry.path(), dst, fs::copy_options::overwrite_existing);
        }
    }

    // ── Collect .nh headers ───────────────────────────────────────────────────
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".nh") continue;
        std::string rel = fs::relative(entry.path(), root).string();
        // Skip staging dirs, build dirs
        if (rel.find(".orbit") != std::string::npos) continue;
        if (rel.find("build")  != std::string::npos) continue;
        info.headers.push_back(rel);
        // Always flatten headers into include/
        fs::path dst = stage / "include" / entry.path().filename();
        fs::copy_file(entry.path(), dst, fs::copy_options::overwrite_existing);
    }

    // ── Embed dependency metadata from orbit.toml ─────────────────────────────
    // We convert the Manifest::Dependency list into PkgDependency so downstream
    // consumers know what transitive deps they need and where to fetch them.
    for (const auto& dep : manifest.dependencies) {
        PkgDependency pd;
        pd.name       = dep.name;
        pd.version    = dep.version;
        pd.url        = dep.url;
        pd.local_path = dep.local_path;
        // description not available from Manifest::Dependency — leave empty
        info.dependencies.push_back(pd);
    }

    // ── Write metadata ────────────────────────────────────────────────────────
    write_package_info(stage.string(), info);

    // ── Create archive ────────────────────────────────────────────────────────
    fs::path out_path = fs::path(out_dir) /
        (manifest.name + "-" + manifest.version + ".orbit");

    std::string cmd = "tar -czf \"" + out_path.string() +
                      "\" -C \"" + stage.string() + "\" .";
    int ret = std::system(cmd.c_str());
    fs::remove_all(stage);

    if (ret != 0)
        throw std::runtime_error("Failed to create archive: " + out_path.string());

    return out_path.string();
}

// ═════════════════════════════════════════════════════════════════════════════
// unpack_package
// ═════════════════════════════════════════════════════════════════════════════

PackageInfo unpack_package(const std::string& orbit_file,
                           const std::string& target_dir) {
    fs::create_directories(target_dir);
    std::string cmd = "tar -xzf \"" + orbit_file + "\" -C \"" + target_dir + "\"";
    int ret = std::system(cmd.c_str());
    if (ret != 0)
        throw std::runtime_error("Failed to unpack: " + orbit_file);
    return read_package_info(target_dir);
}

// ═════════════════════════════════════════════════════════════════════════════
// install_transitive_deps
// ═════════════════════════════════════════════════════════════════════════════

void install_transitive_deps(const PackageInfo& pkg,
                              const std::string& deps_root,
                              std::vector<std::string>& already_seen) {
    if (pkg.dependencies.empty()) return;

    // Collect which transitive deps are actually missing
    std::vector<const PkgDependency*> missing;
    for (const auto& dep : pkg.dependencies) {
        if (dep.name.empty()) continue;
        // Skip already processed (cycle guard)
        bool seen = false;
        for (const auto& s : already_seen)
            if (s == dep.name) { seen = true; break; }
        if (seen) continue;

        fs::path dep_dir = fs::path(deps_root) / dep.name;
        bool installed = fs::exists(dep_dir / "orbit_pkg.json");
        if (!installed) missing.push_back(&dep);
    }

    if (missing.empty()) return;

    // ── Interactive prompt ────────────────────────────────────────────────────
    ui::blank();
    ui::divider();
    printf("\n  \033[1;33m⚠  Package '\033[1;37m%s\033[1;33m' has %zu uninstalled dependenc%s:\033[0m\n\n",
           pkg.name.c_str(), missing.size(),
           missing.size() == 1 ? "y" : "ies");

    for (const auto* dep : missing) {
        printf("    \033[1m%-20s\033[0m", dep->name.c_str());
        if (!dep->version.empty())
            printf("  \033[0;90mv%s\033[0m", dep->version.c_str());
        if (!dep->description.empty())
            printf("  — %s", dep->description.c_str());
        printf("\n");
        if (!dep->url.empty())
            printf("    \033[0;90m  └─ %s\033[0m\n", dep->url.c_str());
    }
    printf("\n");

    // Ask once for all missing deps
    printf("  \033[1;33m?\033[0m  Install %s now? [Y/n]: ",
           missing.size() == 1 ? "it" : "them");
    fflush(stdout);

    std::string answer;
    std::getline(std::cin, answer);

    // Default yes on empty input
    bool do_install = answer.empty() ||
                      answer == "y"  || answer == "Y" ||
                      answer == "yes"|| answer == "YES";

    if (!do_install) {
        ui::warn("Skipping transitive dependencies. Build may fail.");
        return;
    }

    // ── Install each missing dep ──────────────────────────────────────────────
    for (const auto* dep : missing) {
        already_seen.push_back(dep->name);
        fs::path dep_dir  = fs::path(deps_root) / dep->name;
        fs::path dep_orbit = fs::path(deps_root) / (dep->name + ".orbit");

        ui::info("Fetching: " + dep->name + (dep->version.empty() ? "" : " v" + dep->version));

        if (!dep->url.empty()) {
            if (!fs::exists(dep_orbit)) {
                try {
                    download_file(dep->url, dep_orbit.string());
                } catch (const std::exception& e) {
                    ui::error("Failed to download '" + dep->name + "': " + e.what());
                    ui::warn("Skipping. Add it manually with: orbit add " +
                             dep->name + " " + dep->url);
                    continue;
                }
            }
            fs::create_directories(dep_dir);
            try {
                PackageInfo sub = unpack_package(dep_orbit.string(), dep_dir.string());
                ui::success("Installed: " + dep->name +
                            (sub.version.empty() ? "" : " v" + sub.version));
                // Recurse into this dep's own transitive deps
                install_transitive_deps(sub, deps_root, already_seen);
            } catch (const std::exception& e) {
                ui::error("Failed to unpack '" + dep->name + "': " + e.what());
            }
        } else if (!dep->local_path.empty()) {
            // Local path transitive dep (unusual but supported)
            fs::path lp(dep->local_path);
            if (!fs::exists(lp)) {
                ui::warn("Local transitive dep not found: " + dep->local_path);
                continue;
            }
            if (lp.extension() == ".orbit") {
                fs::create_directories(dep_dir);
                PackageInfo sub = unpack_package(lp.string(), dep_dir.string());
                ui::success("Installed (local): " + dep->name);
                install_transitive_deps(sub, deps_root, already_seen);
            } else {
                ui::warn("Cannot auto-install local directory dep '" + dep->name +
                         "'. Add it manually.");
            }
        } else {
            ui::warn("Dependency '" + dep->name +
                     "' has no URL or local path — cannot auto-install.");
            ui::warn("Add it manually: orbit add " + dep->name + " <url-or-path>");
        }
    }
    ui::blank();
}

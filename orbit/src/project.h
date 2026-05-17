#pragma once
#include <string>
#include <vector>
#include <map>
#include <optional>

// ── Dependency descriptor ─────────────────────────────────────────────────────
struct Dependency {
    std::string name;

    // Exactly one of these is set:
    std::string local_path;  // local = "./libs/mylib"
    std::string url;         // url   = "https://…/lib.orbit"
    std::string version;     // (future: registry lookup)

    bool is_local() const { return !local_path.empty(); }
    bool is_url()   const { return !url.empty(); }
};

// ── Build profile ─────────────────────────────────────────────────────────────
struct BuildProfile {
    int  opt_level = 0;      // 0, 2, 3
    bool debug     = true;
    // Future: extra flags
};

// ── Parsed orbit.toml ─────────────────────────────────────────────────────────
struct Manifest {
    // [package]
    std::string name;
    std::string version;
    std::string author;
    std::string description;
    std::string license;
    std::string nova_version;  // minimum n++ version

    // [build]
    std::string entry_point;           // default: "src/main.npp"
    std::string output_name;           // default: package.name
    std::string type;                  // "binary" | "library"
    std::vector<std::string> sources;  // extra source files
    std::vector<std::string> include_dirs;

    // [profile.release] / [profile.dev]
    BuildProfile dev;
    BuildProfile release;

    // [dependencies]
    std::vector<Dependency> dependencies;

    // Full path to the orbit.toml that was loaded
    std::string manifest_path;
    // Directory containing orbit.toml
    std::string project_root;

    bool is_library() const { return type == "library"; }
};

// Load and validate manifest from a directory (looks for orbit.toml inside)
Manifest load_manifest(const std::string& project_dir);

// Save a default manifest to a directory
void write_default_manifest(const std::string& project_dir,
                             const std::string& name,
                             const std::string& type = "binary");

#pragma once
#include "project.h"
#include <string>
#include <vector>

// ── .orbit Package Format (v2) ────────────────────────────────────────────────
//
// Structure inside the .orbit archive (tar.gz):
//
//   orbit_pkg.json        — full package metadata (see below)
//   src/                  — .npp source files
//   include/              — .nh header files
//
// orbit_pkg.json schema:
// {
//   "name":         "mylib",
//   "version":      "1.2.0",
//   "author":       "John",
//   "description":  "Does X",
//   "license":      "MIT",
//   "nova_version": ">=1.0",
//   "sources":      ["src/mylib.npp", ...],
//   "headers":      ["include/mylib.nh", ...],
//   "dependencies": [
//     {
//       "name":        "dep_a",
//       "version":     "0.3.0",
//       "url":         "https://example.com/dep_a-0.3.0.orbit",
//       "description": "..."
//     }
//   ]
// }
//
// When orbit installs a package that declares dependencies, it prompts
// the user interactively and installs them recursively if accepted.
// ─────────────────────────────────────────────────────────────────────────────

struct PkgDependency {
    std::string name;
    std::string version;
    std::string url;
    std::string local_path;
    std::string description;

    bool is_url()   const { return !url.empty(); }
    bool is_local() const { return !local_path.empty(); }
};

struct PackageInfo {
    std::string name;
    std::string version;
    std::string author;
    std::string description;
    std::string license;
    std::string nova_version;

    std::vector<std::string>   sources;
    std::vector<std::string>   headers;
    std::vector<PkgDependency> dependencies;
};

void       write_package_info(const std::string& dir, const PackageInfo& info);
PackageInfo read_package_info(const std::string& dir);

std::string pack_project(const Manifest& manifest, const std::string& out_dir);
PackageInfo unpack_package(const std::string& orbit_file, const std::string& target_dir);

// Inspect pkg.dependencies; for any missing under deps_root, prompt user
// and recursively install. already_seen prevents infinite loops.
void install_transitive_deps(const PackageInfo& pkg,
                              const std::string& deps_root,
                              std::vector<std::string>& already_seen);

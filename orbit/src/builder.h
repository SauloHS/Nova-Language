#pragma once
#include "project.h"
#include <string>
#include <vector>

enum class BuildMode {
    Dev,      // orbit build
    Release,  // orbit build --release
};

struct BuildResult {
    bool success = false;
    int  exit_code = 0;
    std::string output_path;
    double elapsed_secs = 0.0;
};

// Resolve all dependency source files (local + downloaded .orbit packages)
// and install missing ones. Returns list of extra .npp files to compile.
std::vector<std::string> resolve_dependencies(const Manifest& manifest);

// Build the project.
BuildResult build_project(const Manifest& manifest, BuildMode mode);

// Run the compiled binary (only for 'binary' type projects)
int run_project(const Manifest& manifest, const std::vector<std::string>& args);

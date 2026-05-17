#pragma once
#include <string>

// Download a file from URL to dest_path.
// Uses libcurl if available at compile time, otherwise falls back to
// invoking `curl` or `wget` from the system PATH.
//
// Throws std::runtime_error on failure.
// Returns dest_path on success.
std::string download_file(const std::string& url, const std::string& dest_path);

// Returns true if the URL points to a valid .orbit file (checks extension
// and optionally does a HEAD request).
bool is_orbit_url(const std::string& url);

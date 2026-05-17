#include "downloader.h"
#include "ui.h"
#include <stdexcept>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <algorithm>

#ifdef ORBIT_USE_LIBCURL
  #include <curl/curl.h>
#endif

namespace fs = std::filesystem;

// ── libcurl path ──────────────────────────────────────────────────────────────
#ifdef ORBIT_USE_LIBCURL

static size_t write_data(void* ptr, size_t size, size_t nmemb, FILE* stream) {
    return fwrite(ptr, size, nmemb, stream);
}

struct CurlProgress {
    std::string label;
    long last_pct = -1;
};

static int progress_cb(void* userdata,
                       curl_off_t dltotal, curl_off_t dlnow,
                       curl_off_t /*ultotal*/, curl_off_t /*ulnow*/) {
    if (dltotal <= 0) return 0;
    auto* p = static_cast<CurlProgress*>(userdata);
    int pct = (int)(dlnow * 100 / dltotal);
    if (pct != p->last_pct) {
        p->last_pct = pct;
        ui::progress(p->label, pct);
        // Move cursor up one line to overwrite
        printf("\033[1A");
    }
    return 0;
}

std::string download_file(const std::string& url, const std::string& dest_path) {
    CURL* curl = curl_easy_init();
    if (!curl) throw std::runtime_error("Failed to initialize libcurl");

    FILE* fp = fopen(dest_path.c_str(), "wb");
    if (!fp) {
        curl_easy_cleanup(curl);
        throw std::runtime_error("Cannot create file: " + dest_path);
    }

    CurlProgress prog;
    prog.label = "Downloading";

    curl_easy_setopt(curl, CURLOPT_URL,            url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  write_data);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,      fp);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT,      "orbit/" ORBIT_VERSION);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_cb);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA,   &prog);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS,     0L);

    CURLcode res = curl_easy_perform(curl);
    fclose(fp);

    if (res != CURLE_OK) {
        fs::remove(dest_path);
        curl_easy_cleanup(curl);
        throw std::runtime_error("Download failed: " + std::string(curl_easy_strerror(res)));
    }

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    if (http_code >= 400) {
        fs::remove(dest_path);
        throw std::runtime_error("HTTP " + std::to_string(http_code) + " for URL: " + url);
    }

    // Print final 100% bar
    ui::progress("Downloading", 100);
    printf("\n");

    return dest_path;
}

#else
// ── Fallback: invoke system curl or wget ─────────────────────────────────────

static bool has_command(const std::string& cmd) {
    std::string check = "command -v " + cmd + " >/dev/null 2>&1";
    return std::system(check.c_str()) == 0;
}

std::string download_file(const std::string& url, const std::string& dest_path) {
    std::string cmd;
    if (has_command("curl")) {
        cmd = "curl -fSL --progress-bar -o " + dest_path + " \"" + url + "\"";
    } else if (has_command("wget")) {
        cmd = "wget -q --show-progress -O " + dest_path + " \"" + url + "\"";
    } else {
        throw std::runtime_error(
            "No download tool found. Please install curl or wget."
        );
    }

    int ret = std::system(cmd.c_str());
    if (ret != 0) {
        if (fs::exists(dest_path)) fs::remove(dest_path);
        throw std::runtime_error("Download failed for: " + url);
    }
    return dest_path;
}

#endif

// ── URL helpers ───────────────────────────────────────────────────────────────

bool is_orbit_url(const std::string& url) {
    // Must start with http:// or https://
    if (url.find("http://") != 0 && url.find("https://") != 0) return false;
    // Must end with .orbit
    size_t q = url.find('?');
    std::string path = (q != std::string::npos) ? url.substr(0, q) : url;
    return path.size() > 6 && path.substr(path.size() - 6) == ".orbit";
}

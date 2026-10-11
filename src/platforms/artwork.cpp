#include "advait/platforms/platforms.hpp"

#include "advait/core/logger.hpp"

#include <curl/curl.h>

#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

namespace advait::platforms {
namespace {

constexpr size_t kMaxArtworkBytes = 6u * 1024u * 1024u;

size_t write_cb(char* ptr, size_t size, size_t nmemb, void* user) {
    auto* out = static_cast<std::string*>(user);
    if (out->size() + size * nmemb > kMaxArtworkBytes) return 0;
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

std::string sanitize(const std::string& url) {
    std::string key;
    key.reserve(url.size());
    for (char c : url) key.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    // FNV-1a keeps the path short and stable across runs.
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : key) {
        h ^= c;
        h *= 1099511628211ull;
    }
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
    return std::string(buf);
}

}  // namespace

std::string artwork_local_path(const std::string& url) {
    if (url.size() < 8 || url.compare(0, 4, "http") != 0) return {};
    static std::mutex mu;
    std::lock_guard<std::mutex> g(mu);

    std::error_code ec;
    std::filesystem::create_directories("cache/art", ec);
    const std::string dest = "cache/art/" + sanitize(url) + ".jpg";
    if (std::filesystem::exists(dest) && std::filesystem::file_size(dest, ec) > 0 && !ec) return dest;

    std::string body;
    CURL* curl = curl_easy_init();
    if (!curl) return {};
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 8L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0");
    const CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK || status >= 400 || body.empty()) {
        ADVAIT_LOG_DEBUG("platforms", "artwork fetch failed rc=" + std::to_string(static_cast<int>(rc)) +
                                          " status=" + std::to_string(status) + " url=" + url);
        return {};
    }
    std::ofstream out(dest, std::ios::binary);
    if (!out) return {};
    out.write(body.data(), static_cast<std::streamsize>(body.size()));
    out.close();
    return dest;
}

}  // namespace advait::platforms
